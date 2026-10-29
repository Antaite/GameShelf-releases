#include "covers.hpp"

#include <cstdlib>

#include <ctime>

#include <algorithm>

#include <dirent.h>

#include <sys/stat.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>

#include "http.hpp"

#include <set>

#ifdef __SWITCH__
#include <switch.h>
#else
#include <chrono>
#include <condition_variable>
#include <thread>
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#if defined(__aarch64__) && !defined(__x86_64__)
#define STBI_NEON  // the Switch CPU decodes JPEG several times faster with its vector unit
#else
#define STBI_NO_SIMD
#endif
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

namespace covers {

// ------------------------------------------------------------------ title ids
uint64_t titleIdFromName(const std::string& n) {
    uint64_t fallback = 0;
    for (size_t i = 0; i + 16 <= n.size(); i++) {
        bool hex = true;
        for (size_t k = 0; k < 16 && hex; k++) hex = isxdigit((unsigned char)n[i + k]) != 0;
        if (!hex) continue;
        bool boundaryL = i == 0 || !isxdigit((unsigned char)n[i - 1]);
        bool boundaryR = i + 16 == n.size() || !isxdigit((unsigned char)n[i + 16]);
        if (!boundaryL || !boundaryR) continue;
        uint64_t v = strtoull(n.substr(i, 16).c_str(), nullptr, 16);
        if ((v >> 56) == 0x01) return v;  // application ids start with 01
        if (!fallback) fallback = v;
    }
    return fallback;
}

uint64_t baseTitleId(uint64_t tid) {
    uint64_t low = tid & 0xFFF;
    if (low == 0) return tid;                 // base game
    if (low == 0x800) return tid & ~0xFFFULL;  // update
    return (tid & ~0xFFFULL) - 0x1000;        // add-on content
}

// ------------------------------------------------------------------ decoding
bool decode(const std::vector<uint8_t>& file, int box, Image& out) {
    if (file.size() < 16) return false;
    int w = 0, h = 0, comp = 0;
    stbi_uc* src = stbi_load_from_memory(file.data(), (int)file.size(), &w, &h, &comp, 4);
    if (!src || w <= 0 || h <= 0) {
        if (src) stbi_image_free(src);
        return false;
    }
    double scale = (double)box / (w > h ? w : h);
    out.w = (int)(w * scale + 0.5);
    out.h = (int)(h * scale + 0.5);
    if (out.w < 1) out.w = 1;
    if (out.h < 1) out.h = 1;
    out.px.assign((size_t)out.w * out.h, 0);
    // area-average downscale (or nearest upscale), good enough for icons
    for (int y = 0; y < out.h; y++) {
        int sy0 = (int)(y / scale), sy1 = (int)((y + 1) / scale);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > h) sy1 = h;
        for (int x = 0; x < out.w; x++) {
            int sx0 = (int)(x / scale), sx1 = (int)((x + 1) / scale);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > w) sx1 = w;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int yy = sy0; yy < sy1; yy++)
                for (int xx = sx0; xx < sx1; xx++) {
                    const stbi_uc* p = src + ((size_t)yy * w + xx) * 4;
                    r += p[0]; g += p[1]; b += p[2]; n++;
                }
            out.px[(size_t)y * out.w + x] = 0xFF000000u | ((b / n) << 16) | ((g / n) << 8) | (r / n);
        }
    }
    stbi_image_free(src);
    return true;
}

