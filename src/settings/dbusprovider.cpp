/*
 * SPDX-FileCopyrightText: 2020~2020 CSSlayer <wengxt@gmail.com>
 * SPDX-FileCopyrightText: 2026 Fcitx5 Fusion contributors
 *
 * This file incorporates code from fcitx5-configtool, licensed under
 * GPL-2.0-or-later.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */
#include "dbusprovider.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <QEventLoop>
#include <QLocalSocket>
#include <QPointer>
#include <QTimer>
#include <QVariant>
#include <fusion/settings_protocol.hpp>
#include <ipc/pipe_security.hpp>

#include <cstring>
#include <utility>

namespace fcitx::kcm {
namespace {

constexpr qsizetype kHeaderSize = 5;
constexpr int kRequestTimeoutMs = 5000;

QDBusMessage emptyReply() {
    const auto carrier = QDBusMessage::createMethodCall(
        QStringLiteral("org.fcitx.Fusion"), QStringLiteral("/settings"),
        QStringLiteral("org.fcitx.Fusion.Settings"), QStringLiteral("GetConfig"));
    return carrier.createReply();
}

void appendU8(QByteArray &bytes, quint8 value) {
    bytes.append(static_cast<char>(value));
}

void appendU32(QByteArray &bytes, quint32 value) {
    bytes.append(reinterpret_cast<const char *>(&value), sizeof(value));
}

void appendU64(QByteArray &bytes, quint64 value) {
    bytes.append(reinterpret_cast<const char *>(&value), sizeof(value));
}

bool appendString(QByteArray &bytes, const QString &value) {
    const QByteArray utf8 = value.toUtf8();
    if (static_cast<quint32>(utf8.size()) > fusion::ipc::max_string_length) {
        return false;
    }
    appendU32(bytes, static_cast<quint32>(utf8.size()));
    bytes.append(utf8);
    return true;
}

bool readU8(const QByteArray &bytes, qsizetype &offset, quint8 &value) {
    if (offset < 0 || offset >= bytes.size()) {
        return false;
    }
    value = static_cast<quint8>(bytes.at(offset++));
    return true;
}

bool readU32(const QByteArray &bytes, qsizetype &offset, quint32 &value) {
    if (offset < 0 || bytes.size() - offset < static_cast<qsizetype>(sizeof(value))) {
        return false;
    }
    std::memcpy(&value, bytes.constData() + offset, sizeof(value));
    offset += static_cast<qsizetype>(sizeof(value));
    return true;
}

bool readU64(const QByteArray &bytes, qsizetype &offset, quint64 &value) {
    if (offset < 0 || bytes.size() - offset < static_cast<qsizetype>(sizeof(value))) {
        return false;
    }
    std::memcpy(&value, bytes.constData() + offset, sizeof(value));
    offset += static_cast<qsizetype>(sizeof(value));
    return true;
}

bool readString(const QByteArray &bytes, qsizetype &offset, QString &value) {
    quint32 size = 0;
    if (!readU32(bytes, offset, size) || size > fusion::ipc::max_string_length ||
        bytes.size() - offset < static_cast<qsizetype>(size)) {
        return false;
    }
    value = QString::fromUtf8(bytes.constData() + offset, static_cast<qsizetype>(size));
    offset += static_cast<qsizetype>(size);
    return true;
}

bool readValue(const QByteArray &bytes, qsizetype &offset, fusion::SettingsValue &value,
               std::size_t depth, std::size_t &nodes) {
    if (depth > fusion::ipc::max_settings_depth || ++nodes > fusion::ipc::max_settings_nodes) {
        return false;
    }
    quint8 isMap = 0;
    if (!readU8(bytes, offset, isMap) || isMap > 1) {
        return false;
    }
    if (isMap == 0) {
        QString scalar;
        if (!readString(bytes, offset, scalar)) {
            return false;
        }
        value = fusion::SettingsValue::string(scalar.toStdString());
        return true;
    }
    quint32 count = 0;
    if (!readU32(bytes, offset, count) || count > fusion::ipc::max_settings_map_entries ||
        nodes + count > fusion::ipc::max_settings_nodes) {
        return false;
    }
    value = fusion::SettingsValue::object();
    for (quint32 i = 0; i < count; ++i) {
        QString key;
        fusion::SettingsValue child;
        if (!readString(bytes, offset, key) ||
            !readValue(bytes, offset, child, depth + 1, nodes) ||
            !value.entries.emplace(key.toStdString(), std::move(child)).second) {
            return false;
        }
    }
    return true;
}

QVariant toVariant(const fusion::SettingsValue &value) {
    if (!value.is_map) {
        return QString::fromUtf8(value.scalar.data(), static_cast<qsizetype>(value.scalar.size()));
    }
    QVariantMap result;
    for (const auto &[key, child] : value.entries) {
        result.insert(QString::fromUtf8(key.data(), static_cast<qsizetype>(key.size())),
                      toVariant(child));
    }
    return result;
}

fusion::SettingsValue fromVariant(const QVariant &value, std::size_t depth, std::size_t &nodes) {
    if (depth > fusion::ipc::max_settings_depth || ++nodes > fusion::ipc::max_settings_nodes) {
        return {};
    }
    if (value.metaType().id() == QMetaType::QVariantMap) {
        fusion::SettingsValue result = fusion::SettingsValue::object();
        const QVariantMap map = value.toMap();
        if (map.size() > static_cast<qsizetype>(fusion::ipc::max_settings_map_entries)) {
            return {};
        }
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
            result.entries.emplace(it.key().toStdString(),
                                   fromVariant(it.value(), depth + 1, nodes));
        }
        return result;
    }
    if (value.metaType().id() == QMetaType::Bool) {
        return fusion::SettingsValue::string(value.toBool() ? "True" : "False");
    }
    return fusion::SettingsValue::string(value.toString().toStdString());
}

bool appendValue(QByteArray &bytes, const fusion::SettingsValue &value, std::size_t depth,
                 std::size_t &nodes) {
    if (depth > fusion::ipc::max_settings_depth || ++nodes > fusion::ipc::max_settings_nodes) {
        return false;
    }
    appendU8(bytes, value.is_map ? 1 : 0);
    if (!value.is_map) {
        return appendString(bytes, QString::fromUtf8(value.scalar.data(),
                                                     static_cast<qsizetype>(value.scalar.size())));
    }
    if (value.entries.size() > fusion::ipc::max_settings_map_entries) {
        return false;
    }
    appendU32(bytes, static_cast<quint32>(value.entries.size()));
    for (const auto &[key, child] : value.entries) {
        if (!appendString(bytes, QString::fromUtf8(key.data(), static_cast<qsizetype>(key.size()))) ||
            !appendValue(bytes, child, depth + 1, nodes)) {
            return false;
        }
    }
    return true;
}

bool readConfigSnapshot(const QByteArray &bytes, qsizetype &offset,
                        FcitxQtConfigTypeList &types, QVariantMap &values) {
    fusion::SettingsValue rawValues;
    std::size_t nodes = 0;
    if (!readValue(bytes, offset, rawValues, 0, nodes) || !rawValues.is_map) {
        return false;
    }
    values = toVariant(rawValues).toMap();
    quint32 typeCount = 0;
    if (!readU32(bytes, offset, typeCount) || typeCount > fusion::ipc::max_settings_types) {
        return false;
    }
    types.clear();
    for (quint32 i = 0; i < typeCount; ++i) {
        QString typeName;
        quint32 optionCount = 0;
        if (!readString(bytes, offset, typeName) || !readU32(bytes, offset, optionCount) ||
            optionCount > fusion::ipc::max_settings_options ||
            nodes + optionCount * 2 > fusion::ipc::max_settings_nodes) {
            return false;
        }
        FcitxQtConfigOptionList options;
        for (quint32 j = 0; j < optionCount; ++j) {
            QString name;
            QString type;
            QString description;
            fusion::SettingsValue defaultValue;
            fusion::SettingsValue properties;
            if (!readString(bytes, offset, name) || !readString(bytes, offset, type) ||
                !readString(bytes, offset, description) ||
                !readValue(bytes, offset, defaultValue, 0, nodes) ||
                !readValue(bytes, offset, properties, 0, nodes) || !properties.is_map) {
                return false;
            }
            FcitxQtConfigOption option;
            option.setName(name);
            option.setType(type);
            option.setDescription(description);
            option.setDefaultValue(QDBusVariant(toVariant(defaultValue)));
            option.setProperties(toVariant(properties).toMap());
            options.append(option);
        }
        FcitxQtConfigType configType;
        configType.setName(typeName);
        configType.setOptions(options);
        types.append(configType);
    }
    return true;
}

QDBusMessage configReply(const QVariantMap &values,
                         const FcitxQtConfigTypeList &types) {
    auto reply = emptyReply();
    reply.setArguments({QVariant::fromValue(QDBusVariant(values)), QVariant::fromValue(types)});
    return reply;
}

QByteArray makeFrame(fusion::ipc::MessageType type, const QByteArray &payload) {
    if (static_cast<quint32>(payload.size()) > fusion::ipc::max_message_payload) {
        return {};
    }
    QByteArray frame;
    const quint32 size = static_cast<quint32>(payload.size());
    frame.append(reinterpret_cast<const char *>(&size), sizeof(size));
    appendU8(frame, static_cast<quint8>(type));
    frame.append(payload);
    return frame;
}

} // namespace

