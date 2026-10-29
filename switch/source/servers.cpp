#include "servers.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace servers {

Server* Store::find(const std::string& id) {
    for (auto& s : list)
        if (s.id == id) return &s;
    return nullptr;
}
const Server* Store::find(const std::string& id) const {
    for (const auto& s : list)
        if (s.id == id) return &s;
    return nullptr;
}
int Store::indexOf(const std::string& id) const {
    for (size_t i = 0; i < list.size(); i++)
        if (list[i].id == id) return (int)i;
    return -1;
}
std::string Store::newId() const {
    for (int n = 1;; n++) {
        std::string id = "s" + std::to_string(n);
        if (!find(id)) return id;
    }
}
void Store::remove(const std::string& id) {
    int i = indexOf(id);
    if (i < 0) return;
    list.erase(list.begin() + i);
    if (current == id) current = list.empty() ? "" : list[(size_t)i < list.size() ? (size_t)i : list.size() - 1].id;
}

// Values are written as they are; only line breaks are escaped (the keyboard cannot type them).
static std::string esc(const std::string& v) {
    std::string o;
    for (char c : v) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else o += c;
    }
    return o;
}
static std::string unesc(const std::string& v) {
    std::string o;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            char n = v[++i];
            o += n == 'n' ? '\n' : n == 'r' ? '\r' : n;
        } else {
            o += v[i];
        }
    }
    return o;
}

bool load(const std::string& path, Store& out) {
    std::ifstream f(path);
    if (!f) return false;
    out = Store();
    std::string line;
    Server* cur = nullptr;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            Server s;
            s.id = line.substr(1, line.size() - 2);
            if (s.id.empty() || out.find(s.id)) { cur = nullptr; continue; }
            out.list.push_back(s);
            cur = &out.list.back();
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = unesc(line.substr(eq + 1));
        if (!cur) {
            if (k == "current") out.current = v;
            continue;
        }
        if (k == "name") cur->name = v;
        else if (k == "url") cur->url = v;
        else if (k == "user") cur->user = v;
        else if (k == "pass") cur->pass = v;
    }
    for (auto& s : out.list)
        if (s.name.empty()) s.name = hostOf(s.url);
    if (!out.find(out.current)) out.current = out.list.empty() ? "" : out.list.front().id;
    return true;
}

bool save(const std::string& path, const Store& s) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return false;
        f << "# 遊戲書架 servers (written by the app)\n";
        f << "current=" << esc(s.current) << "\n";
        for (const auto& v : s.list) {
            f << "\n[" << v.id << "]\n";
            f << "name=" << esc(v.name) << "\n";
            f << "url=" << esc(v.url) << "\n";
            f << "user=" << esc(v.user) << "\n";
            f << "pass=" << esc(v.pass) << "\n";
        }
        f.flush();
        if (!f) return false;
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static std::string pdec(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0) {
            o += (char)(hexv(s[i + 1]) * 16 + hexv(s[i + 2]));
            i += 2;
        } else {
            o += s[i];
        }
    }
    return o;
}
// %-encodes everything but unreserved characters (for the user:pass part of a URL)
static std::string penc(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
            c == '_' || c == '~')
            o += (char)c;
        else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

static std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}

