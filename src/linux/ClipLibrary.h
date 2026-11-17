#pragma once

// The clip library on Linux: core/Library kept in ~/.local/share/Backtrack/library.json (the same
// file format as on Windows), in line with the clips folder, with each clip's length, size and
// thumbnail read on a worker thread. The library page and the clip page both work on this one.
//
// GUI thread only, except the worker inside.

#include "core/Library.h"

#include <QImage>
#include <QObject>
#include <QString>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

class QFileSystemWatcher;
class QTimer;

namespace bt::linux_app {

class ClipLibrary final : public QObject {
    Q_OBJECT
public:
    explicit ClipLibrary(QObject* parent = nullptr);
    ~ClipLibrary() override;

    // The clips folder (at start and when the setting changes): read again at once.
    void set_folder(const std::string& folder);
    const std::string& folder() const { return folder_; }
    std::string path_of(const std::string& file) const;

    const Library& library() const { return library_; }
    // For edits: change it, then call save() (which also tells the pages).
    Library& edit() { return library_; }
    void save();
    // false: library.json is there but could not be read, so changes are not saved this run.
    bool writable() const { return writable_; }

    // Lists the clips folder and brings the library in line with it.
    void scan();
    // A clip Backtrack just saved.
    void add_saved(const std::string& path, double seconds, const std::string& source);

    // The card picture; a null image until it has been read (thumbnail_ready follows).
    QImage thumbnail(const ClipEntry& clip, bool on_screen);

    // Out of the Backtrack Trash for good: into the desktop's own trash (or deleted when the
    // file system has none). False when the file could not be moved.
    bool delete_forever(const std::string& file);
    // Clips that were in the Trash longer than kTrashDays go to the desktop's trash.
    void run_cleanup();

signals:
    void changed();
    void thumbnail_ready();

private:
    struct Job {
        std::string file;
        std::int64_t size_bytes = 0;
        bool need_info = false;
    };
    struct Result {
        std::string file;
        std::int64_t size_bytes = 0;
        bool ok = false;
        double duration_s = 0.0;
        int width = 0;
        int height = 0;
        QImage thumbnail;
    };

    void load();
    std::string shelf_path(const std::string& folder) const;
    void request_missing_info();
    void request(const ClipEntry& clip, bool need_info, bool urgent);
    void worker();
    Result probe(const Job& job, const std::string& folder);
    void take_results();
    std::string thumb_path(const std::string& folder, const std::string& file, std::int64_t size) const;

    Library library_;
    std::string folder_;
    std::string library_file_;
    std::string cache_dir_;
    bool writable_ = true;
    std::map<std::string, QImage> thumbs_;  // "file|size"
    std::set<std::string> asked_;  // "file|size" requested or done: never asked twice
    QFileSystemWatcher* watcher_ = nullptr;
    QTimer* scan_timer_ = nullptr;
    QTimer* rescan_soon_ = nullptr;
    QTimer* save_soon_ = nullptr;  // lengths and sizes from the worker: one write for a batch
    std::set<std::string> cleanup_skipped_;  // could not go to the desktop's trash: not retried this run

    std::mutex mutex_;  // guards the worker's queue and results
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::vector<Result> results_;
    std::string worker_folder_;
    bool stop_ = false;
    std::thread thread_;  // last: started in the constructor
};

}  // namespace bt::linux_app
