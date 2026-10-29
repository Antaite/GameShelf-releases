#include "titledb.hpp"

#include <sys/stat.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>

#include "http.hpp"

#ifdef __SWITCH__
#include <switch.h>
#else
#include <thread>
#endif

namespace titledb {

std::string formatDate(uint32_t d) {
    if (!d) return "";
    char b[16];
    snprintf(b, sizeof(b), "%04u-%02u-%02u", d / 10000, (d / 100) % 100, d % 100);
    return b;
}

// ------------------------------------------------------------------ streaming parser
StreamParser::StreamParser(std::function<void(uint64_t, Entry&&)> onEntry) : onEntry_(std::move(onEntry)) {}

static void putUtf8(std::string& o, uint32_t cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}

// Strings/primitives at depth 2 (inside one title object) are the only ones we keep.
void StreamParser::value(const std::string& v, bool isString) {
    if (key_ == "id" && isString) id_ = strtoull(v.c_str(), nullptr, 16);
    else if (key_ == "name" && isString) cur_.name = v;
    else if (key_ == "iconUrl" && isString) cur_.iconUrl = v;
    else if (key_ == "releaseDate") {
        uint32_t d = 0;
        if (v.size() == 10 && v[4] == '-' && v[7] == '-')  // "2023-05-12"
            d = (uint32_t)(atoi(v.substr(0, 4).c_str()) * 10000 + atoi(v.substr(5, 2).c_str()) * 100 + atoi(v.substr(8, 2).c_str()));
        else
            d = (uint32_t)strtoul(v.c_str(), nullptr, 10);  // 20230512
        cur_.releaseDate = (d > 19800101 && d < 21000101) ? d : 0;
    }
}

void StreamParser::endString() {
    inString_ = false;
    if (!capture_) return;
    capture_ = false;
    if (stack_.size() == 2 && stack_.back() == Ctx::Object) {
        if (expectKey_.back()) key_ = buf_;
        else value(buf_, true);
    }
}

void StreamParser::endPrimitive() {
    inPrim_ = false;
    if (capture_) value(buf_, false);
    capture_ = false;
}

bool StreamParser::feed(const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        char c = (char)data[i];
        if (inString_) {
            if (uLeft_) {
                int h = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                if (h < 0) return false;
                uVal_ = (uVal_ << 4) | (uint32_t)h;
                if (--uLeft_ == 0 && capture_) {
                    if (uVal_ >= 0xD800 && uVal_ < 0xDC00) pendingHigh_ = uVal_;
                    else if (uVal_ >= 0xDC00 && uVal_ < 0xE000 && pendingHigh_) {
                        putUtf8(buf_, 0x10000 + ((pendingHigh_ - 0xD800) << 10) + (uVal_ - 0xDC00));
                        pendingHigh_ = 0;
                    } else putUtf8(buf_, uVal_);
                }
                continue;
            }
            if (escape_) {
                escape_ = false;
                if (c == 'u') { uLeft_ = 4; uVal_ = 0; continue; }
                if (capture_) {
                    switch (c) {
                        case 'n': buf_ += '\n'; break;
                        case 't': buf_ += '\t'; break;
                        case 'r': buf_ += '\r'; break;
                        case 'b': case 'f': break;
                        default: buf_ += c; break;  // " \ /
                    }
                }
                continue;
            }
            if (c == '\\') { escape_ = true; continue; }
            if (c == '"') { endString(); continue; }
            if (capture_ && buf_.size() < 4096) buf_ += c;
            continue;
        }
        if (inPrim_) {
            if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\n' || c == '\r' || c == '\t') endPrimitive();
            else { if (capture_ && buf_.size() < 64) buf_ += c; continue; }
        }
        switch (c) {
            case ' ': case '\n': case '\r': case '\t': break;
            case '{':
            case '[':
                if (stack_.size() > 64) return false;
                stack_.push_back(c == '{' ? Ctx::Object : Ctx::Array);
                expectKey_.push_back(c == '{');
                if (stack_.size() == 2) { id_ = 0; cur_ = Entry(); key_.clear(); }
                break;
            case '}':
            case ']': {
                if (stack_.empty()) return false;
                if (stack_.size() == 2 && stack_.back() == Ctx::Object) {
                    if (id_) { onEntry_(id_, std::move(cur_)); count_++; }
                    id_ = 0;
                    cur_ = Entry();
                }
                stack_.pop_back();
                expectKey_.pop_back();
                if (stack_.empty()) done_ = true;
                break;
            }
            case ':':
                if (!expectKey_.empty()) expectKey_.back() = false;
                break;
            case ',':
                if (!stack_.empty() && stack_.back() == Ctx::Object) expectKey_.back() = true;
                break;
            case '"':
                inString_ = true;
                buf_.clear();
                pendingHigh_ = 0;
                // capture keys of title objects, and values of the keys we want
                capture_ = stack_.size() == 2 && stack_.back() == Ctx::Object &&
                           (expectKey_.back() || key_ == "id" || key_ == "name" || key_ == "iconUrl" || key_ == "releaseDate");
                break;
            default:
                inPrim_ = true;
                buf_.assign(1, c);
                capture_ = stack_.size() == 2 && stack_.back() == Ctx::Object && !expectKey_.back() && key_ == "releaseDate";
                break;
        }
    }
    return true;
}

