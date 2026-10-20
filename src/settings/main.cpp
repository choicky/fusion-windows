/*
 * Copyright (C) 2026 Fcitx5 Fusion contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This executable contains derivative code and the upstream schema-driven
 * configtool widget subset. See third_party/settings-ui/ for upstream revisions,
 * notices, patch series and corresponding-source/build instructions.
 */

#include "configwidget.h"
#include "dbusprovider.h"
#include "ipc/pipe_security.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantMap>
#include <QWidget>

#include <clocale>
#include <functional>
#include <memory>
#include <string>

namespace {

// The endpoint handed to QLocalSocket::connectToServer(): the *logical* pipe name
// only. Qt prepends `\\.\pipe\` itself (qtbase v6.8.3, qlocalsocket_win.cpp), so
// the client must neither pass the device path nor strip the product identity:
// removing `FusionHost-v1-` as well made the window open on
// `\\.\pipe\<sid>-<session>` while the Host listens on
// `\\.\pipe\FusionHost-v1-<sid>-<session>`, which is the "FusionHost is
// unavailable or does not support settings" report on 0.0.0-settings+1f4b749.
QString endpointName(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    if (QString::fromLocal8Bit(argv[i]) != QLatin1String("--pipe-name")) {
      continue;
    }
    // An explicit endpoint must stand on its own: falling back to the production
    // pipe here would silently connect to a different Host than the one asked for.
    if (i + 1 >= argc) {
      return {};
    }
    const std::wstring supplied = QString::fromLocal8Bit(argv[i + 1]).toStdWString();
    return QString::fromStdWString(fusion::ipc::logical_pipe_name(supplied));
  }
  return QString::fromStdWString(fusion::ipc::logical_pipe_name(fusion::ipc::compute_pipe_name()));
}

QString inputMethodName(const QString &name) {
  if (name == QLatin1String("shuangpin")) {
    return QStringLiteral("双拼");
  }
  if (name == QLatin1String("pinyin")) {
    return QStringLiteral("拼音");
  }
  return name;
}