// Percent-encodes a whole URL for use as a query value.
static std::string encodeQueryValue(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

// The address the console really asks for an eShop icon: through the proxy for Nintendo hosts.
std::string proxiedUrl(const std::string& proxy, const std::string& icon) {
    if (icon.empty() || proxy.empty()) return icon;
    size_t a = icon.find("://");
    if (a == std::string::npos) return icon;
    size_t b = icon.find_first_of("/?", a + 3);
    std::string host = icon.substr(a + 3, b == std::string::npos ? std::string::npos : b - a - 3);
    for (auto& ch : host) ch = (char)tolower((unsigned char)ch);
    bool nintendo = host == "nintendo.net" || (host.size() > 13 && host.compare(host.size() - 13, 13, ".nintendo.net") == 0) ||
                    host == "nintendo.com" || (host.size() > 13 && host.compare(host.size() - 13, 13, ".nintendo.com") == 0);
    if (!nintendo) return icon;
    std::string out = proxy;
    size_t p = out.find("{url}");
    if (p == std::string::npos) return icon;
    out.replace(p, 5, encodeQueryValue(icon));
    return out;
}

// ------------------------------------------------------------------ cache
struct Cache::Impl {
    std::string dir, tmpl;
    std::function<std::string(uint64_t)> lookup;
    std::function<void(const std::string&)> logger;
    int logged = 0;                         // failure lines written (capped)
    std::map<uint64_t, int64_t> retryAt;    // network trouble: when to try that cover again
    int box;
    std::mutex mu;
    std::map<uint64_t, State> state;
    std::map<uint64_t, std::shared_ptr<const Image>> images;
    std::map<uint64_t, uint64_t> touched;  // id -> use counter (decoded pictures kept: the most recent)
    uint64_t useClock = 0;
    std::set<uint64_t> inFlight;   // being loaded right now (not queued again)
    size_t maxImages = 100;

    // Too many decoded pictures in memory (browsing many pages of the cover wall): forget the ones
    // used longest ago. Their files stay on the SD card, so coming back loads them quickly. Locked.
    void trimImages() {
        while (images.size() > maxImages) {
            uint64_t oldest = 0, when = UINT64_MAX;
            for (const auto& kv : images) {
                auto t = touched.find(kv.first);
                uint64_t w = t == touched.end() ? 0 : t->second;
                if (w < when) { when = w; oldest = kv.first; }
            }
            images.erase(oldest);
            touched.erase(oldest);
            state[oldest] = State::None;
        }
    }
    // Covers on the SD card come from one worker (fast: never behind a download); the ones that must be
    // downloaded go to kNetWorkers others, each with its own kept connection.
    static const int kNetWorkers = 3;
    std::deque<uint64_t> queue;     // front = most wanted (SD card / console first)
    std::deque<uint64_t> netQueue;  // to download, front = most wanted
    std::deque<uint64_t> later, netLater;  // prefetch (next / previous page): only when nothing else waits
    std::set<uint64_t> laterSet;
    http::Session* sessions[kNetWorkers] = {nullptr, nullptr, nullptr};
    std::map<uint64_t, int64_t> lastUsed;  // id -> unix time last shown (for trimCache)
    bool paused = false, quit = false;
#ifdef __SWITCH__
    Thread threads[1 + kNetWorkers];
    bool started[1 + kNetWorkers] = {false, false, false, false};
    struct Arg { Impl* d; int k; } args[1 + kNetWorkers];
    // One event per worker: libnx's UEvent must not be waited on by several threads at once (its
    // waiter list breaks and the next signal crashes: v3.0.12 test build).
    UEvent wake[1 + kNetWorkers];
#else
    std::thread threads[1 + kNetWorkers];
    std::condition_variable cv;
#endif

    std::string path(uint64_t id) const {
        char b[32];
        snprintf(b, sizeof(b), "/%016llX.jpg", (unsigned long long)id);
        return dir + b;
    }

    static bool readFile(const std::string& p, std::vector<uint8_t>& out) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        out.assign(std::istreambuf_iterator<char>(f), {});
        return !out.empty();
    }

    // Covers are shown at most `box` pixels wide: a bigger picture (eShop icons are 1024x1024, about
    // 150 KB) is kept on the SD card already scaled (`img`, about 30 KB) so many more fit in the cache.
    // False when the file is already small enough.
    bool shrink(const std::vector<uint8_t>& file, const Image& img, std::vector<uint8_t>& out) {
        int w = 0, h = 0, comp = 0;
        if (!stbi_info_from_memory(file.data(), (int)file.size(), &w, &h, &comp) || (w <= box && h <= box)) return false;
        out.clear();
        int ok = stbi_write_jpg_to_func(
            [](void* ctx, void* d, int n) {
                auto* v = (std::vector<uint8_t>*)ctx;
                v->insert(v->end(), (uint8_t*)d, (uint8_t*)d + n);
            },
            &out, img.w, img.h, 4, img.px.data(), 90);
        return ok && !out.empty() && out.size() < file.size();
    }

    void writeFile(uint64_t id, const std::vector<uint8_t>& data) {
        for (size_t i = dir.find('/', dir.find(':') + 2); ; i = dir.find('/', i + 1)) {
            mkdir((i == std::string::npos ? dir : dir.substr(0, i)).c_str(), 0777);
            if (i == std::string::npos) break;
        }
        std::ofstream f(path(id), std::ios::binary | std::ios::trunc);
        f.write((const char*)data.data(), (std::streamsize)data.size());
    }

    bool fromConsole(uint64_t id, std::vector<uint8_t>& out) {
#ifdef __SWITCH__
        auto* buf = new NsApplicationControlData;
        u64 size = 0;
        Result rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, id, buf, sizeof(*buf), &size);
        bool ok = R_SUCCEEDED(rc) && size > sizeof(buf->nacp);
        if (ok) out.assign(buf->icon, buf->icon + (size - sizeof(buf->nacp)));
        delete buf;
        return ok;
#else
        (void)id; (void)out;
        return false;
#endif
    }

    // Tries the eShop icon (from the game notes), then the cover site. `trouble` is set when a
    // source could not be asked properly (so "not found" is not certain).
    bool fromNetwork(uint64_t id, std::vector<uint8_t>& out, bool& trouble, http::Session* ses) {
        std::function<std::string(uint64_t)> fn;
        {
            std::lock_guard<std::mutex> g(mu);
            fn = lookup;
        }
        auto tryUrl = [&](const std::string& u) {
            Fetch f = fetch(id, u, out, ses);
            if (f == kTrouble) trouble = true;
            if (f != kGot) return false;
            Image probe;
            if (decode(out, 8, probe)) return true;
            char idHex[20];
            snprintf(idHex, sizeof(idHex), "%016llX", (unsigned long long)id);
            log(std::string("cover ") + idHex + ": " + hostOf(u) + " not a JPEG/PNG (" + std::to_string(out.size()) + " bytes)");
            out.clear();
            return false;
        };
        if (fn) {
            std::string u = fn(id);
            if (!u.empty() && tryUrl(u)) return true;
        }
        if (tmpl.empty()) return false;
        char hex[17];
        snprintf(hex, sizeof(hex), "%016llX", (unsigned long long)id);
        std::string url = tmpl;
        size_t p;
        while ((p = url.find("{id}")) != std::string::npos) url.replace(p, 4, hex);
        return tryUrl(url);
    }

    // Result of one download: the picture, a clear "no such picture" from the site, or trouble
    // (network, timeout, server error) worth trying again later.
    enum Fetch { kGot, kNone, kTrouble };

    void log(const std::string& line) {
        std::function<void(const std::string&)> fn;
        {
            std::lock_guard<std::mutex> g(mu);
            if (!logger || logged >= 40) return;  // the first failures say enough
            logged++;
            fn = logger;
        }
        fn(line);
    }

    static std::string hostOf(const std::string& url) {
        size_t a = url.find("://");
        a = a == std::string::npos ? 0 : a + 3;
        size_t b = url.find_first_of("/?", a);
        return url.substr(a, b == std::string::npos ? std::string::npos : b - a);
    }

    Fetch fetch(uint64_t id, const std::string& url, std::vector<uint8_t>& out, http::Session* ses) {
        std::string err;
        out.clear();
        size_t limit = 4 * 1024 * 1024;
        bool encoded = url.find('?') != std::string::npos;  // e.g. the image proxy address
        http::Response res;
        bool tooBig = false, ok;
        if (ses) {  // kept connection: no new TLS handshake per cover
            ok = ses->get(url, limit, out, res.status, err, encoded);
            tooBig = !ok && err == "檔案太大";
        } else {
            http::Request req;
            req.encodedUrl = encoded;
            ok = http::request(url, req, [&](const uint8_t* d, size_t n) {
                if (out.size() + n > limit) { tooBig = true; return false; }
                out.insert(out.end(), d, d + n);
                return true;
            }, res, err);
        }
        char idHex[20];
        snprintf(idHex, sizeof(idHex), "%016llX", (unsigned long long)id);
        if (ok && res.status == 200 && !out.empty()) return kGot;
        Fetch r;
        std::string why;
        if (tooBig) { r = kNone; why = "picture larger than 4 MB"; }
        else if (res.status == 404 || res.status == 410 || (ok && res.status == 200)) { r = kNone; why = "HTTP " + std::to_string(res.status) + (ok && res.status == 200 ? " (empty)" : ""); }
        else if (res.status) { r = kTrouble; why = "HTTP " + std::to_string(res.status); }
        else { r = kTrouble; why = err.empty() ? "no answer" : err; }
        log(std::string("cover ") + idHex + ": " + hostOf(url) + " " + why);
        out.clear();
        return r;
    }

    // From the SD card (or the console's own icon of an installed game). nullptr: must be downloaded.
    std::shared_ptr<Image> loadLocal(uint64_t id) {
        {
            std::lock_guard<std::mutex> g(mu);
            lastUsed[id] = (int64_t)time(nullptr);
        }
        std::vector<uint8_t> data;
        auto img = std::make_shared<Image>();
        if (readFile(path(id), data) && decode(data, box, *img)) {
            std::vector<uint8_t> small;
            if (shrink(data, *img, small)) writeFile(id, small);  // a big one kept by an older version
            return img;
        }
        if (fromConsole(id, data) && decode(data, box, *img)) {
            std::vector<uint8_t> small;
            writeFile(id, shrink(data, *img, small) ? small : data);
            return img;
        }
        return nullptr;
    }

    std::shared_ptr<Image> loadNet(uint64_t id, bool& trouble, http::Session* ses) {
        std::vector<uint8_t> data;
        auto img = std::make_shared<Image>();
        if (fromNetwork(id, data, trouble, ses) && decode(data, box, *img)) {
            std::vector<uint8_t> small;
            writeFile(id, shrink(data, *img, small) ? small : data);
            return img;
        }
        return nullptr;
    }

    std::shared_ptr<Image> load(uint64_t id, bool allowNetwork) {
        auto img = loadLocal(id);
        bool trouble = false;
        if (!img && allowNetwork) img = loadNet(id, trouble, nullptr);
        return img;
    }

    // a picture is ready (locked)
    void ready(uint64_t id, std::shared_ptr<Image> img) {
        images[id] = img;
        touched[id] = ++useClock;
        state[id] = State::Ready;
        retryAt.erase(id);
        trimImages();
    }

    void waitWork(int k) {  // unlocked; returns after a signal or 100 ms (k: this worker)
#ifdef __SWITCH__
        waitSingle(waiterForUEvent(&wake[k]), 100000000LL);
#else
        (void)k;
        std::unique_lock<std::mutex> lk(mu);
        cv.wait_for(lk, std::chrono::milliseconds(100));
#endif
    }

    static bool take(std::deque<uint64_t>& q, uint64_t& id) {
        if (q.empty()) return false;
        id = q.front();
        q.pop_front();
        return true;
    }

    // SD card / console covers, in the order wanted; the rest is handed to the download workers.
    void runLocal() {
        for (;;) {
            uint64_t id = 0;
            bool prefetch = false;
            {
                std::lock_guard<std::mutex> g(mu);
                if (quit) return;
                if (!take(queue, id)) {
                    prefetch = take(later, id);
                    if (prefetch) laterSet.erase(id);
                }
                if (id) {
                    State st = state.count(id) ? state[id] : State::None;
                    if (prefetch && (st != State::None || inFlight.count(id))) id = 0;  // already there / on its way
                    else inFlight.insert(id);
                }
            }
            if (!id) { waitWork(0); continue; }
            std::shared_ptr<Image> img = loadLocal(id);
            std::lock_guard<std::mutex> g(mu);
            inFlight.erase(id);
            if (img) { ready(id, img); continue; }
            if (paused) {  // try again once fetching resumes
                if (!prefetch) state[id] = State::None;
                continue;
            }
            std::deque<uint64_t>& nq = prefetch ? netLater : netQueue;
            if (std::find(nq.begin(), nq.end(), id) == nq.end()) nq.push_front(id);
            if (!prefetch) state[id] = State::Loading;
            signal();
        }
    }

    void runNet(int k) {
        http::Session ses;
        http::setThreadTimeout(8000);
        {
            std::lock_guard<std::mutex> g(mu);
            sessions[k] = &ses;
        }
        for (;;) {
            uint64_t id = 0;
            bool prefetch = false;
            {
                std::lock_guard<std::mutex> g(mu);
                if (quit) { sessions[k] = nullptr; return; }
                if (!paused) {
                    if (!take(netQueue, id)) prefetch = take(netLater, id);
                    if (id && (inFlight.count(id) || (prefetch && state.count(id) && state[id] != State::None))) id = 0;
                    if (id) inFlight.insert(id);
                }
            }
            if (!id) { waitWork(k + 1); continue; }
            bool trouble = false;
            std::shared_ptr<Image> img = loadNet(id, trouble, &ses);
            if (!img) ses.close();
            std::lock_guard<std::mutex> g(mu);
            inFlight.erase(id);
            if (img) { ready(id, img); continue; }
            if (prefetch) continue;  // nobody is looking at it: asked again when shown
            state[id] = State::Missing;
            // network trouble: shown as missing for now, asked again when viewed after 30 s
            if (trouble) retryAt[id] = (int64_t)time(nullptr) + 30;
            else retryAt.erase(id);
        }
    }

    void signal() {
#ifdef __SWITCH__
        for (auto& w : wake) ueventSignal(&w);
#else
        cv.notify_all();
#endif
    }
};

