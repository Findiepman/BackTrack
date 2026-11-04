#pragma once

// Reads what the library shows about a clip file without the user waiting for it: its length,
// picture size and a small thumbnail. One background thread works through the requests in order;
// results are picked up by the UI thread (take_results) after the `ready` callback says so.
//
// Thumbnails are kept in the cache folder so the library opens instantly next time; the cache key
// includes the file size, so a clip rewritten by another program gets a new thumbnail.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bt {

struct MediaInfo {
    std::string file;  // UTF-8 name inside the clips folder, as requested
    std::int64_t size_bytes = 0;  // the file size the request was made for (the thumbnail belongs to it)
    bool ok = false;  // false: not a playable video (damaged, still being written, not an MP4)
    double duration_s = 0.0;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> thumbnail;  // BGRA, kThumbWidth x kThumbHeight; empty if it could not be made
};

inline constexpr int kThumbWidth = 384;
inline constexpr int kThumbHeight = 216;

class MediaProbe {
public:
    // `ready` runs on the worker thread after results were added; it should only post a message.
    MediaProbe(std::filesystem::path cache_dir, std::function<void()> ready);
    ~MediaProbe();
    MediaProbe(const MediaProbe&) = delete;
    MediaProbe& operator=(const MediaProbe&) = delete;

    // Asks for one clip. A file already waiting is not queued twice. `need_info`: the library has
    // no length for it yet; otherwise only the thumbnail is wanted. `urgent`: the card is on screen,
    // so it goes before the clips nobody is looking at.
    void request(const std::filesystem::path& folder, const std::string& file, std::int64_t size_bytes, bool need_info,
                 bool urgent = false);
    std::vector<MediaInfo> take_results();

private:
    struct Job {
        std::filesystem::path folder;
        std::string file;
        std::int64_t size_bytes = 0;
        bool need_info = false;
    };

    void run();
    MediaInfo probe(const Job& job);
    std::filesystem::path cache_path(const Job& job) const;

    const std::filesystem::path cache_dir_;
    const std::function<void()> ready_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::vector<MediaInfo> results_;
    bool stop_ = false;
    std::thread thread_;  // last: starts in the constructor
};

}  // namespace bt
