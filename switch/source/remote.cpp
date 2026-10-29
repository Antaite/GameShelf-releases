#include "remote.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

#include "formats.hpp"
#include "http.hpp"

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace remote {

static const int kDiscoveryPort = 24680;
static const int kMaxDepth = 6;
static const int kMaxRequests = 400;

bool isInstallable(const std::string& n) {
    return fmt::endsWith(n, ".nsp") || fmt::endsWith(n, ".nsz") || fmt::endsWith(n, ".xci") || fmt::endsWith(n, ".xcz");
}

const char* kindName(ServerKind k) {
    switch (k) {
        case ServerKind::LanServer: return "server.py 伺服器";
        case ServerKind::WebDav: return "WebDAV";
        case ServerKind::DirectoryListing: return "網頁目錄";
        case ServerKind::JsonIndex: return "JSON 檔案清單";
        default: return "未知";
    }
}

// ------------------------------------------------------------------ small text helpers
static std::string xmlUnescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') { o += s[i]; continue; }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) { o += s[i]; continue; }
        std::string ent = s.substr(i + 1, semi - i - 1);
        uint32_t cp = 0;
        if (ent == "amp") cp = '&';
        else if (ent == "lt") cp = '<';
        else if (ent == "gt") cp = '>';
        else if (ent == "quot") cp = '"';
        else if (ent == "apos") cp = '\'';
        else if (!ent.empty() && ent[0] == '#')
            cp = (uint32_t)(ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? strtoul(ent.c_str() + 2, nullptr, 16)
                                                                              : strtoul(ent.c_str() + 1, nullptr, 10));
        if (!cp) { o += s[i]; continue; }
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        i = semi;
    }
    return o;
}

// "http://host:port/a/b" -> "/a/b"; "/a/b" stays.
static std::string hrefToPath(const std::string& href) {
    size_t p = href.find("://");
    if (p == std::string::npos) return href;
    size_t slash = href.find('/', p + 3);
    return slash == std::string::npos ? "/" : href.substr(slash);
}

// Decoded path without trailing slash, for comparing two spellings of the same path.
static std::string canonical(const std::string& path) {
    std::string d = http::percentDecode(path);
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    return d;
}

static std::string relativeName(const std::string& path, const std::string& basePath) {
    std::string d = http::percentDecode(path), b = http::percentDecode(basePath);
    if (d.compare(0, b.size(), b) == 0) return d.substr(b.size());
    return d;
}

// ------------------------------------------------------------------ WebDAV
std::vector<DavEntry> parsePropfind(const std::string& xml) {
    std::vector<DavEntry> out;
    DavEntry cur{"", 0, false};
    bool inResponse = false;
    std::string capture;  // local name of the element whose text we are reading
    std::string text;
    size_t i = 0;
    while (i < xml.size()) {
        if (xml[i] != '<') {
            if (!capture.empty()) text += xml[i];
            i++;
            continue;
        }
        size_t end = xml.find('>', i);
        if (end == std::string::npos) break;
        std::string tag = xml.substr(i + 1, end - i - 1);
        i = end + 1;
        if (tag.empty() || tag[0] == '?' || tag[0] == '!') continue;
        bool closing = tag[0] == '/';
        bool selfClosing = !tag.empty() && tag.back() == '/';
        std::string name = tag.substr(closing ? 1 : 0);
        if (selfClosing) name.pop_back();
        size_t sp = name.find_first_of(" \t\r\n");
        if (sp != std::string::npos) name = name.substr(0, sp);
        size_t colon = name.find(':');
        if (colon != std::string::npos) name = name.substr(colon + 1);
        for (auto& c : name) c = (char)tolower((unsigned char)c);

        if (!closing) {
            if (name == "response") { inResponse = true; cur = DavEntry{"", 0, false}; }
            else if (inResponse && name == "collection") cur.collection = true;
            else if (inResponse && !selfClosing && (name == "href" || name == "getcontentlength")) { capture = name; text.clear(); }
        } else {
            if (name == capture) {
                std::string v = xmlUnescape(text);
                while (!v.empty() && isspace((unsigned char)v.back())) v.pop_back();
                while (!v.empty() && isspace((unsigned char)v.front())) v.erase(0, 1);
                if (name == "href" && cur.href.empty()) cur.href = v;
                if (name == "getcontentlength") cur.size = strtoull(v.c_str(), nullptr, 10);
                capture.clear();
            }
            if (name == "response" && inResponse) {
                if (!cur.href.empty()) out.push_back(cur);
                inResponse = false;
            }
        }
    }
    return out;
}