static std::map<uint64_t, int64_t> readRecent(const std::string& dir) {
    std::map<uint64_t, int64_t> m;
    std::ifstream f(dir + "/recent.txt");
    std::string line;
    while (std::getline(f, line)) {
        unsigned long long id = 0;
        long long t = 0;
        if (sscanf(line.c_str(), "%llx %lld", &id, &t) == 2 && id) m[(uint64_t)id] = (int64_t)t;
    }
    return m;
}

static void writeRecent(const std::string& dir, const std::map<uint64_t, int64_t>& m) {
    std::ofstream f(dir + "/recent.txt", std::ios::trunc);
    for (const auto& kv : m) {
        char b[48];
        snprintf(b, sizeof(b), "%016llX %lld\n", (unsigned long long)kv.first, (long long)kv.second);
        f << b;
    }
}

int trimCache(const std::string& dir, uint64_t maxBytes, uint64_t targetBytes) {
    struct F { uint64_t id; uint64_t size; int64_t used; };
    std::vector<F> files;
    uint64_t total = 0;
    std::map<uint64_t, int64_t> recent = readRecent(dir);
    if (DIR* d = opendir(dir.c_str())) {
        while (struct dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() != 20 || n.compare(16, 4, ".jpg") != 0) continue;
            uint64_t id = strtoull(n.substr(0, 16).c_str(), nullptr, 16);
            struct stat st;
            if (!id || stat((dir + "/" + n).c_str(), &st) != 0) continue;
            auto r = recent.find(id);
            files.push_back({id, (uint64_t)st.st_size, r != recent.end() ? r->second : (int64_t)st.st_mtime});
            total += (uint64_t)st.st_size;
        }
        closedir(d);
    }
    int removed = 0;
    if (total > maxBytes) {
        std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.used < b.used; });
        for (const F& f : files) {
            if (total <= targetBytes) break;
            char b[32];
            snprintf(b, sizeof(b), "/%016llX.jpg", (unsigned long long)f.id);
            if (std::remove((dir + b).c_str()) == 0) {
                total -= f.size;
                recent.erase(f.id);
                removed++;
            }
        }
    }
    // keep the index to existing files only
    std::map<uint64_t, int64_t> keep;
    for (const F& f : files) {
        auto r = recent.find(f.id);
        if (r != recent.end()) keep[f.id] = r->second;
    }
    if (!files.empty()) writeRecent(dir, keep);
    return removed;
}

