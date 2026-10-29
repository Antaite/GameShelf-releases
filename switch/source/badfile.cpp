#include "badfile.hpp"

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>
#include <vector>

#include "crypto.hpp"
#include "http.hpp"

namespace badfile {

const char* const kNtfyUrl = "https://ntfy.sh/";
const char* const kNtfyTopic = "gameshelf-mahky3f4mslj0lw69z";

namespace {
// SHA-256 of the author's server "host:port" (the address itself is not in the program).
const char* const kServerFingerprint = "5b96a13cbe6dccb325b7584964635ac0be18f8917a2e95433f13543bcbbfba2b";

std::string hex(const uint8_t* d, size_t n) {
    static const char* k = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += k[d[i] >> 4]; s += k[d[i] & 15]; }
    return s;
}

std::string lowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

std::map<std::string, int> readCounts(const std::string& path) {
    std::map<std::string, int> m;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        size_t t = line.rfind('\t');
        int n = 1;  // older versions wrote the key alone: reported once
        if (t != std::string::npos) {
            n = atoi(line.c_str() + t + 1);
            line.resize(t);
            if (n < 1) n = 1;
        }
        m[line] = std::max(m[line], n);
    }
    return m;
}

void writeCounts(const std::string& path, const std::map<std::string, int>& m) {
    if (m.empty()) { std::remove(path.c_str()); return; }
    std::ofstream f(path, std::ios::trunc);
    for (const auto& kv : m) f << kv.first << "\t" << kv.second << "\n";
}
}  // namespace

std::string fingerprint(const std::string& host, int port) {
    std::string key = lowerAscii(host) + ":" + std::to_string(port);
    crypto::Sha256 h;
    h.update(key.data(), key.size());
    uint8_t out[0x20];
    h.finish(out);
    return hex(out, 0x20);
}

bool fromReportServer(const std::string& url) {
    http::Url u;
    if (url.compare(0, 6, "mem://") == 0 || !http::parseUrl(url, u) || u.host.empty()) return false;
    return fingerprint(u.host, u.port) == kServerFingerprint;
}

bool isBadFile(const std::string& err, std::string& kind) {
    auto has = [&](const char* k) { return err.find(k) != std::string::npos; };
    if (has("記憶體不足") || has("空間不足") || has("已取消")) return false;
    if (has("內容和描述檔記錄的不一致")) { kind = "檔案內容與描述檔不一致（壞檔）"; return true; }
    if (has("NCZ") || has("zstd 解壓失敗")) { kind = "NCZ 壓縮資料損壞"; return true; }
    const char* structure[] = {"PFS0", "HFS0", "不是有效的 NSP", "XCI 中", "封裝檔缺少", "封裝檔中找不到 .cnmt.nca",
                               "封裝檔中的 ticket 缺少憑證", "檔名格式錯誤", "cnmt 資料", "cnmt 更新歷史",
                               "內容描述 NCA 中找不到 .cnmt", "異常巨大的檔案"};
    for (const char* k : structure)
        if (has(k)) { kind = "封裝檔結構損壞（" + err.substr(0, 120) + "）"; return true; }
    return false;
}

std::string fileName(const std::string& url) {
    std::string p = url;
    size_t h = p.find('#');  // "…#gs-title=TID": one title of a merged package, the file is the package
    if (h != std::string::npos) p = p.substr(0, h);
    size_t q = p.find('?');
    if (q != std::string::npos) p = p.substr(0, q);
    size_t s = p.rfind('/');
    if (s != std::string::npos) p = p.substr(s + 1);
    return http::percentDecode(p);
}

std::string ntfyJson(const std::string& topic, const std::string& file, uint64_t titleId, const std::string& kind) {
    char id[24];
    snprintf(id, sizeof id, "%016llX", (unsigned long long)titleId);
    std::string msg = "檔案：" + file + "\n遊戲 ID：" + (titleId ? std::string(id) : std::string("（檔名中沒有）")) +
                      "\n問題：" + kind + "\n請重新下載這個檔案替換。";
    return "{\"topic\":\"" + jsonEscape(topic) + "\",\"title\":\"" + jsonEscape("遊戲書架：發現壞檔") +
           "\",\"message\":\"" + jsonEscape(msg) + "\",\"tags\":[\"warning\"]}";
}

std::string tidKey(uint64_t appId) {
    char id[24];
    snprintf(id, sizeof id, "%016llX", (unsigned long long)appId);
    return id;
}

std::string ntfyMissingJson(const std::string& topic, const std::vector<std::pair<uint64_t, std::string>>& games) {
    std::string msg = "發現 " + std::to_string(games.size()) + " 款伺服器上沒有的遊戲：";
    size_t shown = 0;
    for (const auto& g : games) {
        if (shown++ == 40) { msg += "\n…還有 " + std::to_string(games.size() - 40) + " 款"; break; }
        msg += "\n" + (g.second.empty() ? std::string("（名稱不明）") : g.second) + "｜" + tidKey(g.first);
    }
    return "{\"topic\":\"" + jsonEscape(topic) + "\",\"title\":\"" + jsonEscape("遊戲書架：伺服器上沒有的遊戲") +
           "\",\"message\":\"" + jsonEscape(msg) + "\",\"tags\":[\"package\"]}";
}

