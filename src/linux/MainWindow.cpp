#include "linux/MainWindow.h"

#include "core/LinuxDesktop.h"
#include "linux/Desktop.h"
#include "linux/Log.h"
#include "linux/PipeWireCapture.h"
#include "linux/Portal.h"
#include "linux/X11Capture.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMetaObject>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QSystemTrayIcon>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <set>

namespace bt::linux_app {

namespace {

constexpr int kMaxClipsShown = 30;

QString qs(const std::string& text) {
    return QString::fromStdString(text);
}

// A warning without its "line N: " start: adding a key moves the lines below it.
std::string without_line(const std::string& warning) {
    const auto colon = warning.find(": ");
    return warning.starts_with("line ") && colon != std::string::npos ? warning.substr(colon + 2) : warning;
}

std::string read_token() {
    std::string token = read_text_file(app_paths().token_file);
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r')) {
        token.pop_back();
    }
    return token;
}

QString save_command() {
    const char* appimage = std::getenv("APPIMAGE");
    QString program = appimage != nullptr && *appimage != '\0' ? QString::fromLocal8Bit(appimage) : QStringLiteral("backtrack");
    if (program.contains(QLatin1Char(' '))) {
        program = QLatin1Char('"') + program + QLatin1Char('"');
    }
    return program + QStringLiteral(" --save");
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QWidget(parent), wayland_(wayland_session()) {
    setWindowTitle(QStringLiteral("Backtrack"));
    setWindowIcon(logo_icon());
    load_settings();
    build_ui();

    screencast_ = new ScreenCastPortal(this);
    connect(screencast_, &ScreenCastPortal::ready, this, [this](int fd, unsigned node, const QString& token) {
        if (!token.isEmpty()) {
            write_text_file(app_paths().token_file, token.toStdString() + "\n");
        }
        auto source = std::make_unique<PipeWireCapture>(fd, node, settings_.fps, "Screen (Wayland)");
        std::string error;
        if (!source->start(error)) {
            screencast_->close();  // or KDE keeps showing that the screen is shared
            status_->setText(QStringLiteral("Not recording: %1").arg(qs(error)));
            record_button_->setText(QStringLiteral("Start recording"));
            return;
        }
        recorder_.start(settings_, std::move(source));
        refresh_status();
    });
    connect(screencast_, &ScreenCastPortal::failed, this, [this](const QString& message) {
        status_->setText(QStringLiteral("Not recording: %1").arg(message));
        record_button_->setText(QStringLiteral("Start recording"));
    });
    connect(screencast_, &ScreenCastPortal::closed, this, [this] {
        recorder_.stop();
        status_->setText(QStringLiteral("Screen sharing was stopped. Click Start recording to share the screen again."));
        record_button_->setText(QStringLiteral("Start recording"));
    });

    shortcuts_ = new GlobalShortcutsPortal(this);
    connect(shortcuts_, &GlobalShortcutsPortal::activated, this, [this](const QString& id) {
        if (id == QStringLiteral("save")) {
            save_clip();
        }
    });
    connect(shortcuts_, &GlobalShortcutsPortal::bound, this, [this] {
        // KDE keeps the key confirmed the first time; a new preferred key does not replace it.
        hotkey_hint_->setText(QStringLiteral("The save hotkey is registered with KDE (asked for %1) and works in games too. "
                                             "To change it: System Settings › Shortcuts › Backtrack.")
                                  .arg(qs(settings_.save_hotkey)));
    });
    connect(shortcuts_, &GlobalShortcutsPortal::failed, this, [this](const QString& message) {
        log_warning("Global shortcut: " + message.toStdString());
        hotkey_hint_->setText(QStringLiteral("The desktop did not take the hotkey (%1). Add one yourself in System "
                                             "Settings › Shortcuts › Add Command: %2")
                                  .arg(message, save_command()));
    });

    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        tray_ = new QSystemTrayIcon(logo_icon(), this);
        tray_->setToolTip(QStringLiteral("Backtrack"));
        auto* menu = new QMenu(this);
        menu->addAction(QStringLiteral("Save clip"), this, [this] { save_clip(); });
        menu->addAction(QStringLiteral("Show Backtrack"), this, [this] { show_window(); });
        menu->addAction(QStringLiteral("Open clips folder"), this, [this] { open_clips_folder(); });
        menu->addSeparator();
        menu->addAction(QStringLiteral("Quit"), this, [this] { quit(); });
        tray_->setContextMenu(menu);
        connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger) {
                if (isVisible()) {
                    hide();
                } else {
                    show_window();
                }
            }
        });
        tray_->show();
    }
    QApplication::setQuitOnLastWindowClosed(tray_ == nullptr);
    // Logging out closes every window: hiding to the tray then would hold up the logout.
    connect(qApp, &QGuiApplication::commitDataRequest, this, [this] { quitting_ = true; });

    status_timer_ = new QTimer(this);
    connect(status_timer_, &QTimer::timeout, this, [this] { refresh_status(); });
    status_timer_->start(1000);
    save_message_timer_ = new QTimer(this);
    save_message_timer_->setSingleShot(true);
    save_message_timer_->setInterval(6000);
    connect(save_message_timer_, &QTimer::timeout, this, [this] {
        showing_save_message_ = false;
        refresh_status();
    });

    start_hotkeys();
    refresh_clips();
}