// ------------------------------------------------------------------ database
// Same title listed more than once in one region (standard edition, later re-release, bundle…):
// the entry with the earliest release date wins; missing fields are filled from the others.
void keepEarliest(std::unordered_map<uint64_t, Entry>& m, uint64_t id, Entry&& e) {
    auto it = m.find(id);
    if (it == m.end()) { m.emplace(id, std::move(e)); return; }
    Entry& cur = it->second;
    bool takeNew = e.releaseDate && (!cur.releaseDate || e.releaseDate < cur.releaseDate);
    if (takeNew) std::swap(cur, e);  // `e` is now the loser
    if (cur.name.empty()) cur.name = std::move(e.name);
    if (cur.iconUrl.empty()) cur.iconUrl = std::move(e.iconUrl);
    if (!cur.releaseDate) cur.releaseDate = e.releaseDate;
}

static const char* kCacheHeader = "titledb-cache v2";

struct Db::Impl {
    std::vector<std::string> urls;
    std::string cachePath;
    std::unordered_map<uint64_t, Entry> table;  // guarded by mu
    std::atomic<int> state{(int)State::Idle};
    std::atomic<bool> quit{false}, busy{false};
    std::atomic<uint32_t> generation{0};
    int64_t cacheTime = 0;   // when the cached data was downloaded (unix time)
    bool complete = false;   // every region downloaded that time
    bool forceNext = false;  // argument for the next background run
    mutable std::mutex mu;
    std::string message;
#ifdef __SWITCH__
    Thread thread;
    bool started = false;
#else
    std::thread thread;
#endif

    void setMsg(const std::string& m) {
        std::lock_guard<std::mutex> g(mu);
        message = m;
    }

    static std::string clean(std::string s) {
        for (auto& ch : s)
            if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
        return s;
    }

    // Older cache formats are not read: they kept the wrong date for titles listed twice.
    bool loadCache() {
        std::ifstream f(cachePath);
        if (!f) return false;
        std::string line;
        std::getline(f, line);
        if (line.compare(0, strlen(kCacheHeader), kCacheHeader) != 0) return false;
        long long t = 0;
        int full = 0;
        if (sscanf(line.c_str() + strlen(kCacheHeader), " %lld %d", &t, &full) < 1) return false;
        std::unordered_map<uint64_t, Entry> loaded;
        while (std::getline(f, line)) {
            size_t a = line.find('\t'), b = line.find('\t', a + 1), c = line.find('\t', b + 1);
            if (a == std::string::npos || b == std::string::npos || c == std::string::npos) continue;
            Entry e;
            uint64_t id = strtoull(line.substr(0, a).c_str(), nullptr, 16);
            e.releaseDate = (uint32_t)strtoul(line.substr(a + 1, b - a - 1).c_str(), nullptr, 10);
            e.name = line.substr(b + 1, c - b - 1);
            e.iconUrl = line.substr(c + 1);
            if (id) loaded[id] = std::move(e);
        }
        if (loaded.empty()) return false;
        std::lock_guard<std::mutex> g(mu);
        table.swap(loaded);
        cacheTime = t;
        complete = full != 0;
        return true;
    }

