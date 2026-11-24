// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/mkv_attachments.hpp"

#include "player/ebml.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <sys/types.h>

namespace vplayer {

namespace {

constexpr size_t kMaxFonts = 64;
constexpr size_t kMaxFontBytes = 48u << 20;

struct FileCloser {
    void operator()(std::FILE *f) const { std::fclose(f); }
};

std::string lower(std::string s) {
    for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with(const std::string &s, const char *suffix) {
    const size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

void read_attachments(std::FILE *f, const ebml::Element &att, std::vector<wnk::media::FontAttachment> *out) {
    size_t total = 0;
    for (uint64_t a = att.data; a < att.end() && out->size() < kMaxFonts;) {
        ebml::Element file;
        if (!ebml::header(f, a, &file) || file.size == ebml::kUnknownSize) return;
        if (file.id == ebml::kAttachedFile) {
            std::string name, mime;
            ebml::Element data{};
            for (uint64_t c = file.data; c < file.end();) {
                ebml::Element e;
                if (!ebml::header(f, c, &e) || e.size == ebml::kUnknownSize) break;
                if (e.id == ebml::kFileName) name = ebml::read_string(f, e);
                if (e.id == ebml::kFileMimeType) mime = ebml::read_string(f, e);
                if (e.id == ebml::kFileData) data = e;
                c = e.end();
            }
            if (data.id == ebml::kFileData && is_font_attachment(name, mime) && total + data.size <= kMaxFontBytes) {
                auto bytes = std::make_shared<std::vector<uint8_t>>();
                if (ebml::read_bytes(f, data, bytes.get(), kMaxFontBytes)) {
                    total += bytes->size();
                    out->push_back({name, std::move(bytes)});
                }
            }
        }
        a = file.end();
    }
}

}  // namespace

bool is_font_attachment(const std::string &name, const std::string &mime) {
    const std::string m = lower(mime), n = lower(name);
    if (m.find("font") != std::string::npos || m.find("truetype") != std::string::npos ||
        m.find("opentype") != std::string::npos)
        return true;
    return ends_with(n, ".ttf") || ends_with(n, ".otf") || ends_with(n, ".ttc") || ends_with(n, ".woff");
}

std::vector<wnk::media::FontAttachment> mkv_fonts(const std::string &path) {
    std::vector<wnk::media::FontAttachment> out;
    std::unique_ptr<std::FILE, FileCloser> f(std::fopen(path.c_str(), "rb"));
    if (!f) return out;
    if (fseeko(f.get(), 0, SEEK_END) != 0) return out;
    const auto size = static_cast<uint64_t>(ftello(f.get()));
    uint64_t seg, seg_end;
    if (!ebml::segment(f.get(), size, &seg, &seg_end)) return out;

    uint64_t from_seek_head = 0;  // the Attachments' position, as the SeekHead gives it
    for (uint64_t at = seg; at < seg_end;) {
        ebml::Element e;
        if (!ebml::header(f.get(), at, &e) || e.size == ebml::kUnknownSize || e.id == ebml::kCluster) break;
        if (e.id == ebml::kAttachments) {
            read_attachments(f.get(), e, &out);
            return out;
        }
        if (e.id == ebml::kSeekHead) {
            for (uint64_t s = e.data; s < e.end();) {
                ebml::Element seek;
                if (!ebml::header(f.get(), s, &seek) || seek.size == ebml::kUnknownSize) break;
                if (seek.id == ebml::kSeek) {
                    uint64_t id = 0, pos = 0;
                    bool has_pos = false;
                    for (uint64_t c = seek.data; c < seek.end();) {
                        ebml::Element x;
                        if (!ebml::header(f.get(), c, &x) || x.size == ebml::kUnknownSize) break;
                        if (x.id == ebml::kSeekId) id = ebml::read_uint(f.get(), x);  // the id's bytes, as a number
                        if (x.id == ebml::kSeekPosition) {
                            pos = ebml::read_uint(f.get(), x);
                            has_pos = true;
                        }
                        c = x.end();
                    }
                    if (id == ebml::kAttachments && has_pos) from_seek_head = seg + pos;
                }
                s = seek.end();
            }
        }
        at = e.end();
    }
    if (from_seek_head != 0) {
        ebml::Element e;
        if (ebml::header(f.get(), from_seek_head, &e) && e.id == ebml::kAttachments && e.size != ebml::kUnknownSize)
            read_attachments(f.get(), e, &out);
    }
    return out;
}

}  // namespace vplayer
