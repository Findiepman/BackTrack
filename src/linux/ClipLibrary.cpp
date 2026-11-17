#include "linux/ClipLibrary.h"

#include "core/Json.h"
#include "linux/Desktop.h"
#include "linux/Log.h"
#include "linux/Media.h"

#include <QCollator>
#include <QDateTime>
#include <QFile>
#include <QFileSystemWatcher>
#include <QMetaObject>
#include <QTimer>

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <system_error>

namespace bt::linux_app {

namespace {

namespace fs = std::filesystem;

constexpr int kThumbWidth = 384;
constexpr int kThumbHeight = 216;
constexpr double kThumbPosition = 1.0 / 3.0;  // a third in: past the start of a clip, before the end
constexpr int kScanMs = 30'000;  // a fallback for folders the watcher misses (network shares)

std::int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string key_of(const std::string& file, std::int64_t size) {
    return file + "|" + std::to_string(size);
}

std::uint64_t fnv1a(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool is_clip_name(const std::string& name) {
    if (name.size() <= 4 || name.starts_with('.')) {
        return false;  // hidden: a clip still being written (".name.mp4.part") or not ours
    }
    std::string extension = name.substr(name.size() - 4);
    for (char& c : extension) {
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    return extension == ".mp4";
}

// Search and the name sort follow the user's language, as on Windows (core only knows ASCII).
std::string fold_text(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())).toCaseFolded().toStdString();
}

int compare_text(std::string_view a, std::string_view b) {
    static const QCollator collator = [] {
        QCollator c;
        c.setCaseSensitivity(Qt::CaseInsensitive);
        c.setNumericMode(true);
        return c;
    }();
    return collator.compare(QString::fromUtf8(a.data(), static_cast<qsizetype>(a.size())),
                            QString::fromUtf8(b.data(), static_cast<qsizetype>(b.size())));
}

// Written next to it and renamed over it after a flush, so a power cut leaves the old file or the
// new one, never half of one.
bool write_atomically(const std::string& path, const std::string& text) {
    const std::string temp = path + ".tmp";
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    std::size_t done = 0;
    bool ok = true;
    while (done < text.size()) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n <= 0) {
            ok = false;
            break;
        }
        done += static_cast<std::size_t>(n);
    }
    ok = ok && ::fsync(fd) == 0;
    ok = ::close(fd) == 0 && ok;
    if (ok && std::rename(temp.c_str(), path.c_str()) == 0) {
        return true;
    }
    std::error_code ec;
    fs::remove(temp, ec);
    return false;
}

}  // namespace

ClipLibrary::ClipLibrary(QObject* parent) : QObject(parent) {
    set_text_rules(TextRules{fold_text, compare_text});
    const AppPaths paths = app_paths();
    library_file_ = paths.data_dir + "/library.json";
    cache_dir_ = paths.cache_dir + "/thumbs";
    std::error_code ec;
    fs::create_directories(paths.data_dir, ec);
    fs::create_directories(cache_dir_, ec);
    load();

    watcher_ = new QFileSystemWatcher(this);
    rescan_soon_ = new QTimer(this);
    rescan_soon_->setSingleShot(true);
    rescan_soon_->setInterval(500);  // a copy of many files is one scan, not one per file
    connect(rescan_soon_, &QTimer::timeout, this, [this] { scan(); });
    connect(watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { rescan_soon_->start(); });
    scan_timer_ = new QTimer(this);
    connect(scan_timer_, &QTimer::timeout, this, [this] {
        scan();
        run_cleanup();
    });
    scan_timer_->start(kScanMs);
    save_soon_ = new QTimer(this);
    save_soon_->setSingleShot(true);
    save_soon_->setInterval(1000);
    connect(save_soon_, &QTimer::timeout, this, [this] { save(); });

    thread_ = std::thread([this] { worker(); });
}

ClipLibrary::~ClipLibrary() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
    if (save_soon_->isActive() && writable_) {
        // Quitting within a second of a probe result: written, without telling pages that are going away.
        write_atomically(library_file_, library_.to_json().dump(2) + "\n");
    }
}