// Returns 1 = ok, 0 = not WebDAV (try something else), -1 = hard error (err set).
// Progress of fetchList for a progress bar on another thread (see setListProgress).
static thread_local ListProgress* t_progress = nullptr;
void setListProgress(ListProgress* p) { t_progress = p; }
static void noteFiles(size_t n) { if (t_progress) t_progress->files = n; }

static int davList(const http::Url& base, const std::string& basePath, std::vector<RemoteFile>& out, std::string& err) {
    static const char* kBody =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:resourcetype/><D:getcontentlength/></D:prop></D:propfind>";
    std::vector<std::pair<std::string, int>> queue = {{basePath, 0}};
    std::set<std::string> seen = {canonical(basePath)};
    int requests = 0;
    while (!queue.empty() && requests < kMaxRequests) {
        auto [dir, depth] = queue.back();
        queue.pop_back();
        http::Request req;
        req.method = "PROPFIND";
        req.headers = {"Depth: 1", "Content-Type: application/xml; charset=\"utf-8\""};
        req.body = kBody;
        std::vector<uint8_t> body;
        http::Response res;
        std::string e;
        requests++;
        if (!http::requestToMemory(base.origin() + dir, req, body, res, e)) {
            if (requests == 1) return 0;
            err = e;
            return -1;
        }
        if (res.status != 207) {
            if (requests == 1) {
                if (res.status == 401) { err = http::statusMessage(401); return -1; }
                return 0;
            }
            continue;  // skip unreadable sub-folders
        }
        for (const auto& ent : parsePropfind(std::string(body.begin(), body.end()))) {
            std::string path = hrefToPath(ent.href);
            std::string canon = canonical(path);
            if (canon == canonical(dir)) continue;
            if (ent.collection) {
                if (depth + 1 <= kMaxDepth && seen.insert(canon).second) {
                    if (path.back() != '/') path += '/';
                    queue.push_back({path, depth + 1});
                }
            } else if (isInstallable(http::percentDecode(path))) {
                RemoteFile f;
                f.url = base.origin() + path;
                f.displayName = relativeName(path, basePath);
                f.size = ent.size;
                out.push_back(f);
                noteFiles(out.size());
            }
        }
    }
    return 1;
}

// ------------------------------------------------------------------ HTML directory listing
std::vector<std::string> parseHtmlLinks(const std::string& html) {
    std::vector<std::string> out;
    std::string low = html;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    size_t i = 0;
    while ((i = low.find("href", i)) != std::string::npos) {
        size_t j = i + 4;
        while (j < html.size() && isspace((unsigned char)html[j])) j++;
        if (j >= html.size() || html[j] != '=') { i = j; continue; }
        j++;
        while (j < html.size() && isspace((unsigned char)html[j])) j++;
        if (j >= html.size()) break;
        char q = html[j];
        size_t start, stop;
        if (q == '"' || q == '\'') { start = j + 1; stop = html.find(q, start); }
        else { start = j; stop = html.find_first_of(" \t\r\n>", start); }
        if (stop == std::string::npos) break;
        out.push_back(xmlUnescape(html.substr(start, stop - start)));
        i = stop;
    }
    return out;
}