MainWindow::~MainWindow() {
    // A save still writing on the pool uses recorder_: let it finish first.
    QThreadPool::globalInstance()->waitForDone();
    x11_hotkey_.stop();
    recorder_.stop();
}

void MainWindow::build_ui() {
    auto* root = new QVBoxLayout(this);

    auto* header = new QHBoxLayout;
    auto* logo = new QLabel;
    logo->setPixmap(QPixmap::fromImage(logo_image(48)));
    header->addWidget(logo);
    auto* titles = new QVBoxLayout;
    status_ = new QLabel(QStringLiteral("Starting…"));
    QFont bold = status_->font();
    bold.setBold(true);
    bold.setPointSizeF(bold.pointSizeF() * 1.2);
    status_->setFont(bold);
    status_->setWordWrap(true);
    detail_ = new QLabel;
    detail_->setWordWrap(true);
    titles->addWidget(status_);
    titles->addWidget(detail_);
    header->addLayout(titles, 1);
    root->addLayout(header);

    auto* buttons = new QHBoxLayout;
    save_button_ = new QPushButton(QStringLiteral("Save clip"));
    save_button_->setMinimumHeight(40);
    connect(save_button_, &QPushButton::clicked, this, [this] { save_clip(); });
    record_button_ = new QPushButton(QStringLiteral("Stop recording"));
    connect(record_button_, &QPushButton::clicked, this, [this] {
        if (recorder_.status().running) {
            stop_recording();
            status_->setText(QStringLiteral("Not recording"));
            record_button_->setText(QStringLiteral("Start recording"));
        } else {
            start_recording();
        }
    });
    buttons->addWidget(save_button_, 2);
    buttons->addWidget(record_button_, 1);
    root->addLayout(buttons);

    hotkey_hint_ = new QLabel;
    hotkey_hint_->setWordWrap(true);
    hotkey_hint_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(hotkey_hint_);

    auto* clips_box = new QGroupBox(QStringLiteral("Newest clips"));
    auto* clips_layout = new QVBoxLayout(clips_box);
    clips_ = new QListWidget;
    clips_->setToolTip(QStringLiteral("Double-click to play"));
    connect(clips_, &QListWidget::itemActivated, this, [](QListWidgetItem* item) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(item->data(Qt::UserRole).toString()));
    });
    clips_layout->addWidget(clips_);
    auto* open_folder = new QPushButton(QStringLiteral("Open clips folder"));
    connect(open_folder, &QPushButton::clicked, this, [this] { open_clips_folder(); });
    clips_layout->addWidget(open_folder, 0, Qt::AlignLeft);
    root->addWidget(clips_box, 1);

    auto* settings_box = new QGroupBox(QStringLiteral("Settings"));
    auto* form = new QFormLayout(settings_box);
    clip_seconds_ = new QSpinBox;
    clip_seconds_->setRange(5, 300);
    clip_seconds_->setSuffix(QStringLiteral(" s"));
    form->addRow(QStringLiteral("Clip length"), clip_seconds_);
    fps_ = new QComboBox;
    for (const int fps : {30, 60, 120, 144}) {
        fps_->addItem(QStringLiteral("%1 fps").arg(fps), fps);
    }
    form->addRow(QStringLiteral("Frame rate"), fps_);
    bitrate_ = new QSpinBox;
    bitrate_->setRange(1000, 100000);
    bitrate_->setSingleStep(1000);
    bitrate_->setSuffix(QStringLiteral(" kbit/s"));
    form->addRow(QStringLiteral("Video bitrate"), bitrate_);
    encoder_ = new QComboBox;
    encoder_->addItem(QStringLiteral("Automatic"), static_cast<int>(EncoderPreference::Auto));
    encoder_->addItem(QStringLiteral("Graphics card"), static_cast<int>(EncoderPreference::Hardware));
    encoder_->addItem(QStringLiteral("Processor"), static_cast<int>(EncoderPreference::Software));
    form->addRow(QStringLiteral("Encoder"), encoder_);
    resolution_ = new QComboBox;
    resolution_->addItem(QStringLiteral("Screen size"), 0);
    resolution_->addItem(QStringLiteral("1440p"), 1440);
    resolution_->addItem(QStringLiteral("1080p"), 1080);
    resolution_->addItem(QStringLiteral("720p"), 720);
    form->addRow(QStringLiteral("Resolution"), resolution_);
    system_sound_ = new QCheckBox(QStringLiteral("Record what you hear"));
    form->addRow(QStringLiteral("Sound"), system_sound_);
    microphone_ = new QCheckBox(QStringLiteral("Record the microphone"));
    form->addRow(QString(), microphone_);
    noise_suppression_ = new QCheckBox(QStringLiteral("Noise suppression on the microphone"));
    form->addRow(QString(), noise_suppression_);
    hotkey_ = new QLineEdit;
    hotkey_->setPlaceholderText(QStringLiteral("F8, Ctrl+Alt+S, …"));
    form->addRow(QStringLiteral("Save hotkey"), hotkey_);
    auto* folder_row = new QHBoxLayout;
    output_dir_ = new QLineEdit;
    auto* browse = new QPushButton(QStringLiteral("Browse…"));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString chosen = QFileDialog::getExistingDirectory(this, QStringLiteral("Clips folder"), qs(folder()));
        if (!chosen.isEmpty()) {
            output_dir_->setText(chosen);
        }
    });
    folder_row->addWidget(output_dir_, 1);
    folder_row->addWidget(browse);
    form->addRow(QStringLiteral("Clips folder"), folder_row);
    auto* apply = new QPushButton(QStringLiteral("Apply"));
    connect(apply, &QPushButton::clicked, this, [this] { apply_settings(); });
    settings_message_ = new QLabel;
    settings_message_->setWordWrap(true);
    form->addRow(apply, settings_message_);
    root->addWidget(settings_box);

    fill_settings_form();
    resize(560, 720);
}