Cache::Cache(std::string cacheDir, std::string urlTemplate, int box) : d_(new Impl) {
    d_->dir = std::move(cacheDir);
    d_->lastUsed = readRecent(d_->dir);
    d_->tmpl = std::move(urlTemplate);
    d_->box = box;
#ifdef __SWITCH__
    for (auto& w : d_->wake) ueventCreate(&w, true);
    // background work stays off the core drawing the screen (scrolling keeps smooth)
    u64 mask = 0;
    svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    int self = (int)svcGetCurrentProcessorNumber();
    std::vector<int> cores;
    for (int c = 0; c < 4; c++)
        if (((mask >> c) & 1) && c != self) cores.push_back(c);
    // own threads with a roomy stack: JPEG decoding + TLS
    for (int k = 0; k <= Impl::kNetWorkers; k++) {
        d_->args[k] = {d_, k};
        auto fn = [](void* p) {
            Impl::Arg* a = (Impl::Arg*)p;
            if (a->k == 0) a->d->runLocal();
            else a->d->runNet(a->k - 1);
        };
        int core = cores.empty() ? -2 : cores[(size_t)k % cores.size()];
        if (R_SUCCEEDED(threadCreate(&d_->threads[k], fn, &d_->args[k], nullptr, 0x40000, k == 0 ? 0x2D : 0x2F, core))) {
            d_->started[k] = R_SUCCEEDED(threadStart(&d_->threads[k]));
            if (!d_->started[k]) threadClose(&d_->threads[k]);
        }
    }
#else
    d_->threads[0] = std::thread([this] { d_->runLocal(); });
    for (int k = 0; k < Impl::kNetWorkers; k++) d_->threads[k + 1] = std::thread([this, k] { d_->runNet(k); });
#endif
}

