#include "linux/LibraryPage.h"

#include "linux/ClipLibrary.h"
#include "linux/Desktop.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QScrollBar>
#include <QStackedLayout>
#include <QStyledItemDelegate>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>

namespace bt::linux_app {

namespace {

constexpr int kCardWidth = 256;
constexpr int kThumbHeight = kCardWidth * 9 / 16;
constexpr int kCardHeight = kThumbHeight + 52;
constexpr int kGap = 14;
constexpr int kFileRole = Qt::UserRole;

std::int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

QString qs(const std::string& text) {
    return QString::fromStdString(text);
}

struct SortName {
    ClipSort sort;
    const char* label;
};
constexpr SortName kSortNames[] = {
    {ClipSort::Newest, "Newest"},     {ClipSort::Oldest, "Oldest"},   {ClipSort::Longest, "Longest"},
    {ClipSort::Shortest, "Shortest"}, {ClipSort::Largest, "Largest"}, {ClipSort::Name, "Name"},
};

QRect star_rect(const QRect& card) {
    return QRect(card.right() - 6 - 26, card.top() + 6, 26, 26);
}

// One card: thumbnail, length, favorite star, title, resolution · size · age.
class CardDelegate final : public QStyledItemDelegate {
public:
    explicit CardDelegate(ClipLibrary* library, QObject* parent) : QStyledItemDelegate(parent), library_(library) {}

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(kCardWidth, kCardHeight);
    }

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const ClipEntry* clip = library_->library().find(index.data(kFileRole).toString().toStdString());
        if (clip == nullptr) {
            return;
        }
        const QPalette& pal = option.palette;
        const QRect card = option.rect.adjusted(2, 2, -2, -2);
        const bool selected = (option.state & QStyle::State_Selected) != 0;
        const bool hover = (option.state & QStyle::State_MouseOver) != 0;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        QPainterPath shape;
        shape.addRoundedRect(QRectF(card), 8, 8);
        p->fillPath(shape, pal.color(QPalette::Base));

        const QRect thumb(card.left(), card.top(), card.width(), card.width() * 9 / 16);
        p->save();
        p->setClipPath(shape);
        const QImage picture = library_->thumbnail(*clip, true);
        if (!picture.isNull()) {
            p->drawImage(thumb, picture);
        } else {
            p->fillRect(thumb, QColor(0x1d, 0x23, 0x30));
        }
        p->restore();

        QFont small = option.font;
        small.setPointSizeF(small.pointSizeF() * 0.9);
        if (clip->duration_s > 0) {
            const QString length = qs(format_duration(clip->duration_s));
            p->setFont(small);
            const int w = p->fontMetrics().horizontalAdvance(length) + 12;
            const QRect badge(thumb.right() - 6 - w, thumb.bottom() - 6 - 18, w, 18);
            QPainterPath pill;
            pill.addRoundedRect(QRectF(badge), 5, 5);
            p->fillPath(pill, QColor(0, 0, 0, 184));
            p->setPen(Qt::white);
            p->drawText(badge, Qt::AlignCenter, length);
        }
        if (clip->favorite || hover) {
            const QRect star = star_rect(card);
            QPainterPath pill;
            pill.addRoundedRect(QRectF(star), 6, 6);
            p->fillPath(pill, QColor(10, 12, 16, 160));
            p->setFont(option.font);
            p->setPen(clip->favorite ? QColor(0xf5, 0xc5, 0x42) : QColor(Qt::white));
            p->drawText(star, Qt::AlignCenter, clip->favorite ? QStringLiteral("★") : QStringLiteral("☆"));
        }

        QFont bold = option.font;
        bold.setBold(true);
        p->setFont(bold);
        p->setPen(pal.color(QPalette::Text));
        const QRect title(card.left() + 10, thumb.bottom() + 6, card.width() - 20, 20);
        p->drawText(title, Qt::AlignLeft | Qt::AlignVCenter,
                    p->fontMetrics().elidedText(qs(display_title(*clip)), Qt::ElideRight, title.width()));
        QStringList meta;
        for (const std::string& part : {format_resolution(clip->width, clip->height), format_size(clip->size_bytes),
                                        clip->trashed_at ? std::string("In Trash") : format_age(now_unix() - clip->created)}) {
            if (!part.empty()) {
                meta << qs(part);
            }
        }
        p->setFont(small);
        p->setPen(pal.color(QPalette::PlaceholderText));
        const QRect info(card.left() + 10, title.bottom() + 2, card.width() - 20, 18);
        p->drawText(info, Qt::AlignLeft | Qt::AlignVCenter,
                    p->fontMetrics().elidedText(meta.join(QStringLiteral("  ·  ")), Qt::ElideRight, info.width()));

