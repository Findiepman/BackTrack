#include "linux/ClipPage.h"

#include "core/Library.h"
#include "linux/ClipLibrary.h"
#include "linux/Desktop.h"
#include "linux/LibraryPage.h"
#include "linux/Log.h"
#include "linux/Media.h"
#include "linux/Player.h"

#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>

namespace bt::linux_app {

namespace {

namespace fs = std::filesystem;

constexpr double kSpeeds[] = {0.5, 1.0, 1.5, 2.0};
constexpr int kSpeedCount = static_cast<int>(std::size(kSpeeds));
constexpr double kMinTrim = 1.0;  // seconds; the handles never cross closer than this
constexpr int kStripFrames = 12;
constexpr int kStripWidth = 160;
constexpr int kStripHeight = 90;
constexpr int kTickMs = 15;
constexpr int kHandleGrab = 10;  // pixels either side of a trim handle that pick it up

QString qs(const std::string& text) {
    return QString::fromStdString(text);
}

// "1:05" (player) and "1:05.3" (trim times).
QString clock_text(double seconds, bool tenths) {
    if (!std::isfinite(seconds) || seconds < 0) {
        seconds = 0;
    }
    if (tenths) {
        const auto t = static_cast<long long>(std::floor(seconds * 10 + 0.5));
        return QStringLiteral("%1:%2.%3").arg(t / 600).arg((t / 10) % 60, 2, 10, QLatin1Char('0')).arg(t % 10);
    }
    const auto t = static_cast<long long>(std::floor(seconds));
    if (t >= 3600) {
        return QStringLiteral("%1:%2:%3")
            .arg(t / 3600)
            .arg((t / 60) % 60, 2, 10, QLatin1Char('0'))
            .arg(t % 60, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(t / 60).arg(t % 60, 2, 10, QLatin1Char('0'));
}

QString escaped(const std::string& text) {
    return qs(text).toHtmlEscaped();
}

}  // namespace

// The picture, kept in its shape on black. A click plays or pauses.
class VideoView final : public QWidget {
public:
    std::function<void()> on_click;
    std::function<void(int, int)> on_resize;

    explicit VideoView(QWidget* parent) : QWidget(parent) {
        setMinimumSize(320, 180);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setAttribute(Qt::WA_OpaquePaintEvent);
    }
    void set_frame(QImage frame) {
        frame_ = std::move(frame);
        update();
    }
    void set_message(const QString& message) {
        message_ = message;
        update();
    }
    void clear() {
        frame_ = QImage();
        message_.clear();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        if (!frame_.isNull()) {
            const QSizeF shown = QSizeF(frame_.size()).scaled(QSizeF(size()), Qt::KeepAspectRatio);
            const QRectF target((width() - shown.width()) / 2, (height() - shown.height()) / 2, shown.width(), shown.height());
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.drawImage(target, frame_);
        }
        if (!message_.isEmpty()) {
            p.setPen(Qt::white);
            p.drawText(rect().adjusted(20, 20, -20, -20), Qt::AlignCenter | Qt::TextWordWrap, message_);
        }
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()) && on_click) {
            on_click();
        }
    }
    void resizeEvent(QResizeEvent*) override {
        if (on_resize) {
            const qreal ratio = devicePixelRatioF();
            on_resize(static_cast<int>(width() * ratio), static_cast<int>(height() * ratio));
        }
    }

private:
    QImage frame_;
    QString message_;
};

// The seek bar: where the player is, the trimmed part and the clip's bookmarks (click one to go
// to just before it).
class SeekBar final : public QWidget {
public:
    std::function<void(double, bool)> on_scrub;  // seconds, released
    std::function<void(std::size_t)> on_bookmark;
    double length = 0;
    double position = 0;
    double in = 0;
    double out = 0;
    std::vector<double> bookmarks;