static int htmlList(const http::Url& base, const std::string& basePath, std::vector<RemoteFile>& out, std::string& err) {
    std::vector<std::pair<std::string, int>> queue = {{basePath, 0}};
    std::set<std::string> seen = {canonical(basePath)};
    int requests = 0;
    bool any = false;
    bool rootHasEntries = false;  // a real directory listing links to files / sub-folders
    while (!queue.empty() && requests < kMaxRequests) {
        auto [dir, depth] = queue.back();
        queue.pop_back();
        std::vector<uint8_t> body;
        http::Response res;
        std::string e;
        requests++;
        bool isRoot = requests == 1;
        if (!http::requestToMemory(base.origin() + dir, http::Request(), body, res, e)) {
            if (requests == 1) { err = e; return -1; }
            continue;
        }
        if (res.status != 200) {
            if (requests == 1) { err = http::statusMessage(res.status); return -1; }
            continue;
        }
        any = true;
        for (std::string link : parseHtmlLinks(std::string(body.begin(), body.end()))) {
            if (link.empty() || link[0] == '?' || link[0] == '#' || link.compare(0, 2, "..") == 0) continue;
            if (link.find("://") != std::string::npos) {
                http::Url lu;
                if (!http::parseUrl(link, lu) || lu.host != base.host || lu.port != base.port) continue;
                link = lu.path;
            }
            std::string path;
            if (link[0] == '/') {
                if (canonical(link).compare(0, canonical(dir).size(), canonical(dir)) != 0) continue;  // outside
                path = link;
            } else {
                if (link.compare(0, 2, "./") == 0) link = link.substr(2);
                path = dir + link;
            }
            size_t q = path.find_first_of("?#");
            if (q != std::string::npos) path = path.substr(0, q);
            std::string canon = canonical(path);
            if (canon == canonical(dir) || canon.size() < canonical(dir).size()) continue;
            if (isRoot && (path.back() == '/' || isInstallable(http::percentDecode(path)))) rootHasEntries = true;
            if (path.back() == '/') {
                if (depth + 1 <= kMaxDepth && seen.insert(canon).second) queue.push_back({path, depth + 1});
            } else if (isInstallable(http::percentDecode(path)) && seen.insert(canon).second) {
                RemoteFile f;
                f.url = base.origin() + path;
                f.displayName = relativeName(path, basePath);
                std::string se;
                http::probeSize(f.url, f.size, se);
                out.push_back(f);
                noteFiles(out.size());
            }
        }
    }
    if (any && !rootHasEntries) {
        err = "這個網址是一般網頁（可能是伺服器的管理或登入頁面），不是檔案目錄。"
              "Synology 請安裝「WebDAV Server」套件，改用 5005（HTTP）或 5006（HTTPS）埠";
        return -1;
    }
    return any ? 1 : 0;
}


// ------------------------------------------------------------------ Tinfoil-style JSON index
namespace {
// Minimal JSON reader: enough for {"files":[{"url":"..","size":123}],"directories":[".."],"success":".."}.
struct Json {
    const std::string& s;
    size_t i = 0;
    int depth = 0;
    explicit Json(const std::string& t) : s(t) {}
    void ws() { while (i < s.size() && isspace((unsigned char)s[i])) i++; }
    bool eat(char c) { ws(); if (i < s.size() && s[i] == c) { i++; return true; } return false; }