    void saveCache() {
        for (size_t i = cachePath.find('/', cachePath.find(':') + 2); i != std::string::npos; i = cachePath.find('/', i + 1))
            mkdir(cachePath.substr(0, i).c_str(), 0777);
        std::string tmp = cachePath + ".tmp";
        {
            std::ofstream f(tmp, std::ios::trunc);
            std::lock_guard<std::mutex> g(mu);
            f << kCacheHeader << ' ' << (long long)cacheTime << ' ' << (complete ? 1 : 0) << '\n';
            char id[20];
            for (const auto& kv : table) {
                snprintf(id, sizeof(id), "%016llX", (unsigned long long)kv.first);
                f << id << '\t' << kv.second.releaseDate << '\t' << clean(kv.second.name) << '\t' << clean(kv.second.iconUrl) << '\n';
            }
        }
        remove(cachePath.c_str());
        rename(tmp.c_str(), cachePath.c_str());
    }

    // One source: either the prepared notes file ("titledb-mini v1 …" + TSV lines, small) or a
    // titledb region JSON file (large, fallback). The format is recognised from the first byte.
    bool download(const std::string& url, std::unordered_map<uint64_t, Entry>& out) {
        std::unordered_map<uint64_t, Entry> fresh;
        StreamParser p([&](uint64_t id, Entry&& e) { keepEarliest(fresh, id, std::move(e)); });
        enum { Unknown, Json, Tsv } kind = Unknown;
        bool headerOk = false;
        std::string line;
        auto tsvLine = [&](const std::string& l) {
            if (!headerOk) { headerOk = l.compare(0, 15, "titledb-mini v1") == 0; return headerOk; }
            size_t a = l.find('\t'), b = l.find('\t', a + 1), c = l.find('\t', b + 1);
            if (a == std::string::npos || b == std::string::npos || c == std::string::npos) return true;
            uint64_t id = strtoull(l.substr(0, a).c_str(), nullptr, 16);
            if (!id) return true;
            Entry e;
            uint32_t d = (uint32_t)strtoul(l.substr(a + 1, b - a - 1).c_str(), nullptr, 10);
            e.releaseDate = (d > 19800101 && d < 21000101) ? d : 0;
            e.name = l.substr(b + 1, c - b - 1);
            e.iconUrl = l.substr(c + 1);
            if (!e.iconUrl.empty() && e.iconUrl.back() == '\r') e.iconUrl.pop_back();
            keepEarliest(fresh, id, std::move(e));
            return true;
        };
        uint64_t got = 0, lastShown = 0;
        bool bad = false;
        std::string err;
        http::Request req;
        http::Response res;
        req.onHeaders = [&](const http::Response& r) { return r.status == 200; };
        bool ok = http::request(url, req, [&](const uint8_t* d, size_t n) {
            if (quit) return false;
            size_t i = 0;
            if (kind == Unknown) {
                while (i < n && (d[i] == ' ' || d[i] == '\n' || d[i] == '\r' || d[i] == '\t' || d[i] == 0xEF || d[i] == 0xBB || d[i] == 0xBF)) i++;
                if (i < n) kind = d[i] == '{' ? Json : Tsv;
            }
            if (kind == Json) {
                if (!p.feed(d + i, n - i)) { bad = true; return false; }
            } else if (kind == Tsv) {
                for (; i < n; i++) {
                    if (d[i] == '\n') {
                        if (!tsvLine(line)) { bad = true; return false; }
                        line.clear();
                    } else if (line.size() < 4096) {
                        line += (char)d[i];
                    }
                }
            }
            got += n;
            if (got - lastShown > 512 * 1024) {
                lastShown = got;
                char b[96];
                snprintf(b, sizeof(b), "正在更新遊戲資料庫… %.1f MB", got / 1048576.0);
                setMsg(b);
            }
            return true;
        }, res, err);
        if (ok && kind == Tsv && !line.empty()) tsvLine(line);
        bool finished = kind == Json ? p.finished() : (kind == Tsv && headerOk);
        if (!ok || bad || quit || !finished || fresh.empty()) return false;
        out.swap(fresh);
        return true;
    }

    static int64_t now() { return (int64_t)time(nullptr); }