    explicit SeekBar(QWidget* parent) : QWidget(parent) {
        setFixedHeight(26);
        setCursor(Qt::PointingHandCursor);
    }

protected:
    double x_of(double seconds) const {
        return length > 0 ? kPad + std::clamp(seconds / length, 0.0, 1.0) * (width() - 2 * kPad) : kPad;
    }
    double seconds_at(double x) const {
        return length > 0 ? std::clamp((x - kPad) / (width() - 2 * kPad), 0.0, 1.0) * length : 0.0;
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double mid = height() * 0.62;
        const QRectF track(kPad, mid - 3, width() - 2 * kPad, 6);
        p.setPen(Qt::NoPen);
        p.setBrush(palette().color(QPalette::Mid));
        p.drawRoundedRect(track, 3, 3);
        if (length > 0) {
            const double end = out > 0 ? out : length;
            p.setBrush(palette().color(QPalette::Midlight));
            p.drawRect(QRectF(x_of(in), mid - 3, x_of(end) - x_of(in), 6));
            p.setBrush(palette().color(QPalette::Highlight));
            p.drawRoundedRect(QRectF(kPad, mid - 3, x_of(position) - kPad, 6), 3, 3);
            p.setBrush(QColor(0xf5, 0xc5, 0x42));
            for (const double mark : bookmarks) {
                const double x = x_of(mark);
                const QPointF tri[3] = {{x - 4, 2}, {x + 4, 2}, {x, 9}};
                p.drawPolygon(tri, 3);
            }
            p.setBrush(palette().color(QPalette::Highlight).lighter(120));
            p.drawEllipse(QPointF(x_of(position), mid), 7, 7);
        }
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || length <= 0) {
            return;
        }
        const QPointF at = event->position();
        if (at.y() < height() * 0.45 && on_bookmark) {
            for (std::size_t i = 0; i < bookmarks.size(); ++i) {
                if (std::abs(x_of(bookmarks[i]) - at.x()) <= 6) {
                    on_bookmark(i);
                    return;
                }
            }
        }
        dragging_ = true;
        if (on_scrub) {
            on_scrub(seconds_at(at.x()), false);
        }
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (dragging_ && on_scrub) {
            on_scrub(seconds_at(event->position().x()), false);
        }
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (dragging_ && on_scrub) {
            on_scrub(seconds_at(event->position().x()), true);
        }
        dragging_ = false;
    }

private:
    static constexpr double kPad = 8;
    bool dragging_ = false;
};

// The trim bar: the filmstrip, dimmed outside the two handles. Drag a handle to move the start or
// the end; click or drag anywhere else to look at that moment.
class TrimBar final : public QWidget {
public:
    enum Drag { In, Out, Look };
    std::function<void(Drag, double, bool)> on_drag;
    double length = 0;
    double position = 0;
    double in = 0;
    double out = 0;
    std::vector<QImage> strip;

    explicit TrimBar(QWidget* parent) : QWidget(parent) { setFixedHeight(64); }

protected:
    QRectF area() const { return QRectF(kHandle, 4, width() - 2 * kHandle, height() - 8); }
    double x_of(double seconds) const {
        const QRectF a = area();
        return length > 0 ? a.left() + std::clamp(seconds / length, 0.0, 1.0) * a.width() : a.left();
    }
    double seconds_at(double x) const {
        const QRectF a = area();
        return length > 0 ? std::clamp((x - a.left()) / a.width(), 0.0, 1.0) * length : 0.0;
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF a = area();
        p.fillRect(a, QColor(0x1d, 0x23, 0x30));
        if (!strip.empty()) {
            const double w = a.width() / static_cast<double>(strip.size());
            for (std::size_t i = 0; i < strip.size(); ++i) {
                p.drawImage(QRectF(a.left() + w * static_cast<double>(i), a.top(), w, a.height()), strip[i]);
            }
        }
        if (length <= 0) {
            return;
        }
        const double end = out > 0 ? out : length;
        const double xi = x_of(in);
        const double xo = x_of(end);
        p.fillRect(QRectF(a.left(), a.top(), xi - a.left(), a.height()), QColor(0, 0, 0, 150));
        p.fillRect(QRectF(xo, a.top(), a.right() - xo, a.height()), QColor(0, 0, 0, 150));
        const QColor accent = palette().color(QPalette::Highlight);
        p.setPen(QPen(accent, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(xi, a.top(), xo - xi, a.height()));
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawRoundedRect(QRectF(xi - kHandle, a.top() - 2, kHandle, a.height() + 4), 3, 3);
        p.drawRoundedRect(QRectF(xo, a.top() - 2, kHandle, a.height() + 4), 3, 3);
        p.setPen(QPen(Qt::white, 2));
        const double xp = x_of(position);
        p.drawLine(QPointF(xp, a.top()), QPointF(xp, a.bottom()));
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || length <= 0) {
            return;
        }
        const double x = event->position().x();
        const double end = out > 0 ? out : length;
        // The nearer handle wins when both are in reach (a very short selection).
        const double to_in = std::abs(x - (x_of(in) - kHandle / 2));
        const double to_out = std::abs(x - (x_of(end) + kHandle / 2));
        if (std::min(to_in, to_out) <= kHandleGrab) {
            drag_ = to_in <= to_out ? In : Out;
        } else {
            drag_ = Look;
        }
        dragging_ = true;
        report(x, false);
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        const double x = event->position().x();
        if (dragging_) {
            report(x, false);
            return;
        }
        const double end = out > 0 ? out : length;
        const bool near = length > 0 && (std::abs(x - (x_of(in) - kHandle / 2)) <= kHandleGrab ||
                                         std::abs(x - (x_of(end) + kHandle / 2)) <= kHandleGrab);
        setCursor(near ? Qt::SizeHorCursor : Qt::PointingHandCursor);
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (dragging_) {
            report(event->position().x(), true);
        }
        dragging_ = false;
    }

private:
    void report(double x, bool done) {
        if (on_drag) {
            on_drag(drag_, seconds_at(x), done);
        }
    }
    static constexpr double kHandle = 8;
    Drag drag_ = Look;
    bool dragging_ = false;
};