void ClipLibrary::load() {
    std::error_code ec;
    if (!fs::exists(library_file_, ec)) {
        if (ec) {
            // Cannot even tell whether it is there: do not risk replacing it this run.
            writable_ = false;
            log_warning("Cannot check " + library_file_ + " (" + ec.message() + "); library changes are not saved");
        }
        return;  // first run: the folder scan fills it
    }
    QFile file(QString::fromStdString(library_file_));
    if (!file.open(QIODevice::ReadOnly)) {
        // There but unreadable: treating it as a first run would overwrite the real one.
        writable_ = false;
        log_warning("Cannot read " + library_file_ + "; library changes are not saved this run");
        return;
    }
    const QByteArray bytes = file.readAll();
    const JsonParseResult parsed = parse_json(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
    const auto keep_copy = [this](const char* why) {
        const std::string copy = std::format("{}/library.{}-{}.json", fs::path(library_file_).parent_path().string(),
                                             why, QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss").toStdString());
        std::error_code copy_ec;
        fs::copy_file(library_file_, copy, fs::copy_options::overwrite_existing, copy_ec);
        if (copy_ec) {
            // Without the copy the original is the only one left: it must not be saved over.
            writable_ = false;
            log_warning("Could not keep a copy of library.json (" + copy_ec.message() + "); changes are not saved");
        } else {
            log_warning("Kept a copy of library.json as " + copy);
        }
    };
    if (!parsed.value || !parsed.value->is_object() || !(*parsed.value)["clips"].is_array()) {
        // Never throw away what the user set up: keep the damaged file and start over from the folder.
        log_warning("library.json could not be read" + (parsed.value ? std::string() : " (" + parsed.error + ")"));
        keep_copy("damaged");
        return;
    }
    if ((*parsed.value)["version"].as_int(1) > kLibraryFileVersion) {
        keep_copy("newer");  // a newer Backtrack wrote it; saving drops what this one does not know
    }
    library_ = Library::from_json(*parsed.value);
}

void ClipLibrary::save() {
    if (writable_ && !write_atomically(library_file_, library_.to_json().dump(2) + "\n")) {
        static std::int64_t last_warning = 0;
        if (now_unix() - last_warning > 60) {
            last_warning = now_unix();
            log_warning("Could not write " + library_file_);
        }
    }
    emit changed();
}

std::string ClipLibrary::shelf_path(const std::string& folder) const {
    return std::format("{}/library-{:016x}.json", fs::path(library_file_).parent_path().string(), fnv1a(folder));
}

void ClipLibrary::set_folder(const std::string& folder) {
    if (folder == folder_) {
        return;
    }
    const bool switching = !folder_.empty();
    if (switching) {
        watcher_->removePath(QString::fromStdString(folder_));
        // Each clips folder keeps its own favorites, collections, tags and notes: the library of the
        // folder being left is put aside, and the one of the folder being opened comes back if it
        // was used before. Otherwise the scan below would drop every clip of the old folder, and a
        // mistyped folder in Settings would cost the user all of that.
        if (writable_ && !write_atomically(shelf_path(folder_), library_.to_json().dump(2) + "\n")) {
            writable_ = false;
            log_warning("Could not put the library of " + folder_ + " aside; library changes are not saved this run");
        }
        Library next;
        QFile shelf(QString::fromStdString(shelf_path(folder)));
        if (shelf.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = shelf.readAll();
            const JsonParseResult parsed =
                parse_json(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
            if (parsed.value && parsed.value->is_object() && (*parsed.value)["clips"].is_array()) {
                next = Library::from_json(*parsed.value);
            }
        }
        library_ = std::move(next);
        cleanup_skipped_.clear();
        save_soon_->stop();
        log_info("Clips folder is now " + folder);
    }
    folder_ = folder;
    {
        std::lock_guard lock(mutex_);
        jobs_.clear();  // they belong to the old folder
        results_.clear();  // so do results the GUI has not taken yet
        worker_folder_ = folder_;
    }
    asked_.clear();
    thumbs_.clear();
    std::error_code ec;
    fs::create_directories(folder_, ec);
    watcher_->addPath(QString::fromStdString(folder_));
    scan();
    if (switching) {
        save();  // also when the scan changed nothing: library.json is now this folder's
    }
}

std::string ClipLibrary::path_of(const std::string& file) const {
    return (fs::path(folder_) / file).string();
}

void ClipLibrary::scan() {
    if (folder_.empty()) {
        return;
    }
    std::error_code ec;
    if (!fs::is_directory(folder_, ec)) {
        // Unplugged drive, network gone: says nothing about the clips. The library stays as it is.
        return;
    }
    std::vector<FolderFile> files;
    fs::directory_iterator it(folder_, ec);
    for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        std::error_code entry_ec;
        if (!is_clip_name(name) || !it->is_regular_file(entry_ec)) {
            continue;
        }
        FolderFile file;
        file.file = name;
        file.size_bytes = static_cast<std::int64_t>(it->file_size(entry_ec));
        const auto written = it->last_write_time(entry_ec);
        if (entry_ec) {
            continue;  // gone while listing
        }
        file.modified = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::file_clock::to_sys(written).time_since_epoch())
                            .count();
        files.push_back(std::move(file));
    }
    if (ec) {
        return;  // the listing broke off: a partial list would drop clips
    }
    if (library_.sync_with_folder(files)) {
        save();
    }
    request_missing_info();
}

void ClipLibrary::add_saved(const std::string& path, double seconds, const std::string& source) {
    const fs::path file(path);
    if (file.parent_path() != fs::path(folder_)) {
        scan();
        return;
    }
    ClipEntry clip;
    clip.file = file.filename().string();
    clip.created = now_unix();
    std::error_code ec;
    clip.size_bytes = static_cast<std::int64_t>(fs::file_size(file, ec));
    clip.duration_s = seconds;
    clip.source = source;
    library_.add_saved_clip(clip);
    save();
    request_missing_info();  // the picture size and the thumbnail
}

void ClipLibrary::request_missing_info() {
    for (const std::string& file : library_.needs_probe()) {
        if (const ClipEntry* clip = library_.find(file)) {
            request(*clip, true, false);
        }
    }
    for (const ClipEntry& clip : library_.clips()) {
        if (clip.width == 0 && clip.duration_s > 0) {
            request(clip, true, false);  // saved by Backtrack: the length is known, the size is not
        }
    }
}

void ClipLibrary::request(const ClipEntry& clip, bool need_info, bool urgent) {
    const std::string key = key_of(clip.file, clip.size_bytes);
    if (!asked_.insert(key).second) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        Job job{clip.file, clip.size_bytes, need_info};
        if (urgent) {
            jobs_.push_front(std::move(job));
        } else {
            jobs_.push_back(std::move(job));
        }
    }
    wake_.notify_one();
}