void PendingCall::complete(const QDBusMessage &reply, SettingsSelection selection) {
    if (finished_) {
        return;
    }
    finished_ = true;
    reply_ = reply;
    selection_ = std::move(selection);
    if (!completionQueued_) {
        completionQueued_ = true;
        QTimer::singleShot(0, this, [this] { Q_EMIT finished(); });
    }
}

void PendingCall::fail(const QString &error) {
    complete(QDBusMessage::createError(QStringLiteral("org.fcitx.Fusion.Error"), error));
}

void PendingCall::waitForFinished() {
    if (finished_) {
        return;
    }
    QEventLoop loop;
    connect(this, &PendingCall::finished, &loop, &QEventLoop::quit);
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(kRequestTimeoutMs + 1500);
    loop.exec();
}

class PipeRequest final : public QObject {
public:
    PipeRequest(const QString &pipeName, ControllerProxy::Operation operation,
                const QString &uri, const QVariant &value, PendingCall *call,
                ControllerProxy *controller)
        : QObject(controller), socket_(new QLocalSocket(this)), timer_(new QTimer(this)),
          operation_(operation), uri_(uri), value_(value), call_(call),
          controller_(controller) {
        timer_->setSingleShot(true);
        connect(timer_, &QTimer::timeout, this, [this] {
            fail(operation_ == ControllerProxy::Operation::SetConfig
                     ? QStringLiteral("FusionHost did not confirm the apply; reload before retrying.")
                     : QStringLiteral("Timed out while contacting FusionHost."));
        });
        connect(socket_, &QLocalSocket::connected, this, [this] {
            const auto descriptor = socket_->socketDescriptor();
            if (descriptor == static_cast<qintptr>(-1) ||
                !fusion::ipc::verify_server_identity(reinterpret_cast<HANDLE>(descriptor))) {
                fail(QStringLiteral("The FusionHost pipe server identity could not be verified."));
                return;
            }
            sendHello();
        });
        connect(socket_, &QLocalSocket::readyRead, this, [this] { readAvailable(); });
        connect(socket_, &QLocalSocket::disconnected, this, [this] {
            if (!finished_) {
                fail(operation_ == ControllerProxy::Operation::SetConfig
                         ? QStringLiteral("FusionHost disconnected before confirming the apply; reload before retrying.")
                         : QStringLiteral("FusionHost disconnected before replying."));
            }
        });
        connect(socket_, &QLocalSocket::errorOccurred, this,
                [this](QLocalSocket::LocalSocketError) {
                    if (!finished_ && socket_->state() == QLocalSocket::UnconnectedState) {
                        fail(QStringLiteral("FusionHost is unavailable or does not support settings. Restart or update the Host, then retry."));
                    }
                });
        timer_->start(kRequestTimeoutMs);
        socket_->connectToServer(pipeName);
    }

private:
    void sendHello() {
        QByteArray payload;
        appendU32(payload, fusion::ipc::protocol_version);
        appendU32(payload, fusion::ipc::settings_api_version);
        const QByteArray frame = makeFrame(fusion::ipc::MessageType::SettingsHelloRequest, payload);
        if (frame.isEmpty() || socket_->write(frame) != frame.size()) {
            fail(QStringLiteral("Could not start the FusionHost settings handshake."));
            return;
        }
        phase_ = 1;
    }

