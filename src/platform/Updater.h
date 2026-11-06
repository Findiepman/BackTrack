#pragma once

// Checks GitHub for a newer Backtrack, downloads its installer in the background and runs it.
//
// Threads: one worker thread of the Updater does every network and disk step. It never touches
// the window; after each change of state() it posts the message given to notify() (wparam and
// lparam 0) and the UI thread then reads state(), which is guarded by a mutex. Everything public
// is called from the UI thread.
//
// Installing: the installer (Inno Setup, installer/backtrack.iss) runs silently. It waits until
// this process has ended (/WAITPID), so Backtrack quits the normal way and finishes the clip it is
// saving; if Backtrack is somehow still running, the installer closes it through the Restart
// Manager (WM_ENDSESSION with ENDSESSION_CLOSEAPP, handled in App.cpp). Only a copy that runs
// from its installed folder updates itself: a developer build reports the new version and the
// release page, nothing more.
//
// Files: %LOCALAPPDATA%\Backtrack\updates\ holds the downloaded installer, state.json (last check,
// the verified installer waiting) and install.log (the installer's own log of the last update).

#include "core/UpdateCheck.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace bt {

enum class UpdateStage {
    Idle,  // nothing checked this run
    Checking,
    UpToDate,  // also when the repository has no release yet
    Available,  // a newer version; can_install says whether it can be downloaded and installed here
    Downloading,
    Ready,  // the installer is downloaded and verified; it runs when Backtrack quits, or install_now()
    Failed,  // error says why in one line; the next check tries again
};

struct UpdateState {
    UpdateStage stage = UpdateStage::Idle;
    std::string version;  // Available, Downloading, Ready: "0.4.0"
    std::string notes;  // Available, Downloading, Ready: the release text from GitHub (Markdown)
    std::string page_url;  // the release's page on GitHub (always set: the releases page otherwise)
    double fraction = 0.0;  // Downloading: 0..1
    // Available, Ready: true when this copy can download and install it. False for a copy that
    // does not run from its installed folder (a developer build) or a release without an
    // installer: then show page_url instead.
    bool can_install = false;
    std::string error;  // Failed: one plain-English line
    std::int64_t last_check = 0;  // Unix seconds of the last check that reached GitHub or tried to; 0 never
};

struct UpdaterOptions {
    std::string current_version;  // BT_VERSION
    std::filesystem::path exe_path;  // this backtrack.exe
    std::filesystem::path folder;  // empty: %LOCALAPPDATA%\Backtrack\updates
    ReleaseSource source;
    // The update_check tool only: download even though this exe is not the installed Backtrack.
    bool download_anywhere = false;
};

class Updater {
public:
    explicit Updater(UpdaterOptions options);
    // Cancels a check or download in progress (the partial file is deleted) and waits for the
    // worker: well under a second normally, at most one network timeout (10 s) when the
    // connection has gone silent.
    ~Updater();
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    // The window to tell about changes of state(). Until it is set, changes are only kept.
    void notify(HWND window, UINT message);

    // Settings::auto_update. On: checks shortly after startup and then once a day (also across
    // restarts), and downloads a newer version straight away, also after a manual check().
    void set_automatic(bool on);

    UpdateState state() const;

    // "Check for updates". False when a check or download is already running.
    bool check();
    // Downloads the version check() found (stage Available with can_install). False otherwise.
    bool download();

    // Each install_* verifies the downloaded installer again (size and SHA-256), starts it and
    // returns true; the caller then ends Backtrack. False when there is nothing to install (or it
    // failed verification: state() then says so), and Backtrack just carries on.
    //
    // "Install now" button: Backtrack starts again after the update.
    bool install_now();
    // The automatic mode at "Quit" from the tray, after the window's finish(): installs without
    // starting Backtrack again. Nothing when the automatic mode is off.
    bool install_on_exit();
    // The automatic mode at the start of a run, before the window and the recorder start: installs
    // the update that a logoff or shutdown left waiting (an installer started while Windows ends
    // the session would be killed half way) and starts Backtrack again, hidden when `hidden`.
    // Nothing when the automatic mode is off.
    bool install_at_start(bool hidden);

    // True when this exe is the installed Backtrack (it may update itself).
    bool installed_copy() const { return installed_; }
    const std::filesystem::path& folder() const { return folder_; }

private:
    enum class Job { None, Check, AutoCheck, Download };
    enum class Relaunch { No, Shown, Hidden };

    void load();  // state.json and the folder, at the start
    void run();
    void do_check(bool automatic);
    void do_download(const Release& release);
    bool install(Relaunch relaunch, bool automatic);
    void start_worker();
    void stop_worker();

    // These expect mutex_ to be held.
    void set_state(UpdateState state);
    void save_record();
    void forget_ready();
    void clean_folder();
    UpdateState ready_state() const;
    std::optional<std::int64_t> seconds_until_auto_check() const;

    const UpdaterOptions options_;
    const Version current_;
    const std::filesystem::path folder_;
    const bool installed_;
    const std::chrono::steady_clock::time_point started_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    UpdateState state_;
    UpdaterRecord record_;
    std::optional<Release> available_;  // what the last check found
    HWND window_ = nullptr;
    UINT message_ = 0;
    bool automatic_ = false;
    bool schedule_changed_ = false;  // set_automatic() was called: the worker works out its wait again
    Job job_ = Job::None;
    bool busy_ = false;  // the worker is running a job
    bool stop_ = false;

    // Set by stop_worker(); the worker's network and disk loops stop at the next step.
    std::atomic<bool> cancelled_ = false;

    std::thread thread_;
};

// The SHA-256 of a file as 64 lower-case hex digits, or nullopt when it cannot be read.
std::optional<std::string> sha256_of_file(const std::filesystem::path& file);

}  // namespace bt