ClipPage::ClipPage(ClipLibrary* library, LibraryPage* library_page, QWidget* parent)
    : QWidget(parent), library_(library), library_page_(library_page) {
    setFocusPolicy(Qt::StrongFocus);
    build();
    timer_ = new QTimer(this);
    timer_->setInterval(kTickMs);
    connect(timer_, &QTimer::timeout, this, [this] { tick(); });
    connect(library_, &ClipLibrary::changed, this, [this] {
        if (file_.empty()) {
            return;
        }
        if (library_->library().find(file_) == nullptr) {
            close_clip();  // deleted outside Backtrack
            emit back();
            return;
        }
        refresh();
    });
}

ClipPage::~ClipPage() = default;

void ClipPage::build() {
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(16, 12, 16, 12);
    root->setSpacing(20);

    auto* main = new QVBoxLayout;
    auto* top = new QHBoxLayout;
    back_ = new QPushButton(QStringLiteral("‹ Library"));
    prev_ = new QPushButton(QStringLiteral("Previous"));
    next_ = new QPushButton(QStringLiteral("Next"));
    for (QPushButton* b : {back_, prev_, next_}) {
        b->setFocusPolicy(Qt::NoFocus);
    }
    connect(back_, &QPushButton::clicked, this, [this] {
        close_clip();
        emit back();
    });
    connect(prev_, &QPushButton::clicked, this, [this] { step(-1); });
    connect(next_, &QPushButton::clicked, this, [this] { step(1); });
    top->addWidget(back_);
    top->addStretch(1);
    top->addWidget(prev_);
    top->addWidget(next_);
    main->addLayout(top);

    video_ = new VideoView(this);
    video_->on_click = [this] { toggle_play(); };
    video_->on_resize = [this](int w, int h) {
        if (player_) {
            player_->set_view_size(w, h);
        }
    };
    main->addWidget(video_, 1);

    seek_ = new SeekBar(this);
    seek_->on_scrub = [this](double seconds, bool done) {
        if (!player_) {
            return;
        }
        if (!scrubbing_) {
            // Paused while dragging so the picture follows the mouse, playing again after.
            scrubbing_ = true;
            resume_after_scrub_ = player_->playing();
            player_->pause();
        }
        const double end = out_ > 0 ? out_ : length();
        player_->seek(std::clamp(seconds, in_, std::max(in_, end)));
        if (done) {
            if (resume_after_scrub_) {
                player_->play();
            }
            scrubbing_ = false;
            resume_after_scrub_ = false;
        }
        update_times();
    };
    seek_->on_bookmark = [this](std::size_t index) {
        const ClipEntry* clip = library_->library().find(file_);
        if (player_ && clip != nullptr && index < clip->bookmarks.size()) {
            // A moment before the mark: the bookmark key is pressed just after the thing happened.
            const double end = out_ > 0 ? out_ : length();
            player_->seek(std::clamp(clip->bookmarks[index] - 2.0, in_, std::max(in_, end)));
        }
    };
    main->addWidget(seek_);

    auto* controls = new QHBoxLayout;
    play_ = new QPushButton(QStringLiteral("Play"));
    auto* back5 = new QPushButton(QStringLiteral("−5 s"));
    auto* fwd5 = new QPushButton(QStringLiteral("+5 s"));
    time_ = new QLabel;
    mute_ = new QPushButton(QStringLiteral("Mute"));
    volume_slider_ = new QSlider(Qt::Horizontal);
    volume_slider_->setRange(0, 100);
    volume_slider_->setValue(100);
    volume_slider_->setMaximumWidth(110);
    speed_button_ = new QPushButton(QStringLiteral("1x"));
    for (QWidget* w : std::initializer_list<QWidget*>{play_, back5, fwd5, mute_, volume_slider_, speed_button_}) {
        w->setFocusPolicy(Qt::NoFocus);  // Space and the arrows stay with the page
    }
    connect(play_, &QPushButton::clicked, this, [this] { toggle_play(); });
    connect(back5, &QPushButton::clicked, this, [this] {
        if (player_) {
            seek(player_->position() - 5);
        }
    });
    connect(fwd5, &QPushButton::clicked, this, [this] {
        if (player_) {
            seek(player_->position() + 5);
        }
    });
    connect(mute_, &QPushButton::clicked, this, [this] {
        muted_ = !muted_;
        if (!muted_ && volume_ <= 0.01) {
            volume_ = 0.5;
            volume_slider_->setValue(50);
        }
        if (player_) {
            player_->set_muted(muted_);
            player_->set_volume(volume_);
        }
        mute_->setText(muted_ ? QStringLiteral("Unmute") : QStringLiteral("Mute"));
    });
    connect(volume_slider_, &QSlider::valueChanged, this, [this](int value) {
        volume_ = value / 100.0;
        muted_ = value == 0;
        mute_->setText(muted_ ? QStringLiteral("Unmute") : QStringLiteral("Mute"));
        if (player_) {
            player_->set_volume(volume_);
            player_->set_muted(muted_);
        }
    });
    connect(speed_button_, &QPushButton::clicked, this, [this] {
        speed_ = (speed_ + 1) % kSpeedCount;
        speed_button_->setText(QStringLiteral("%1x").arg(kSpeeds[speed_]));
        speed_button_->setToolTip(kSpeeds[speed_] == 1.0 ? QString() : QStringLiteral("No sound at this speed"));
        if (player_) {
            player_->set_rate(kSpeeds[speed_]);
        }
    });
    controls->addWidget(play_);
    controls->addWidget(back5);
    controls->addWidget(fwd5);
    controls->addWidget(time_);
    controls->addStretch(1);
    controls->addWidget(mute_);
    controls->addWidget(volume_slider_);
    controls->addWidget(speed_button_);
    main->addLayout(controls);

    trim_ = new TrimBar(this);
    trim_->on_drag = [this](TrimBar::Drag what, double seconds, bool done) {
        drag_trim(static_cast<int>(what), seconds, done);
    };
    main->addWidget(trim_);

    auto* trim_row = new QHBoxLayout;
    trim_times_ = new QLabel;
    auto* set_in = new QPushButton(QStringLiteral("Set start (I)"));
    auto* set_out = new QPushButton(QStringLiteral("Set end (O)"));
    auto* reset = new QPushButton(QStringLiteral("Reset"));
    save_trim_ = new QPushButton(QStringLiteral("Save as new clip"));
    save_trim_->setToolTip(QStringLiteral("Copies the selection into a new clip without re-encoding. It starts on the "
                                          "keyframe at or before the start (at most a second earlier)."));
    for (QPushButton* b : {set_in, set_out, reset, save_trim_}) {
        b->setFocusPolicy(Qt::NoFocus);
    }
    connect(set_in, &QPushButton::clicked, this, [this] { set_in_here(); });
    connect(set_out, &QPushButton::clicked, this, [this] { set_out_here(); });
    connect(reset, &QPushButton::clicked, this, [this] {
        in_ = 0;
        out_ = length();
        update_times();
    });
    connect(save_trim_, &QPushButton::clicked, this, [this] { save_trim(); });
    trim_row->addWidget(trim_times_);
    trim_row->addStretch(1);
    trim_row->addWidget(set_in);
    trim_row->addWidget(set_out);
    trim_row->addWidget(reset);
    trim_row->addWidget(save_trim_);
    main->addLayout(trim_row);
    trim_message_ = new QLabel;
    trim_message_->setWordWrap(true);
    trim_message_->setTextFormat(Qt::RichText);
    connect(trim_message_, &QLabel::linkActivated, this, [this](const QString& file) { open(file.toStdString()); });
    main->addWidget(trim_message_);
    root->addLayout(main, 1);

    // The side panel.
    auto* side = new QWidget;
    auto* panel = new QVBoxLayout(side);
    panel->setContentsMargins(0, 0, 8, 0);
    edit_panel_ = new QWidget;
    auto* edits = new QVBoxLayout(edit_panel_);
    edits->setContentsMargins(0, 0, 0, 0);
    title_ = new QLineEdit;
    QFont big = title_->font();
    big.setBold(true);
    big.setPointSizeF(big.pointSizeF() * 1.2);
    title_->setFont(big);
    title_->setPlaceholderText(QStringLiteral("Title"));
    connect(title_, &QLineEdit::editingFinished, this, [this] { commit_title(); });
    favorite_ = new QPushButton;
    connect(favorite_, &QPushButton::clicked, this, [this] {
        if (const ClipEntry* clip = library_->library().find(file_)) {
            library_->edit().set_favorite(file_, !clip->favorite);
            library_->save();
        }
    });
    edits->addWidget(title_);
    edits->addWidget(favorite_);

    edits->addWidget(new QLabel(QStringLiteral("<b>Collections</b>")));
    collections_ = new QLabel;
    collections_->setWordWrap(true);
    collections_->setTextFormat(Qt::RichText);
    connect(collections_, &QLabel::linkActivated, this, [this](const QString& id) { emit show_collection(id); });
    edits->addWidget(collections_);
    add_collection_ = new QPushButton(QStringLiteral("Add to collection…"));
    connect(add_collection_, &QPushButton::clicked, this, [this] {
        QMenu menu(this);
        library_page_->fill_collection_menu(&menu, file_);
        menu.exec(add_collection_->mapToGlobal(QPoint(0, add_collection_->height())));
    });
    edits->addWidget(add_collection_, 0, Qt::AlignLeft);

    edits->addWidget(new QLabel(QStringLiteral("<b>Tags</b>")));
    tags_ = new QLabel;
    tags_->setWordWrap(true);
    tags_->setTextFormat(Qt::RichText);
    tags_->setToolTip(QStringLiteral("Click a tag to remove it"));
    connect(tags_, &QLabel::linkActivated, this, [this](const QString& index) {
        const ClipEntry* clip = library_->library().find(file_);
        const auto i = static_cast<std::size_t>(index.toULongLong());
        if (clip != nullptr && i < clip->tags.size() && library_->edit().remove_tag(file_, std::string(clip->tags[i]))) {
            library_->save();
        }
    });
    edits->addWidget(tags_);
    tag_box_ = new QLineEdit;
    tag_box_->setPlaceholderText(QStringLiteral("Add a tag and press Enter"));
    connect(tag_box_, &QLineEdit::returnPressed, this, [this] {
        const std::string tag = tag_box_->text().trimmed().toStdString();
        tag_box_->clear();
        if (!tag.empty() && library_->edit().add_tag(file_, tag)) {
            library_->save();
        }
    });
    edits->addWidget(tag_box_);

    edits->addWidget(new QLabel(QStringLiteral("<b>Notes</b>")));
    notes_ = new QPlainTextEdit;
    notes_->setPlaceholderText(QStringLiteral("Anything to remember about this clip"));
    notes_->setMinimumHeight(90);
    auto* notes_timer = new QTimer(this);
    notes_timer->setSingleShot(true);
    notes_timer->setInterval(800);  // saved a moment after typing stops, not on every key
    connect(notes_timer, &QTimer::timeout, this, [this] { commit_notes(); });
    connect(notes_, &QPlainTextEdit::textChanged, notes_timer, qOverload<>(&QTimer::start));
    edits->addWidget(notes_);
    panel->addWidget(edit_panel_);

    panel->addWidget(new QLabel(QStringLiteral("<b>Details</b>")));
    details_ = new QLabel;
    details_->setWordWrap(true);
    details_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    panel->addWidget(details_);

    auto* show = new QPushButton(QStringLiteral("Show in folder"));
    connect(show, &QPushButton::clicked, this, [this] { show_in_folder(library_->path_of(file_)); });
    auto* external = new QPushButton(QStringLiteral("Play in your video player"));
    connect(external, &QPushButton::clicked, this,
            [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(qs(library_->path_of(file_)))); });
    trash_ = new QPushButton(QStringLiteral("Move to Trash"));
    connect(trash_, &QPushButton::clicked, this, [this] {
        // The next clip opens (or the one before, or the library), as on Windows.
        const std::vector<std::string> order = library_page_->order();
        const auto it = std::find(order.begin(), order.end(), file_);
        std::string next;
        if (it != order.end() && it + 1 != order.end()) {
            next = *(it + 1);
        } else if (it != order.end() && it != order.begin()) {
            next = *(it - 1);
        }
        const std::string file = file_;
        close_clip();
        library_page_->move_to_trash(file);
        if (!next.empty()) {
            open(next);
        } else {
            emit back();
        }
    });
    restore_ = new QPushButton(QStringLiteral("Restore"));
    connect(restore_, &QPushButton::clicked, this, [this] { library_page_->restore(file_); });
    for (QPushButton* b : {show, external, trash_, restore_}) {
        b->setFocusPolicy(Qt::NoFocus);
        panel->addWidget(b);
    }
    panel->addStretch(1);

    auto* scroll = new QScrollArea;
    scroll->setWidget(side);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFixedWidth(330);
    scroll->setFocusPolicy(Qt::NoFocus);  // the arrow keys stay with the player
    root->addWidget(scroll);
}

