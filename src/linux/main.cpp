// Backtrack for Linux (KDE Plasma, Arch, any X11 or Wayland desktop with xdg-desktop-portal).
//
//   backtrack                 starts recording (or shows the window of the running Backtrack)
//   backtrack --save          tells the running Backtrack to save a clip, for a desktop shortcut
//   backtrack --hidden        starts in the tray
//   backtrack --version
// Test options (CI): --save-after <s> (repeatable) and --exit-after <s>; the exit code is 1 when
// a save failed or a scheduled save did not finish. --write-icon <file.png> writes the logo.
// --check-media <clip> <trim.mp4> reads a clip the way the library and the clip page do (length,
// thumbnail, filmstrip, a trim from 1 s to 3.5 s, two seconds of playing) and says what it found.

#include "linux/Desktop.h"
#include "linux/Log.h"
#include "linux/MainWindow.h"
#include "linux/Media.h"
#include "linux/Player.h"
#include "linux/Portal.h"
#include "linux/X11Errors.h"

#include <QApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSystemTrayIcon>
#include <QTimer>

#include <X11/Xlib.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifndef BT_VERSION
#define BT_VERSION "dev"
#endif

namespace {

QString server_name() {
    return QStringLiteral("backtrack-%1").arg(getuid());
}

int send_to_running(const char* command) {
    QLocalSocket socket;
    socket.connectToServer(server_name());
    if (!socket.waitForConnected(2000)) {
        std::fprintf(stderr, "Backtrack is not running.\n");
        return 1;
    }
    socket.write(command);
    socket.write("\n");
    socket.waitForBytesWritten(2000);
    socket.disconnectFromServer();
    return 0;
}

// CI (packaging/linux/smoke-test.sh): the parts of the library and the clip page that need no
// window, on a clip the smoke test just saved. Exit code 1 when one of them fails.
int check_media(const std::string& clip, const std::string& trimmed) {
    using namespace bt::linux_app;
    using namespace std::chrono_literals;
    bool ok = true;
    const auto expect = [&ok](bool condition, const char* what) {
        std::printf("%s: %s\n", condition ? "ok  " : "FAIL", what);
        ok = ok && condition;
    };
    const MediaFacts facts = probe_media(clip);
    std::printf("probe: %.2f s, %dx%d\n", facts.duration_s, facts.width, facts.height);
    expect(facts.ok && facts.duration_s > 3.6, "probe finds the length and the picture size");
    const Still still = read_still(clip, 1.0 / 3.0, 384, 216);
    expect(still.width == 384 && still.height == 216 && still.pixels.size() == 384u * 216u * 4u, "thumbnail");
    const std::vector<Still> strip = read_filmstrip(clip, 12, 160, 90);
    expect(strip.size() == 12, "filmstrip of 12 pictures");
    const TrimResult trim = trim_clip(clip, trimmed, 1.0, 3.5);
    std::printf("trim: %s, starts at %.3f s, %.3f s long, %llu bytes\n", trim.ok ? "saved" : trim.error.c_str(),
                trim.start_s, trim.seconds, static_cast<unsigned long long>(trim.bytes));
    // The keyframe at or before 1 s: the one at 1 s (a keyframe every second), or the first one
    // when the timestamps put that one a hair after 1 s.
    expect(trim.ok && trim.start_s <= 1.0 && trim.start_s >= 0.0 && std::abs(trim.seconds - (3.5 - trim.start_s)) < 0.1,
           "lossless trim from the keyframe at or before 1 s up to 3.5 s");
    const TrimResult again = trim_clip(clip, trimmed, 1.0, 3.5);
    expect(!again.ok, "a trim never replaces an existing file");
    const MediaFacts cut = probe_media(trimmed);
    expect(cut.ok && cut.width == facts.width && cut.duration_s > 2.4 && cut.duration_s < 3.7 + 0.1, "the trimmed clip reads back");

    Player player(clip);
    for (int i = 0; i < 300 && !player.ready() && player.error().empty(); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    expect(player.ready(), "player opens the clip");
    player.set_view_size(640, 360);
    QImage frame;
    int pictures = 0;
    player.play();
    for (int i = 0; i < 200; ++i) {  // two seconds
        std::this_thread::sleep_for(10ms);
        if (player.take_frame(frame)) {
            ++pictures;
        }
    }
    std::printf("player: %d pictures in 2 s, at %.2f s\n", pictures, player.position());
    expect(pictures >= 40 && frame.width() == 640, "player shows the pictures in the view size");
    expect(player.position() > 1.5 && player.position() < 2.6, "player keeps time");
    player.pause();
    player.seek(0.5);
    bool shown = false;
    for (int i = 0; i < 200 && !shown; ++i) {
        std::this_thread::sleep_for(10ms);
        shown = player.take_frame(frame);
    }
    expect(shown && std::abs(player.position() - 0.5) < 0.001, "a seek while paused shows the picture there");
    player.seek(facts.duration_s);
    shown = false;
    for (int i = 0; i < 300 && !shown; ++i) {
        std::this_thread::sleep_for(10ms);
        shown = player.take_frame(frame);
    }
    expect(shown, "a seek to the very end shows the last picture");
    expect(player.error().empty(), "player reports no error");
    return ok ? 0 : 1;
}

int seconds_argument(int& i, int argc, char** argv) {
    if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a number of seconds\n", argv[i]);
        std::exit(2);
    }
    return std::atoi(argv[++i]);
}

}  // namespace

