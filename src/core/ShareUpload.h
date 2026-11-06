#pragma once

// Sharing a clip as a link (Share on the clip page): Backtrack uploads a copy to catbox.moe, a free
// anonymous file host, and gets back a direct https link to the MP4. Discord plays a direct MP4
// link in the chat with a player, so no website of our own is needed.
//
//   catbox.moe     files.catbox.moe/<id>.mp4   kept until it goes 2 years without being watched;
//                                              anonymous uploads cannot be deleted by the uploader
//   Litterbox      litter.catbox.moe/<id>.mp4  deleted after 72 hours (catbox's temporary host)
//
// The API (catbox.moe/tools.php): POST multipart/form-data with reqtype=fileupload and the file
// in fileToUpload (Litterbox also takes time=1h/12h/24h/72h); the reply body is the link as plain
// text. catbox takes files up to 200 MB, Litterbox up to 1 GB.
//
// Pure logic: the request's form around the file, and checking the reply. platform/Uploader sends it.

#include "core/Config.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bt {

struct ShareEndpoint {
    std::wstring_view host;  // "catbox.moe"
    std::wstring_view path;  // "/user/api.php"
    std::uint64_t max_bytes = 0;
    std::int64_t keeps_seconds = 0;  // 0: until it goes unwatched for years
};

ShareEndpoint share_endpoint(ShareHost host);

// What a share copy must stay under, with room for the form around the file.
std::uint64_t share_size_limit(ShareHost host);

// multipart/form-data: everything before the file's bytes, and after them. The boundary must not
// occur in the file; a random one of 32+ characters is practically safe (what browsers do).
struct UploadForm {
    std::string content_type;  // the Content-Type header value
    std::string head;
    std::string tail;
};
UploadForm make_upload_form(ShareHost host, std::string_view file_name, std::string_view boundary);

// The link from the reply body, or nothing when the reply is not a link this host gives out (an
// error page, a message such as "File too large", anything with spaces or odd characters).
std::optional<std::string> parse_upload_reply(ShareHost host, std::string_view body);

}  // namespace bt