void ClipPage::open(const std::string& file) {
    const ClipEntry* clip = library_->library().find(file);
    if (clip == nullptr) {
        return;
    }
    close_clip();
    file_ = file;
    in_ = 0;
    out_ = clip->duration_s;
    trim_message_->clear();
    notes_->blockSignals(true);
    notes_->setPlainText(qs(clip->notes));
    notes_->blockSignals(false);
    tag_box_->clear();
    title_->setText(qs(display_title(*clip)));
    video_->clear();
    trim_->strip.clear();
    player_ = std::make_unique<Player>(library_->path_of(file));
    const qreal ratio = video_->devicePixelRatioF();
    player_->set_view_size(static_cast<int>(video_->width() * ratio), static_cast<int>(video_->height() * ratio));
    player_->set_volume(volume_);
    player_->set_muted(muted_);
    player_->set_rate(kSpeeds[speed_]);
    timer_->start();
    request_filmstrip();
    refresh();
    update_times();
    setFocus();
}

void ClipPage::close_clip() {
    if (file_.empty()) {
        return;
    }
    // Before the file is forgotten: the title box loses focus (and says so) only afterwards.
    commit_title();
    commit_notes();
    timer_->stop();
    player_.reset();  // lets go of the file: it can be moved to the trash now
    ++strip_request_;
    scrubbing_ = false;
    file_.clear();
}

