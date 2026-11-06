#include "core/ShareUpload.h"

#include <format>

namespace bt {

ShareEndpoint share_endpoint(ShareHost host) {
    if (host == ShareHost::Litterbox) {
        return {L"litterbox.catbox.moe", L"/resources/internals/api.php", 1'000'000'000, 72 * 60 * 60};
    }
    return {L"catbox.moe", L"/user/api.php", 200'000'000, 0};
}

std::uint64_t share_size_limit(ShareHost host) {
    // "200 MB": whichever way catbox counts a megabyte, 2 % under the smaller reading is safe.
    return share_endpoint(host).max_bytes / 100 * 98;
}

namespace {

std::string field(std::string_view boundary, std::string_view name, std::string_view value) {
    return std::format("--{}\r\nContent-Disposition: form-data; name=\"{}\"\r\n\r\n{}\r\n", boundary, name, value);
}

}  // namespace

UploadForm make_upload_form(ShareHost host, std::string_view file_name, std::string_view boundary) {
    // The name goes into a quoted header: no quotes, line breaks or non-ASCII (the host renames
    // the file anyway, only the extension counts).
    std::string safe;
    for (const char c : file_name) {
        const auto u = static_cast<unsigned char>(c);
        safe += (u < 0x20 || u >= 0x7f || c == '"' || c == '\\') ? '_' : c;
    }
    if (safe.empty()) safe = "clip.mp4";
    UploadForm form;
    form.content_type = std::format("multipart/form-data; boundary={}", boundary);
    form.head = field(boundary, "reqtype", "fileupload");
    if (host == ShareHost::Litterbox) form.head += field(boundary, "time", "72h");
    form.head += std::format(
        "--{}\r\nContent-Disposition: form-data; name=\"fileToUpload\"; filename=\"{}\"\r\nContent-Type: video/mp4\r\n\r\n",
        boundary, safe);
    form.tail = std::format("\r\n--{}--\r\n", boundary);
    return form;
}

std::optional<std::string> parse_upload_reply(ShareHost host, std::string_view body) {
    const auto first = body.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return std::nullopt;
    const auto last = body.find_last_not_of(" \t\r\n");
    const std::string_view link = body.substr(first, last - first + 1);
    const std::string_view prefix =
        host == ShareHost::Litterbox ? std::string_view("https://litter.catbox.moe/") : "https://files.catbox.moe/";
    if (!link.starts_with(prefix) || link.size() == prefix.size() || link.size() > 200) return std::nullopt;
    // A file name: letters, digits, '.', '-', '_'. Nothing that could make it a different address.
    for (const char c : link.substr(prefix.size())) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '-' || c == '_';
        if (!ok) return std::nullopt;
    }
    return std::string(link);
}

}  // namespace bt