Cache::~Cache() {
    {
        std::lock_guard<std::mutex> g(d_->mu);
        d_->quit = true;
        for (auto* ses : d_->sessions)
            if (ses) ses->interrupt();  // a download in progress stops at once
    }
    d_->signal();
#ifdef __SWITCH__
    for (int k = 0; k <= Impl::kNetWorkers; k++)
        if (d_->started[k]) {
            threadWaitForExit(&d_->threads[k]);
            threadClose(&d_->threads[k]);
        }
#else
    for (auto& t : d_->threads)
        if (t.joinable()) t.join();
#endif
    if (!d_->lastUsed.empty()) {
        mkdir(d_->dir.c_str(), 0777);
        writeRecent(d_->dir, d_->lastUsed);  // when each cover was last shown, for trimCache
    }
    delete d_;
}

void Cache::request(uint64_t id, size_t keep) {
    if (!id) return;
    std::lock_guard<std::mutex> g(d_->mu);
    State& st = d_->state[id];
    if (st == State::Missing) {
        auto r = d_->retryAt.find(id);
        if (r == d_->retryAt.end() || (int64_t)time(nullptr) < r->second) return;
        d_->retryAt.erase(r);  // that failure was network trouble: try again now
        st = State::None;
    }
    if (st == State::Ready || d_->inFlight.count(id)) return;
    for (auto it = d_->netQueue.begin(); it != d_->netQueue.end(); ++it)
        if (*it == id) {  // already waiting for a download: first in line now
            d_->netQueue.erase(it);
            d_->netQueue.push_front(id);
            st = State::Loading;
            d_->signal();
            return;
        }
    for (auto it = d_->queue.begin(); it != d_->queue.end(); ++it)
        if (*it == id) { d_->queue.erase(it); break; }
    d_->queue.push_front(id);
    if (keep < 1) keep = 1;
    while (d_->queue.size() > keep) {  // forget stale requests when scrolling fast
        d_->state[d_->queue.back()] = State::None;
        d_->queue.pop_back();
    }
    while (d_->netQueue.size() > keep + 8) {
        d_->state[d_->netQueue.back()] = State::None;
        d_->netQueue.pop_back();
    }
    st = State::Loading;
    d_->signal();
}