        p->setPen(QPen(selected ? pal.color(QPalette::Highlight)
                                : hover ? pal.color(QPalette::Mid) : pal.color(QPalette::Midlight),
                       selected ? 2 : 1));
        p->drawPath(shape);
        p->restore();
    }

private:
    ClipLibrary* library_;
};

}  // namespace

// The grid itself: a click on a card's star toggles the favorite instead of selecting the card.
class ClipGrid final : public QListWidget {
public:
    ClipGrid(ClipLibrary* library, QWidget* parent) : QListWidget(parent), library_(library) {
        setViewMode(QListView::IconMode);
        setResizeMode(QListView::Adjust);
        setMovement(QListView::Static);
        setUniformItemSizes(true);
        setSpacing(kGap / 2);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setMouseTracking(true);
        setFrameShape(QFrame::NoFrame);
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        setItemDelegate(new CardDelegate(library, this));
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            if (QListWidgetItem* item = itemAt(event->position().toPoint())) {
                const QRect card = visualItemRect(item).adjusted(2, 2, -2, -2);
                const std::string file = item->data(kFileRole).toString().toStdString();
                const ClipEntry* clip = library_->library().find(file);
                if (clip != nullptr && !clip->trashed_at && star_rect(card).contains(event->position().toPoint())) {
                    library_->edit().set_favorite(file, !clip->favorite);
                    library_->save();
                    return;
                }
            }
        }
        QListWidget::mousePressEvent(event);
    }

private:
    ClipLibrary* library_;
};

QColor collection_color(int index) {
    static const QColor colors[kCollectionColorCount] = {QColor(0x3ec9a7), QColor(0xf0904b), QColor(0x9d7bff),
                                                         QColor(0x4f8cff), QColor(0xff6f91), QColor(0xe8c547)};
    return colors[std::clamp(index, 0, kCollectionColorCount - 1)];
}

QIcon collection_icon(int color) {
    QPixmap dot(16, 16);
    dot.fill(Qt::transparent);
    QPainter p(&dot);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(collection_color(color));
    p.drawEllipse(QRectF(3, 3, 10, 10));
    return QIcon(dot);
}

LibraryPage::LibraryPage(ClipLibrary* library, QWidget* parent) : QWidget(parent), library_(library) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 12, 16, 12);

    auto* header = new QHBoxLayout;
    title_ = new QLabel;
    QFont big = title_->font();
    big.setBold(true);
    big.setPointSizeF(big.pointSizeF() * 1.4);
    title_->setFont(big);
    count_ = new QLabel;
    header->addWidget(title_);
    header->addWidget(count_);
    header->addStretch(1);
    search_ = new QLineEdit;
    search_->setPlaceholderText(QStringLiteral("Search titles, tags and notes"));
    search_->setClearButtonEnabled(true);
    search_->setMinimumWidth(240);
    tag_ = new QComboBox;
    sort_ = new QComboBox;
    for (const SortName& s : kSortNames) {
        sort_->addItem(QString::fromLatin1(s.label), static_cast<int>(s.sort));
    }
    header->addWidget(search_);
    header->addWidget(tag_);
    header->addWidget(sort_);
    root->addLayout(header);

    grid_ = new ClipGrid(library_, this);
    empty_ = new QLabel;
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setWordWrap(true);
    auto* holder = new QWidget;
    stack_ = new QStackedLayout(holder);
    stack_->addWidget(grid_);
    stack_->addWidget(empty_);
    root->addWidget(holder, 1);

    connect(search_, &QLineEdit::textChanged, this, [this] { refresh(); });
    connect(tag_, &QComboBox::activated, this, [this] { refresh(); });
    connect(sort_, &QComboBox::activated, this, [this] { refresh(); });
    connect(grid_, &QListWidget::itemActivated, this,
            [this](QListWidgetItem* item) { emit open_clip(item->data(kFileRole).toString()); });
    grid_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(grid_, &QWidget::customContextMenuRequested, this, [this](const QPoint& at) {
        if (QListWidgetItem* item = grid_->itemAt(at)) {
            clip_menu(item->data(kFileRole).toString().toStdString(), grid_->viewport()->mapToGlobal(at));
        }
    });
    connect(library_, &ClipLibrary::changed, this, [this] { refresh(); });
    connect(library_, &ClipLibrary::thumbnail_ready, grid_->viewport(), [this] { grid_->viewport()->update(); });
    refresh();
}