int main(int argc, char** argv) {
    using namespace bt::linux_app;

    bool save = false;
    bool hidden = false;
    std::vector<int> save_after;
    int exit_after = -1;
    const char* icon_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--version") {
            std::printf("Backtrack %s\n", BT_VERSION);
            return 0;
        } else if (arg == "--save") {
            save = true;
        } else if (arg == "--hidden") {
            hidden = true;
        } else if (arg == "--save-after") {
            save_after.push_back(seconds_argument(i, argc, argv));
        } else if (arg == "--exit-after") {
            exit_after = seconds_argument(i, argc, argv);
        } else if (arg == "--check-media" && i + 2 < argc) {
            return check_media(argv[i + 1], argv[i + 2]);
        } else if (arg == "--write-icon" && i + 1 < argc) {
            icon_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::printf("Usage: backtrack [--save] [--hidden] [--version]\n");
            return 0;
        }
    }

    if (save) {
        QCoreApplication app(argc, argv);
        return send_to_running("save");
    }
    if (icon_path != nullptr) {
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
        }
        QGuiApplication app(argc, argv);
        return logo_image(256).save(QString::fromLocal8Bit(icon_path), "PNG") ? 0 : 1;
    }

    // The AppImage carries only Qt's X11 platform plugin: on Wayland the window runs on XWayland
    // (always there on KDE), while recording and the hotkey still go through the portal.
    const bool appimage = !qEnvironmentVariableIsEmpty("APPIMAGE") || !qEnvironmentVariableIsEmpty("APPDIR");
    if (appimage && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "xcb");
    }
    // Capture and the hotkey talk to X11 from their own threads.
    XInitThreads();
    install_x11_error_handler();
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Backtrack"));
    QApplication::setApplicationVersion(QStringLiteral(BT_VERSION));
    QGuiApplication::setDesktopFileName(QStringLiteral("backtrack"));

    const AppPaths paths = app_paths();
    std::error_code ec;
    std::filesystem::create_directories(paths.state_dir, ec);
    log_open(paths.log_file);
    log_info(std::string("Backtrack ") + BT_VERSION + " starting (" + (wayland_session() ? "Wayland" : "X11") + ")");

    // One Backtrack per user: a second start shows the first one's window.
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server.listen(server_name())) {
        QLocalSocket probe;
        probe.connectToServer(server_name());
        if (probe.waitForConnected(1000)) {
            probe.write("show\n");
            probe.waitForBytesWritten(1000);
            log_info("Backtrack is already running: showing its window");
            return 0;
        }
        // Left over from a crash: nobody answers on it.
        QLocalServer::removeServer(server_name());
        if (!server.listen(server_name())) {
            log_warning("No single-instance socket: " + server.errorString().toStdString());
        }
    }

    if (wayland_session()) {
        register_portal_app_id(QStringLiteral("backtrack"));
    }
    install_desktop_entry();

    MainWindow window;
    QObject::connect(&server, &QLocalServer::newConnection, &window, [&server, &window] {
        while (QLocalSocket* client = server.nextPendingConnection()) {
            QObject::connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
            QObject::connect(client, &QLocalSocket::readyRead, &window, [client, &window] {
                while (client->canReadLine()) {
                    const QByteArray line = client->readLine().trimmed();
                    if (line == "save") {
                        window.save_clip();
                    } else if (line == "show") {
                        window.show_window();
                    }
                }
            });
        }
    });

    if (!hidden || !QSystemTrayIcon::isSystemTrayAvailable()) {
        window.show();
    }
    window.start_recording();

    for (const int seconds : save_after) {
        QTimer::singleShot(seconds * 1000, &window, [&window] { window.save_clip(); });
    }
    if (exit_after >= 0) {
        QTimer::singleShot(exit_after * 1000, &window, [&window] { window.quit(); });
    }
    const int code = app.exec();
    log_info("Backtrack quits");
    if (exit_after >= 0) {
        const int expected = static_cast<int>(save_after.size());
        if (window.failed_saves() > 0 || window.finished_saves() < expected) {
            std::fprintf(stderr, "Test run: %d of %d saves finished, %d failed\n", window.finished_saves(), expected,
                         window.failed_saves());
            return 1;
        }
    }
    return code;
}