    QByteArray makeRequestPayload() {
        QByteArray payload;
        switch (operation_) {
        case ControllerProxy::Operation::GetConfig:
            if (!appendString(payload, uri_)) {
                return {};
            }
            break;
        case ControllerProxy::Operation::SetConfig: {
            if (!appendString(payload, uri_)) {
                return {};
            }
            std::size_t nodes = 0;
            const auto values = fromVariant(value_.toMap(), 0, nodes);
            if (!values.is_map || !appendValue(payload, values, 0, nodes)) {
                return {};
            }
            break;
        }
        case ControllerProxy::Operation::GetSelection:
            break;
        case ControllerProxy::Operation::SelectInputMethod:
            if (!appendString(payload, value_.toString())) {
                return {};
            }
            break;
        }
        return payload;
    }

    fusion::ipc::MessageType requestType() const {
        switch (operation_) {
        case ControllerProxy::Operation::GetConfig:
            return fusion::ipc::MessageType::SettingsGetConfigRequest;
        case ControllerProxy::Operation::SetConfig:
            return fusion::ipc::MessageType::SettingsSetConfigRequest;
        case ControllerProxy::Operation::GetSelection:
            return fusion::ipc::MessageType::SettingsGetSelectionRequest;
        case ControllerProxy::Operation::SelectInputMethod:
            return fusion::ipc::MessageType::SettingsSelectInputMethodRequest;
        }
        return fusion::ipc::MessageType::SettingsGetConfigRequest;
    }

