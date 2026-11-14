#include "linux/Portal.h"

#include "core/LinuxDesktop.h"
#include "linux/Log.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusUnixFileDescriptor>
#include <QList>

#include <fcntl.h>

namespace bt::linux_app {

struct PortalShortcut {
    QString id;
    QVariantMap properties;
};

}  // namespace bt::linux_app

Q_DECLARE_METATYPE(bt::linux_app::PortalShortcut)

namespace bt::linux_app {

QDBusArgument& operator<<(QDBusArgument& argument, const PortalShortcut& shortcut) {
    argument.beginStructure();
    argument << shortcut.id << shortcut.properties;
    argument.endStructure();
    return argument;
}

const QDBusArgument& operator>>(const QDBusArgument& argument, PortalShortcut& shortcut) {
    argument.beginStructure();
    argument >> shortcut.id >> shortcut.properties;
    argument.endStructure();
    return argument;
}

namespace {

const QString kService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString kPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString kRequest = QStringLiteral("org.freedesktop.portal.Request");
const QString kSession = QStringLiteral("org.freedesktop.portal.Session");
const QString kScreenCast = QStringLiteral("org.freedesktop.portal.ScreenCast");
const QString kShortcuts = QStringLiteral("org.freedesktop.portal.GlobalShortcuts");

// ScreenCast values from the portal specification.
constexpr unsigned kSourceMonitor = 1;
constexpr unsigned kCursorHidden = 1;
constexpr unsigned kCursorEmbedded = 2;
constexpr unsigned kPersistUntilRevoked = 2;

QString variant_path(const QVariant& value) {
    if (value.canConvert<QDBusObjectPath>()) {
        const QString path = value.value<QDBusObjectPath>().path();
        if (!path.isEmpty()) {
            return path;
        }
    }
    return value.toString();
}

QString response_text(unsigned response) {
    return response == 1 ? QStringLiteral("cancelled") : QStringLiteral("refused by the desktop");
}

// Each request gets its own token; the reply's path is computed from it before the call, so the
// Response signal cannot arrive before we listen for it.
QString next_token(int& counter) {
    return QStringLiteral("backtrack%1").arg(++counter);
}

QString request_path_for(const QString& token) {
    const std::string unique = QDBusConnection::sessionBus().baseService().toStdString();
    return QString::fromStdString(portal_request_path(unique, token.toStdString()));
}

unsigned available_cursor_modes() {
    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                                                          QStringLiteral("Get"));
    message << kScreenCast << QStringLiteral("AvailableCursorModes");
    const QDBusMessage reply = QDBusConnection::sessionBus().call(message);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        return 0;
    }
    return reply.arguments().first().value<QDBusVariant>().variant().toUInt();
}

}  // namespace

void register_portal_app_id(const QString& app_id) {
    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, QStringLiteral("org.freedesktop.host.portal.Registry"),
                                                          QStringLiteral("Register"));
    message << app_id << QVariantMap{};
    const QDBusMessage reply = QDBusConnection::sessionBus().call(message, QDBus::Block, 2000);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        log_info("Portal app id not registered (older portal): " + reply.errorMessage().toStdString());
    }
}

// ---- ScreenCast ----

ScreenCastPortal::ScreenCastPortal(QObject* parent) : QObject(parent) {}

ScreenCastPortal::~ScreenCastPortal() {
    close();
}

void ScreenCastPortal::start(bool cursor, const QString& restore_token) {
    close();
    cursor_ = cursor;
    restore_token_ = restore_token;
    const QString handle = next_token(token_counter_);
    const QVariantMap options{{QStringLiteral("handle_token"), handle},
                              {QStringLiteral("session_handle_token"), next_token(token_counter_)}};
    request_path_ = request_path_for(handle);
    call(QStringLiteral("CreateSession"), {options}, Step::CreateSession);
}

bool ScreenCastPortal::call(const QString& method, const QVariantList& arguments, Step next) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        fail(QStringLiteral("No D-Bus session: screen sharing needs a desktop session"));
        return false;
    }
    bus.connect(kService, request_path_, kRequest, QStringLiteral("Response"), this,
                SLOT(on_response(uint, QVariantMap)));
    step_ = next;
    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kScreenCast, method);
    message.setArguments(arguments);
    const QDBusMessage reply = bus.call(message);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        bus.disconnect(kService, request_path_, kRequest, QStringLiteral("Response"), this,
                       SLOT(on_response(uint, QVariantMap)));
        fail(QStringLiteral("%1 failed: %2").arg(method, reply.errorMessage()));
        return false;
    }
    return true;
}

