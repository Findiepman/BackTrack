#pragma once

// Colours, sizes and icons of the "Night shift" look from the mockup (backtrack.html, :root
// tokens). Sizes are DIPs. Names follow the mockup's CSS variables so the two can be compared.

#include "ui/Graphics.h"

namespace bt::ui::theme {

inline constexpr Color kBg = Color::hex(0x0e1014);  // --bg
inline constexpr Color kSide = Color::hex(0x12151b);  // --side
inline constexpr Color kCard = Color::hex(0x171a21);  // --card
inline constexpr Color kInk = Color::hex(0xe9ecf2);  // --ink
inline constexpr Color kMute = Color::hex(0xa3aab8);  // --mute
inline constexpr Color kLine = Color::hex(0x2a2f3a);  // --line
inline constexpr Color kRaise = Color::hex(0x1f232c);  // --raise
inline constexpr Color kPop = Color::hex(0x1d212a);  // --pop
inline constexpr Color kAccent = Color::hex(0xffb547);  // --accent
inline constexpr Color kOnAccent = Color::hex(0x1a1200);  // --onacc
inline constexpr Color kDanger = Color::hex(0xff7a6e);  // --danger
inline constexpr Color kRec = Color::hex(0xff5d66);  // --rec
inline constexpr Color kWarn = Color::hex(0xf5b84a);  // --warn
inline constexpr Color kOk = Color::hex(0x6fdc9f);  // --ok
inline constexpr Color kOff = Color::hex(0x4a5160);  // --off
inline constexpr Color kSoft = kAccent.mix(kBg, 0.15f);  // --soft
inline constexpr Color kHover = kInk.with_alpha(0.08f);  // --hov
inline constexpr Color kStar = Color::hex(0xffd466);  // .st-tb.fav

// Collection dots, by Collection::color.
inline constexpr Color kCollectionColors[] = {
    Color::hex(0x3ec9a7), Color::hex(0xf0904b), Color::hex(0x9d7bff),
    Color::hex(0x4f8cff), Color::hex(0xff6f91), Color::hex(0xe8c547),
};
inline constexpr int kCollectionColorCount = static_cast<int>(sizeof(kCollectionColors) / sizeof(kCollectionColors[0]));

// Gradient pairs for a card whose thumbnail is not there yet (same family as the mockup's).
struct Pair {
    Color top;
    Color bottom;
};
inline constexpr Pair kPlaceholders[] = {
    {Color::hex(0x2c4a8a), Color::hex(0x141c33)}, {Color::hex(0x8a5a22), Color::hex(0x2a1b0c)},
    {Color::hex(0x8a3a2a), Color::hex(0x2b130e)}, {Color::hex(0x4b3b8a), Color::hex(0x191433)},
    {Color::hex(0x2a6a44), Color::hex(0x0f2618)}, {Color::hex(0x236a66), Color::hex(0x0c2523)},
};
inline constexpr int kPlaceholderCount = static_cast<int>(sizeof(kPlaceholders) / sizeof(kPlaceholders[0]));

// Layout (DIPs).
inline constexpr float kTitleBar = 40;  // .app grid-template-rows
inline constexpr float kSidebar = 236;  // .shell grid-template-columns
inline constexpr float kNavHeight = 32;  // .nav
inline constexpr float kRadius = 8;  // --r
inline constexpr float kControl = 32;  // --h
inline constexpr float kPad = 40;  // --pad
inline constexpr float kCardMin = 228;  // --gm
inline constexpr float kGridGap = 12;  // .st-grid gap
inline constexpr float kGridPadX = 14;
inline constexpr float kGridPadY = 12;
inline constexpr float kCardRadius = 10;  // .st-card
inline constexpr float kCardInfo = 52;  // .st-info: 8 + 18 title + 3 + 16 meta + 7
inline constexpr float kTopStrip = 46;  // .st-top
inline constexpr float kHeaderBar = 48;  // .st-bar
inline constexpr float kSavedBar = 74;  // .bar min-height
inline constexpr float kCaptionButton = 46;  // .tb-w button

// Glyphs of "Segoe Fluent Icons" / "Segoe MDL2 Assets".
namespace glyph {
inline constexpr wchar_t kAppMark = 0xE7A7;  // Undo: a backwards arrow
inline constexpr wchar_t kAllClips = 0xE80A;  // Tiles
inline constexpr wchar_t kFavorite = 0xE734;  // FavoriteStar
inline constexpr wchar_t kFavoriteFill = 0xE735;  // FavoriteStarFill
inline constexpr wchar_t kRecent = 0xE823;  // Recent
inline constexpr wchar_t kShare = 0xE72D;  // Share
inline constexpr wchar_t kTrash = 0xE74D;  // Delete
inline constexpr wchar_t kRecord = 0xE7C8;  // Record
inline constexpr wchar_t kUpload = 0xE898;  // Upload
inline constexpr wchar_t kSettings = 0xE713;  // Setting
inline constexpr wchar_t kAdd = 0xE710;  // Add
inline constexpr wchar_t kSearch = 0xE721;  // Search
inline constexpr wchar_t kPlay = 0xE768;  // Play
inline constexpr wchar_t kPause = 0xE769;  // Pause
inline constexpr wchar_t kFolder = 0xE838;  // FolderOpen
inline constexpr wchar_t kClose = 0xE711;  // Cancel
inline constexpr wchar_t kCheck = 0xE73E;  // CheckMark
inline constexpr wchar_t kSort = 0xE8CB;  // Sort
inline constexpr wchar_t kChevronDown = 0xE70D;  // ChevronDown
inline constexpr wchar_t kMinimize = 0xE921;  // ChromeMinimize
inline constexpr wchar_t kMaximize = 0xE922;  // ChromeMaximize
inline constexpr wchar_t kRestore = 0xE923;  // ChromeRestore
inline constexpr wchar_t kChromeClose = 0xE8BB;  // ChromeClose
inline constexpr wchar_t kVideo = 0xE714;  // Video
}  // namespace glyph

}  // namespace bt::ui::theme