void LibraryPage::set_view(LibraryView view, const std::string& collection) {
    view_ = view;
    collection_ = view == LibraryView::Collection ? collection : std::string();
    refresh();
}

LibraryQuery LibraryPage::query() const {
    LibraryQuery q;
    q.view = view_;
    q.collection = collection_;
    q.search = search_->text().trimmed().toStdString();
    q.tag = tag_->currentIndex() > 0 ? tag_->currentData().toString().toStdString() : std::string();
    q.sort = static_cast<ClipSort>(sort_->currentData().toInt());
    q.now = now_unix();
    return q;
}

std::vector<std::string> LibraryPage::order() const {
    std::vector<std::string> files;
    for (const std::size_t i : library_->library().query(query())) {
        files.push_back(library_->library().clips()[i].file);
    }
    return files;
}

QString LibraryPage::view_title() const {
    switch (view_) {
        case LibraryView::All: return QStringLiteral("All clips");
        case LibraryView::Favorites: return QStringLiteral("Favorites");
        case LibraryView::Recent: return QStringLiteral("Recent");
        case LibraryView::Trash: return QStringLiteral("Trash");
        case LibraryView::Shared: return QStringLiteral("Shared");
        case LibraryView::Collection: {
            const Collection* c = library_->library().find_collection(collection_);
            return c != nullptr ? qs(c->name) : QStringLiteral("Collection");
        }
    }
    return QStringLiteral("All clips");
}

void LibraryPage::refresh_tags() {
    const QString chosen = tag_->currentIndex() > 0 ? tag_->currentData().toString() : QString();
    tag_->clear();
    tag_->addItem(QStringLiteral("All tags"));
    for (const std::string& tag : library_->library().all_tags()) {
        tag_->addItem(QStringLiteral("#") + qs(tag), qs(tag));
    }
    // A tag that no clip has any more falls back to all tags.
    tag_->setCurrentIndex(chosen.isEmpty() ? 0 : std::max(0, tag_->findData(chosen)));
    tag_->setVisible(tag_->count() > 1);
}

void LibraryPage::refresh() {
    if (view_ == LibraryView::Collection && library_->library().find_collection(collection_) == nullptr) {
        view_ = LibraryView::All;  // the collection was deleted
        collection_.clear();
    }
    refresh_tags();
    title_->setText(view_title());
    const QString selected = grid_->currentItem() != nullptr ? grid_->currentItem()->data(kFileRole).toString() : QString();
    const std::vector<std::string> files = order();
    // Rebuilt on every change (a star, a thumbnail's length): the scroll position stays.
    const int scrolled = grid_->verticalScrollBar()->value();
    grid_->setUpdatesEnabled(false);
    grid_->clear();
    for (const std::string& file : files) {
        auto* item = new QListWidgetItem;
        item->setData(kFileRole, qs(file));
        item->setSizeHint(QSize(kCardWidth, kCardHeight));
        grid_->addItem(item);
        if (qs(file) == selected) {
            grid_->setCurrentItem(item);
        }
    }
    grid_->doItemsLayout();
    grid_->verticalScrollBar()->setValue(scrolled);
    grid_->setUpdatesEnabled(true);
    count_->setText(files.empty() ? QString() : QStringLiteral("%1 clip%2").arg(files.size()).arg(files.size() == 1 ? "" : "s"));

    if (files.empty()) {
        const bool searching = !search_->text().trimmed().isEmpty() || tag_->currentIndex() > 0;
        QString text;
        if (searching) {
            text = QStringLiteral("No clips match the search.");
        } else if (view_ == LibraryView::Favorites) {
            text = QStringLiteral("No favorites yet. Click the star on a clip to keep it here.");
        } else if (view_ == LibraryView::Trash) {
            text = QStringLiteral("The Trash is empty. Clips stay here for %1 days before they go to the desktop's trash.")
                       .arg(kTrashDays);
        } else if (view_ == LibraryView::Shared) {
            text = QStringLiteral("No shared clips.");
        } else if (view_ == LibraryView::Collection) {
            text = QStringLiteral("This collection is empty. Right-click a clip › Add to collection.");
        } else if (view_ == LibraryView::Recent) {
            text = QStringLiteral("No clips from the last %1 days.").arg(kRecentDays);
        } else {
            text = QStringLiteral("No clips yet. Press the save hotkey to keep the last moments of your screen.");
        }
        empty_->setText(text);
        stack_->setCurrentWidget(empty_);
    } else {
        stack_->setCurrentWidget(grid_);
    }
}