    static void utf8(std::string& o, uint32_t cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    bool hex4(uint32_t& v) {
        if (i + 4 > s.size()) return false;
        v = 0;
        for (int k = 0; k < 4; k++) {
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool str(std::string& o) {
        ws();
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        o.clear();
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { o += c; continue; }
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
                case '"': o += '"'; break;
                case '\\': o += '\\'; break;
                case '/': o += '/'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2;
                        uint32_t lo;
                        if (!hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(o, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    // Skips any value; numbers are returned through `num` when present.
    bool value(double* num = nullptr, std::string* text = nullptr) {
        ws();
        if (i >= s.size() || ++depth > 64) return false;
        bool ok = true;
        char c = s[i];
        if (c == '"') { std::string t; ok = str(t); if (text) *text = t; }
        else if (c == '{') {
            i++;
            if (!eat('}')) {
                do { std::string k; if (!str(k) || !eat(':') || !value()) { ok = false; break; } } while (eat(','));
                ok = ok && eat('}');
            }
        } else if (c == '[') {
            i++;
            if (!eat(']')) {
                do { if (!value()) { ok = false; break; } } while (eat(','));
                ok = ok && eat(']');
            }
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            size_t st = i;
            while (i < s.size() && (isdigit((unsigned char)s[i]) || strchr("+-.eE", s[i]))) i++;
            if (num) *num = strtod(s.substr(st, i - st).c_str(), nullptr);
        } else if (s.compare(i, 4, "true") == 0 || s.compare(i, 4, "null") == 0) i += 4;
        else if (s.compare(i, 5, "false") == 0) i += 5;
        else ok = false;
        depth--;
        return ok;
    }
};
}  // namespace

bool parseJsonIndex(const std::string& text, JsonIndex& out) {
    out = JsonIndex();
    Json j(text);
    if (text.size() >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) j.i = 3;
    if (!j.eat('{')) return false;
    bool sawList = false;
    if (j.eat('}')) return false;
    do {
        std::string key;
        if (!j.str(key) || !j.eat(':')) return false;
        if (key == "files" && j.eat('[')) {
            sawList = true;
            if (!j.eat(']')) {
                do {
                    JsonIndex::File f{"", 0};
                    j.ws();
                    if (j.i < text.size() && text[j.i] == '"') {  // bare string entry
                        if (!j.str(f.url)) return false;
                    } else {
                        if (!j.eat('{')) return false;
                        if (!j.eat('}')) {
                            do {
                                std::string k;
                                if (!j.str(k) || !j.eat(':')) return false;
                                double num = -1;
                                std::string t;
                                if (!j.value(&num, &t)) return false;
                                if (k == "url") f.url = t;
                                else if (k == "size") f.size = num > 0 ? (uint64_t)num : (t.empty() ? 0 : strtoull(t.c_str(), nullptr, 10));
                            } while (j.eat(','));
                            if (!j.eat('}')) return false;
                        }
                    }
                    if (!f.url.empty()) out.files.push_back(f);
                } while (j.eat(','));
                if (!j.eat(']')) return false;
            }
        } else if (key == "directories" && j.eat('[')) {
            sawList = true;
            if (!j.eat(']')) {
                do {
                    std::string d;
                    j.ws();
                    if (j.i < text.size() && text[j.i] == '"') { if (!j.str(d)) return false; out.directories.push_back(d); }
                    else if (!j.value()) return false;
                } while (j.eat(','));
                if (!j.eat(']')) return false;
            }
        } else if (key == "success" || key == "error") {
            std::string t;
            if (!j.value(nullptr, &t)) return false;
            out.message = t;
            out.isError = key == "error";
            sawList = sawList || out.isError;
        } else if (!j.value()) {
            return false;
        }
    } while (j.eat(','));
    return j.eat('}') && sawList;
}

// Removes "." and ".." segments from an absolute path (RFC 3986 5.2.4), keeping a trailing slash.
static std::string normalizePath(const std::string& path) {
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string seg = path.substr(i, j - i);
        if (seg == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!seg.empty() && seg != ".") parts.push_back(seg);
        i = j + 1;
    }
    std::string out;
    for (const auto& p : parts) out += "/" + p;
    if (out.empty() || (!path.empty() && path.back() == '/')) out += "/";
    return out;
}

// Resolves a link found in an index against the index URL (keeps credentials for the same host).
static std::string resolveLink(const http::Url& base, const std::string& dirPath, const std::string& link) {
    if (link.find("://") != std::string::npos) {
        http::Url u;
        if (http::parseUrl(link, u) && u.userinfo.empty() && !base.userinfo.empty() && u.host == base.host) {
            size_t p = link.find("://") + 3;
            return link.substr(0, p) + base.userinfo + "@" + link.substr(p);
        }
        return link;
    }
    std::string path = (!link.empty() && link[0] == '/') ? link : dirPath + link;
    std::string tail;  // keep ?query / #fragment untouched
    size_t q = path.find_first_of("?#");
    if (q != std::string::npos) { tail = path.substr(q); path = path.substr(0, q); }
    return base.origin() + normalizePath(path) + tail;
}

// Display name for an index entry: the "#name" fragment if present, else the decoded path.
static std::string jsonDisplayName(const std::string& link, const std::string& basePath) {
    size_t hash = link.rfind('#');
    if (hash != std::string::npos && hash + 1 < link.size()) return http::percentDecode(link.substr(hash + 1));
    std::string path = link;
    size_t p = path.find("://");
    if (p != std::string::npos) {
        size_t slash = path.find('/', p + 3);
        path = slash == std::string::npos ? "/" : path.substr(slash);
    }
    size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);
    std::string name = relativeName(path, basePath);
    if (!name.empty() && name[0] == '/') name = name.substr(1);
    return name;
}

// The "success" text of the last list read by this thread (Tinfoil-style index greeting), "" if none.
static thread_local std::string t_listMessage;
std::string lastListMessage() { return t_listMessage; }

// Returns 1 = ok, 0 = not a JSON index, -1 = hard error (err set).
static int jsonList(const http::Url& base, const std::string& basePath, const std::string& firstBody,
                    std::vector<RemoteFile>& out, std::string& err) {
    JsonIndex idx;
    if (!parseJsonIndex(firstBody, idx)) return 0;
    if (idx.isError) { err = "伺服器回報錯誤：" + idx.message; return -1; }
    t_listMessage = idx.message;  // the server's greeting (root index only)

    std::vector<std::pair<std::string, int>> pending;  // directory index URLs still to read
    std::set<std::string> seen;
    auto take = [&](const JsonIndex& ix, const std::string& dirPath, int depth) {
        for (const auto& f : ix.files) {
            std::string url = resolveLink(base, dirPath, f.url);
            size_t hash = url.find('#');
            if (hash != std::string::npos) url = url.substr(0, hash);
            std::string name = jsonDisplayName(f.url.find('#') != std::string::npos ? f.url : url, basePath);
            if (!isInstallable(name) && !isInstallable(http::percentDecode(url))) continue;
            if (!seen.insert(url).second) continue;
            RemoteFile r;
            r.url = url;
            r.displayName = name;
            r.size = f.size;
            if (!r.size) { std::string se; http::probeSize(r.url, r.size, se); }
            out.push_back(r);
            noteFiles(out.size());
        }
        for (const auto& d : ix.directories)
            if (depth + 1 <= kMaxDepth) pending.push_back({resolveLink(base, dirPath, d), depth + 1});
    };
    take(idx, basePath, 0);

    int requests = 0;
    std::set<std::string> visited;
    while (!pending.empty() && requests < kMaxRequests) {
        auto [url, depth] = pending.back();
        pending.pop_back();
        if (!visited.insert(url).second) continue;
        requests++;
        std::vector<uint8_t> body;
        http::Response res;
        std::string e;
        if (!http::requestToMemory(url, http::Request(), body, res, e) || res.status != 200) continue;
        JsonIndex sub;
        if (!parseJsonIndex(std::string(body.begin(), body.end()), sub)) continue;
        http::Url su;
        if (!http::parseUrl(url, su)) continue;
        std::string dir = su.path.substr(0, su.path.rfind('/') + 1);
        take(sub, dir, depth);
    }
    return 1;
}

// ------------------------------------------------------------------ public
bool fetchList(const std::string& serverUrl, std::vector<RemoteFile>& out, ServerKind& kind, std::string& err) {
    t_listMessage.clear();
    out.clear();
    kind = ServerKind::None;
    http::Url base;
    if (!http::parseUrl(serverUrl, base)) {
        err = "網址格式錯誤（請用 http:// 或 https:// 開頭）";
        return false;
    }
    std::string basePath = base.path;
    if (basePath.empty() || basePath.back() != '/') basePath += '/';

    // 1. server.py
    {
        std::vector<uint8_t> body;
        http::Response res;
        std::string e;
        if (!http::requestToMemory(base.origin() + basePath + "api/list.txt", http::Request(), body, res, e)) {
            err = e;  // cannot even connect
            return false;
        }
        if (res.status == 401) { err = http::statusMessage(401); return false; }
        if (res.status == 400 && !base.tls) {
            std::string b(body.begin(), body.end());
            for (auto& c : b) c = (char)tolower((unsigned char)c);
            if (b.find("https") != std::string::npos) {
                err = "這個埠是 HTTPS：請把網址開頭改成 https://";
                return false;
            }
        }
        if (t_progress) t_progress->connected = true;  // the server answers and lets us in
        bool ours = res.headers["server"].find("LanInstaller") != std::string::npos;
        if (res.status == 200 && ours) {
            std::istringstream in(std::string(body.begin(), body.end()));
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                size_t tab = line.find('\t');
                if (tab == std::string::npos) continue;
                RemoteFile f;
                f.size = strtoull(line.substr(0, tab).c_str(), nullptr, 10);
                std::string enc = line.substr(tab + 1);
                f.displayName = http::percentDecode(enc);
                f.url = base.origin() + basePath + "files/" + enc;
                if (isInstallable(f.displayName)) out.push_back(f);
            }
            noteFiles(out.size());
            kind = ServerKind::LanServer;
            return true;
        }
    }

    // 1b. JSON index (Tinfoil-style), served at the URL itself
    {
        std::vector<uint8_t> body;
        http::Response res;
        std::string e;
        if (http::requestToMemory(base.origin() + base.path, http::Request(), body, res, e) && res.status == 200) {
            int jr = jsonList(base, basePath, std::string(body.begin(), body.end()), out, err);
            if (jr < 0) return false;
            if (jr > 0) {
                kind = ServerKind::JsonIndex;
                std::sort(out.begin(), out.end(), [](const RemoteFile& a, const RemoteFile& b) { return a.displayName < b.displayName; });
                return true;
            }
        }
    }

    // 2. WebDAV
    int r = davList(base, basePath, out, err);
    if (r < 0) return false;
    if (r > 0) kind = ServerKind::WebDav;

    // 3. directory listing
    if (r == 0) {
        r = htmlList(base, basePath, out, err);
        if (r < 0) return false;
        if (r == 0) {
            err = "這個網址不是 server.py 伺服器、WebDAV 或網頁目錄";
            return false;
        }
        kind = ServerKind::DirectoryListing;
    }
    std::sort(out.begin(), out.end(), [](const RemoteFile& a, const RemoteFile& b) { return a.displayName < b.displayName; });
    return true;
}

std::string discover(int timeoutMs) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return "";
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

    const char msg[] = "LANINSTALLER_DISCOVER";
    std::vector<uint32_t> targets = {INADDR_BROADCAST};
#ifdef __SWITCH__
    // Some network stacks drop 255.255.255.255; also try the subnet broadcast address.
    u32 ip = 0, mask = 0, gw = 0, dns1 = 0, dns2 = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&ip, &mask, &gw, &dns1, &dns2)) && ip && mask)
        targets.push_back(ntohl((ip & mask) | ~mask));
