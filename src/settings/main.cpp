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
#include <QListWidgetItem>
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
#include <cstdio>
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

// The Global-page probe value used by the self-test/verify pair. It is written to
// the disposable test profile only; a real user's global configuration is never
// touched by the harness.
constexpr int kGlobalPageSizeProbe = 7;

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
  explicit SettingsWindow(QString pipeName, bool selfTest = false, bool verifyOnly = false,
                          QString screenshot = {})
      : provider_(this, pipeName), selfTest_(selfTest), verifyOnly_(verifyOnly),
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
    // The Global page renders the official fcitx://config/global schema, which
    // contains options this Windows path cannot honour. They stay visible — the
    // schema is Fcitx's — so the page says which categories do not apply instead
    // of implying that every option shown here changes input behaviour.
    tabs_->addTab(makePage(globalUri_, global_, globalPageNote()), QStringLiteral("全局设置"));
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
      auto *moved = new QListWidgetItem(inputMethodName(name), enabledList_);
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
      auto *moved = new QListWidgetItem(inputMethodName(name), availableList_);
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
      if (selfTest_ || verifyOnly_) {
        loadAllPages([this](bool ok, const QString &error) {
          if (!ok) {
            showError(status_, error);
            QCoreApplication::exit(13);
            return;
          }
          QTimer::singleShot(0, this, [this] {
            if (verifyOnly_) {
              runVerify();
            } else {
              runSelfTest();
            }
          });
        });
        return;
      }
      loadCurrentPage(true);
    });
  }

  // Only used by --self-test-verify: the order the harness read from the persisted
  // profile before the restart.
  void setExpectedImList(const QStringList &list) { expectedImList_ = list; }

