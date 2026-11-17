#pragma once

// The clip page on Linux: the player, the seek bar with the clip's bookmarks, the trim bar with a
// filmstrip and "Save as new clip" (lossless, linux/Media trim_clip), and the side panel with the
// title, favorite, collections, tags, notes and details. The same parts as the Windows clip page.

#include <QImage>
#include <QWidget>

#include <memory>
#include <string>
#include <vector>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QTimer;

namespace bt::linux_app {

class ClipLibrary;
class LibraryPage;
class Player;
class VideoView;
class SeekBar;
class TrimBar;

class ClipPage final : public QWidget {
    Q_OBJECT
public:
    ClipPage(ClipLibrary* library, LibraryPage* library_page, QWidget* parent = nullptr);
    ~ClipPage() override;

    void open(const std::string& file);
    // Leaving the page: the notes are kept and the player lets go of the file.
    void close_clip();
    const std::string& file() const { return file_; }

signals:
    void back();
    void show_collection(const QString& id);
    void trimmed(const QString& file);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void build();
    void refresh();  // the side panel and the buttons, from the library
    void tick();
    void toggle_play();
    void seek(double seconds);
    void step(int direction);  // previous / next clip in the library's order
    void drag_trim(int handle, double seconds, bool done);
    void set_in_here();
    void set_out_here();
    void save_trim();
    void commit_notes();
    void request_filmstrip();
    double length() const;
    void update_times();

    ClipLibrary* library_;
    LibraryPage* library_page_;
    std::string file_;
    std::unique_ptr<Player> player_;
    QTimer* timer_ = nullptr;
    double in_ = 0.0;
    double out_ = 0.0;  // 0: the end
    double volume_ = 1.0;
    bool muted_ = false;
    int speed_ = 1;  // index into the speeds
    bool resume_after_scrub_ = false;
    bool scrubbing_ = false;
    bool trimming_ = false;
    unsigned strip_request_ = 0;  // a filmstrip for an older clip is dropped

    VideoView* video_ = nullptr;
    SeekBar* seek_ = nullptr;
    TrimBar* trim_ = nullptr;
    QPushButton* back_ = nullptr;
    QPushButton* prev_ = nullptr;
    QPushButton* next_ = nullptr;
    QPushButton* play_ = nullptr;
    QPushButton* mute_ = nullptr;
    QPushButton* speed_button_ = nullptr;
    QSlider* volume_slider_ = nullptr;
    QLabel* time_ = nullptr;
    QLabel* trim_times_ = nullptr;
    QLabel* trim_message_ = nullptr;
    QPushButton* save_trim_ = nullptr;
    QLineEdit* title_ = nullptr;
    QPushButton* favorite_ = nullptr;
    QLabel* collections_ = nullptr;
    QPushButton* add_collection_ = nullptr;
    QLabel* tags_ = nullptr;
    QLineEdit* tag_box_ = nullptr;
    QPlainTextEdit* notes_ = nullptr;
    QLabel* details_ = nullptr;
    QPushButton* trash_ = nullptr;
    QPushButton* restore_ = nullptr;
    QWidget* edit_panel_ = nullptr;
};

}  // namespace bt::linux_app