void MainWindow::fill_settings_form() {
    clip_seconds_->setValue(settings_.clip_seconds);
    fps_->setCurrentIndex(std::max(0, fps_->findData(settings_.fps)));
    bitrate_->setValue(settings_.video_bitrate_kbps);
    encoder_->setCurrentIndex(std::max(0, encoder_->findData(static_cast<int>(settings_.encoder))));
    resolution_->setCurrentIndex(std::max(0, resolution_->findData(settings_.resolution)));
    system_sound_->setChecked(settings_.audio_enabled);
    microphone_->setChecked(settings_.mic_enabled);
    noise_suppression_->setChecked(settings_.noise_suppression);
    hotkey_->setText(qs(settings_.save_hotkey));
    output_dir_->setText(qs(folder()));
}

void MainWindow::load_settings() {
    const AppPaths paths = app_paths();
    std::error_code ec;
    std::filesystem::create_directories(paths.config_dir, ec);
    if (!std::filesystem::exists(paths.ini_file, ec)) {
        // The same commented file as on Windows. Its output_dir (%USERPROFILE%\Videos\Backtrack)
        // means ~/Videos/Backtrack here (core resolve_output_dir).
        write_text_file(paths.ini_file, std::string(default_ini_text()));
        log_info("Settings written: " + paths.ini_file);
    }
    const LoadedSettings loaded = bt::load_settings(read_text_file(paths.ini_file));
    for (const std::string& warning : loaded.warnings) {
        log_warning("backtrack.ini " + warning);
    }
    settings_ = loaded.settings;
}