    fusion::ipc::MessageType responseType() const {
        switch (operation_) {
        case ControllerProxy::Operation::GetConfig:
            return fusion::ipc::MessageType::SettingsGetConfigResponse;
        case ControllerProxy::Operation::SetConfig:
            return fusion::ipc::MessageType::SettingsSetConfigResponse;
        case ControllerProxy::Operation::GetSelection:
            return fusion::ipc::MessageType::SettingsGetSelectionResponse;
        case ControllerProxy::Operation::SelectInputMethod:
            return fusion::ipc::MessageType::SettingsSelectInputMethodResponse;
        }
        return fusion::ipc::MessageType::SettingsGetConfigResponse;
    }

    void sendOperation() {
        const QByteArray payload = makeRequestPayload();
        if (payload.size() < 0 || static_cast<quint32>(payload.size()) > fusion::ipc::max_settings_payload) {
            fail(QStringLiteral("The settings value exceeds the supported request size."));
            return;
        }
        const QByteArray frame = makeFrame(requestType(), payload);
        if (frame.isEmpty() || socket_->write(frame) != frame.size()) {
            fail(QStringLiteral("Could not send the settings request to FusionHost."));
            return;
        }
        phase_ = 2;
    }

    void readAvailable() {
        incoming_.append(socket_->readAll());
        if (incoming_.size() > static_cast<qsizetype>(fusion::ipc::max_message_payload +
                                                       fusion::ipc::header_size)) {
            fail(QStringLiteral("FusionHost sent a settings response above the protocol bound."));
            return;
        }
        while (!finished_ && incoming_.size() >= fusion::ipc::header_size) {
            quint32 size = 0;
            std::memcpy(&size, incoming_.constData(), sizeof(size));
            if (size > fusion::ipc::max_message_payload) {
                fail(QStringLiteral("FusionHost sent an invalid settings frame length."));
                return;
            }
            const qsizetype frameSize = fusion::ipc::header_size + static_cast<qsizetype>(size);
            if (incoming_.size() < frameSize) {
                return;
            }
            const auto type = static_cast<fusion::ipc::MessageType>(
                static_cast<quint8>(incoming_.at(4)));
            const QByteArray payload = incoming_.mid(fusion::ipc::header_size,
                                                      static_cast<qsizetype>(size));
            incoming_.remove(0, frameSize);
            if (phase_ == 1) {
                handleHello(type, payload);
            } else if (phase_ == 2) {
                handleOperation(type, payload);
            } else {
                fail(QStringLiteral("FusionHost sent an unexpected settings message."));
            }
        }
    }

    void handleHello(fusion::ipc::MessageType type, const QByteArray &payload) {
        if (type != fusion::ipc::MessageType::SettingsHelloResponse) {
            fail(QStringLiteral("FusionHost does not support the settings API; update or restart the Host."));
            return;
        }
        qsizetype offset = 0;
        quint64 incarnation = 0;
        quint32 protocol = 0;
        quint64 session = 0;
        quint32 api = 0;
        quint8 status = 0;
        QString error;
        if (!readU64(payload, offset, incarnation) || incarnation == 0 ||
            !readU32(payload, offset, protocol) || !readU64(payload, offset, session) ||
            !readU32(payload, offset, api) || !readU8(payload, offset, status) ||
            !readString(payload, offset, error) || offset != payload.size()) {
            fail(QStringLiteral("FusionHost returned a malformed settings capability response."));
            return;
        }
        (void)session;
        if (protocol != fusion::ipc::protocol_version || api != fusion::ipc::settings_api_version ||
            status != static_cast<quint8>(fusion::SettingsStatus::Ok)) {
            controller_->provider_->setAvailable(false);
            fail(error.isEmpty()
                     ? QStringLiteral("FusionHost settings API version mismatch; update the package.")
                     : error);
            return;
        }
        incarnation_ = incarnation;
        controller_->provider_->setAvailable(true);
        sendOperation();
    }

