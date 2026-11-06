// Development tool: uploads one file the way Share does (platform/Uploader) and prints the link.
//   share_check <file.mp4> [catbox|litterbox] [abort after ms]
// Not shipped. Everything uploaded is public: use test files only.

#include "platform/Uploader.h"
#include "platform/Text.h"

#include <cstdio>
#include <chrono>
#include <string>
#include <thread>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: share_check <file.mp4> [catbox|litterbox]\n");
        return 2;
    }
    const bt::ShareHost host =
        argc > 2 && std::wstring(argv[2]) == L"catbox" ? bt::ShareHost::Catbox : bt::ShareHost::Litterbox;
    bt::UploadAbort abort;
    // Optional: abort from another thread after a while, the way the window's Cancel does.
    std::thread aborter;
    if (argc > 3) {
        const int ms = _wtoi(argv[3]);
        aborter = std::thread([&abort, ms] {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            abort.abort();
        });
    }
    const auto started = std::chrono::steady_clock::now();
    int shown = -1;
    const bt::UploadResult result = bt::upload_clip(argv[1], host, [&](double f) {
        const int percent = static_cast<int>(f * 100);
        if (percent / 10 != shown / 10) std::printf("%d%%\n", shown = percent);
    }, abort);
    if (aborter.joinable()) aborter.join();
    std::printf("took %lld ms\n", static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - started).count()));
    if (!result.ok) {
        std::printf("FAILED: %s\n", result.message.c_str());
        return 1;
    }
    std::printf("%s\n", result.url.c_str());
    return 0;
}