#endif
    for (uint32_t t : targets) {
        struct sockaddr_in to;
        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        to.sin_port = htons(kDiscoveryPort);
        to.sin_addr.s_addr = htonl(t);
        sendto(fd, msg, sizeof(msg) - 1, 0, (struct sockaddr*)&to, sizeof(to));
    }

    std::string found;
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    if (poll(&p, 1, timeoutMs) > 0 && (p.revents & POLLIN)) {
        char buf[256];
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            std::string reply(buf);
            const std::string prefix = "LANINSTALLER ";
            if (reply.compare(0, prefix.size(), prefix) == 0) found = reply.substr(prefix.size());
            while (!found.empty() && (found.back() == '\n' || found.back() == '\r' || found.back() == ' ')) found.pop_back();
        }
    }
    close(fd);
    return found;
}

std::string loadSetting(const std::string& path, const std::string& key, const std::string& fallback) {
    std::ifstream f(path);
    std::string line, prefix = key + "=";
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, prefix.size(), prefix) == 0) return line.substr(prefix.size());
    }
    return fallback;
}

bool saveSetting(const std::string& path, const std::string& key, const std::string& value) {
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.compare(0, key.size() + 1, key + "=") != 0 && !line.empty()) lines.push_back(line);
        }
    }
    lines.push_back(key + "=" + value);
    for (size_t i = path.find('/', path.find(':') + 2); i != std::string::npos; i = path.find('/', i + 1))
        mkdir(path.substr(0, i).c_str(), 0777);
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    for (const auto& l : lines) f << l << "\n";
    return (bool)f;
}

std::string loadServer(const std::string& path) { return loadSetting(path, "server", ""); }
bool saveServer(const std::string& path, const std::string& url) { return saveSetting(path, "server", url); }

}  // namespace remote
