#pragma once

// The library on Linux: the clips of one view (All clips, Favorites, Recent, Shared, Trash or a
// collection) as cards with their thumbnail, with search, a tag filter and the sort order, and a
// right-click menu with what the Windows library offers.

#include "core/Library.h"

#include <QColor>
#include <QIcon>
#include <QWidget>

#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QStackedLayout;

namespace bt::linux_app {

class ClipLibrary;
class ClipGrid;

// The colours collections can have, the same six as on Windows (Collection::color is an index).
QColor collection_color(int index);
inline constexpr int kCollectionColorCount = 6;
// The dot shown next to a collection's name in menus and the sidebar.
QIcon collection_icon(int color);

class LibraryPage final : public QWidget {
    Q_OBJECT
public:
    LibraryPage(ClipLibrary* library, QWidget* parent = nullptr);

    void set_view(LibraryView view, const std::string& collection = {});
    LibraryView view() const { return view_; }
    const std::string& view_collection() const { return collection_; }
    // The files in the order the grid shows them, for Previous and Next on the clip page.
    std::vector<std::string> order() const;
    void refresh();

    // The menus the clip page shares.
    void fill_collection_menu(QMenu* menu, const std::string& file);
    void rename_clip(const std::string& file);
    void move_to_trash(const std::string& file);
    void restore(const std::string& file);
    bool delete_forever(const std::string& file);

signals:
    void open_clip(const QString& file);

private:
    LibraryQuery query() const;
    void clip_menu(const std::string& file, const QPoint& at);
    void refresh_tags();
    QString view_title() const;

    ClipLibrary* library_;
    LibraryView view_ = LibraryView::All;
    std::string collection_;
    std::vector<std::string> shown_tags_;  // the tag filter is only rebuilt when these change

    QLabel* title_ = nullptr;
    QLabel* count_ = nullptr;
    QLineEdit* search_ = nullptr;
    QComboBox* tag_ = nullptr;
    QComboBox* sort_ = nullptr;
    ClipGrid* grid_ = nullptr;
    QLabel* empty_ = nullptr;
    QStackedLayout* stack_ = nullptr;
};

}  // namespace bt::linux_app