void ScreenCastPortal::on_response(unsigned response, const QVariantMap& results) {
    QDBusConnection::sessionBus().disconnect(kService, request_path_, kRequest, QStringLiteral("Response"), this,
                                             SLOT(on_response(uint, QVariantMap)));
    if (response != 0) {
        fail(QStringLiteral("Screen sharing was %1").arg(response_text(response)));
        return;
    }
    switch (step_) {
    case Step::CreateSession:
        session_ = variant_path(results.value(QStringLiteral("session_handle")));
        if (session_.isEmpty()) {
            fail(QStringLiteral("The portal gave no session"));
            return;
        }
        QDBusConnection::sessionBus().connect(kService, session_, kSession, QStringLiteral("Closed"), this,
                                              SLOT(on_session_closed()));
        select_sources();
        break;
    case Step::SelectSources:
        start_stream();
        break;
    case Step::Start: {
        step_ = Step::Idle;
        unsigned node = 0;
        bool have_node = false;
        const QVariant streams = results.value(QStringLiteral("streams"));
        if (streams.canConvert<QDBusArgument>()) {
            const QDBusArgument argument = streams.value<QDBusArgument>();
            argument.beginArray();
            while (!argument.atEnd()) {
                unsigned id = 0;
                QVariantMap properties;
                argument.beginStructure();
                argument >> id >> properties;
                argument.endStructure();
                if (!have_node) {
                    node = id;
                    have_node = true;
                }
            }
            argument.endArray();
        }
        if (!have_node) {
            fail(QStringLiteral("No screen was chosen"));
            return;
        }
        const QString token = results.value(QStringLiteral("restore_token")).toString();

        QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kScreenCast, QStringLiteral("OpenPipeWireRemote"));
        message << QVariant::fromValue(QDBusObjectPath(session_)) << QVariantMap{};
        const QDBusMessage reply = QDBusConnection::sessionBus().call(message);
        if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
            fail(QStringLiteral("Cannot open the screen stream: %1").arg(reply.errorMessage()));
            return;
        }
        const auto descriptor = reply.arguments().first().value<QDBusUnixFileDescriptor>();
        // QDBusUnixFileDescriptor closes its own copy; the capture gets a duplicate it owns.
        const int fd = descriptor.isValid() ? fcntl(descriptor.fileDescriptor(), F_DUPFD_CLOEXEC, 3) : -1;
        if (fd < 0) {
            fail(QStringLiteral("The portal gave no screen stream"));
            return;
        }
        log_info("Screen sharing started (PipeWire node " + std::to_string(node) + ")");
        emit ready(fd, node, token);
        break;
    }
    case Step::Idle:
        break;
    }
}

void ScreenCastPortal::select_sources() {
    QVariantMap options{{QStringLiteral("handle_token"), next_token(token_counter_)},
                        {QStringLiteral("types"), QVariant::fromValue(kSourceMonitor)},
                        {QStringLiteral("multiple"), false},
                        {QStringLiteral("persist_mode"), QVariant::fromValue(kPersistUntilRevoked)}};
    const unsigned modes = available_cursor_modes();
    const unsigned wanted = cursor_ ? kCursorEmbedded : kCursorHidden;
    if ((modes & wanted) != 0) {
        options.insert(QStringLiteral("cursor_mode"), QVariant::fromValue(wanted));
    }
    if (!restore_token_.isEmpty()) {
        options.insert(QStringLiteral("restore_token"), restore_token_);
    }
    request_path_ = request_path_for(options.value(QStringLiteral("handle_token")).toString());
    call(QStringLiteral("SelectSources"), {QVariant::fromValue(QDBusObjectPath(session_)), options}, Step::SelectSources);
}

void ScreenCastPortal::start_stream() {
    const QVariantMap options{{QStringLiteral("handle_token"), next_token(token_counter_)}};
    request_path_ = request_path_for(options.value(QStringLiteral("handle_token")).toString());
    call(QStringLiteral("Start"), {QVariant::fromValue(QDBusObjectPath(session_)), QString(), options}, Step::Start);
}

void ScreenCastPortal::on_session_closed() {
    log_info("Screen sharing session closed by the desktop");
    session_.clear();
    step_ = Step::Idle;
    emit closed();
}

