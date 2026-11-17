#include "linux/Desktop.h"

#include "core/LinuxDesktop.h"
#include "linux/Log.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDesktopServices>
#include <QFileInfo>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>
#include <QUrl>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace bt::linux_app {

namespace {

std::optional<std::string> env(std::string_view name) {
    const char* value = std::getenv(std::string(name).c_str());
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

std::string home() {
    return env("HOME").value_or("/tmp");
}

std::string xdg(const char* variable, const char* fallback) {
    if (const auto value = env(variable); value && value->starts_with('/')) {
        return *value;
    }
    return home() + fallback;
}

// Quotes a path for the Exec key of a desktop entry (Desktop Entry Specification, "Exec key").
std::string exec_quoted(const std::string& path) {
    std::string out = "\"";
    for (const char c : path) {
        if (c == '"' || c == '`' || c == '$' || c == '\\') {
            out += '\\';
        }
        if (c == '%') {
            out += '%';  // field codes start with %; a literal one is written twice
        }
        out += c;
    }
    out += '"';
    // The entry's own escape level: a backslash in the value is written twice.
    std::string escaped;
    for (const char c : out) {
        escaped += c;
        if (c == '\\') {
            escaped += '\\';
        }
    }
    return escaped;
}

}  // namespace

AppPaths app_paths() {
    AppPaths paths;
    paths.config_dir = xdg("XDG_CONFIG_HOME", "/.config") + "/Backtrack";
    paths.state_dir = xdg("XDG_STATE_HOME", "/.local/state") + "/Backtrack";
    paths.data_dir = xdg("XDG_DATA_HOME", "/.local/share") + "/Backtrack";
    paths.cache_dir = xdg("XDG_CACHE_HOME", "/.cache") + "/Backtrack";
    paths.ini_file = paths.config_dir + "/backtrack.ini";
    paths.log_file = paths.state_dir + "/backtrack.log";
    paths.token_file = paths.state_dir + "/screencast-token";
    return paths;
}

std::string clips_folder(const std::string& setting) {
    const std::string user_dirs = read_text_file(xdg("XDG_CONFIG_HOME", "/.config") + "/user-dirs.dirs");
    return resolve_output_dir(setting, env, user_dirs);
}

bool wayland_session() {
    return env("WAYLAND_DISPLAY").has_value() || env("XDG_SESSION_TYPE").value_or("") == "wayland";
}

QImage logo_image(int size) {
    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(size / 256.0, size / 256.0);  // the SVG's own coordinates from here on

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0x0e, 0x10, 0x14));
    painter.drawRoundedRect(QRectF(0, 0, 256, 256), 58, 58);

    // The ring: the circle around (128,128) with radius 78, clockwise from 15 to 75 degrees (300 degrees).
    const QColor amber(0xff, 0xb5, 0x47);
    QPainterPath ring;
    ring.arcMoveTo(QRectF(50, 50, 156, 156), 15.0);
    ring.arcTo(QRectF(50, 50, 156, 156), 15.0, -300.0);
    painter.setPen(QPen(amber, 18, Qt::SolidLine, Qt::RoundCap));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(ring);

    painter.setPen(QPen(amber, 8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(amber);
    painter.drawPolygon(QPolygonF({QPointF(226.5, 101.6), QPointF(196.1, 80.8), QPointF(180.1, 114)}));

    const QColor light(0xf4, 0xef, 0xe6);
    painter.setPen(QPen(light, 6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(light);
    painter.drawPolygon(QPolygonF({QPointF(134, 106), QPointF(134, 150), QPointF(102, 128)}));
    painter.drawPolygon(QPolygonF({QPointF(168, 106), QPointF(168, 150), QPointF(136, 128)}));
    return image;
}

QIcon logo_icon() {
    QIcon icon;
    for (const int size : {16, 22, 24, 32, 48, 64, 128, 256}) {
        icon.addPixmap(QPixmap::fromImage(logo_image(size)));
    }
    return icon;
}

void install_desktop_entry() {
    const auto appimage = env("APPIMAGE");
    if (!appimage) {
        return;  // a distribution package installs its own entry
    }
    const std::string data = xdg("XDG_DATA_HOME", "/.local/share");
    std::error_code ec;
    const std::string icon_dir = data + "/icons/hicolor/256x256/apps";
    std::filesystem::create_directories(icon_dir, ec);
    const std::string icon = icon_dir + "/backtrack.png";
    if (!std::filesystem::exists(icon, ec)) {
        logo_image(256).save(QString::fromStdString(icon), "PNG");
    }

    const std::string exec = exec_quoted(*appimage);
    std::ostringstream entry;
    entry << "[Desktop Entry]\n"
          << "Type=Application\n"
          << "Name=Backtrack\n"
          << "GenericName=Instant replay recorder\n"
          << "Comment=Keeps the last seconds of your screen and sound and saves them with a hotkey\n"
          << "Exec=" << exec << "\n"
          << "Icon=backtrack\n"
          << "Terminal=false\n"
          << "Categories=AudioVideo;Video;Recorder;\n"
          << "Keywords=clip;replay;record;screen;\n"
          << "StartupWMClass=backtrack\n"
          << "Actions=save;\n"
          << "\n"
          << "[Desktop Action save]\n"
          << "Name=Save clip\n"
          << "Exec=" << exec << " --save\n";
    const std::string applications = data + "/applications";
    std::filesystem::create_directories(applications, ec);
    const std::string path = applications + "/backtrack.desktop";
    if (read_text_file(path) != entry.str()) {
        if (write_text_file(path, entry.str())) {
            log_info("Menu entry written: " + path);
        }
    }
}

void show_in_folder(const std::string& path) {
    const QString file = QString::fromStdString(path);
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("/org/freedesktop/FileManager1"),
        QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("ShowItems"));
    call.setArguments({QStringList{QUrl::fromLocalFile(file).toString()}, QString()});
    // Answered at once by the file manager (or by D-Bus when none is installed): short wait.
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 3000);
    if (reply.type() != QDBusMessage::ReplyMessage) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(file).absolutePath()));
    }
}

std::string read_text_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool write_text_file(const std::string& path, const std::string& text) {
    // Written next to the file and renamed over it, so a crash never leaves half a settings file.
    const std::string temp = path + ".new";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text;
        out.flush();
        if (!out) {
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    return !ec;
}

}  // namespace bt::linux_app