QImage ClipLibrary::thumbnail(const ClipEntry& clip, bool on_screen) {
    const auto found = thumbs_.find(key_of(clip.file, clip.size_bytes));
    if (found != thumbs_.end()) {
        return found->second;
    }
    request(clip, clip.duration_s <= 0 || clip.width == 0, on_screen);
    return {};
}

std::string ClipLibrary::thumb_path(const std::string& folder, const std::string& file, std::int64_t size) const {
    // The folder is part of the key: a same-named clip of the same size elsewhere is another clip.
    return std::format("{}/{:016x}.png", cache_dir_, fnv1a(folder + "/" + key_of(file, size)));
}

void ClipLibrary::worker() {
    try {
        while (true) {
            Job job;
            std::string folder;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                if (stop_) {
                    return;
                }
                job = std::move(jobs_.front());
                jobs_.pop_front();
                folder = worker_folder_;
            }
            Result result = probe(job, folder);
            {
                std::lock_guard lock(mutex_);
                if (stop_) {
                    return;
                }
                if (folder != worker_folder_) {
                    continue;  // the folder changed meanwhile: a same-named file there is another clip
                }
                results_.push_back(std::move(result));
            }
            QMetaObject::invokeMethod(this, [this] { take_results(); }, Qt::QueuedConnection);
        }
    } catch (const std::exception& e) {
        log_error(std::string("Reading clip thumbnails stopped: ") + e.what());
    }
}

