#pragma once

// Colours, sizes and icons of the look from the mockup (backtrack.html, :root
// tokens). Sizes are DIPs. Names follow the mockup's CSS variables so the two can be compared.

#include "core/ThemeColors.h"
#include "ui/Graphics.h"

namespace bt::ui::theme {

// The colour tokens follow the chosen theme (Settings > Appearance): apply_theme() sets them, on
// the UI thread only, before the next paint. They start as Night shift.
inline Color kBg = Color::hex(0x0e1014);  // --bg
inline Color kSide = Color::hex(0x12151b);  // --side
inline Color kCard = Color::hex(0x171a21);  // --card
inline Color kInk = Color::hex(0xe9ecf2);  // --ink
inline Color kMute = Color::hex(0xa3aab8);  // --mute
inline Color kLine = Color::hex(0x2a2f3a);  // --line
inline Color kRaise = Color::hex(0x1f232c);  // --raise
inline Color kPop = Color::hex(0x1d212a);  // --pop
inline Color kAccent = Color::hex(0xffb547);  // --accent
inline Color kOnAccent = Color::hex(0x1a1200);  // --onacc
inline Color kDanger = Color::hex(0xff7a6e);  // --danger
inline Color kRec = Color::hex(0xff5d66);  // --rec
inline Color kWarn = Color::hex(0xf5b84a);  // --warn
inline Color kOk = Color::hex(0x6fdc9f);  // --ok
inline Color kOff = Color::hex(0x4a5160);  // --off
inline Color kSoft = kAccent.mix(kBg, 0.15f);  // --soft
inline Color kHover = kInk.with_alpha(0.08f);  // --hov
inline bool kLightTheme = false;  // Daylight: Windows draws the frame around the window light

inline Color to_color(Rgb c) {
    return Color::hex(c.value());
}

inline void apply_theme(ThemeId id) {
    const ThemeColors c = theme_colors(id);
    kBg = to_color(c.bg);
    kSide = to_color(c.side);
    kCard = to_color(c.card);
    kInk = to_color(c.ink);
    kMute = to_color(c.mute);
    kLine = to_color(c.line);
    kRaise = to_color(c.raise);
    kPop = to_color(c.pop);
    kAccent = to_color(c.accent);
    kOnAccent = to_color(c.on_accent);
    kDanger = to_color(c.danger);
    kRec = to_color(c.rec);
    kWarn = to_color(c.warn);
    kOk = to_color(c.ok);
    kOff = to_color(c.off);
    kSoft = kAccent.mix(kBg, 0.15f);
    kHover = kInk.with_alpha(0.08f);
    kLightTheme = c.light;
}

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
// .st-grid: "gap: 30px 28px; padding: 26px var(--pad) 28px", compact "gap: 20px 18px", --pad 28px.
// Cards never get narrower than kCardMin; a narrow window shows fewer per row.
inline constexpr float kCardMin = 200;
inline constexpr float kGridGapX = 28;
inline constexpr float kGridGapY = 30;
inline constexpr float kGridGapCompactX = 18;
inline constexpr float kGridGapCompactY = 20;
inline constexpr float kGridPadX = 40;
inline constexpr float kGridPadCompactX = 28;
inline constexpr float kGridPadY = 26;
inline constexpr float kGridPadBottom = 28;
inline constexpr float kCardRadius = 10;  // .st-card
inline constexpr float kCardInfo = 52;  // .st-info: 8 + 18 title + 3 + 16 meta + 7
inline constexpr float kTopStrip = 46;  // .st-top
inline constexpr float kHeaderBar = 48;  // .st-bar
inline constexpr float kSavedBar = 74;  // .bar min-height
inline constexpr float kCaptionButton = 46;  // .tb-w button
// The pages (Record, Settings, clip page) use the mockup's page tokens.
inline constexpr float kPageControl = 38;  // --h on the pages
inline constexpr float kPageRadius = 10;  // --r on the pages
inline constexpr float kPageTop = 26;  // #page padding-top
inline constexpr float kPanelRadius = 16;  // .card2, .panel2, .sg

// Glyphs of "Segoe Fluent Icons" / "Segoe MDL2 Assets".
namespace glyph {
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
inline constexpr wchar_t kChevronLeft = 0xE76B;  // ChevronLeft
inline constexpr wchar_t kChevronRight = 0xE76C;  // ChevronRight
inline constexpr wchar_t kVolume = 0xE767;  // Volume
inline constexpr wchar_t kMute = 0xE74F;  // Mute
inline constexpr wchar_t kExpand = 0xE740;  // FullScreen
inline constexpr wchar_t kCollapse = 0xE73F;  // BackToWindow
inline constexpr wchar_t kCut = 0xE8C6;  // Cut
inline constexpr wchar_t kDownload = 0xE896;  // Download
inline constexpr wchar_t kTag = 0xE8EC;  // Tag
inline constexpr wchar_t kPencil = 0xE70F;  // Edit
inline constexpr wchar_t kInfo = 0xE946;  // Info
inline constexpr wchar_t kCamera = 0xE722;  // Camera
inline constexpr wchar_t kBolt = 0xE945;  // LightningBolt
inline constexpr wchar_t kMicrophone = 0xE720;  // Microphone
inline constexpr wchar_t kHeadphones = 0xE7F6;  // Headphone
inline constexpr wchar_t kKeyboard = 0xE765;  // KeyboardClassic
inline constexpr wchar_t kDrive = 0xEDA2;  // HardDrive
inline constexpr wchar_t kBell = 0xEA8F;  // Ringer
inline constexpr wchar_t kPalette = 0xE790;  // Color
inline constexpr wchar_t kFilm = 0xE8B2;  // SlideShow / film strip
inline constexpr wchar_t kRestoreDefaults = 0xE777;  // UpdateRestore
inline constexpr wchar_t kCopy = 0xE8C8;  // Copy
inline constexpr wchar_t kDisplay = 0xE7F4;  // TVMonitor
inline constexpr wchar_t kSpeaker = 0xE7F5;  // Speakers
}  // namespace glyph

}  // namespace bt::ui::theme