class SettingsWindow final : public QMainWindow {
public:
  explicit SettingsWindow(QString pipeName, bool selfTest = false, QString screenshot = {})
      : provider_(this, pipeName), selfTest_(selfTest),
        screenshot_(std::move(screenshot)) {
    setWindowTitle(QStringLiteral("Fusion 输入法设置"));
    resize(920, 760);

    auto *central = new QWidget(this);
    auto *outer = new QVBoxLayout(central);
    auto *status = new QLabel(QStringLiteral("正在连接 FusionHost…"), central);
    status_ = status;
    status->setWordWrap(true);
    outer->addWidget(status);

    // Stage A input-method management: the packaged runtime's defined methods,
    // the ordered enabled list (top to bottom, first row = the active method a
    // bound context follows), and add / remove / reorder controls. Writing the
    // list goes through the Host's official setGroup/save; it never deletes a
    // method's definition or learning data. This replaces the old two-method
    // dropdown, which could only switch the active method, not manage membership.
    auto *manageRow = new QHBoxLayout;
    auto *leftCol = new QVBoxLayout;
    leftCol->addWidget(new QLabel(QStringLiteral("可用输入方式："), central));
    availableList_ = new QListWidget(central);
    leftCol->addWidget(availableList_);
    auto *moveCol = new QVBoxLayout;
    auto *addButton = new QPushButton(QStringLiteral("添加 >"), central);
    auto *removeButton = new QPushButton(QStringLiteral("< 移除"), central);
    moveCol->addStretch(1);
    moveCol->addWidget(addButton);
    moveCol->addWidget(removeButton);
    moveCol->addStretch(1);
    auto *rightCol = new QVBoxLayout;
    rightCol->addWidget(
        new QLabel(QStringLiteral("已启用（从上到下；第一项为当前输入方式）："), central));
    enabledList_ = new QListWidget(central);
    rightCol->addWidget(enabledList_);
    auto *orderCol = new QVBoxLayout;
    auto *upButton = new QPushButton(QStringLiteral("上移"), central);
    auto *downButton = new QPushButton(QStringLiteral("下移"), central);
    imApply_ = new QPushButton(QStringLiteral("应用输入方式列表"), central);
    orderCol->addStretch(1);
    orderCol->addWidget(upButton);
    orderCol->addWidget(downButton);
    orderCol->addWidget(imApply_);
    orderCol->addStretch(1);
    manageRow->addLayout(leftCol, 1);
    manageRow->addLayout(moveCol);
    manageRow->addLayout(rightCol, 1);
    manageRow->addLayout(orderCol);
    outer->addLayout(manageRow);

    tabs_ = new QTabWidget(central);
    pinyinUri_ = QStringLiteral("fcitx://config/inputmethod/pinyin");
    shuangpinUri_ = QStringLiteral("fcitx://config/inputmethod/shuangpin");
    globalUri_ = QStringLiteral("fcitx://config/global");
    tabs_->addTab(makePage(pinyinUri_, pinyin_), QStringLiteral("拼音设置"));
    tabs_->addTab(makePage(shuangpinUri_, shuangpin_), QStringLiteral("双拼设置"));
    tabs_->addTab(makePage(globalUri_, global_), QStringLiteral("全局设置"));
    outer->addWidget(tabs_, 1);

    auto *buttons = new QHBoxLayout;
    apply_ = new QPushButton(QStringLiteral("应用"), central);
    cancel_ = new QPushButton(QStringLiteral("取消未应用的更改"), central);
    reload_ = new QPushButton(QStringLiteral("重新加载"), central);
    buttons->addStretch(1);
    buttons->addWidget(apply_);
    buttons->addWidget(cancel_);
    buttons->addWidget(reload_);
    outer->addLayout(buttons);

    auto *limitations = new QLabel(
        QStringLiteral("提示：输入法或设置变更会在当前输入组合结束后生效。词库管理及外部配置工具暂不提供。"),
        central);
    limitations->setWordWrap(true);
    outer->addWidget(limitations);
    setCentralWidget(central);

    connect(apply_, &QPushButton::clicked, this, [this] { applyCurrentPage(); });
    connect(cancel_, &QPushButton::clicked, this, [this, status] {
      status->setText(QStringLiteral("正在放弃未应用更改并重新加载…"));
      reloadAll(status, QStringLiteral("已放弃未应用的更改并重新加载。"));
    });
    connect(reload_, &QPushButton::clicked, this, [this, status] {
      status->setText(QStringLiteral("正在从 FusionHost 重新加载…"));
      reloadAll(status, QStringLiteral("已从 FusionHost 重新加载。"));
    });
    connect(addButton, &QPushButton::clicked, this, [this] {
      auto *item = availableList_->currentItem();
      if (!item) {
        return;
      }
      const QString name = item->data(Qt::UserRole).toString();
      availableList_->takeItem(availableList_->row(item));
      auto *moved = enabledList_->addItem(inputMethodName(name));
      moved->setData(Qt::UserRole, name);
      status_->setText(QStringLiteral("输入方式列表有未应用的更改。"));
    });
    connect(removeButton, &QPushButton::clicked, this, [this] {
      auto *item = enabledList_->currentItem();
      if (!item) {
        return;
      }
      if (enabledList_->count() <= 1) {
        status_->setText(QStringLiteral("至少需保留一种已启用的输入方式。"));
        return;
      }
      const QString name = item->data(Qt::UserRole).toString();
      enabledList_->takeItem(enabledList_->row(item));
      auto *moved = availableList_->addItem(inputMethodName(name));
      moved->setData(Qt::UserRole, name);
      status_->setText(QStringLiteral("输入方式列表有未应用的更改。"));
    });
    connect(upButton, &QPushButton::clicked, this,
            [this] { moveEnabledRow(enabledList_->currentRow(), -1); });
    connect(downButton, &QPushButton::clicked, this,
            [this] { moveEnabledRow(enabledList_->currentRow(), 1); });
    connect(imApply_, &QPushButton::clicked, this, [this, status] {
      applyImList(enabledOrder(), status);
    });
    connect(&provider_, &fcitx::kcm::DBusProvider::availabilityChanged, this,
            [status](bool available) {
              if (!available) {
                status->setText(QStringLiteral("FusionHost 未连接或不支持设置 API；请确认已启动兼容版本的 Host。"));
              }
            });
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int) { loadCurrentPage(); });
    connect(pinyin_, &fcitx::kcm::ConfigWidget::configurationLoaded, this,
            [this, status](bool ok, const QString &error) {
              pinyinLoaded_ = ok;
              if (ok) {
                pinyinLoads_++;
                baseline_[pinyin_] = pinyin_->value().toMap();
                status->setText(QStringLiteral("已从 FusionHost 加载拼音设置。"));
              } else {
                showError(status, error);
              }
            });
    connect(shuangpin_, &fcitx::kcm::ConfigWidget::configurationLoaded, this,
            [this, status](bool ok, const QString &error) {
              shuangpinLoaded_ = ok;
              if (ok) {
                shuangpinLoads_++;
                baseline_[shuangpin_] = shuangpin_->value().toMap();
                status->setText(QStringLiteral("已从 FusionHost 加载双拼设置。"));
              } else {
                showError(status, error);
              }
            });
    connect(pinyin_, &fcitx::kcm::ConfigWidget::changed, this, [this, status] {
      status->setText(QStringLiteral("拼音设置有未应用的更改。"));
    });
    connect(shuangpin_, &fcitx::kcm::ConfigWidget::changed, this, [this, status] {
      status->setText(QStringLiteral("双拼设置有未应用的更改。"));
    });
    connect(global_, &fcitx::kcm::ConfigWidget::configurationLoaded, this,
            [this, status](bool ok, const QString &error) {
              globalLoaded_ = ok;
              if (ok) {
                baseline_[global_] = global_->value().toMap();
                status->setText(QStringLiteral("已从 FusionHost 加载全局设置。"));
              } else {
                showError(status, error);
              }
            });
    connect(global_, &fcitx::kcm::ConfigWidget::changed, this, [this, status] {
      status->setText(QStringLiteral("全局设置有未应用的更改。"));
    });

    // Probe the input-method list first, then load the page(s). The Host admits
    // one settings request at a time; serializing startup avoids a false Busy
    // result.
    reloadImList([this] {
      if (selfTest_) {
        loadBothPages([this](bool ok, const QString &error) {
          if (!ok) {
            showError(status_, error);
            QCoreApplication::exit(13);
            return;
          }
          QTimer::singleShot(0, this, [this] { runSelfTest(); });
        });
        return;
      }
      loadCurrentPage(true);
    });
  }