void ClipPage::commit_title() {
    const ClipEntry* clip = library_->library().find(file_);
    if (clip == nullptr || clip->trashed_at) {
        return;
    }
    const std::string title = title_->text().trimmed().toStdString();
    if (title != display_title(*clip) && library_->edit().set_title(file_, title)) {
        library_->save();
    }
}

void ClipPage::commit_notes() {
    const ClipEntry* clip = library_->library().find(file_);
    if (clip == nullptr) {
        return;
    }
    const std::string notes = notes_->toPlainText().toStdString();
    if (notes != clip->notes && library_->edit().set_notes(file_, notes)) {
        library_->save();
    }
}

double ClipPage::length() const {
    if (player_ && player_->duration() > 0) {
        return player_->duration();
    }
    const ClipEntry* clip = library_->library().find(file_);
    return clip != nullptr ? clip->duration_s : 0.0;
}

void ClipPage::refresh() {
    const ClipEntry* clip = library_->library().find(file_);
    if (clip == nullptr) {
        return;
    }
    const bool trashed = clip->trashed_at.has_value();
    if (!title_->hasFocus()) {
        title_->setText(qs(display_title(*clip)));
    }
    edit_panel_->setEnabled(!trashed);
    favorite_->setText(clip->favorite ? QStringLiteral("★ Favorite") : QStringLiteral("☆ Add to favorites"));

    QStringList chips;
    for (const std::string& id : clip->collections) {
        if (const Collection* c = library_->library().find_collection(id)) {
            chips << QStringLiteral("<a href=\"%1\" style=\"color:%2; text-decoration:none\">● %3</a>")
                         .arg(qs(c->id).toHtmlEscaped(), collection_color(c->color).name(),
                              c->name.empty() ? QStringLiteral("Untitled") : escaped(c->name));
        }
    }
    collections_->setText(chips.isEmpty() ? QStringLiteral("<i>None</i>") : chips.join(QStringLiteral("&nbsp;&nbsp; ")));
    QStringList tags;
    for (std::size_t i = 0; i < clip->tags.size(); ++i) {
        tags << QStringLiteral("<a href=\"%1\" style=\"text-decoration:none\">#%2 ✕</a>").arg(i).arg(escaped(clip->tags[i]));
    }
    tags_->setText(tags.isEmpty() ? QStringLiteral("<i>No tags</i>") : tags.join(QStringLiteral("&nbsp;&nbsp; ")));

    QStringList lines;
    const QString when = clip->created > 0
                             ? QLocale().toString(QDateTime::fromSecsSinceEpoch(clip->created), QLocale::LongFormat)
                             : QStringLiteral("Unknown");
    lines << QStringLiteral("Saved: %1").arg(when);
    if (clip->duration_s > 0) {
        lines << QStringLiteral("Length: %1").arg(qs(format_duration(clip->duration_s)));
    }
    lines << QStringLiteral("Size: %1").arg(qs(format_size(clip->size_bytes)));
    if (clip->width > 0) {
        lines << QStringLiteral("Picture: %1 × %2").arg(clip->width).arg(clip->height);
    }
    if (!clip->source.empty()) {
        lines << QStringLiteral("Recorded: %1").arg(qs(clip->source));
    }
    if (!clip->bookmarks.empty()) {
        lines << QStringLiteral("Bookmarks: %1 (the marks on the seek bar)").arg(clip->bookmarks.size());
    }
    lines << QStringLiteral("File: %1").arg(qs(clip->file));
    if (trashed) {
        lines << QStringLiteral("In the Trash: it goes to the desktop's trash %1 days after it was put there.")
                     .arg(kTrashDays);
    }
    details_->setText(lines.join(QLatin1Char('\n')));
    trash_->setVisible(!trashed);
    restore_->setVisible(trashed);
    save_trim_->setEnabled(!trimming_);

    const std::vector<std::string> order = library_page_->order();
    const auto it = std::find(order.begin(), order.end(), file_);
    prev_->setEnabled(it != order.end() && it != order.begin());
    next_->setEnabled(it != order.end() && it + 1 != order.end());
    seek_->bookmarks = clip->bookmarks;
    seek_->update();
}

