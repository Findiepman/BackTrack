#pragma once

#include "core/Config.h"
#include "core/Library.h"
#include "linux/Recorder.h"
#include "linux/X11Hotkey.h"

#include <QWidget>

#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QSystemTrayIcon;
class QTimer;

namespace bt::linux_app {

class ClipLibrary;
class ClipPage;
class GlobalShortcutsPortal;
class LibraryPage;
class ScreenCastPortal;

// The Linux window: a sidebar with the recording page (status, Save clip, the main settings) and
// the library (its views and collections), the library page and the clip page. Smaller than the
// Windows app (no export, share or repair yet, docs/LINUX-PORT.md); settings are the same
// backtrack.ini keys. It keeps recording from the tray when closed.
class MainWindow final : public QWidget {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Recording starts right away; on Wayland KDE first asks which screen to share (once).
    void start_recording();
    // From the hotkey, the tray, `backtrack --save` and the test options.
    void save_clip();
    void show_window();
    // Test option --exit-after: the number of saves that failed becomes the exit code.
    int failed_saves() const { return failed_saves_; }
    int finished_saves() const { return finished_saves_; }
    void quit();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void build_ui();
    QWidget* build_record_page();
    void refresh_sidebar();  // soon, not now: it may be called from a click on the sidebar itself
    void rebuild_sidebar();
    void sidebar_menu(const QPoint& at);
    void open_clip(const QString& file);
    void show_library(LibraryView view, const std::string& collection = {});
    void load_settings();
    void apply_settings();
    void fill_settings_form();
    void start_hotkeys();
    void stop_recording();
    void on_saved(const SaveResult& result);
    void show_save_message(const QString& text);
    void refresh_status();
    void open_clips_folder();
    void notify(const QString& title, const QString& text);
    std::string folder() const;

    Settings settings_;
    Recorder recorder_;
    X11Hotkey x11_hotkey_;
    ScreenCastPortal* screencast_ = nullptr;
    GlobalShortcutsPortal* shortcuts_ = nullptr;
    QSystemTrayIcon* tray_ = nullptr;
    QTimer* status_timer_ = nullptr;
    bool wayland_ = false;
    bool saving_ = false;
    bool save_queued_ = false;  // a save was asked for while one was being written
    bool quitting_ = false;
    bool told_about_tray_ = false;
    int failed_saves_ = 0;
    int finished_saves_ = 0;
    bool showing_save_message_ = false;  // the last save's result stays up for a few seconds
    QTimer* save_message_timer_ = nullptr;

    QLabel* status_ = nullptr;
    QLabel* detail_ = nullptr;
    QLabel* hotkey_hint_ = nullptr;
    QPushButton* save_button_ = nullptr;
    QPushButton* record_button_ = nullptr;
    ClipLibrary* library_ = nullptr;
    QListWidget* sidebar_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QWidget* record_page_ = nullptr;
    LibraryPage* library_page_ = nullptr;
    ClipPage* clip_page_ = nullptr;
    QLabel* sidebar_status_ = nullptr;
    bool sidebar_pending_ = false;
    QSpinBox* clip_seconds_ = nullptr;
    QComboBox* fps_ = nullptr;
    QSpinBox* bitrate_ = nullptr;
    QComboBox* encoder_ = nullptr;
    QComboBox* resolution_ = nullptr;
    QCheckBox* system_sound_ = nullptr;
    QCheckBox* microphone_ = nullptr;
    QCheckBox* noise_suppression_ = nullptr;
    QLineEdit* hotkey_ = nullptr;
    QLineEdit* output_dir_ = nullptr;
    QLabel* settings_message_ = nullptr;
};

}  // namespace bt::linux_app
