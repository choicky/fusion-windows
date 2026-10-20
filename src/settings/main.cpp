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
#include <QComboBox>
#include <QCoreApplication>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardItemModel>
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
  return name == QLatin1String("shuangpin") ? QStringLiteral("双拼")
                                             : QStringLiteral("拼音");
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

    auto *selectionRow = new QHBoxLayout;
    selectionRow->addWidget(new QLabel(QStringLiteral("当前输入方式："), central));
    selection_ = new QComboBox(central);
    selection_->addItem(QStringLiteral("拼音"), QStringLiteral("pinyin"));
    selection_->addItem(QStringLiteral("双拼"), QStringLiteral("shuangpin"));
    selectionRow->addWidget(selection_, 1);
    auto *applySelection = new QPushButton(QStringLiteral("应用输入方式"), central);
    selectionRow->addWidget(applySelection);
    outer->addLayout(selectionRow);

    tabs_ = new QTabWidget(central);
    pinyinUri_ = QStringLiteral("fcitx://config/inputmethod/pinyin");
    shuangpinUri_ = QStringLiteral("fcitx://config/inputmethod/shuangpin");
    tabs_->addTab(makePage(pinyinUri_, pinyin_), QStringLiteral("拼音设置"));
    tabs_->addTab(makePage(shuangpinUri_, shuangpin_), QStringLiteral("双拼设置"));
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
    connect(applySelection, &QPushButton::clicked, this, [this, status, applySelection] {
      if (!provider_.controller()) {
        showError(status, QStringLiteral("无法创建 FusionHost 重连请求。"));
        return;
      }
      applySelection->setEnabled(false);
      status->setText(QStringLiteral("正在切换输入方式…"));
      auto *call = provider_.controller()->SelectInputMethod(selection_->currentData().toString());
      connect(call, &fcitx::kcm::PendingCall::finished, this,
              [this, call, applySelection, status] {
                const auto reply = call->reply();
                const auto selection = call->selection();
                call->deleteLater();
                applySelection->setEnabled(true);
                if (reply.type() != QDBusMessage::ReplyMessage) {
                  const QString error = reply.errorMessage();
                  showError(status, error.isEmpty()
                                        ? QStringLiteral("FusionHost 未确认输入方式切换；请重新加载状态。")
                                        : error);
                  reloadSelection();
                  return;
                }
                setSelection(selection);
                loadCurrentPage();
                status->setText(QStringLiteral("输入方式已由 FusionHost 确认：%1。")
                                    .arg(inputMethodName(selection.selected)));
              });
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

    // Probe the selection first, then load the page(s). The Host admits one
    // settings request at a time; serializing startup avoids a false Busy result.
    reloadSelection([this] {
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
    return tabs_->currentIndex() == 0 ? pinyin_ : shuangpin_;
  }

  void loadCurrentPage(bool force = false) {
    if (!provider_.available()) {
      return;
    }
    auto *page = currentPage();
    if (!page) {
      return;
    }
    const bool loaded = page == pinyin_ ? pinyinLoaded_ : shuangpinLoaded_;
    if (force || !loaded) {
      page->load();
    }
  }

  void reloadAll(QLabel *status, const QString &successMessage) {
    reloadSelection([this, status, successMessage] {
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
    return page == pinyin_ ? pinyinUri_ : shuangpinUri_;
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

  void reloadSelection(std::function<void()> continuation = {}) {
    if (!provider_.controller()) {
      if (continuation) {
        continuation();
      }
      return;
    }
    auto *call = provider_.controller()->GetSelection();
    connect(call, &fcitx::kcm::PendingCall::finished, this,
            [this, call, continuation = std::move(continuation)]() mutable {
              const auto reply = call->reply();
              const auto selection = call->selection();
              call->deleteLater();
              if (reply.type() == QDBusMessage::ReplyMessage) {
                setSelection(selection);
                if (continuation) {
                  continuation();
                }
              } else {
                showError(status_, reply.errorMessage());
              }
            });
  }

  void setSelection(const fcitx::kcm::SettingsSelection &selection) {
    const int index = selection_->findData(selection.selected);
    if (index >= 0) {
      selection_->setCurrentIndex(index);
      const bool wasBlocked = tabs_->blockSignals(true);
      tabs_->setCurrentIndex(index);
      tabs_->blockSignals(wasBlocked);
    }
    auto *model = qobject_cast<QStandardItemModel *>(selection_->model());
    for (int i = 0; model && i < selection_->count(); ++i) {
      if (auto *item = model->item(i)) {
        item->setEnabled(selection.available.contains(selection_->itemData(i).toString()));
      }
    }
  }

  fcitx::kcm::DBusProvider provider_;
  QLabel *status_ = nullptr;
  QTabWidget *tabs_ = nullptr;
  QComboBox *selection_ = nullptr;
  QPushButton *apply_ = nullptr;
  QPushButton *cancel_ = nullptr;
  QPushButton *reload_ = nullptr;
  fcitx::kcm::ConfigWidget *pinyin_ = nullptr;
  fcitx::kcm::ConfigWidget *shuangpin_ = nullptr;
  QString pinyinUri_;
  QString shuangpinUri_;
  // Last authoritative values per page, used to submit only what this page changed.
  QHash<fcitx::kcm::ConfigWidget *, QVariantMap> baseline_;
  int pinyinLoads_ = 0;
  int shuangpinLoads_ = 0;
  bool pinyinLoaded_ = false;
  bool shuangpinLoaded_ = false;
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