void ClipPage::tick() {
    if (!player_) {
        return;
    }
    QImage frame;
    if (player_->take_frame(frame)) {
        video_->set_frame(std::move(frame));
    }
    const std::string error = player_->error();
    if (!error.empty()) {
        video_->set_message(qs(error));
    }
    if (player_->ready() && out_ <= 0) {
        out_ = length();
    }
    if (player_->playing() && !scrubbing_ && out_ > 0 && player_->position() >= out_ - 0.01) {
        // Playback stays inside the trim: it stops at the end handle.
        player_->pause();
        player_->seek(out_);
    }
    update_times();
}

void ClipPage::update_times() {
    const double len = length();
    const double at = player_ ? player_->position() : 0.0;
    const double end = out_ > 0 ? out_ : len;
    time_->setText(QStringLiteral("%1 / %2").arg(clock_text(at, false), clock_text(len, false)));
    play_->setText(player_ && player_->playing() ? QStringLiteral("Pause") : QStringLiteral("Play"));
    trim_times_->setText(QStringLiteral("Start %1   End %2   (%3)")
                             .arg(clock_text(in_, true), clock_text(end, true), clock_text(std::max(0.0, end - in_), true)));
    seek_->length = trim_->length = len;
    seek_->position = trim_->position = at;
    seek_->in = trim_->in = in_;
    seek_->out = trim_->out = end;
    seek_->update();
    trim_->update();
}