    void handleOperation(fusion::ipc::MessageType type, const QByteArray &payload) {
        if (type != responseType()) {
            fail(QStringLiteral("FusionHost returned an unexpected settings response type."));
            return;
        }
        qsizetype offset = 0;
        quint64 incarnation = 0;
        quint8 statusCode = 0;
        QString error;
        if (!readU64(payload, offset, incarnation) || incarnation != incarnation_ ||
            !readU8(payload, offset, statusCode) ||
            statusCode > static_cast<quint8>(fusion::SettingsStatus::VersionMismatch) ||
            !readString(payload, offset, error)) {
            fail(QStringLiteral("FusionHost returned malformed or stale settings data."));
            return;
        }
        const auto status = static_cast<fusion::SettingsStatus>(statusCode);
        if (status != fusion::SettingsStatus::Ok) {
            if (status == fusion::SettingsStatus::VersionMismatch) {
                controller_->provider_->setAvailable(false);
            }
            call_->fail(error.isEmpty() ? QStringLiteral("FusionHost refused the settings request.") : error);
            finish();
            return;
        }
        if (operation_ == ControllerProxy::Operation::GetConfig ||
            operation_ == ControllerProxy::Operation::SetConfig) {
            FcitxQtConfigTypeList types;
            QVariantMap values;
            if (!readConfigSnapshot(payload, offset, types, values) ||
                offset != payload.size() || types.isEmpty()) {
                fail(QStringLiteral("FusionHost returned a malformed settings schema/value tree."));
                return;
            }
            call_->complete(configReply(values, types));
        } else {
            QString selected;
            quint32 count = 0;
            if (!readString(payload, offset, selected) || !readU32(payload, offset, count) ||
                count > fusion::ipc::max_vector_length) {
                fail(QStringLiteral("FusionHost returned a malformed input-method selection."));
                return;
            }
            SettingsSelection selection;
            selection.selected = selected;
            for (quint32 i = 0; i < count; ++i) {
                QString method;
                if (!readString(payload, offset, method)) {
                    fail(QStringLiteral("FusionHost returned a malformed input-method list."));
                    return;
                }
                selection.available.append(method);
            }
            if (offset != payload.size()) {
                fail(QStringLiteral("FusionHost returned trailing settings data."));
                return;
            }
            call_->complete(emptyReply(), std::move(selection));
        }
        finish();
    }

    void fail(const QString &error) {
        if (finished_) {
            return;
        }
        controller_->provider_->setAvailable(false);
        call_->fail(error);
        finish();
    }

    void finish() {
        if (finished_) {
            return;
        }
        finished_ = true;
        timer_->stop();
        socket_->abort();
        deleteLater();
    }

    QLocalSocket *socket_;
    QTimer *timer_;
    ControllerProxy::Operation operation_;
    QString uri_;
    QVariant value_;
    PendingCall *call_;
    ControllerProxy *controller_;
    QByteArray incoming_;
    quint64 incarnation_ = 0;
    int phase_ = 0;
    bool finished_ = false;
};

ControllerProxy::ControllerProxy(const QString &pipeName, DBusProvider *provider, QObject *parent)
    : QObject(parent), pipeName_(pipeName), provider_(provider) {}

void ControllerProxy::startRequest(Operation operation, const QString &uri,
                                   const QVariant &value, PendingCall *call) {
    if (pipeName_.isEmpty()) {
        QTimer::singleShot(0, call, [call] {
            call->fail(QStringLiteral("The current user/session FusionHost pipe is unavailable."));
        });
        provider_->setAvailable(false);
        return;
    }
    new PipeRequest(pipeName_, operation, uri, value, call, this);
}

PendingCall *ControllerProxy::GetConfig(const QString &uri) {
    auto *call = new PendingCall(this);
    startRequest(Operation::GetConfig, uri, {}, call);
    return call;
}

PendingCall *ControllerProxy::SetConfig(const QString &uri, const QDBusVariant &value) {
    auto *call = new PendingCall(this);
    startRequest(Operation::SetConfig, uri, value.variant(), call);
    return call;
}

PendingCall *ControllerProxy::GetSelection() {
    auto *call = new PendingCall(this);
    startRequest(Operation::GetSelection, {}, {}, call);
    return call;
}

PendingCall *ControllerProxy::SelectInputMethod(const QString &method) {
    auto *call = new PendingCall(this);
    startRequest(Operation::SelectInputMethod, {}, method, call);
    return call;
}

DBusProvider::DBusProvider(QObject *parent, const QString &pipeName) : QObject(parent) {
    registerFcitxQtDBusTypes();
    controller_ = new ControllerProxy(pipeName, this, this);
}

void DBusProvider::setAvailable(bool available) {
    if (available_ != available) {
        available_ = available;
        Q_EMIT availabilityChanged(available_);
    }
}

} // namespace fcitx::kcm