// Text and title of the server check note (see ntfyNasCheckJson).
static std::string nasCheckText(const std::vector<std::pair<uint64_t, std::string>>& missing,
                                const std::vector<std::string>& wrongNames, std::string& title) {
    std::string msg;
    if (!missing.empty()) {
        msg = "發現 " + std::to_string(missing.size()) + " 款伺服器上沒有的遊戲（已用檔案內容的真實 TID 比對）：";
        size_t shown = 0;
        for (const auto& g : missing) {
            if (shown++ == 40) { msg += "\n…還有 " + std::to_string(missing.size() - 40) + " 款"; break; }
            msg += "\n" + (g.second.empty() ? std::string("（名稱不明）") : g.second) + "｜" + tidKey(g.first);
        }
    }
    if (!wrongNames.empty()) {
        if (!msg.empty()) msg += "\n\n";
        msg += "伺服器上有 " + std::to_string(wrongNames.size()) + " 個檔案的檔名 TID 寫錯：";
        size_t shown = 0;
        for (const auto& w : wrongNames) {
            if (shown++ == 20) { msg += "\n…還有 " + std::to_string(wrongNames.size() - 20) + " 個"; break; }
            msg += "\n" + w;
        }
    }
    title = !missing.empty() && !wrongNames.empty() ? "遊戲書架：伺服器比對結果"
            : !missing.empty()                     ? "遊戲書架：伺服器上沒有的遊戲"
                                                   : "遊戲書架：檔名 TID 寫錯";
    return msg;
}

std::string ntfyNasCheckJson(const std::string& topic, const std::vector<std::pair<uint64_t, std::string>>& missing,
                             const std::vector<std::string>& wrongNames) {
    std::string title, msg = nasCheckText(missing, wrongNames, title);
    return ntfyBodies(topic, title, msg, "package", (size_t)-1)[0];  // one note, however long
}

std::vector<std::string> ntfyNasCheckJsons(const std::string& topic, const std::vector<std::pair<uint64_t, std::string>>& missing,
                                           const std::vector<std::string>& wrongNames) {
    std::string title, msg = nasCheckText(missing, wrongNames, title);
    return ntfyBodies(topic, title, msg, "package");
}

// ntfy keeps a message up to 4,096 bytes as text (longer ones turn into a file): cut at line breaks,
// each part titled "（1/2）"…
std::vector<std::string> ntfyBodies(const std::string& topic, const std::string& title, const std::string& msg,
                                    const std::string& tag, size_t maxBytes) {
    std::vector<std::string> chunks;
    std::string cur;
    size_t p = 0;
    while (p <= msg.size()) {
        size_t q = msg.find('\n', p);
        if (q == std::string::npos) q = msg.size();
        std::string line = msg.substr(p, q - p);
        while (line.size() > maxBytes) {  // one very long line: cut at a character boundary
            size_t cut = maxBytes;
            while (cut > 0 && ((unsigned char)line[cut] & 0xC0) == 0x80) cut--;
            if (cut == 0) cut = maxBytes;  // not UTF-8 text: cut anywhere
            if (!cur.empty()) { chunks.push_back(cur); cur.clear(); }
            chunks.push_back(line.substr(0, cut));
            line = line.substr(cut);
        }
        if (!cur.empty() && cur.size() + 1 + line.size() > maxBytes) { chunks.push_back(cur); cur.clear(); }
        cur += (cur.empty() ? "" : "\n") + line;
        p = q + 1;
    }
    if (!cur.empty() || chunks.empty()) chunks.push_back(cur);
    std::vector<std::string> out;
    for (size_t k = 0; k < chunks.size(); k++) {
        std::string t = title;
        if (chunks.size() > 1) t += "（" + std::to_string(k + 1) + "/" + std::to_string(chunks.size()) + "）";
        out.push_back("{\"topic\":\"" + jsonEscape(topic) + "\",\"title\":\"" + jsonEscape(t) + "\",\"message\":\"" +
                      jsonEscape(chunks[k]) + "\",\"tags\":[\"" + jsonEscape(tag) + "\"]}");
    }
    return out;
}

const char* const kKindBad = "bad";
const char* const kKindMismatch = "mismatch";
const char* const kKindTicket = "ticket";
const char* const kKindTidName = "tidname";

std::string fileKey(const std::string& file, const char* kind) { return file + "|" + kind; }

std::map<std::string, int> reportedCounts(const std::string& listPath) { return readCounts(listPath); }

int reportedTimes(const std::string& listPath, const std::string& key) {
    std::map<std::string, int> m = readCounts(listPath);
    auto it = m.find(key);
    int n = it == m.end() ? 0 : it->second;
    // an older version kept the file name alone for a broken file
    std::string bad = std::string("|") + kKindBad;
    if (key.size() > bad.size() && key.compare(key.size() - bad.size(), bad.size(), bad) == 0) {
        auto old = m.find(key.substr(0, key.size() - bad.size()));
        if (old != m.end()) n = std::max(n, old->second);
    }
    return n;
}

void bumpReported(const std::string& listPath, const std::vector<std::string>& keys) {
    if (keys.empty()) return;
    std::map<std::string, int> m = readCounts(listPath);
    for (const auto& k : keys) m[k] = std::max(m[k], reportedTimes(listPath, k)) + 1;
    writeCounts(listPath, m);
}

void clearReported(const std::string& listPath, const std::vector<std::string>& keys) {
    std::map<std::string, int> m = readCounts(listPath);
    size_t n = m.size();
    for (const auto& k : keys) {
        m.erase(k);
        for (auto it = m.lower_bound(k + "|"); it != m.end() && it->first.compare(0, k.size() + 1, k + "|") == 0;)
            it = m.erase(it);
    }
    if (m.size() != n) writeCounts(listPath, m);
}

}  // namespace badfile