void ClipPage::toggle_play() {
    if (!player_ || !player_->ready()) {
        return;
    }
    if (player_->playing()) {
        player_->pause();
    } else {
        const double at = player_->position();
        const double end = out_ > 0 ? out_ : length();
        if (at < in_ - 0.05 || at >= end - 0.05) {
            player_->seek(in_);  // from the start of the trim
        }
        player_->play();
    }
    update_times();
}

void ClipPage::seek(double seconds) {
    if (!player_) {
        return;
    }
    const double end = out_ > 0 ? out_ : length();
    player_->seek(std::clamp(seconds, in_, std::max(in_, end)));
    update_times();
}

void ClipPage::step(int direction) {
    const std::vector<std::string> order = library_page_->order();
    const auto it = std::find(order.begin(), order.end(), file_);
    if (it == order.end()) {
        return;
    }
    if (direction < 0 && it != order.begin()) {
        open(*(it - 1));
    } else if (direction > 0 && it + 1 != order.end()) {
        open(*(it + 1));
    }
}

void ClipPage::drag_trim(int handle, double seconds, bool done) {
    const double len = length();
    if (!player_ || len <= 0) {
        return;
    }
    if (out_ <= 0) {
        out_ = len;
    }
    if (!scrubbing_) {
        scrubbing_ = true;
        resume_after_scrub_ = player_->playing() && handle == TrimBar::Look;
        player_->pause();
    }
    if (handle == TrimBar::In) {
        in_ = std::clamp(seconds, 0.0, std::max(0.0, out_ - kMinTrim));
        player_->seek(in_);
    } else if (handle == TrimBar::Out) {
        out_ = std::clamp(seconds, std::min(len, in_ + kMinTrim), len);
        player_->seek(out_);
    } else {
        player_->seek(std::clamp(seconds, in_, out_));
    }
    if (done) {
        if (resume_after_scrub_) {
            player_->play();
        }
        scrubbing_ = false;
        resume_after_scrub_ = false;
    }
    update_times();
}

void ClipPage::set_in_here() {
    const double len = length();
    if (player_ && len > 0) {
        const double end = out_ > 0 ? out_ : len;
        in_ = std::clamp(player_->position(), 0.0, std::max(0.0, end - kMinTrim));
        update_times();
    }
}

void ClipPage::set_out_here() {
    const double len = length();
    if (player_ && len > 0) {
        out_ = std::clamp(player_->position(), std::min(len, in_ + kMinTrim), len);
        update_times();
    }
}