void MainWindow::apply_settings() {
    Settings next = settings_;
    next.clip_seconds = clip_seconds_->value();
    next.fps = fps_->currentData().toInt();
    next.video_bitrate_kbps = bitrate_->value();
    next.encoder = static_cast<EncoderPreference>(encoder_->currentData().toInt());
    next.resolution = resolution_->currentData().toInt();
    next.audio_enabled = system_sound_->isChecked();
    next.mic_enabled = microphone_->isChecked();
    next.noise_suppression = noise_suppression_->isChecked();

    const HotkeyParseResult hotkey = parse_hotkey(hotkey_->text().toStdString());
    if (!hotkey.hotkey) {
        settings_message_->setText(QStringLiteral("Hotkey: %1").arg(qs(hotkey.error)));
        return;
    }
    next.save_hotkey = hotkey_to_string(*hotkey.hotkey);
    const QString dir = output_dir_->text().trimmed();
    // Unchanged: keep the setting as written (the Windows-style default stays portable).
    if (dir != qs(folder())) {
        if (dir.isEmpty()) {
            settings_message_->setText(QStringLiteral("Choose a clips folder"));
            return;
        }
        next.output_dir = dir.toStdString();
    }

    const AppPaths paths = app_paths();
    // Check the result the way the next start reads it, so a value the file cannot hold shows now.
    const std::string existing = read_text_file(paths.ini_file);
    const std::string text = update_ini(existing, next);
    const LoadedSettings reread = bt::load_settings(text);
    // Only new warnings count: one the file already had (a key edited by hand) must not block Apply.
    std::set<std::string> known;
    for (const std::string& warning : bt::load_settings(existing).warnings) {
        known.insert(without_line(warning));
    }
    for (const std::string& warning : reread.warnings) {
        if (!known.contains(without_line(warning))) {
            settings_message_->setText(qs(warning));
            return;
        }
    }
    if (!write_text_file(paths.ini_file, text)) {
        settings_message_->setText(QStringLiteral("Cannot write %1").arg(qs(paths.ini_file)));
        return;
    }
    const bool restart = reread.settings.clip_seconds != settings_.clip_seconds || reread.settings.fps != settings_.fps ||
                         reread.settings.video_bitrate_kbps != settings_.video_bitrate_kbps ||
                         reread.settings.encoder != settings_.encoder || reread.settings.resolution != settings_.resolution ||
                         reread.settings.audio_enabled != settings_.audio_enabled ||
                         reread.settings.mic_enabled != settings_.mic_enabled ||
                         reread.settings.noise_suppression != settings_.noise_suppression;
    const bool hotkey_changed = reread.settings.save_hotkey != settings_.save_hotkey;
    settings_ = reread.settings;
    fill_settings_form();
    if (hotkey_changed) {
        start_hotkeys();
    }
    if (restart) {
        // The replay buffer starts over: what it held was recorded with the old settings.
        stop_recording();
        start_recording();
        settings_message_->setText(QStringLiteral("Saved. Recording again with the new settings."));
    } else {
        settings_message_->setText(QStringLiteral("Saved."));
    }
    refresh_clips();
}

void MainWindow::start_hotkeys() {
    x11_hotkey_.stop();
    const HotkeyParseResult parsed = parse_hotkey(settings_.save_hotkey);
    if (!parsed.hotkey) {
        hotkey_hint_->setText(QStringLiteral("No save hotkey. Click Save clip, or run: %1").arg(save_command()));
        return;
    }
    save_button_->setText(QStringLiteral("Save clip (%1)").arg(qs(settings_.save_hotkey)));
    if (wayland_) {
        hotkey_hint_->setText(QStringLiteral("Asking KDE for the %1 hotkey…").arg(qs(settings_.save_hotkey)));
        shortcuts_->bind(qs(portal_shortcut_trigger(*parsed.hotkey)));
        return;
    }
    std::string error;
    const bool ok = x11_hotkey_.start(
        *parsed.hotkey, [this] { QMetaObject::invokeMethod(this, [this] { save_clip(); }, Qt::QueuedConnection); }, error);
    if (ok) {
        hotkey_hint_->setText(QStringLiteral("%1 saves a clip, also in games.").arg(qs(settings_.save_hotkey)));
    } else {
        hotkey_hint_->setText(QStringLiteral("%1 does not work: %2. Pick another key below, or bind this command in "
                                             "your desktop's shortcut settings: %3")
                                  .arg(qs(settings_.save_hotkey), qs(error), save_command()));
    }
}

void MainWindow::start_recording() {
    record_button_->setText(QStringLiteral("Stop recording"));
    if (wayland_) {
        if (screencast_->busy()) {
            return;  // KDE's dialog is still open from the last click
        }
        status_->setText(QStringLiteral("Waiting for screen sharing… (KDE asks once which screen to record)"));
        screencast_->start(settings_.capture_cursor, qs(read_token()));
        return;
    }
    auto source = std::make_unique<X11Capture>(settings_.monitor, settings_.capture_cursor);
    std::string error;
    if (!source->start(error)) {
        status_->setText(QStringLiteral("Not recording: %1").arg(qs(error)));
        record_button_->setText(QStringLiteral("Start recording"));
        return;
    }
    recorder_.start(settings_, std::move(source));
    refresh_status();
}

void MainWindow::stop_recording() {
    recorder_.stop();
    screencast_->close();
}