private:
  // One precise sentence per unsupported category. The official schema is reused
  // as-is, so these options are neither hidden nor disabled; the note states what
  // does not reach the engine on this Windows path.
  static QString globalPageNote() {
    return QStringLiteral(
        "说明：本页由 Fcitx 官方全局配置架构生成。以下类别在 Windows 上不生效或不完全生效——"
        "全局快捷键（触发/激活/停用/翻页/候选移动/ModifierOnlyKeyTimeout）：Fusion 不把单独的修饰键或 "
        "Ctrl/Alt/Win 组合键交给引擎，Ctrl+Space 由 Windows 输入法切换负责；"
        "输入法状态提示与面板相关选项：本产品不提供 Fcitx 面板，候选窗是产品自己的投影；"
        "密码框相关选项：Windows 输入范围策略先于引擎生效，这里的设置不会覆盖它。"
        "ActiveByDefault 与 resetStateWhenFocusIn 仅部分生效。其余选项按 Fcitx 语义生效。");
  }

  QWidget *makePage(const QString &uri, fcitx::kcm::ConfigWidget *&page, const QString &note = {}) {
    auto *wrapper = new QWidget(tabs_);
    auto *layout = new QVBoxLayout(wrapper);
    layout->setContentsMargins(0, 0, 0, 0);
    if (!note.isEmpty()) {
      auto *noteLabel = new QLabel(note, wrapper);
      noteLabel->setWordWrap(true);
      layout->addWidget(noteLabel);
    }
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

  // onDone is for the unattended self-test, which has to know when the reload
  // chain finished; the buttons pass nothing and rely on the status label.
  void reloadAll(QLabel *status, const QString &successMessage,
                 std::function<void(bool, const QString &)> onDone = {}) {
    reloadImList([this, status, successMessage, onDone] {
      loadAllPages([this, status, successMessage, onDone](bool ok, const QString &error) {
        if (ok) {
          status->setText(successMessage);
        } else {
          showError(status, error);
        }
        if (onDone) {
          onDone(ok, error);
        }
      });
    });
  }

  // The Host admits one settings request per connection, so a full reload walks
  // the pages sequentially: the next request starts only after the previous reply.
  // Every page the Cancel/Reload buttons claim to reload is included — leaving the
  // Global page out left its edits and baseline untouched while reporting success.
  struct ReloadChain {
    std::vector<fcitx::kcm::ConfigWidget *> pages;
    std::function<void(bool, const QString &)> onDone;
    std::size_t index = 0;
  };

  void stepReload(const std::shared_ptr<ReloadChain> &chain) {
    if (chain->index >= chain->pages.size()) {
      chain->onDone(true, QString());
      return;
    }
    auto *page = chain->pages[chain->index];
    connect(page, &fcitx::kcm::ConfigWidget::configurationLoaded, this,
            [this, chain](bool ok, const QString &error) {
              if (!ok) {
                chain->onDone(false, error);
                return;
              }
              chain->index++;
              stepReload(chain);
            },
            Qt::SingleShotConnection);
    page->load();
  }

  void loadAllPages(std::function<void(bool, const QString &)> onDone) {
    pinyinLoaded_ = false;
    shuangpinLoaded_ = false;
    globalLoaded_ = false;
    auto chain = std::make_shared<ReloadChain>();
    chain->pages = {pinyin_, shuangpin_, global_};
    chain->onDone = std::move(onDone);
    stepReload(chain);
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
  void readHostValues(const QString &uri,
                      std::function<void(bool, const QVariantMap &)> onDone) {
    if (!provider_.controller()) {
      onDone(false, {});
      return;
    }
    auto *call = provider_.controller()->GetConfig(uri);
    connect(call, &fcitx::kcm::PendingCall::finished, this, [call, onDone] {
      const QDBusMessage reply = call->reply();
      call->deleteLater();
      if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
        onDone(false, {});
        return;
      }
      onDone(true, reply.arguments().at(0).value<QDBusVariant>().variant().toMap());
    });
  }

  void readHostValue(const QString &uri, const QString &key,
                     std::function<void(bool, const QString &)> onDone) {
    readHostValues(uri, [key, onDone = std::move(onDone)](bool ok, const QVariantMap &values) {
      onDone(ok, ok ? values.value(key).toString() : QString());
    });
  }

  // Writes through the Host's apply path without adopting the result into a page,
  // which is what a *second* settings client does: the page keeps the values it
  // loaded, so a Reload that does not actually fetch cannot pass this check by
  // replaying its own baseline.
  void applyHostValues(const QString &uri, const QVariantMap &values,
                       std::function<void(bool, const QString &)> onDone) {
    if (!provider_.controller()) {
      onDone(false, QStringLiteral("无法创建 FusionHost 连接请求。"));
      return;
    }
    auto *call = provider_.controller()->SetConfig(uri, QDBusVariant(values));
    connect(call, &fcitx::kcm::PendingCall::finished, this, [call, onDone] {
      const QDBusMessage reply = call->reply();
      call->deleteLater();
      if (reply.type() != QDBusMessage::ReplyMessage) {
        onDone(false, reply.errorMessage());
        return;
      }
      onDone(true, QString());
    });
  }

  // The Cancel/Reload buttons start an asynchronous chain with no completion
  // signal of its own. The self-test waits for the state it expects instead of
  // assuming a fixed delay, and fails when the state never arrives.
  void waitFor(std::function<bool()> ready, std::function<void(bool)> settled,
               int timeoutMs = 15000) {
    auto elapsed = std::make_shared<int>(0);
    auto tick = std::make_shared<std::function<void()>>();
    *tick = [this, ready, settled, elapsed, tick, timeoutMs] {
      if (ready()) {
        settled(true);
        return;
      }
      if (*elapsed >= timeoutMs) {
        settled(false);
        return;
      }
      *elapsed += 50;
      QTimer::singleShot(50, this, [tick] { (*tick)(); });
    };
    QTimer::singleShot(0, this, [tick] { (*tick)(); });
  }

  int globalPageSize() const {
    return global_->value()
        .toMap()
        .value(QStringLiteral("Behavior"))
        .toMap()
        .value(QStringLiteral("DefaultPageSize"))
        .toInt();
  }

  void setGlobalPageSize(int size) {
    QVariantMap values = global_->value().toMap();
    QVariantMap behavior = values.value(QStringLiteral("Behavior")).toMap();
    behavior.insert(QStringLiteral("DefaultPageSize"), size);
    values.insert(QStringLiteral("Behavior"), behavior);
    global_->setValue(values);
  }

  // A page size that differs from both arguments, kept inside the schema's range
  // so the write is a real value rather than a rejected one.
  static int otherPageSize(int avoid1, int avoid2) {
    for (int candidate : {8, 9, 10, 6, 5}) {
      if (candidate != avoid1 && candidate != avoid2) {
        return candidate;
      }
    }
    return avoid1;
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
                        runGlobalSelfTest();
                      });
      });
    });
  }

  void finishSelfTest() {
    status_->setText(QStringLiteral(
        "SELF-TEST PASS: cross-page shared value, global configuration, Cancel/Reload on the "
        "Global page and the input-method list round-tripped through the Host."));
    if (!screenshot_.isEmpty()) {
      windowScreenshot_ = grab().save(screenshot_);
    }
    QCoreApplication::exit(windowScreenshot_ || screenshot_.isEmpty() ? 0 : 16);
  }

  // Stage A: the Global page must render the official schema, accept a supported
  // change through the real Host apply path, and read back the value the Host
  // persisted. The option chosen here is read by engines that have no page size of
  // their own, so the write is a real configuration change, not a no-op.
  void runGlobalSelfTest() {
    if (!globalLoaded_) {
      status_->setText(QStringLiteral("SELF-TEST FAIL: the Global page did not load."));
      QCoreApplication::exit(23);
      return;
    }
    QVariantMap values = global_->value().toMap();
    QVariantMap behavior = values.value(QStringLiteral("Behavior")).toMap();
    if (behavior.isEmpty() || !behavior.contains(QStringLiteral("DefaultPageSize"))) {
      status_->setText(
          QStringLiteral("SELF-TEST FAIL: the global schema did not expose Behavior/DefaultPageSize."));
      QCoreApplication::exit(23);
      return;
    }
    behavior.insert(QStringLiteral("DefaultPageSize"), kGlobalPageSizeProbe);
    values.insert(QStringLiteral("Behavior"), behavior);
    global_->setValue(values);
    tabs_->setCurrentIndex(2);
    applyCurrentPage([this](bool applied, const QString &error) {
      if (!applied) {
        showError(status_, error);
        QCoreApplication::exit(24);
        return;
      }
      readHostValues(globalUri_, [this](bool read, const QVariantMap &hostValues) {
        if (!read) {
          status_->setText(
              QStringLiteral("SELF-TEST FAIL: could not read the applied global settings."));
          QCoreApplication::exit(25);
          return;
        }
        const QString applied = hostValues.value(QStringLiteral("Behavior"))
                                    .toMap()
                                    .value(QStringLiteral("DefaultPageSize"))
                                    .toString();
        if (applied != QString::number(kGlobalPageSizeProbe)) {
          showError(status_, QStringLiteral("SELF-TEST FAIL: the Host kept DefaultPageSize=%1.")
                                 .arg(applied));
          QCoreApplication::exit(25);
          return;
        }
        runGlobalCancelReloadChecks();
      });
    });
  }

  // Cancel and Reload must act on the Global page, not only on the pages they used
  // to walk: leaving it out kept its edits and baseline untouched while the status
  // line reported success. Each step drives the same button a user clicks, and the
  // Host is changed behind the page so that a Reload which does not fetch cannot
  // pass by replaying the page's own baseline.
  struct GlobalReloadCheck {
    int host = 0;
    int dirty = 0;
    int foreign = 0;
    int step = 0;
  };

  void runGlobalCancelReloadChecks() {
    if (!globalLoaded_) {
      status_->setText(QStringLiteral("SELF-TEST FAIL: the Global page did not load."));
      QCoreApplication::exit(33);
      return;
    }
    globalReloadStep(std::make_shared<GlobalReloadCheck>());
  }

  void failGlobalReload(const QString &message) {
    showError(status_, QStringLiteral("SELF-TEST FAIL: %1").arg(message));
    QCoreApplication::exit(33);
  }

  void globalReloadStep(const std::shared_ptr<GlobalReloadCheck> &check) {
    switch (check->step++) {
    case 0: {
      // Read the authoritative value, make an unsaved edit, then Cancel.
      readHostValues(globalUri_, [this, check](bool ok, const QVariantMap &values) {
        if (!ok) {
          failGlobalReload(QStringLiteral("could not read the global configuration."));
          return;
        }
        check->host = values.value(QStringLiteral("Behavior"))
                          .toMap()
                          .value(QStringLiteral("DefaultPageSize"))
                          .toInt();
        if (check->host <= 0) {
          failGlobalReload(QStringLiteral("the Host did not report a DefaultPageSize."));
          return;
        }
        check->dirty = otherPageSize(check->host, -1);
        check->foreign = otherPageSize(check->host, check->dirty);
        setGlobalPageSize(check->dirty);
        if (globalPageSize() != check->dirty) {
          failGlobalReload(QStringLiteral("the Global page did not take an unsaved edit."));
          return;
        }
        cancel_->click();
        waitFor([this, check] { return globalPageSize() == check->host; },
                [this, check](bool settled) {
                  if (!settled) {
                    failGlobalReload(QStringLiteral("Cancel did not restore the Host's value."));
                    return;
                  }
                  if (baselinePageSize() != check->host) {
                    failGlobalReload(QStringLiteral("Cancel did not refresh the Global baseline."));
                    return;
                  }
                  if (!globalLoaded_) {
                    failGlobalReload(QStringLiteral("Cancel left the Global page unloaded."));
                    return;
                  }
                  std::fprintf(stderr, "[self-test] global cancel restored=%d\n", check->host);
                  globalReloadStep(check);
                });
      });
      return;
    }
    case 1: {
      // A second client changes the Host through the same official apply path. The
      // page must still show the value it loaded, then Reload must fetch the new one.
      QVariantMap values = global_->value().toMap();
      QVariantMap behavior = values.value(QStringLiteral("Behavior")).toMap();
      behavior.insert(QStringLiteral("DefaultPageSize"), check->foreign);
      values.insert(QStringLiteral("Behavior"), behavior);
      applyHostValues(globalUri_, values, [this, check](bool ok, const QString &error) {
        if (!ok) {
          failGlobalReload(
              QStringLiteral("a second client could not change the global value: %1").arg(error));
          return;
        }
        if (globalPageSize() != check->host) {
          failGlobalReload(QStringLiteral("the page adopted a change it did not load."));
          return;
        }
        readHostValues(globalUri_, [this, check](bool hostRead, const QVariantMap &hostValues) {
          if (!hostRead) {
            failGlobalReload(QStringLiteral("could not read the changed global value."));
            return;
          }
          const int applied = hostValues.value(QStringLiteral("Behavior"))
                                  .toMap()
                                  .value(QStringLiteral("DefaultPageSize"))
                                  .toInt();
          if (applied != check->foreign) {
            failGlobalReload(QStringLiteral("the Host did not keep the second client's value."));
            return;
          }
          setGlobalPageSize(check->dirty);
          reload_->click();
          waitFor([this, check] { return globalPageSize() == check->foreign; },
                  [this, check](bool settled) {
                    if (!settled) {
                      failGlobalReload(
                          QStringLiteral("Reload did not fetch the Host's changed value."));
                      return;
                    }
                    if (baselinePageSize() != check->foreign) {
                      failGlobalReload(QStringLiteral("Reload did not refresh the Global baseline."));
                      return;
                    }
                    std::fprintf(stderr, "[self-test] global reload fetched=%d\n", check->foreign);
                    globalReloadStep(check);
                  });
        });
      });
      return;
    }
    case 2: {
      // Switching tabs must keep an ordinary unsaved edit, and the baseline a
      // reload left behind must still support a normal apply.
      const int pending = otherPageSize(check->foreign, check->host);
      setGlobalPageSize(pending);
      tabs_->setCurrentIndex(0);
      tabs_->setCurrentIndex(2);
      if (globalPageSize() != pending) {
        failGlobalReload(QStringLiteral("switching tabs discarded an unsaved edit."));
        return;
      }
      std::fprintf(stderr, "[self-test] global tab-switch kept=%d\n", pending);
      cancel_->click();
      waitFor([this, check] { return globalPageSize() == check->foreign; },
              [this, check](bool settled) {
                if (!settled) {
                  failGlobalReload(QStringLiteral("Cancel did not discard the tab-switch edit."));
                  return;
                }
                // Leave the value the restart check expects.
                setGlobalPageSize(kGlobalPageSizeProbe);
                applyCurrentPage([this](bool applied, const QString &error) {
                  if (!applied) {
                    failGlobalReload(
                        QStringLiteral("apply after Reload failed: %1").arg(error));
                    return;
                  }
                  readHostValues(globalUri_, [this](bool ok, const QVariantMap &values) {
                    if (!ok) {
                      failGlobalReload(
                          QStringLiteral("could not read the value after the final apply."));
                      return;
                    }
                    const int applied = values.value(QStringLiteral("Behavior"))
                                            .toMap()
                                            .value(QStringLiteral("DefaultPageSize"))
                                            .toInt();
                    if (applied != kGlobalPageSizeProbe) {
                      failGlobalReload(
                          QStringLiteral("the apply after Reload did not reach the Host."));
                      return;
                    }
                    std::fprintf(stderr, "[self-test] global apply-after-reload=%d\n",
                                 kGlobalPageSizeProbe);
                    runImListSelfTest();
                  });
                });
              });
      return;
    }
    default:
      return;
    }
  }

  int baselinePageSize() const {
    return baseline_.value(global_)
        .value(QStringLiteral("Behavior"))
        .toMap()
        .value(QStringLiteral("DefaultPageSize"))
        .toInt();
  }

  // Stage A: the list surface must drive the Host's official setGroup/save and
  // adopt its authoritative result. The steps run in the disposable test profile
  // only: reorder, then remove the first entry, then add it back. Removing an entry
  // is a group-membership edit and never deletes a method's definition or learning
  // data — that is asserted at file level by the validation script, not here.
  // Two requests the Host must refuse before touching the group: an empty list and a
  // name this runtime does not define. They go through the same controller call the
  // panes use, but not through the panes, so a refusal cannot disturb the list the
  // following steps assert.
  void runImListRejectProbe(std::function<void()> onDone) {
    if (!provider_.controller()) {
      QCoreApplication::exit(31);
      return;
    }
    auto *emptyCall = provider_.controller()->SetImList({});
    connect(emptyCall, &fcitx::kcm::PendingCall::finished, this,
            [this, emptyCall, onDone] {
              const auto emptyReply = emptyCall->reply();
              emptyCall->deleteLater();
              if (emptyReply.type() == QDBusMessage::ReplyMessage) {
                showError(status_, QStringLiteral("SELF-TEST FAIL: the Host accepted an empty "
                                                  "input-method list."));
                QCoreApplication::exit(31);
                return;
              }
              auto *unknownCall = provider_.controller()->SetImList(
                  {QStringLiteral("fusion-unavailable-probe")});
              connect(unknownCall, &fcitx::kcm::PendingCall::finished, this,
                      [this, unknownCall, onDone, emptyReply] {
                        const auto unknownReply = unknownCall->reply();
                        unknownCall->deleteLater();
                        if (unknownReply.type() == QDBusMessage::ReplyMessage) {
                          showError(status_,
                                    QStringLiteral("SELF-TEST FAIL: the Host accepted an input "
                                                   "method this runtime does not define."));
                          QCoreApplication::exit(31);
                          return;
                        }
                        std::fprintf(stderr,
                                     "[self-test] im-list reject ok empty=%s unknown=%s\n",
                                     qPrintable(emptyReply.errorMessage()),
                                     qPrintable(unknownReply.errorMessage()));
                        onDone();
                      });
            });
  }

  void runImListSelfTest() {
    runImListRejectProbe([this] { runImListSteps(); });
  }

  void runImListSteps() {
    const QStringList initial = enabledOrder();
    if (initial.size() < 2) {
      status_->setText(
          QStringLiteral("SELF-TEST FAIL: the disposable profile did not enable two methods."));
      QCoreApplication::exit(26);
      return;
    }
    QStringList reordered = initial;
    reordered.move(0, 1);
    // Each step asserts the panes show the Host's authoritative reply, not the
    // request: populateImList() rebuilds both panes from what setGroup/save
    // confirmed. The unattended run prints the three lists so a failure is
    // diagnosable from the harness output instead of only from an exit code.
    applyImList(reordered, status_, [this, initial, reordered](bool applied, const QString &error) {
      if (!applied) {
        showError(status_, error);
        QCoreApplication::exit(26);
        return;
      }
      const QStringList observed = enabledOrder();
      std::fprintf(stderr, "[self-test] im-list initial=%s reordered=%s observed=%s\n",
                   qPrintable(initial.join(QLatin1Char(','))),
                   qPrintable(reordered.join(QLatin1Char(','))),
                   qPrintable(observed.join(QLatin1Char(','))));
      if (observed != reordered) {
        showError(status_,
                  QStringLiteral("SELF-TEST FAIL: the Host did not confirm the reordered list."));
        QCoreApplication::exit(27);
        return;
      }
      QStringList withoutFirst = reordered;
      const QString first = withoutFirst.takeFirst();
      applyImList(withoutFirst, status_, [this, reordered, withoutFirst,
                                          first](bool removed, const QString &removeError) {        if (!removed) {
          showError(status_, removeError);
          QCoreApplication::exit(26);
          return;
        }
        if (enabledOrder() != withoutFirst) {
          std::fprintf(stderr, "[self-test] im-list remove expected=%s observed=%s\n",
                       qPrintable(withoutFirst.join(QLatin1Char(','))),
                       qPrintable(enabledOrder().join(QLatin1Char(','))));
          showError(status_, QStringLiteral("SELF-TEST FAIL: removing a method did not reach the "
                                            "Host's list."));
          QCoreApplication::exit(27);
          return;
        }
        QStringList restored = withoutFirst;
        restored.append(first);
        applyImList(restored, status_,
                    [this, restored, reordered](bool added, const QString &addError) {
                      if (!added) {
                        showError(status_, addError);
                        QCoreApplication::exit(26);
                        return;
                      }
                      if (enabledOrder() != restored) {
                        std::fprintf(stderr, "[self-test] im-list add-back expected=%s observed=%s\n",
                                     qPrintable(restored.join(QLatin1Char(','))),
                                     qPrintable(enabledOrder().join(QLatin1Char(','))));
                        showError(status_, QStringLiteral("SELF-TEST FAIL: adding a method back did "
                                                          "not reach the Host's list."));
                        QCoreApplication::exit(27);
                        return;
                      }
                      // End on the reordered list, not the original: the restart check
                      // then proves a *changed* membership order was written to the
                      // profile and reloaded, rather than that nothing moved.
                      applyImList(reordered, status_, [this](bool finalOk,
                                                             const QString &finalError) {
                        if (!finalOk) {
                          showError(status_, finalError);
                          QCoreApplication::exit(26);
                          return;
                        }
                        finishSelfTest();
                      });
                    });
      });
    });
  }

  // Read-only restart check. It asserts exactly the state the apply scenario
  // leaves, so the two invocations together form one restart test: the first
  // applies, the Host is stopped in an orderly way, and this one proves the values
  // came back from the profile rather than from a live process.
  void runVerify() {
    if (!pinyinLoaded_ || !shuangpinLoaded_ || !globalLoaded_) {
      status_->setText(
          QStringLiteral("VERIFY FAIL: not every settings page loaded after the restart."));
      QCoreApplication::exit(23);
      return;
    }
    // The enabled order is asserted against what the profile held before the
    // restart, passed in by the harness from the persisted file. That proves the
    // Host reloaded exactly the membership and order that were saved, without
    // hard-coding this profile's contents.
    const QStringList liveList = enabledOrder();
    if (liveList.isEmpty() || (!expectedImList_.isEmpty() && liveList != expectedImList_)) {
      std::fprintf(stderr, "[verify] im-list expected=%s observed=%s\n",
                   qPrintable(expectedImList_.join(QLatin1Char(','))),
                   qPrintable(liveList.join(QLatin1Char(','))));
      showError(status_, QStringLiteral("VERIFY FAIL: the input-method list after the restart does "
                                        "not match the one that was saved."));
      QCoreApplication::exit(30);
      return;
    }
    readHostValue(pinyinUri_, QStringLiteral("PageSize"),
                  [this](bool read, const QString &pageSize) {
                    if (!read) {
                      status_->setText(
                          QStringLiteral("VERIFY FAIL: could not read PageSize after the restart."));
                      QCoreApplication::exit(28);
                      return;
                    }
                    if (pageSize != QLatin1String("8")) {
                      showError(status_,
                                QStringLiteral("VERIFY FAIL: PageSize=%1 after the restart, "
                                               "expected the applied 8.")
                                    .arg(pageSize));
                      QCoreApplication::exit(28);
                      return;
                    }
                    readHostValues(globalUri_, [this](bool globalRead,
                                                      const QVariantMap &hostValues) {
                      if (!globalRead) {
                        status_->setText(QStringLiteral(
                            "VERIFY FAIL: could not read the global configuration after the restart."));
                        QCoreApplication::exit(29);
                        return;
                      }
                      const QString applied = hostValues.value(QStringLiteral("Behavior"))
                                                  .toMap()
                                                  .value(QStringLiteral("DefaultPageSize"))
                                                  .toString();
                      if (applied != QString::number(kGlobalPageSizeProbe)) {
                        showError(status_,
                                  QStringLiteral("VERIFY FAIL: DefaultPageSize=%1 after the "
                                                 "restart, expected the applied %2.")
                                      .arg(applied)
                                      .arg(kGlobalPageSizeProbe));
                        QCoreApplication::exit(29);
                        return;
                      }
                      status_->setText(QStringLiteral(
                          "VERIFY PASS: the applied per-input-method and global values survived an "
                          "orderly Host restart."));
                      runNoChangeChecks();
                    });
                  });
  }

  // A no-change Apply must not reach the Host at all: the page computes an empty
  // delta and returns without a request, so no setter runs and no file is written.
  // The list apply is sent, but the Host recognises an unchanged list and returns
  // the current group without calling save(). The harness hashes the profile tree
  // around this pass, so "no file written" is asserted, not assumed.
  void runNoChangeChecks() { noChangeStep(0, tabs_->currentIndex()); }

  void noChangeStep(int tab, int originalTab) {
    if (tab >= 3) {
      const QStringList current = enabledOrder();
      applyImList(current, status_, [this, originalTab](bool ok, const QString &error) {
        if (!ok) {
          showError(status_, error);
          QCoreApplication::exit(32);
          return;
        }
        tabs_->setCurrentIndex(originalTab);
        status_->setText(QStringLiteral(
            "VERIFY PASS: no-change Apply issued no settings write and the unchanged list was not "
            "re-saved."));
        QCoreApplication::exit(0);
      });
      return;
    }
    tabs_->setCurrentIndex(tab);
    applyCurrentPage([this, tab, originalTab](bool ok, const QString &error) {
      if (!ok) {
        showError(status_, error);
        QCoreApplication::exit(32);
        return;
      }
      if (!status_->text().contains(QStringLiteral("没有需要应用的更改"))) {
        showError(status_, QStringLiteral("VERIFY FAIL: a no-change Apply sent a settings request "
                                          "instead of short-circuiting."));
        QCoreApplication::exit(32);
        return;
      }
      noChangeStep(tab + 1, originalTab);
    });
  }

  void showError(QLabel *status, const QString &error) {
    const QString message = error.isEmpty()
                                ? QStringLiteral("FusionHost 未确认操作结果。请重新加载；不要重复提交不确定的更改。")
                                : error;
    status->setText(QStringLiteral("操作未完成：%1").arg(message));
    // An unattended self-test/verify run has nobody to dismiss a modal dialog, so it
    // reports through the status line and the exit code instead of blocking here.
    if (selfTest_ || verifyOnly_) {
      return;
    }
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
      auto *item = new QListWidgetItem(inputMethodName(name), enabledList_);
      item->setData(Qt::UserRole, name);
    }
    for (const auto &name : list.available) {
      if (list.enabled.contains(name)) {
        continue;
      }
      auto *item = new QListWidgetItem(inputMethodName(name), availableList_);
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
  void applyImList(const QStringList &enabled, QLabel *status,
                   std::function<void(bool, const QString &)> onDone = {}) {
    if (enabled.isEmpty()) {
      const QString message = QStringLiteral("至少需保留一种已启用的输入方式。");
      status->setText(message);
      if (onDone) {
        onDone(false, message);
      }
      return;
    }
    if (!provider_.controller()) {
      const QString message = QStringLiteral("无法创建 FusionHost 重连请求。");
      showError(status, message);
      if (onDone) {
        onDone(false, message);
      }
      return;
    }
    imApply_->setEnabled(false);
    status->setText(QStringLiteral("正在更新输入方式列表…"));
    auto *call = provider_.controller()->SetImList(enabled);
    connect(call, &fcitx::kcm::PendingCall::finished, this,
            [this, call, status, onDone] {
              const auto reply = call->reply();
              const auto list = call->imList();
              call->deleteLater();
              imApply_->setEnabled(true);
              if (reply.type() != QDBusMessage::ReplyMessage) {
                showError(status, reply.errorMessage());
                reloadImList();
                if (onDone) {
                  onDone(false, reply.errorMessage());
                }
                return;
              }
              populateImList(list);
              status->setText(QStringLiteral("FusionHost 已确认输入方式列表。"));
              if (onDone) {
                onDone(true, QString());
              }
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
  bool verifyOnly_ = false;
  QStringList expectedImList_;
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
  bool verifyOnly = false;
  QStringList expectedImList;
  QString screenshot;
  for (int i = 1; i < argc; ++i) {
    const QString arg = QString::fromLocal8Bit(argv[i]);
    if (arg == QLatin1String("--self-test")) {
      selfTest = true;
    } else if (arg == QLatin1String("--self-test-verify")) {
      // Read-only: asserts the state the apply scenario leaves, so the pair proves
      // the values came back from the profile after an orderly Host restart.
      verifyOnly = true;
    } else if (arg == QLatin1String("--expect-im-list") && i + 1 < argc) {
      const QString csv = QString::fromLocal8Bit(argv[++i]);
      if (!csv.isEmpty()) {
        expectedImList = csv.split(QLatin1Char(','));
      }
    } else if (arg == QLatin1String("--screenshot") && i + 1 < argc) {
      screenshot = QString::fromLocal8Bit(argv[++i]);
    }
  }
  if (pipeName.isEmpty()) {
    QMessageBox::critical(nullptr, QStringLiteral("Fusion 输入法设置"),
                          QStringLiteral("无法确定当前用户和会话的 FusionHost 管道。"));
    return 2;
  }
  SettingsWindow window(pipeName, selfTest, verifyOnly, screenshot);
  window.setExpectedImList(expectedImList);
  window.show();
  return application.exec();
}