ClipLibrary::Result ClipLibrary::probe(const Job& job, const std::string& folder) {
    Result result;
    result.file = job.file;
    result.size_bytes = job.size_bytes;
    const std::string path = (fs::path(folder) / job.file).string();
    const std::string cache = thumb_path(folder, job.file, job.size_bytes);
    if (!job.need_info && result.thumbnail.load(QString::fromStdString(cache), "PNG")) {
        result.ok = true;
        return result;
    }
    const MediaFacts facts = probe_media(path);
    result.ok = facts.ok;
    result.duration_s = facts.duration_s;
    result.width = facts.width;
    result.height = facts.height;
    if (!facts.ok) {
        return result;  // still being written, or not a video: a plain card
    }
    if (result.thumbnail.load(QString::fromStdString(cache), "PNG")) {
        return result;
    }
    const Still still = read_still(path, kThumbPosition, kThumbWidth, kThumbHeight);
    if (!still.pixels.empty()) {
        result.thumbnail = QImage(still.pixels.data(), still.width, still.height, still.width * 4, QImage::Format_RGB32)
                               .copy();  // owns its pixels once `still` is gone
        result.thumbnail.save(QString::fromStdString(cache), "PNG");  // a missing cache only costs time
    }
    return result;
}

void ClipLibrary::take_results() {
    std::vector<Result> results;
    {
        std::lock_guard lock(mutex_);
        results.swap(results_);
    }
    bool changed = false;
    bool pictures = false;
    for (Result& result : results) {
        const ClipEntry* clip = library_.find(result.file);
        // A result for an older size of the file belongs to nothing on screen.
        if (clip == nullptr || clip->size_bytes != result.size_bytes) {
            continue;
        }
        if (result.ok && result.duration_s > 0 &&
            (clip->duration_s <= 0 || clip->width != result.width || clip->height != result.height)) {
            library_.set_media_info(result.file, clip->duration_s > 0 ? clip->duration_s : result.duration_s,
                                    result.width, result.height);
            changed = true;
        }
        thumbs_[key_of(result.file, result.size_bytes)] = std::move(result.thumbnail);  // null: none can be made
        pictures = true;
    }
    if (changed) {
        save_soon_->start();
    }
    if (pictures) {
        emit thumbnail_ready();
    }
}

bool ClipLibrary::delete_forever(const std::string& file) {
    if (library_.find(file) == nullptr) {
        return false;
    }
    QFile clip(QString::fromStdString(path_of(file)));
    // Never deleted for good when the desktop's trash cannot take it (a share or a drive without
    // one): it stays in Backtrack's Trash, as Windows keeps a clip the Recycle Bin refuses.
    if (clip.exists() && !clip.moveToTrash()) {
        log_warning("Could not move " + path_of(file) + " to the trash: " + clip.errorString().toStdString());
        return false;
    }
    library_.remove(file);
    save();
    return true;
}

void ClipLibrary::run_cleanup() {
    if (!writable_ || folder_.empty()) {
        return;
    }
    // Only the Trash's 30 days on Linux; the storage limits are not there yet (docs/LINUX-PORT.md).
    const std::vector<std::string> due = library_.due_for_removal(CleanupPolicy{}, now_unix());
    bool removed = false;
    for (const std::string& file : due) {
        if (cleanup_skipped_.contains(file)) {
            continue;
        }
        QFile clip(QString::fromStdString(path_of(file)));
        if (!clip.exists() || clip.moveToTrash()) {
            library_.remove(file);
            removed = true;
        } else {
            // Waits for the next start instead of a try (and a warning) every 30 seconds.
            cleanup_skipped_.insert(file);
            log_warning("Could not move " + path_of(file) + " to the trash; it stays in Backtrack's Trash");
        }
    }
    if (removed) {
        save();
    }
}

}  // namespace bt::linux_app