void MainWindow::save_clip() {
    if (saving_) {
        // One more save once this one is written (a second press, the tray, --save): kept, not
        // dropped. More presses meanwhile still give just one.
        save_queued_ = true;
        return;
    }
    saving_ = true;
    save_button_->setEnabled(false);
    const std::string target = folder();
    const std::string pattern = settings_.filename;
    const int seconds = saved_seconds(settings_);
    QThreadPool::globalInstance()->start([this, target, pattern, seconds] {
        const SaveResult result = recorder_.save(target, pattern, seconds);
        QMetaObject::invokeMethod(this, [this, result] { on_saved(result); }, Qt::QueuedConnection);
    });
}

void MainWindow::on_saved(const SaveResult& result) {
    saving_ = false;
    save_button_->setEnabled(true);
    ++finished_saves_;
    if (save_queued_) {
        save_queued_ = false;
        QMetaObject::invokeMethod(this, [this] { save_clip(); }, Qt::QueuedConnection);
    }
    if (result.ok) {
        const QString name = QFileInfo(qs(result.path)).fileName();
        show_save_message(QStringLiteral("Saved %1 (%2 s)").arg(name).arg(result.seconds, 0, 'f', 1));
        if (settings_.notification) {
            notify(QStringLiteral("Clip saved"), name);
        }
        refresh_clips();
    } else {
        ++failed_saves_;
        show_save_message(QStringLiteral("Not saved: %1").arg(qs(result.error)));
        notify(QStringLiteral("Clip not saved"), qs(result.error));
    }
}

void MainWindow::refresh_status() {
    const RecorderStatus status = recorder_.status();
    if (status.running) {
        status_->setText(QStringLiteral("Recording · the last %1 s are kept").arg(settings_.clip_seconds));
        QStringList parts;
        for (const std::string& part : {status.capture, status.encoder, status.audio}) {
            if (!part.empty()) {
                parts << qs(part);
            }
        }
        QString text = parts.join(QStringLiteral(" · "));
        if (!status.error.empty()) {
            text += QStringLiteral("\n") + qs(status.error);
        }
        if (!showing_save_message_) {
            detail_->setText(text);
        }
        detail_->setToolTip(text);
        record_button_->setText(QStringLiteral("Stop recording"));
    } else if (!status.error.empty()) {
        status_->setText(QStringLiteral("Not recording: %1").arg(qs(status.error)));
        record_button_->setText(QStringLiteral("Start recording"));
    }
}

void MainWindow::show_save_message(const QString& text) {
    detail_->setText(text);
    showing_save_message_ = true;
    save_message_timer_->start();
}

void MainWindow::refresh_clips() {
    clips_->clear();
    const QDir dir(qs(folder()));
    const QFileInfoList files = dir.entryInfoList({QStringLiteral("*.mp4")}, QDir::Files, QDir::Time);
    for (int i = 0; i < files.size() && i < kMaxClipsShown; ++i) {
        const QFileInfo& file = files[i];
        auto* item = new QListWidgetItem(QStringLiteral("%1   %2 MB   %3")
                                             .arg(file.fileName())
                                             .arg(static_cast<double>(file.size()) / 1e6, 0, 'f', 1)
                                             .arg(file.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
        item->setData(Qt::UserRole, file.absoluteFilePath());
        clips_->addItem(item);
    }
}

void MainWindow::open_clips_folder() {
    std::error_code ec;
    std::filesystem::create_directories(folder(), ec);
    QDesktopServices::openUrl(QUrl::fromLocalFile(qs(folder())));
}

void MainWindow::notify(const QString& title, const QString& text) {
    if (tray_ != nullptr) {
        tray_->showMessage(title, text, logo_icon(), 4000);
        return;
    }
    // No tray (GNOME has none without an extension): the desktop's own notifications, which
    // every desktop has. Without this a clip saved with the hotkey in a game says nothing at all.
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("/org/freedesktop/Notifications"),
        QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("Notify"));
    call.setArguments({QStringLiteral("Backtrack"), 0u, QStringLiteral("backtrack"), title, text, QStringList(),
                       QVariantMap{{QStringLiteral("desktop-entry"), QStringLiteral("backtrack")}}, 4000});
    QDBusConnection::sessionBus().asyncCall(call);  // no answer needed: never waits for the desktop
}

std::string MainWindow::folder() const {
    return clips_folder(settings_.output_dir);
}

void MainWindow::show_window() {
    show();
    raise();
    activateWindow();
}

void MainWindow::quit() {
    quitting_ = true;
    stop_recording();
    QApplication::quit();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (quitting_ || tray_ == nullptr) {
        quitting_ = true;
        stop_recording();
        event->accept();
        return;
    }
    hide();
    event->ignore();
    if (!told_about_tray_) {
        told_about_tray_ = true;
        notify(QStringLiteral("Backtrack keeps recording"), QStringLiteral("It is in the tray. Quit from its menu."));
    }
}

}  // namespace bt::linux_app