void ClipPage::request_filmstrip() {
    const unsigned request = ++strip_request_;
    const std::string path = library_->path_of(file_);
    QPointer<ClipPage> self(this);
    QThreadPool::globalInstance()->start([self, request, path] {
        std::vector<QImage> strip;
        for (const Still& still : read_filmstrip(path, kStripFrames, kStripWidth, kStripHeight)) {
            strip.push_back(QImage(still.pixels.data(), still.width, still.height, still.width * 4, QImage::Format_RGB32).copy());
        }
        // qApp, not the page, as the context: it outlives every page, and the page is checked.
        QMetaObject::invokeMethod(
            qApp,
            [self, request, strip = std::move(strip)] {
                if (self && self->strip_request_ == request) {
                    self->trim_->strip = strip;
                    self->trim_->update();
                }
            },
            Qt::QueuedConnection);
    });
}

void ClipPage::save_trim() {
    const ClipEntry* clip = library_->library().find(file_);
    if (clip == nullptr || trimming_) {
        return;
    }
    const double len = length();
    const double out = out_ > 0 ? out_ : len;
    const fs::path source(library_->path_of(file_));
    // Next to the original: "<name>_trim.mp4", "<name>_trim2.mp4", ...
    fs::path destination;
    for (int n = 1; n < 1000; ++n) {
        destination = source.parent_path() / (source.stem().string() + (n == 1 ? "_trim" : "_trim" + std::to_string(n)) + ".mp4");
        std::error_code ec;
        const fs::path part = destination.parent_path() / ("." + destination.filename().string() + ".part");
        if (!fs::exists(destination, ec) && !fs::exists(part, ec)) {
            break;
        }
    }
    const std::string title = display_title(*clip) + " (trimmed)";
    const std::vector<std::string> tags = clip->tags;
    const std::vector<double> marks = clip->bookmarks;
    const double in = in_;
    trimming_ = true;
    save_trim_->setEnabled(false);
    trim_message_->setText(QStringLiteral("Saving the trimmed clip…"));
    log_info("Trimming " + file_ + " from " + std::to_string(in) + " s to " + std::to_string(out) + " s");
    QPointer<ClipPage> self(this);
    const std::string from = source.string();
    const std::string to = destination.string();
    QThreadPool::globalInstance()->start([self, from, to, in, out, title, tags, marks] {
        const TrimResult result = trim_clip(from, to, in, out);
        QMetaObject::invokeMethod(
            qApp,
            [self, result, to, title, tags, marks] {
                if (!self) {
                    return;
                }
                ClipPage& page = *self;
                page.trimming_ = false;
                page.save_trim_->setEnabled(true);
                if (!result.ok) {
                    log_warning("Trimming failed: " + result.error);
                    page.trim_message_->setText(QStringLiteral("Could not save the trimmed clip: %1").arg(escaped(result.error)));
                    return;
                }
                const std::string file = fs::path(to).filename().string();
                page.library_->scan();
                if (page.library_->library().find(file) != nullptr) {
                    Library& library = page.library_->edit();
                    library.set_title(file, title);
                    for (const std::string& tag : tags) {
                        library.add_tag(file, tag);
                    }
                    // The cut starts at the keyframe before the start handle (start_s): that is time 0.
                    library.set_bookmarks(file, bookmarks_within(marks, result.start_s, result.seconds));
                    page.library_->save();
                }
                page.trim_message_->setText(QStringLiteral("Saved %1 (%2, %3). <a href=\"%4\">Open it</a>")
                                                .arg(escaped(file), qs(format_duration(result.seconds)),
                                                     qs(format_size(static_cast<std::int64_t>(result.bytes))),
                                                     qs(file).toHtmlEscaped()));
                emit page.trimmed(qs(file));
            },
            Qt::QueuedConnection);
    });
}

void ClipPage::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
        case Qt::Key_Space:
        case Qt::Key_K: toggle_play(); return;
        case Qt::Key_Left:
            if (player_) {
                seek(player_->position() - 5);
            }
            return;
        case Qt::Key_Right:
            if (player_) {
                seek(player_->position() + 5);
            }
            return;
        case Qt::Key_I: set_in_here(); return;
        case Qt::Key_O: set_out_here(); return;
        case Qt::Key_Escape:
            if (qobject_cast<QLineEdit*>(focusWidget()) != nullptr || qobject_cast<QPlainTextEdit*>(focusWidget()) != nullptr) {
                setFocus();  // out of the text box, not off the page
                return;
            }
            close_clip();
            emit back();
            return;
        default: QWidget::keyPressEvent(event);
    }
}

}  // namespace bt::linux_app
