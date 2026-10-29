#pragma once
// Game cover art for the file list, loaded in the background.
// Sources, in order: SD-card cache -> the console's installed copy (ns control data, Switch only)
// -> the eShop icon from the game notes -> an online URL template ("{id}" is replaced by the
// 16-digit base title id, upper case). "Not found" is only final when every site answered; after
// network trouble the cover is asked for again (30 s later) when it is viewed.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace covers {

// Finds a 16-hex-digit title id in a file name like "Game [0100ABCD12345000][v0].nsp". 0 if none.
uint64_t titleIdFromName(const std::string& name);
// Base application id for an update (…800) or DLC (base + 0x1000 + n) title id.
uint64_t baseTitleId(uint64_t tid);

// The address to fetch an eShop icon from: Nintendo hosts (blocked on the console by Atmosphère's
// hosts file) go through `proxy`, where "{url}" is replaced by the encoded icon address.
// Empty proxy, or any other host: the icon address itself.
std::string proxiedUrl(const std::string& proxy, const std::string& icon);

struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> px;  // RGBA8888 (R in the low byte), like gfx::Canvas
};

// Decodes a JPEG/PNG and scales it to fit `box` x `box`. Returns false on bad data.
bool decode(const std::vector<uint8_t>& file, int box, Image& out);

enum class State { None, Loading, Ready, Missing };

// Keeps the SD-card cover cache small: when the *.jpg files in `dir` exceed maxBytes, the covers
// viewed longest ago are deleted until targetBytes remain (they are fetched again when needed).
// "Viewed" times come from dir/recent.txt (written by Cache), else the file time. Returns files removed.
int trimCache(const std::string& dir, uint64_t maxBytes, uint64_t targetBytes);

class Cache {
public:
    // cacheDir: e.g. "sdmc:/config/lan-installer/covers"; urlTemplate may be empty (no online source).
    Cache(std::string cacheDir, std::string urlTemplate, int box);
    ~Cache();

    // Asks for a cover; the most recent request is fetched first. Cheap to call every frame.
    // `keep`: how many waiting requests are kept (older ones are dropped): 8 for the list, a page
    // for the cover wall (request the page last-to-first and the cursor's game last).
    void request(uint64_t baseId, size_t keep = 8);
    // Current state; when Ready, `img` points at the decoded picture.
    State get(uint64_t baseId, std::shared_ptr<const Image>& img);
    // Pause network fetching (e.g. while installing). Cached/local covers still load.
    void setPaused(bool paused);
    // Optional extra source tried before the URL template (e.g. the eShop icon from titledb).
    // Called on the loader thread; return "" if unknown.
    void setUrlLookup(std::function<std::string(uint64_t)> fn);
    // Forget "not found" results so they are tried again (e.g. once the titledb has loaded, or
    // from the menu: 重新抓取封面).
    void retryMissing();
    // Covers currently marked "not found" (in this session).
    int missingCount();
    // Where download failures are reported (host + reason, the first 40 only). Called on the loader thread.
    void setLogger(std::function<void(const std::string&)> fn);

    // Covers likely to be shown next (next / previous page): loaded when nothing else waits.
    void prefetch(const std::vector<uint64_t>& baseIds);
    // How many decoded pictures stay in memory (default 100).
    void setMaxImages(size_t n);

    // Exposed so tests can run the loader synchronously.
    std::shared_ptr<Image> loadNow(uint64_t baseId);

private:
    struct Impl;
    Impl* d_;
};

}  // namespace covers