State Cache::get(uint64_t id, std::shared_ptr<const Image>& img) {
    std::lock_guard<std::mutex> g(d_->mu);
    auto it = d_->state.find(id);
    if (it == d_->state.end()) return State::None;
    if (it->second == State::Ready) {
        img = d_->images[id];
        d_->touched[id] = ++d_->useClock;
    }
    return it->second;
}

void Cache::setUrlLookup(std::function<std::string(uint64_t)> fn) {
    std::lock_guard<std::mutex> g(d_->mu);
    d_->lookup = std::move(fn);
}

void Cache::retryMissing() {
    std::lock_guard<std::mutex> g(d_->mu);
    for (auto& kv : d_->state)
        if (kv.second == State::Missing) kv.second = State::None;
    d_->retryAt.clear();
}

int Cache::missingCount() {
    std::lock_guard<std::mutex> g(d_->mu);
    int n = 0;
    for (const auto& kv : d_->state) n += kv.second == State::Missing;
    return n;
}

void Cache::setLogger(std::function<void(const std::string&)> fn) {
    std::lock_guard<std::mutex> g(d_->mu);
    d_->logger = std::move(fn);
    d_->logged = 0;
}

void Cache::setPaused(bool p) {
    std::lock_guard<std::mutex> g(d_->mu);
    d_->paused = p;
}

std::shared_ptr<Image> Cache::loadNow(uint64_t id) { return d_->load(id, true); }

void Cache::prefetch(const std::vector<uint64_t>& ids) {
    std::lock_guard<std::mutex> g(d_->mu);
    bool any = false;
    for (uint64_t id : ids) {
        if (!id || d_->laterSet.count(id) || d_->inFlight.count(id)) continue;
        auto st = d_->state.find(id);
        if (st != d_->state.end() && st->second != State::None) continue;
        d_->later.push_back(id);
        d_->laterSet.insert(id);
        any = true;
    }
    while (d_->later.size() > 96) {
        d_->laterSet.erase(d_->later.front());
        d_->later.pop_front();
    }
    if (any) d_->signal();
}

void Cache::setMaxImages(size_t n) {
    std::lock_guard<std::mutex> g(d_->mu);
    d_->maxImages = n < 16 ? 16 : n;
    d_->trimImages();
}

}  // namespace covers