void LibraryPage::fill_collection_menu(QMenu* menu, const std::string& file) {
    const ClipEntry* clip = library_->library().find(file);
    if (clip == nullptr) {
        return;
    }
    for (const Collection& c : library_->library().collections()) {
        QAction* action = menu->addAction(collection_icon(c.color), c.name.empty() ? QStringLiteral("Untitled") : qs(c.name));
        action->setCheckable(true);
        action->setChecked(std::find(clip->collections.begin(), clip->collections.end(), c.id) != clip->collections.end());
        const std::string id = c.id;
        connect(action, &QAction::triggered, this, [this, file, id](bool member) {
            if (library_->edit().set_in_collection(file, id, member)) {
                library_->save();
            }
        });
    }
    if (!library_->library().collections().empty()) {
        menu->addSeparator();
    }
    menu->addAction(QStringLiteral("New collection…"), this, [this, file] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("New collection"), QStringLiteral("Name"),
                                                   QLineEdit::Normal, QStringLiteral("New collection"), &ok)
                                 .trimmed();
        if (!ok || name.isEmpty() || library_->library().find(file) == nullptr) {
            return;
        }
        const std::string id = library_->edit().create_collection(
            name.toStdString(), static_cast<int>(library_->library().collections().size()) % kCollectionColorCount);
        library_->edit().set_in_collection(file, id, true);
        library_->save();
    });
}

void LibraryPage::rename_clip(const std::string& file) {
    const ClipEntry* clip = library_->library().find(file);
    if (clip == nullptr || clip->trashed_at) {
        return;
    }
    bool ok = false;
    const QString title = QInputDialog::getText(this, QStringLiteral("Rename clip"), QStringLiteral("Title"),
                                                QLineEdit::Normal, qs(display_title(*clip)), &ok);
    if (ok && library_->edit().set_title(file, title.trimmed().toStdString())) {
        library_->save();
    }
}

void LibraryPage::move_to_trash(const std::string& file) {
    if (library_->edit().move_to_trash(file, now_unix())) {
        library_->save();
    }
}

void LibraryPage::restore(const std::string& file) {
    if (library_->edit().restore(file, now_unix())) {
        library_->save();
    }
}

bool LibraryPage::delete_forever(const std::string& file) {
    if (library_->delete_forever(file)) {
        return true;
    }
    QMessageBox::warning(this, QStringLiteral("Backtrack"),
                         QStringLiteral("Could not move the clip to the trash. Check that the clips folder can be written."));
    return false;
}

void LibraryPage::clip_menu(const std::string& file, const QPoint& at) {
    const ClipEntry* clip = library_->library().find(file);
    if (clip == nullptr) {
        return;
    }
    QMenu menu(this);
    menu.addAction(QStringLiteral("Open"), this, [this, file] { emit open_clip(qs(file)); });
    menu.addAction(QStringLiteral("Play in your video player"), this,
                   [this, file] { QDesktopServices::openUrl(QUrl::fromLocalFile(qs(library_->path_of(file)))); });
    if (share_link_live(*clip, now_unix())) {
        const QString url = qs(clip->share_url);
        menu.addAction(QStringLiteral("Copy share link"), this, [url] { QApplication::clipboard()->setText(url); });
    }
    menu.addAction(QStringLiteral("Show in folder"), this, [this, file] { show_in_folder(library_->path_of(file)); });
    menu.addSeparator();
    if (!clip->trashed_at) {
        menu.addAction(clip->favorite ? QStringLiteral("Remove from favorites") : QStringLiteral("Add to favorites"), this,
                       [this, file] {
                           if (const ClipEntry* c = library_->library().find(file)) {
                               library_->edit().set_favorite(file, !c->favorite);
                               library_->save();
                           }
                       });
        menu.addAction(QStringLiteral("Rename…"), this, [this, file] { rename_clip(file); });
        fill_collection_menu(menu.addMenu(QStringLiteral("Add to collection")), file);
        menu.addSeparator();
        menu.addAction(QStringLiteral("Move to Trash"), this, [this, file] { move_to_trash(file); });
    } else {
        menu.addAction(QStringLiteral("Restore"), this, [this, file] { restore(file); });
        menu.addAction(QStringLiteral("Move to the desktop's trash now"), this, [this, file] { delete_forever(file); });
    }
    menu.exec(at);
}

}  // namespace bt::linux_app
