#include "core/ShareUpload.h"

#include "Check.h"

using namespace bt;

namespace {

void endpoints() {
    CHECK(share_endpoint(ShareHost::Catbox).host == L"catbox.moe");
    CHECK(share_endpoint(ShareHost::Catbox).path == L"/user/api.php");
    CHECK(share_endpoint(ShareHost::Litterbox).host == L"litterbox.catbox.moe");
    CHECK(share_endpoint(ShareHost::Litterbox).keeps_seconds == 72 * 3600);
    CHECK(share_size_limit(ShareHost::Catbox) < 200'000'000 && share_size_limit(ShareHost::Catbox) > 190'000'000);
    CHECK(share_size_limit(ShareHost::Litterbox) > share_size_limit(ShareHost::Catbox));
}

void form() {
    const UploadForm f = make_upload_form(ShareHost::Catbox, "clip_2026.mp4", "BOUND");
    CHECK_EQ(f.content_type, std::string("multipart/form-data; boundary=BOUND"));
    CHECK_EQ(f.head, std::string("--BOUND\r\nContent-Disposition: form-data; name=\"reqtype\"\r\n\r\nfileupload\r\n"
                                 "--BOUND\r\nContent-Disposition: form-data; name=\"fileToUpload\"; "
                                 "filename=\"clip_2026.mp4\"\r\nContent-Type: video/mp4\r\n\r\n"));
    CHECK_EQ(f.tail, std::string("\r\n--BOUND--\r\n"));
    // Litterbox also says how long to keep it.
    const UploadForm l = make_upload_form(ShareHost::Litterbox, "a.mp4", "B");
    CHECK(l.head.find("name=\"time\"\r\n\r\n72h\r\n") != std::string::npos);
    // A name that could break the header is made safe.
    const UploadForm odd = make_upload_form(ShareHost::Catbox, "a\"b\r\nc\xC3\xA9.mp4", "B");
    CHECK(odd.head.find("filename=\"a_b__c__.mp4\"") != std::string::npos);
    CHECK(make_upload_form(ShareHost::Catbox, "", "B").head.find("filename=\"clip.mp4\"") != std::string::npos);
}

void replies() {
    CHECK(parse_upload_reply(ShareHost::Catbox, "https://files.catbox.moe/k3p9xq.mp4") ==
          std::optional<std::string>("https://files.catbox.moe/k3p9xq.mp4"));
    CHECK(parse_upload_reply(ShareHost::Catbox, "  https://files.catbox.moe/k3p9xq.mp4\r\n") ==
          std::optional<std::string>("https://files.catbox.moe/k3p9xq.mp4"));
    CHECK(parse_upload_reply(ShareHost::Litterbox, "https://litter.catbox.moe/ab12cd.mp4").has_value());
    // The wrong host for the service, an error text, an empty reply, odd characters.
    CHECK(!parse_upload_reply(ShareHost::Litterbox, "https://files.catbox.moe/k3p9xq.mp4"));
    CHECK(!parse_upload_reply(ShareHost::Catbox, "File too large."));
    CHECK(!parse_upload_reply(ShareHost::Catbox, ""));
    CHECK(!parse_upload_reply(ShareHost::Catbox, "https://files.catbox.moe/"));
    CHECK(!parse_upload_reply(ShareHost::Catbox, "https://files.catbox.moe/a b.mp4"));
    CHECK(!parse_upload_reply(ShareHost::Catbox, "https://files.catbox.moe/a/../../x"));
    CHECK(!parse_upload_reply(ShareHost::Catbox, "https://files.catbox.moe.evil.com/a.mp4"));
    CHECK(!parse_upload_reply(ShareHost::Catbox, "http://files.catbox.moe/a.mp4"));
    CHECK(!parse_upload_reply(ShareHost::Catbox, "<html>502 Bad Gateway</html>"));
}

}  // namespace

int main() {
    endpoints();
    form();
    replies();
    return bt_test::finish("ShareUploadTests");
}