private:
  QWidget *makePage(const QString &uri, fcitx::kcm::ConfigWidget *&page) {
    auto *wrapper = new QWidget(tabs_);
    auto *layout = new QVBoxLayout(wrapper);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(wrapper);
    scroll->setWidgetResizable(true);
    page = new fcitx::kcm::ConfigWidget(uri, &provider_);
    scroll->setWidget(page);
    layout->addWidget(scroll);
    return wrapper;
  }

  fcitx::kcm::ConfigWidget *currentPage() const {
    switch (tabs_->currentIndex()) {
    case 0:
      return pinyin_;
    case 1:
      return shuangpin_;
    case 2:
      return global_;
    default:
      return nullptr;
    }
  }

  bool loadedFor(const fcitx::kcm::ConfigWidget *page) const {
    if (page == pinyin_) {
      return pinyinLoaded_;
    }
    if (page == shuangpin_) {
      return shuangpinLoaded_;
    }
    if (page == global_) {
      return globalLoaded_;
    }
    return false;
  }

  void loadCurrentPage(bool force = false) {
    if (!provider_.available()) {
      return;
    }
    auto *page = currentPage();
    if (!page) {
      return;
    }
    if (force || !loadedFor(page)) {
      page->load();
    }
  }

  void reloadAll(QLabel *status, const QString &successMessage) {
    reloadImList([this, status, successMessage] {
      loadBothPages([this, status, successMessage](bool ok, const QString &error) {
        if (ok) {
          status->setText(successMessage);
        } else {
          showError(status, error);
        }
      });
    });
  }

  // The Host admits one settings request per connection, so the two pages are
  // loaded sequentially: Pinyin first, then Shuangpin.
  void loadBothPages(std::function<void(bool, const QString &)> onDone) {
    pinyinLoaded_ = false;
    shuangpinLoaded_ = false;
    auto secondPage = std::make_shared<std::function<void()>>();
    *secondPage = [this, onDone] {
      connect(shuangpin_, &fcitx::kcm::ConfigWidget::configurationLoaded, this,
              [onDone](bool ok, const QString &error) { onDone(ok, error); },
              Qt::SingleShotConnection);
      shuangpin_->load();
    };
    connect(pinyin_, &fcitx::kcm::ConfigWidget::configurationLoaded, this,
            [secondPage, onDone](bool ok, const QString &error) {
              if (ok) {
                (*secondPage)();
              } else {
                onDone(false, error);
              }
            }, Qt::SingleShotConnection);
    pinyin_->load();
  }

  QString uriFor(const fcitx::kcm::ConfigWidget *page) const {
    if (page == pinyin_) {
      return pinyinUri_;
    }
    if (page == shuangpin_) {
      return shuangpinUri_;
    }
    if (page == global_) {
      return globalUri_;
    }
    return {};
  }

  // Both pages stay loaded so switching tabs keeps unsaved edits. Submitting the
  // whole page would therefore write back values the other page read before its
  // last apply, so only the options this page actually changed are submitted; the
  // Host merges submitted options onto its live configuration and leaves the rest
  // untouched. An empty delta means there is nothing to apply.
  void applyCurrentPage(std::function<void(bool, const QString &)> onDone = {}) {
    auto *page = currentPage();
    if (!page) {
      return;
    }
    if (!provider_.controller()) {
      const QString error = QStringLiteral("无法创建 FusionHost 连接请求。");
      showError(status_, error);
      if (onDone) {
        onDone(false, error);
      }
      return;
    }
    const QVariantMap current = page->value().toMap();
    const QVariantMap baseline = baseline_.value(page);
    QVariantMap delta;
    for (auto it = current.cbegin(); it != current.cend(); ++it) {
      if (baseline.value(it.key()) != it.value()) {
        delta.insert(it.key(), it.value());
      }
    }
    if (delta.isEmpty()) {
      status_->setText(QStringLiteral("没有需要应用的更改。"));
      if (onDone) {
        onDone(true, QString());
      }
      return;
    }
    apply_->setEnabled(false);
    status_->setText(QStringLiteral("正在应用；不会重启 FusionHost…"));
    auto *call = provider_.controller()->SetConfig(uriFor(page), QDBusVariant(delta));
    connect(call, &fcitx::kcm::PendingCall::finished, this,
            [this, call, page, onDone] {
              const QDBusMessage reply = call->reply();
              call->deleteLater();
              apply_->setEnabled(true);
              QString error;
              if (reply.type() != QDBusMessage::ReplyMessage) {
                error = reply.errorMessage();
                if (error.isEmpty()) {
                  error = QStringLiteral("FusionHost 未确认应用结果；请重新加载后再试。");
                }
              } else if (reply.arguments().size() < 2) {
                error = QStringLiteral("FusionHost 未返回应用后的设置。");
              }
              if (!error.isEmpty()) {
                showError(status_, error);
                if (onDone) {
                  onDone(false, error);
                }
                return;
              }
              // Adopt the Host's authoritative result for the page that was just
              // applied; the other page keeps its own unsaved edits untouched.
              page->setValue(reply.arguments().at(0).value<QDBusVariant>().variant());
              baseline_[page] = page->value().toMap();
              status_->setText(QStringLiteral("FusionHost 已确认应用设置。"));
              if (onDone) {
                onDone(true, QString());
              }
            });
  }

  // Reads authoritative Host state without touching either page's widgets, so the
  // self-test can assert what the Host persisted rather than what the UI shows.
  void readHostValue(const QString &uri, const QString &key,
                     std::function<void(bool, const QString &)> onDone) {
    if (!provider_.controller()) {
      onDone(false, QString());
      return;
    }
    auto *call = provider_.controller()->GetConfig(uri);
    connect(call, &fcitx::kcm::PendingCall::finished, this, [call, key, onDone] {
      const QDBusMessage reply = call->reply();
      call->deleteLater();
      if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
        onDone(false, QString());
        return;
      }
      const QVariantMap values =
          reply.arguments().at(0).value<QDBusVariant>().variant().toMap();
      onDone(true, values.value(key).toString());
    });
  }

  void runSelfTest() {
    if (!selfTestTimer_.isValid()) {
      selfTestTimer_.start();
    }
    if (selfTestTimer_.elapsed() > 20000) {
      status_->setText(QStringLiteral("SELF-TEST FAIL: timed out waiting for the Host/config schema."));
      QCoreApplication::exit(10);
      return;
    }
    if (!pinyinLoaded_ || !shuangpinLoaded_) {
      QTimer::singleShot(100, this, [this] { runSelfTest(); });
      return;
    }
    if (pinyinLoads_ != 1 || shuangpinLoads_ != 1) {
      status_->setText(
          QStringLiteral("SELF-TEST FAIL: a page was loaded more than once before the scenario."));
      QCoreApplication::exit(22);
      return;
    }
    const QString initial = pinyin_->value().toMap().value(QStringLiteral("PageSize")).toString();
    if (initial.isEmpty()) {
      status_->setText(QStringLiteral("SELF-TEST FAIL: PageSize was not rendered."));
      QCoreApplication::exit(11);
      return;
    }
    // Step 1: change the shared value on the page that is already current.
    QVariantMap pinyinValues = pinyin_->value().toMap();
    pinyinValues.insert(QStringLiteral("PageSize"), 9);
    pinyin_->setValue(pinyinValues);
    applyCurrentPage([this](bool applied, const QString &error) {
      if (!applied) {
        showError(status_, error);
        QCoreApplication::exit(12);
        return;
      }
      // Step 2: return to the already-loaded Shuangpin page. Switching tabs must
      // not reload it, otherwise the stale-page scenario would be bypassed.
      const int loadsBeforeSwitch = shuangpinLoads_;
      tabs_->setCurrentIndex(1);
      if (shuangpinLoads_ != loadsBeforeSwitch) {
        status_->setText(
            QStringLiteral("SELF-TEST FAIL: switching tabs reloaded the already-loaded page."));
        QCoreApplication::exit(17);
        return;
      }
      // Step 3: change a different, Shuangpin-specific setting and apply.
      QVariantMap shuangpinValues = shuangpin_->value().toMap();
      if (!shuangpinValues.contains(QStringLiteral("ShuangpinProfile"))) {
        status_->setText(QStringLiteral("SELF-TEST FAIL: ShuangpinProfile was not rendered."));
        QCoreApplication::exit(18);
        return;
      }
      const QString profile = shuangpinValues.value(QStringLiteral("ShuangpinProfile")).toString();
      shuangpinValues.insert(QStringLiteral("ShuangpinProfile"),
                             profile == QLatin1String("MS") ? QStringLiteral("Ziranma")
                                                            : QStringLiteral("MS"));
      shuangpin_->setValue(shuangpinValues);
      applyCurrentPage([this](bool appliedSecond, const QString &secondError) {
        if (!appliedSecond) {
          showError(status_, secondError);
          QCoreApplication::exit(15);
          return;
        }
        // Step 4: read authoritative Host state, not the widget value.
        readHostValue(shuangpinUri_, QStringLiteral("PageSize"),
                      [this](bool read, const QString &pageSize) {
                        if (!read) {
                          status_->setText(
                              QStringLiteral("SELF-TEST FAIL: could not read the applied settings."));
                          QCoreApplication::exit(19);
                          return;
                        }
                        if (pageSize != QLatin1String("9")) {
                          showError(status_,
                                    QStringLiteral(
                                        "SELF-TEST FAIL: the Shuangpin apply restored stale PageSize=%1.")
                                        .arg(pageSize));
                          QCoreApplication::exit(14);
                          return;
                        }
                        runReverseSelfTest();
                      });
      });
    });
  }

  // Reverse direction: Shuangpin changes the shared value, then the already-loaded
  // (now stale) Pinyin page applies an unrelated change.
  void runReverseSelfTest() {
    QVariantMap shuangpinValues = shuangpin_->value().toMap();
    shuangpinValues.insert(QStringLiteral("PageSize"), 8);
    shuangpin_->setValue(shuangpinValues);
    applyCurrentPage([this](bool applied, const QString &error) {
      if (!applied) {
        showError(status_, error);
        QCoreApplication::exit(20);
        return;
      }
      tabs_->setCurrentIndex(0);
      QVariantMap pinyinValues = pinyin_->value().toMap();
      if (!pinyinValues.contains(QStringLiteral("SpellEnabled"))) {
        status_->setText(QStringLiteral("SELF-TEST FAIL: SpellEnabled was not rendered."));
        QCoreApplication::exit(18);
        return;
      }
      pinyinValues.insert(QStringLiteral("SpellEnabled"),
                          !pinyinValues.value(QStringLiteral("SpellEnabled")).toBool());
      pinyin_->setValue(pinyinValues);
      applyCurrentPage([this](bool appliedSecond, const QString &secondError) {
        if (!appliedSecond) {
          showError(status_, secondError);
          QCoreApplication::exit(20);
          return;
        }
        readHostValue(pinyinUri_, QStringLiteral("PageSize"),
                      [this](bool read, const QString &pageSize) {
                        if (!read) {
                          status_->setText(QStringLiteral(
                              "SELF-TEST FAIL: could not read the reverse applied settings."));
                          QCoreApplication::exit(19);
                          return;
                        }
                        if (pageSize != QLatin1String("8")) {
                          showError(status_,
                                    QStringLiteral(
                                        "SELF-TEST FAIL: the Pinyin apply restored stale PageSize=%1.")
                                        .arg(pageSize));
                          QCoreApplication::exit(21);
                          return;
                        }
                        status_->setText(QStringLiteral(
                            "SELF-TEST PASS: a shared value survived an unrelated apply on the other page in both directions."));
                        if (!screenshot_.isEmpty()) {
                          windowScreenshot_ = grab().save(screenshot_);
                        }
                        QCoreApplication::exit(windowScreenshot_ || screenshot_.isEmpty() ? 0
                                                                                         : 16);
                      });
      });
    });
  }

  void showError(QLabel *status, const QString &error) {
    const QString message = error.isEmpty()
                                ? QStringLiteral("FusionHost 未确认操作结果。请重新加载；不要重复提交不确定的更改。")
                                : error;
    status->setText(QStringLiteral("操作未完成：%1").arg(message));
    QMessageBox::warning(this, QStringLiteral("Fusion 输入法设置"), message);
  }

  void reloadImList(std::function<void()> continuation = {}) {
    if (!provider_.controller()) {
      if (continuation) {
        continuation();
      }
      return;
    }
    auto *call = provider_.controller()->GetImList();
    connect(call, &fcitx::kcm::PendingCall::finished, this,
            [this, call, continuation = std::move(continuation)]() mutable {
              const auto reply = call->reply();
              const auto list = call->imList();
              call->deleteLater();
              if (reply.type() == QDBusMessage::ReplyMessage) {
                populateImList(list);
                if (continuation) {
                  continuation();
                }
              } else {
                showError(status_, reply.errorMessage());
              }
            });
  }

  // Splits the Host's authoritative list into the two panes: the enabled methods
  // keep their order (top row = Items/0 = the active method), and every defined
  // method not in the enabled list appears as available to add.
  void populateImList(const fcitx::kcm::SettingsImList &list) {
    availableList_->clear();
    enabledList_->clear();
    for (const auto &name : list.enabled) {
      auto *item = enabledList_->addItem(inputMethodName(name));
      item->setData(Qt::UserRole, name);
    }
    for (const auto &name : list.available) {
      if (list.enabled.contains(name)) {
        continue;
      }
      auto *item = availableList_->addItem(inputMethodName(name));
      item->setData(Qt::UserRole, name);
    }
  }

  QStringList enabledOrder() const {
    QStringList order;
    for (int i = 0; i < enabledList_->count(); ++i) {
      order.append(enabledList_->item(i)->data(Qt::UserRole).toString());
    }
    return order;
  }

  void moveEnabledRow(int row, int delta) {
    const int target = row + delta;
    if (row < 0 || target < 0 || target >= enabledList_->count()) {
      return;
    }
    auto *item = enabledList_->takeItem(row);
    enabledList_->insertItem(target, item);
    enabledList_->setCurrentRow(target);
    status_->setText(QStringLiteral("输入方式列表有未应用的更改。"));
  }

  // Writes the enabled order through the Host's official setGroup/save. The Host
  // refuses an empty list and any method it does not define; a failure reloads the
  // authoritative list so the panes never drift from what the Host persisted.
  void applyImList(const QStringList &enabled, QLabel *status) {
    if (enabled.isEmpty()) {
      status->setText(QStringLiteral("至少需保留一种已启用的输入方式。"));
      return;
    }
    if (!provider_.controller()) {
      showError(status, QStringLiteral("无法创建 FusionHost 重连请求。"));
      return;
    }
    imApply_->setEnabled(false);
    status->setText(QStringLiteral("正在更新输入方式列表…"));
    auto *call = provider_.controller()->SetImList(enabled);
    connect(call, &fcitx::kcm::PendingCall::finished, this,
            [this, call, status] {
              const auto reply = call->reply();
              const auto list = call->imList();
              call->deleteLater();
              imApply_->setEnabled(true);
              if (reply.type() != QDBusMessage::ReplyMessage) {
                showError(status, reply.errorMessage());
                reloadImList();
                return;
              }
              populateImList(list);
              status->setText(QStringLiteral("FusionHost 已确认输入方式列表。"));
            });
  }

  fcitx::kcm::DBusProvider provider_;
  QLabel *status_ = nullptr;
  QTabWidget *tabs_ = nullptr;
  QListWidget *availableList_ = nullptr;
  QListWidget *enabledList_ = nullptr;
  QPushButton *imApply_ = nullptr;
  QPushButton *apply_ = nullptr;
  QPushButton *cancel_ = nullptr;
  QPushButton *reload_ = nullptr;
  fcitx::kcm::ConfigWidget *pinyin_ = nullptr;
  fcitx::kcm::ConfigWidget *shuangpin_ = nullptr;
  fcitx::kcm::ConfigWidget *global_ = nullptr;
  QString pinyinUri_;
  QString shuangpinUri_;
  QString globalUri_;
  // Last authoritative values per page, used to submit only what this page changed.
  QHash<fcitx::kcm::ConfigWidget *, QVariantMap> baseline_;
  int pinyinLoads_ = 0;
  int shuangpinLoads_ = 0;
  bool pinyinLoaded_ = false;
  bool shuangpinLoaded_ = false;
  bool globalLoaded_ = false;
  bool selfTest_ = false;
  bool windowScreenshot_ = false;
  QString screenshot_;
  QElapsedTimer selfTestTimer_;
};

} // namespace

int main(int argc, char **argv) {
  std::setlocale(LC_ALL, "");
  QApplication application(argc, argv);
  QApplication::setApplicationName(QStringLiteral("Fusion Settings"));
  QApplication::setOrganizationName(QStringLiteral("Fcitx5 Fusion"));
  const QString pipeName = endpointName(argc, argv);
  bool selfTest = false;
  QString screenshot;
  for (int i = 1; i < argc; ++i) {
    const QString arg = QString::fromLocal8Bit(argv[i]);
    if (arg == QLatin1String("--self-test")) {
      selfTest = true;
    } else if (arg == QLatin1String("--screenshot") && i + 1 < argc) {
      screenshot = QString::fromLocal8Bit(argv[++i]);
    }
  }
  if (pipeName.isEmpty()) {
    QMessageBox::critical(nullptr, QStringLiteral("Fusion 输入法设置"),
                          QStringLiteral("无法确定当前用户和会话的 FusionHost 管道。"));
    return 2;
  }
  SettingsWindow window(pipeName, selfTest, screenshot);
  window.show();
  return application.exec();
}
