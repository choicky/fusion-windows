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
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <clocale>
#include <functional>
#include <memory>
#include <string>

namespace {

QString endpointName(int argc, char **argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (QString::fromLocal8Bit(argv[i]) == QLatin1String("--pipe-name")) {
      const QString supplied = QString::fromLocal8Bit(argv[i + 1]);
      const QString prefix = QString::fromStdWString(fusion::ipc::default_pipe_name_prefix);
      if (!supplied.startsWith(prefix) || supplied.size() == prefix.size()) {
        return {};
      }
      return supplied.mid(prefix.size());
    }
  }
  const std::wstring pipe = fusion::ipc::compute_pipe_name();
  const std::wstring prefix = fusion::ipc::default_pipe_name_prefix;
  if (pipe.empty() || pipe.rfind(prefix, 0) != 0) {
    return {};
  }
  return QString::fromStdWString(pipe.substr(prefix.size()));
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
    tabs_->addTab(makePage(QStringLiteral("fcitx://config/inputmethod/pinyin"), pinyin_),
                  QStringLiteral("拼音设置"));
    tabs_->addTab(makePage(QStringLiteral("fcitx://config/inputmethod/shuangpin"), shuangpin_),
                  QStringLiteral("双拼设置"));
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

    connect(apply_, &QPushButton::clicked, this, [this, status] {
      auto *page = currentPage();
      if (!page) {
        return;
      }
      apply_->setEnabled(false);
      status->setText(QStringLiteral("正在应用；不会重启 FusionHost…"));
      page->save();
    });
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
                status->setText(QStringLiteral("已从 FusionHost 加载拼音设置。"));
              } else {
                showError(status, error);
              }
            });
    connect(shuangpin_, &fcitx::kcm::ConfigWidget::configurationLoaded, this,
            [this, status](bool ok, const QString &error) {
              shuangpinLoaded_ = ok;
              if (ok) {
                status->setText(QStringLiteral("已从 FusionHost 加载双拼设置。"));
              } else {
                showError(status, error);
              }
            });
    const auto applied = [this, status](bool ok, const QString &error) {
      apply_->setEnabled(true);
      if (ok) {
        status->setText(QStringLiteral("FusionHost 已确认应用设置。"));
      } else {
        showError(status, error);
      }
    };
    connect(pinyin_, &fcitx::kcm::ConfigWidget::saveFinished, this, applied);
    connect(shuangpin_, &fcitx::kcm::ConfigWidget::saveFinished, this, applied);
    connect(pinyin_, &fcitx::kcm::ConfigWidget::changed, this, [this, status] {
      status->setText(QStringLiteral("拼音设置有未应用的更改。"));
    });
    connect(shuangpin_, &fcitx::kcm::ConfigWidget::changed, this, [this, status] {
      status->setText(QStringLiteral("双拼设置有未应用的更改。"));
    });

    // Probe the selection first, then load the active page. The Host admits one
    // settings request at a time; serializing startup avoids a false Busy result.
    reloadSelection([this] {
      loadCurrentPage(true);
      if (selfTest_) {
        QTimer::singleShot(0, this, [this] { runSelfTest(); });
      }
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
      pinyinLoaded_ = false;
      shuangpinLoaded_ = false;
      auto secondPage = std::make_shared<std::function<void()>>();
      *secondPage = [this, status, successMessage] {
        connect(shuangpin_, &fcitx::kcm::ConfigWidget::configurationLoaded, this,
                [this, status, successMessage](bool ok, const QString &error) {
                  if (ok) {
                    status->setText(successMessage);
                  } else {
                    showError(status, error);
                  }
                }, Qt::SingleShotConnection);
        shuangpin_->load();
      };
      connect(pinyin_, &fcitx::kcm::ConfigWidget::configurationLoaded, this,
              [this, status, secondPage](bool ok, const QString &error) {
                if (ok) {
                  (*secondPage)();
                } else {
                  showError(status, error);
                }
              }, Qt::SingleShotConnection);
      pinyin_->load();
    });
  }

  void runSelfTest() {
    if (!selfTestTimer_.isValid()) {
      selfTestTimer_.start();
    }
    if (selfTestTimer_.elapsed() > 8000) {
      status_->setText(QStringLiteral("SELF-TEST FAIL: timed out waiting for the Host/config schema."));
      QCoreApplication::exit(10);
      return;
    }
    if (!pinyinLoaded_) {
      QTimer::singleShot(100, this, [this] { runSelfTest(); });
      return;
    }
    const QString before = pinyin_->value().toMap().value(QStringLiteral("PageSize")).toString();
    if (before.isEmpty()) {
      status_->setText(QStringLiteral("SELF-TEST FAIL: PageSize was not rendered."));
      QCoreApplication::exit(11);
      return;
    }
    QVariantMap values = pinyin_->value().toMap();
    values.insert(QStringLiteral("PageSize"), 9);
    pinyin_->setValue(values);
    connect(pinyin_, &fcitx::kcm::ConfigWidget::saveFinished, this,
            [this, before](bool ok, const QString &error) {
              if (!ok) {
                showError(status_, error);
                QCoreApplication::exit(12);
                return;
              }
              pinyinLoaded_ = false;
              connect(shuangpin_, &fcitx::kcm::ConfigWidget::configurationLoaded, this,
                      [this](bool loaded, const QString &loadError) {
                        if (!loaded) {
                          showError(status_, loadError);
                          QCoreApplication::exit(13);
                          return;
                        }
                        QVariantMap sharedCheck = shuangpin_->value().toMap();
                        const QString sharedPageSize = sharedCheck.value(QStringLiteral("PageSize")).toString();
                        if (sharedPageSize != QStringLiteral("9")) {
                          showError(status_, QStringLiteral("SELF-TEST FAIL: Shuangpin restored stale PageSize=%1.").arg(sharedPageSize));
                          QCoreApplication::exit(14);
                          return;
                        }
                        if (sharedCheck.contains(QStringLiteral("ShuangpinProfile"))) {
                          sharedCheck.insert(QStringLiteral("ShuangpinProfile"), QStringLiteral("MS"));
                        }
                        shuangpin_->setValue(sharedCheck);
                        connect(shuangpin_, &fcitx::kcm::ConfigWidget::saveFinished, this,
                                [this, before](bool applied, const QString &applyError) {
                                  if (!applied) {
                                    showError(status_, applyError);
                                    QCoreApplication::exit(15);
                                    return;
                                  }
                                  status_->setText(QStringLiteral("SELF-TEST PASS: PageSize %1 -> 9 and shared-page preservation confirmed.").arg(before));
                                  if (!screenshot_.isEmpty()) {
                                    windowScreenshot_ = grab().save(screenshot_);
                                  }
                                  QCoreApplication::exit(windowScreenshot_ || screenshot_.isEmpty() ? 0 : 16);
                                }, Qt::SingleShotConnection);
                        shuangpin_->save();
                      }, Qt::SingleShotConnection);
              shuangpin_->load();
            }, Qt::SingleShotConnection);
    pinyin_->save();
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