    bool needsRefresh() const {
        if (!complete) return true;
        int64_t t = now();
        if (t < 1500000000) return false;  // clock not set: keep what we have
        return t - cacheTime > kRefreshDays * 86400LL || t < cacheTime;
    }

    // Tries the sources in order (notes file first, then the fallback) and replaces the table with
    // the first one that works.
    bool downloadAll(bool hadData) {
        for (size_t i = 0; i < urls.size(); i++) {
            const std::string& u = urls[i];
            if (u.empty()) continue;
            if (quit) return false;
            setMsg(i == 0 ? "正在更新遊戲資料庫…" : "正在更新遊戲資料庫（改用備用來源，檔案較大）…");
            std::unordered_map<uint64_t, Entry> fresh;
            if (!download(u, fresh)) continue;
            size_t n = fresh.size();
            {
                std::lock_guard<std::mutex> g(mu);
                table.swap(fresh);
                cacheTime = now();
                complete = true;
            }
            saveCache();
            generation++;
            setMsg("遊戲資料庫已更新（" + std::to_string(n) + " 款遊戲）。");
            return true;
        }
        setMsg(hadData ? "無法更新遊戲資料庫，先沿用舊資料。" : "無法下載遊戲資料庫：發售日與封面不會顯示。");
        return false;
    }

    bool run(bool force) {
        busy = true;
        bool have = generation > 0;
        if (!have) {
            state = (int)State::Loading;
            setMsg("正在讀取遊戲資料庫…");
            if (loadCache()) {
                have = true;
                generation++;
                state = (int)State::Ready;
                setMsg("");
            }
        }
        bool ok = have;
        if (force || !have || needsRefresh()) {
            if (!have) setMsg("正在下載遊戲資料庫（第一次使用，只需下載一次）…");
            ok = downloadAll(have) || have;
            if (generation > 0) state = (int)State::Ready;
        }
        if (!ok) state = (int)State::Failed;
        busy = false;
        return ok;
    }

    void start(bool force) {
#ifdef __SWITCH__
        if (started) {
            threadWaitForExit(&thread);
            threadClose(&thread);
            started = false;
        }
        forceNext = force;
        busy = true;
        // not on the core that draws the screen (reading the cache at start made the list stutter)
        int core = -2;
        u64 mask = 0;
        int self = (int)svcGetCurrentProcessorNumber();
        svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
        for (int c = 0; c < 4 && core < 0; c++)
            if (((mask >> c) & 1) && c != self) core = c;
        if (R_SUCCEEDED(threadCreate(&thread, [](void* p) { ((Impl*)p)->run(((Impl*)p)->forceNext); }, this, nullptr, 0x40000, 0x2F, core))) {
            started = R_SUCCEEDED(threadStart(&thread));
            if (!started) threadClose(&thread);
        }
        if (!started) busy = false;
#else
        (void)force;  // host: tests call runNow()
#endif
    }
};

Db::Db(std::vector<std::string> urls, std::string cachePath) : d_(new Impl) {
    d_->urls = std::move(urls);
    d_->cachePath = std::move(cachePath);
    d_->start(false);
}

Db::~Db() {
    d_->quit = true;  // an unfinished download stops at its next chunk
#ifdef __SWITCH__
    if (d_->started) {
        threadWaitForExit(&d_->thread);  // a running download ends when its connection does
        threadClose(&d_->thread);
    }
#else
    if (d_->thread.joinable()) d_->thread.join();
#endif
    delete d_;
}

Db::State Db::state() const { return (State)d_->state.load(); }

std::string Db::message() const {
    std::lock_guard<std::mutex> g(d_->mu);
    return d_->message;
}

bool Db::find(uint64_t id, Entry& out) const {
    if (d_->generation == 0) return false;
    std::lock_guard<std::mutex> g(d_->mu);
    auto it = d_->table.find(id);
    if (it == d_->table.end()) return false;
    out = it->second;
    return true;
}

uint32_t Db::generation() const { return d_->generation.load(); }
bool Db::busy() const { return d_->busy.load(); }

int64_t Db::downloadedAt() const {
    std::lock_guard<std::mutex> g(d_->mu);
    return d_->cacheTime;
}

bool Db::refresh() {
    if (d_->busy) return false;
    d_->start(true);
    return true;
}

bool Db::runNow(bool force) { return d_->run(force); }

}  // namespace titledb
