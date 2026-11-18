#pragma once

// The xdg-desktop-portal calls the Wayland version needs (KDE Plasma's portal backend answers
// them; on X11 they are not used). Every portal method answers later through a Response signal on
// a request path computed in advance (core/LinuxDesktop.h), so each class is a small state machine
// on the Qt event loop.

#include <QDBusObjectPath>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <cstdint>

namespace bt::linux_app {

// Tells the portal which app this is (xdg-desktop-portal 1.19+), so KDE shows "Backtrack" in its
// dialogs and remembers permissions per app. Must come before any other portal call; harmless
// when the portal is older.
void register_portal_app_id(const QString& app_id);

// ScreenCast: asks once which screen to share (KDE's dialog), then hands over the PipeWire stream.
// With the restore token from the last run the dialog does not come again.
class ScreenCastPortal final : public QObject {
    Q_OBJECT
public:
    explicit ScreenCastPortal(QObject* parent = nullptr);
    ~ScreenCastPortal() override;

    // Starts the handshake; ends in exactly one of ready() or failed().
    void start(bool cursor, const QString& restore_token);
    // Closes the portal session (KDE's "sharing" indicator goes away) and drops a handshake that
    // is still waiting for an answer.
    void close();
    // A handshake is under way (KDE's dialog may be open).
    bool busy() const { return step_ != Step::Idle; }

signals:
    // fd: a PipeWire remote, owned by the receiver. restore_token: store it for the next start
    // (each token works once); empty when the portal gave none.
    void ready(int fd, unsigned node, QString restore_token);
    void failed(QString message);
    // The user stopped sharing (from KDE's tray) or the session ended otherwise.
    void closed();

private slots:
    void on_response(unsigned response, const QVariantMap& results);
    void on_session_closed();

private:
    enum class Step { Idle, CreateSession, SelectSources, Start };
    bool call(const QString& method, const QVariantList& arguments, Step next);
    void fail(const QString& message);
    void select_sources();
    void start_stream();

    Step step_ = Step::Idle;
    bool cursor_ = true;
    QString restore_token_;
    QString request_path_;
    QString session_;
};

// GlobalShortcuts (Plasma 6): registers the save hotkey with the desktop. KDE asks once to confirm
// the key; activated() comes each time it is pressed, also while a game has the focus.
class GlobalShortcutsPortal final : public QObject {
    Q_OBJECT
public:
    explicit GlobalShortcutsPortal(QObject* parent = nullptr);

    // trigger: portal_shortcut_trigger() of the save hotkey. Ends in bound() or failed().
    void bind(const QString& trigger);

signals:
    void bound();
    void failed(QString message);
    void activated(QString id);

private slots:
    void on_response(unsigned response, const QVariantMap& results);
    void on_activated(const QDBusObjectPath& session, const QString& id, qulonglong timestamp, const QVariantMap& options);

private:
    enum class Step { Idle, CreateSession, Bind };
    bool call(const QString& method, const QVariantList& arguments, Step next);

    Step step_ = Step::Idle;
    QString trigger_;
    QString request_path_;
    QString session_;
};

}  // namespace bt::linux_app