std::string checkAddress(const std::string& typed) {
    std::string t = trim(typed);
    if (t.empty()) return "位址是空的";
    // full-width characters typed with the Chinese keyboard
    static const char* wide[][2] = {{"：", ":"}, {"／", "/"}, {"．", "."}, {"。", "."}, {"＠", "@"}};
    for (auto& w : wide)
        if (t.find(w[0]) != std::string::npos)
            return std::string("位址裡有全形的「") + w[0] + "」，請改成半形的「" + w[1] + "」";
    std::string low = t;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    size_t sch = low.find("://");
    if (sch != std::string::npos && low.compare(0, sch, "http") != 0 && low.compare(0, sch, "https") != 0)
        return "開頭要是 http:// 或 https://";
    if (sch == std::string::npos && (low.compare(0, 5, "http:") == 0 || low.compare(0, 6, "https:") == 0))
        return "開頭少了「//」：應該是 http:// 或 https://";
    std::string rest = sch == std::string::npos ? t : t.substr(sch + 3);
    size_t slash = rest.find('/');
    std::string auth = slash == std::string::npos ? rest : rest.substr(0, slash);
    size_t at = auth.rfind('@');
    if (at != std::string::npos) auth = auth.substr(at + 1);
    if (auth.find(' ') != std::string::npos) return "位址中間有空白";
    std::string host = auth;
    size_t colon = auth.rfind(':');
    if (colon != std::string::npos) {
        host = auth.substr(0, colon);
        std::string port = auth.substr(colon + 1);
        if (port.empty()) return "冒號後面少了埠號（例如 :8001）";
        for (char c : port)
            if (c < '0' || c > '9') return "埠號只能是數字（例如 :8001）";
        long p = atol(port.c_str());
        if (p < 1 || p > 65535) return "埠號要在 1 到 65535 之間";
    }
    if (host.empty()) return "少了主機位址（例如 192.168.1.10 或伺服器的網址）";
    for (char c : host)
        if (!(isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_' || (unsigned char)c >= 0x80))
            return std::string("主機位址裡有不該出現的字元「") + c + "」";
    return "";
}

void splitAddress(const std::string& typed, std::string& url, std::string& user, std::string& pass) {
    std::string t = trim(typed);
    user.clear();
    pass.clear();
    if (t.empty()) { url.clear(); return; }
    size_t sch = t.find("://");
    std::string scheme = "http://";
    if (sch != std::string::npos) {
        scheme = t.substr(0, sch + 3);
        for (auto& c : scheme) c = (char)tolower((unsigned char)c);
        t = t.substr(sch + 3);
    }
    size_t slash = t.find('/');
    std::string auth = slash == std::string::npos ? t : t.substr(0, slash);
    std::string rest = slash == std::string::npos ? "/" : t.substr(slash);
    size_t at = auth.rfind('@');
    if (at != std::string::npos) {
        std::string ui = auth.substr(0, at);
        auth = auth.substr(at + 1);
        size_t colon = ui.find(':');
        user = pdec(colon == std::string::npos ? ui : ui.substr(0, colon));
        pass = colon == std::string::npos ? "" : pdec(ui.substr(colon + 1));
    }
    if (rest.empty() || rest.back() != '/') {
        // a folder address: keep a trailing '/', unless it points at a file (index .json / .txt)
        size_t lastSlash = rest.rfind('/');
        std::string leaf = rest.substr(lastSlash + 1);
        if (leaf.find('.') == std::string::npos) rest += '/';
    }
    url = scheme + auth + rest;
}

std::string fullUrl(const Server& s) {
    if (s.user.empty() && s.pass.empty()) return s.url;
    size_t sch = s.url.find("://");
    if (sch == std::string::npos) return s.url;
    return s.url.substr(0, sch + 3) + penc(s.user) + (s.pass.empty() ? "" : ":" + penc(s.pass)) + "@" +
           s.url.substr(sch + 3);
}

std::string hostOf(const std::string& url) {
    std::string h = url;
    size_t sch = h.find("://");
    bool tls = sch != std::string::npos && (h.compare(0, 5, "https") == 0 || h.compare(0, 5, "HTTPS") == 0);
    if (sch != std::string::npos) h = h.substr(sch + 3);
    size_t sl = h.find('/');
    if (sl != std::string::npos) h = h.substr(0, sl);
    size_t at = h.rfind('@');
    if (at != std::string::npos) h = h.substr(at + 1);
    size_t colon = h.rfind(':');
    if (colon != std::string::npos) {
        std::string port = h.substr(colon + 1);
        if ((tls && port == "443") || (!tls && port == "80")) h = h.substr(0, colon);
    }
    return h;
}

bool migrateOld(Store& s, const std::string& oldUrl, const std::string& name) {
    if (!s.list.empty() || oldUrl.empty()) return false;
    Server v;
    v.id = s.newId();
    v.name = name;
    splitAddress(oldUrl, v.url, v.user, v.pass);
    s.list.push_back(v);
    s.current = v.id;
    return true;
}

}  // namespace servers