void ScreenCastPortal::fail(const QString& message) {
    close();
    log_warning("Screen sharing: " + message.toStdString());
    emit failed(message);
}

void ScreenCastPortal::close() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (step_ != Step::Idle) {
        // A handshake still waiting for its answer: that answer must not reach the next one.
        bus.disconnect(kService, request_path_, kRequest, QStringLiteral("Response"), this,
                       SLOT(on_response(uint, QVariantMap)));
        step_ = Step::Idle;
    }
    if (session_.isEmpty()) {
        return;
    }
    bus.disconnect(kService, session_, kSession, QStringLiteral("Closed"), this, SLOT(on_session_closed()));
    bus.call(QDBusMessage::createMethodCall(kService, session_, kSession, QStringLiteral("Close")), QDBus::NoBlock);
    session_.clear();
}

// ---- GlobalShortcuts ----

GlobalShortcutsPortal::GlobalShortcutsPortal(QObject* parent) : QObject(parent) {
    qDBusRegisterMetaType<PortalShortcut>();
    qDBusRegisterMetaType<QList<PortalShortcut>>();
}

void GlobalShortcutsPortal::bind(const QString& trigger) {
    trigger_ = trigger;
    if (!session_.isEmpty()) {
        // A session exists: bind again (a changed key in the settings).
        const QVariantMap options{{QStringLiteral("handle_token"), next_token(token_counter_)}};
        request_path_ = request_path_for(options.value(QStringLiteral("handle_token")).toString());
        const QList<PortalShortcut> shortcuts{
            {QStringLiteral("save"), {{QStringLiteral("description"), QStringLiteral("Save a clip")},
                                      {QStringLiteral("preferred_trigger"), trigger_}}}};
        call(QStringLiteral("BindShortcuts"),
             {QVariant::fromValue(QDBusObjectPath(session_)), QVariant::fromValue(shortcuts), QString(), options}, Step::Bind);
        return;
    }
    const QVariantMap options{{QStringLiteral("handle_token"), next_token(token_counter_)},
                              {QStringLiteral("session_handle_token"), next_token(token_counter_)}};
    request_path_ = request_path_for(options.value(QStringLiteral("handle_token")).toString());
    call(QStringLiteral("CreateSession"), {options}, Step::CreateSession);
}

bool GlobalShortcutsPortal::call(const QString& method, const QVariantList& arguments, Step next) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        emit failed(QStringLiteral("No D-Bus session"));
        return false;
    }
    bus.connect(kService, request_path_, kRequest, QStringLiteral("Response"), this,
                SLOT(on_response(uint, QVariantMap)));
    step_ = next;
    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kShortcuts, method);
    message.setArguments(arguments);
    const QDBusMessage reply = bus.call(message);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        bus.disconnect(kService, request_path_, kRequest, QStringLiteral("Response"), this,
                       SLOT(on_response(uint, QVariantMap)));
        step_ = Step::Idle;
        emit failed(QStringLiteral("%1: %2").arg(method, reply.errorMessage()));
        return false;
    }
    return true;
}

void GlobalShortcutsPortal::on_response(unsigned response, const QVariantMap& results) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.disconnect(kService, request_path_, kRequest, QStringLiteral("Response"), this, SLOT(on_response(uint, QVariantMap)));
    const Step step = step_;
    step_ = Step::Idle;
    if (response != 0) {
        emit failed(QStringLiteral("The shortcut was %1").arg(response_text(response)));
        return;
    }
    if (step == Step::CreateSession) {
        session_ = variant_path(results.value(QStringLiteral("session_handle")));
        if (session_.isEmpty()) {
            emit failed(QStringLiteral("The portal gave no session"));
            return;
        }
        bus.connect(kService, kPath, kShortcuts, QStringLiteral("Activated"), this,
                    SLOT(on_activated(QDBusObjectPath, QString, qulonglong, QVariantMap)));
        bind(trigger_);
    } else if (step == Step::Bind) {
        log_info("Global shortcut registered with the desktop: " + trigger_.toStdString());
        emit bound();
    }
}

void GlobalShortcutsPortal::on_activated(const QDBusObjectPath& session, const QString& id, qulonglong, const QVariantMap&) {
    if (session.path() == session_) {
        emit activated(id);
    }
}

}  // namespace bt::linux_app
