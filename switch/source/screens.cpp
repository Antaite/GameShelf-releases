#include "screens.hpp"

#include <cstring>

#include <map>

#include <cctype>
#include <algorithm>
#include <cstdio>

#include "http.hpp"

namespace screens {

using gfx::rgb;

namespace col {
constexpr uint32_t bg = rgb(22, 24, 29);
constexpr uint32_t panel = rgb(32, 35, 43);
constexpr uint32_t row = rgb(27, 30, 36);
constexpr uint32_t cursor = rgb(36, 58, 80);
constexpr uint32_t accent = rgb(0, 178, 226);
constexpr uint32_t text = rgb(234, 236, 242);
constexpr uint32_t dim = rgb(146, 152, 168);
constexpr uint32_t line = rgb(52, 56, 68);
constexpr uint32_t key = rgb(70, 76, 92);
constexpr uint32_t green = rgb(88, 204, 126);
constexpr uint32_t red = rgb(238, 96, 96);
constexpr uint32_t yellow = rgb(242, 194, 84);
constexpr uint32_t barBg = rgb(46, 50, 62);
}  // namespace col

std::string humanSize(uint64_t b) {
    char buf[32];
    if (b >= (1ULL << 30)) snprintf(buf, sizeof(buf), "%.2f GB", b / 1073741824.0);
    else if (b >= (1ULL << 20)) snprintf(buf, sizeof(buf), "%.1f MB", b / 1048576.0);
    else snprintf(buf, sizeof(buf), "%.0f KB", b / 1024.0);
    return buf;
}

static std::string encodeUtf8(const std::vector<uint32_t>& cps, size_t from, size_t to) {
    std::string s;
    for (size_t i = from; i < to; i++) {
        uint32_t c = cps[i];
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
        else { s += (char)(0xF0 | (c >> 18)); s += (char)(0x80 | ((c >> 12) & 0x3F)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
    }
    return s;
}

// Breaks text into lines no wider than maxW (works for CJK, which has no spaces).
static std::vector<std::string> wrap(gfx::Font& f, const std::string& text, int px, int maxW) {
    std::vector<std::string> out;
    std::vector<uint32_t> cps = gfx::decodeUtf8(text);
    size_t start = 0;
    int w = 0;
    for (size_t i = 0; i < cps.size(); i++) {
        int cw = f.measure(encodeUtf8(cps, i, i + 1), px);
        if (w + cw > maxW && i > start) {
            out.push_back(encodeUtf8(cps, start, i));
            start = i;
            w = 0;
        }
        w += cw;
    }
    out.push_back(encodeUtf8(cps, start, cps.size()));
    return out;
}

// Thick anti-aliased-ish line made of small squares (used for the check mark).
static void thickLine(gfx::Canvas& c, float x0, float y0, float x1, float y1, int t, uint32_t color) {
    int steps = 24;
    for (int i = 0; i <= steps; i++) {
        float k = (float)i / steps;
        gfx::fillRect(c, (int)(x0 + (x1 - x0) * k) - t / 2, (int)(y0 + (y1 - y0) * k) - t / 2, t, t, color);
    }
}

// A button badge such as [A]; returns its width.
static int drawKey(gfx::Canvas& c, gfx::Font& f, int x, int y, const std::string& label) {
    const int px = 18, h = 28;
    int w = f.measure(label, px) + 16;
    if (w < h) w = h;
    gfx::fillRoundRect(c, x, y, w, h, 14, col::key);
    int tw = f.measure(label, px);
    f.draw(c, x + (w - tw) / 2, y + 3, label, px, col::text);
    return w;
}

// Row of "[key] label" hints; returns width used.
static int drawHints(gfx::Canvas& c, gfx::Font& f, int x, int y, const std::vector<std::pair<std::string, std::string>>& hints) {
    int start = x;
    for (const auto& h : hints) {
        x += drawKey(c, f, x, y, h.first) + 6;
        x += f.draw(c, x, y + 2, h.second, 20, col::dim) + 20;
    }
    return x - start;
}

static SysStatus g_sys;
void setSysStatus(const SysStatus& st) { g_sys = st; }

// Clock + network at the far right of the header (every screen).
static void drawSysStatus(gfx::Canvas& c, gfx::Font& f) {
    int x = kWidth - 24;
    if (!g_sys.clock.empty()) {
        int w = f.measure(g_sys.clock, 24);
        x -= w;
        f.draw(c, x, 21, g_sys.clock, 24, col::text);
        x -= 14;
    }
    if (g_sys.net == -2) return;
    if (g_sys.net == -1) {
        const char* t = g_sys.connecting ? "連線中" : "網路中斷";
        int w = f.measure(t, 18) + 20;
        x -= w;
        gfx::fillRoundRect(c, x, 22, w, 28, 14, g_sys.connecting ? col::yellow : col::red);
        f.draw(c, x + 10, 25, t, 18, col::bg);
        return;
    }
    if (g_sys.net == 4) {
        int w = f.measure("有線", 18);
        x -= w;
        f.draw(c, x, 25, "有線", 18, col::green);
        return;
    }
    // Wi-Fi: three bars, lit by signal strength (0..3)
    const int bw = 6, gap = 3, base = 48;
    x -= 3 * bw + 2 * gap;
    uint32_t lit = g_sys.net >= 2 ? col::green : g_sys.net == 1 ? col::yellow : col::red;
    for (int i = 0; i < 3; i++) {
        int h = 8 + i * 7;
        gfx::fillRect(c, x + i * (bw + gap), base - h, bw, h, i < g_sys.net ? lit : col::key);
    }
}

static void drawHeader(gfx::Canvas& c, gfx::Font& f, const std::string& title, const std::string& right,
                       uint32_t rightColor) {
    gfx::fillRect(c, 0, 0, kWidth, 72, col::panel);
    gfx::fillRect(c, 0, 72, kWidth, 2, col::accent);
    f.draw(c, 32, 16, title, 32, col::text);
    if (!right.empty()) {
        int w = f.measure(right, 22) + 28;
        gfx::fillRoundRect(c, kWidth - kStatusW - 16 - w, 18, w, 36, 18, col::bg);
        f.draw(c, kWidth - kStatusW - 16 - w + 14, 24, right, 22, rightColor);
    }
    drawSysStatus(c, f);
}

static void drawFooter(gfx::Canvas& c, gfx::Font& f, const std::vector<std::pair<std::string, std::string>>& hints) {
    gfx::fillRect(c, 0, kHeight - 52, kWidth, 52, col::panel);
    gfx::fillRect(c, 0, kHeight - 52, kWidth, 1, col::line);
    drawHints(c, f, 32, kHeight - 40, hints);
}

static void darken(gfx::Canvas& c, int num, int den) {
    for (int y = 0; y < c.h; y++) {
        uint32_t* r = c.px + (size_t)y * c.stride;
        for (int x = 0; x < c.w; x++) {
            uint32_t p = r[x];
            uint32_t R = (p & 0xFF) * num / den, G = ((p >> 8) & 0xFF) * num / den, B = ((p >> 16) & 0xFF) * num / den;
            r[x] = 0xFF000000u | (B << 16) | (G << 8) | R;
        }
    }
}

// ------------------------------------------------------------------ list
// Draws `text` in `width` px; if it does not fit it scrolls (after a pause) like a marquee.
// Returns true while scrolling.
static bool marquee(gfx::Canvas& c, gfx::Font& f, int x, int y, const std::string& text, int px, uint32_t color,
                    int width, uint64_t ms) {
    int tw = f.measure(text, px);
    if (tw <= width) {
        f.draw(c, x, y, text, px, color);
        return false;
    }
    const uint64_t pause = 1200;
    const int gap = 80, speed = 60;  // px per second
    int offset = 0;
    if (ms > pause) offset = (int)(((ms - pause) * speed / 1000) % (uint64_t)(tw + gap));
    f.drawClipped(c, x, y, text, px, color, width, offset);
    if (offset > gap) f.drawClipped(c, x, y, text, px, color, width, offset - tw - gap);
    return true;
}

static void drawBadge(gfx::Canvas& c, gfx::Font& f, int right, int y, Badge b, int& usedWidth) {
    usedWidth = 0;
    const char* t = nullptr;
    uint32_t bg = col::key, fg = col::text;
    switch (b) {
        case Badge::Installed: t = "已安裝"; bg = col::row; fg = col::dim; break;
        case Badge::Update: t = "可更新"; bg = rgb(32, 92, 56); fg = rgb(150, 240, 180); break;
        case Badge::Dlc: t = "可裝 DLC"; bg = rgb(96, 76, 24); fg = rgb(250, 214, 120); break;
        case Badge::Reinstall: t = "需重裝"; bg = rgb(110, 36, 36); fg = rgb(255, 180, 170); break;
        case Badge::Queued: t = "排隊中"; bg = rgb(52, 60, 96); fg = rgb(190, 205, 255); break;
        case Badge::Installing: t = "安裝中"; bg = col::accent; fg = col::bg; break;
        default: return;
    }
    int w = f.measure(t, 16) + 16;
    gfx::fillRoundRect(c, right - w, y, w, 24, 12, bg);
    f.draw(c, right - w + 8, y + 2, t, 16, fg);
    usedWidth = w;
}

void splitName(const std::string& file, std::string& title, std::string& details) {
    std::string base = file.substr(file.rfind('/') == std::string::npos ? 0 : file.rfind('/') + 1);
    size_t dot = base.rfind('.');
    std::string ext = dot == std::string::npos ? "" : base.substr(dot + 1);
    if (dot != std::string::npos) base = base.substr(0, dot);
    // title = text before the first '[' ; details = bracket tags + extension
    size_t br = base.find('[');
    title = br == std::string::npos ? base : base.substr(0, br);
    while (!title.empty() && (title.back() == ' ' || title.back() == '_')) title.pop_back();
    if (title.empty()) title = base;
    details.clear();
    size_t i = br;
    while (i != std::string::npos && i < base.size()) {
        size_t close = base.find(']', i);
        if (close == std::string::npos) break;
        std::string tag = base.substr(i + 1, close - i - 1);
        if (!tag.empty()) details += (details.empty() ? "" : "  ") + tag;
        i = base.find('[', close);
    }
    for (auto& ch : ext) ch = (char)toupper((unsigned char)ch);
    if (!ext.empty()) details += (details.empty() ? "" : "  ") + ext;
}

static void drawCoverPanel(gfx::Canvas& c, gfx::Font& f, const ListState& s, int x, int y, int w, int h) {
    gfx::fillRoundRect(c, x, y, w, h, 12, col::panel);
    if (s.items.empty()) return;
    const ListItem& it = s.items[s.cursor];
    const int box = w - 40;
    int bx = x + 20, by = y + 20;
    gfx::fillRoundRect(c, bx, by, box, box, 10, col::row);
    if (s.cover.state == CoverState::Ready && s.cover.px) {
        int ox = bx + (box - s.cover.w) / 2, oy = by + (box - s.cover.h) / 2;
        for (int yy = 0; yy < s.cover.h; yy++) {
            int py = oy + yy;
            if (py < 0 || py >= c.h) continue;
            uint32_t* row = c.px + (size_t)py * c.stride;
            const uint32_t* src = s.cover.px + (size_t)yy * s.cover.w;
            for (int xx = 0; xx < s.cover.w; xx++) {
                int px = ox + xx;
                if (px >= 0 && px < c.w) row[px] = src[xx];
            }
        }
    } else {
        std::string msg = s.cover.titleId == 0 ? "檔名中沒有遊戲 ID"
                        : s.cover.state == CoverState::Missing ? "找不到封面"
                        : "封面載入中…";
        int mw = f.measure(msg, 20);
        f.draw(c, bx + (box - mw) / 2, by + box / 2 - 12, msg, 20, col::dim);
    }
    std::string title, details, fileTitle;
    splitName(it.file.empty() ? it.name : it.file, fileTitle, details);
    title = it.file.empty() ? fileTitle : it.name;
    int ty = by + box + 12;
    if (marquee(c, f, x + 20, ty, title, 22, col::text, w - 40, s.marqueeMs)) const_cast<ListState&>(s).animating = true;
    f.draw(c, x + 20, ty + 32, details, 16, col::dim, w - 40);
    std::string date = it.releaseDate ? "發售日 " + std::to_string(it.releaseDate / 10000) + "-" +
                                            (it.releaseDate / 100 % 100 < 10 ? "0" : "") + std::to_string(it.releaseDate / 100 % 100) + "-" +
                                            (it.releaseDate % 100 < 10 ? "0" : "") + std::to_string(it.releaseDate % 100)
                                      : "發售日 不明";
    f.draw(c, x + 20, ty + 58, date, 18, it.releaseDate ? col::text : col::dim);
    if (!it.versionLine.empty()) f.draw(c, x + 20, ty + 84, it.versionLine, 18, rgb(120, 236, 160), w - 40);
    if (!it.contentsLine.empty()) f.draw(c, x + 20, ty + 108, it.contentsLine, 16, col::dim, w - 40);
    std::string size = humanSize(it.size);
    int sw = f.measure(size, 22);
    f.draw(c, x + w - 20 - sw, ty + 56, size, 22, col::accent);
}


// ---- cover pictures at other sizes (cover wall tiles, its narrow side panel)
// Scaled copies are kept for the pictures on screen, keyed by title id and size; a copy is rebuilt
// when the source picture changes.
namespace {
struct Thumb {
    const uint32_t* src = nullptr;
    int sw = 0, sh = 0;
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    uint64_t used = 0;
};
std::map<std::pair<uint64_t, int>, Thumb> g_thumbs;
uint64_t g_thumbClock = 0;
}  // namespace

static const Thumb& thumbFor(const CoverView& cv, int box) {
    Thumb& t = g_thumbs[{cv.titleId, box}];
    t.used = ++g_thumbClock;
    if (t.src == cv.px && t.sw == cv.w && t.sh == cv.h && !t.px.empty()) return t;
    t.src = cv.px; t.sw = cv.w; t.sh = cv.h;
    double scale = (double)box / std::max(cv.w, cv.h);
    t.w = std::max(1, (int)(cv.w * scale + 0.5));
    t.h = std::max(1, (int)(cv.h * scale + 0.5));
    t.px.assign((size_t)t.w * t.h, 0);
    for (int y = 0; y < t.h; y++) {  // area average
        int y0 = (int)(y / scale), y1 = std::max(y0 + 1, std::min(cv.h, (int)((y + 1) / scale)));
        for (int x = 0; x < t.w; x++) {
            int x0 = (int)(x / scale), x1 = std::max(x0 + 1, std::min(cv.w, (int)((x + 1) / scale)));
            unsigned r = 0, g = 0, b = 0, k = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    uint32_t p = cv.px[(size_t)yy * cv.w + xx];
                    r += p & 0xFF; g += (p >> 8) & 0xFF; b += (p >> 16) & 0xFF; k++;
                }
            t.px[(size_t)y * t.w + x] = 0xFF000000u | ((b / k) << 16) | ((g / k) << 8) | (r / k);
        }
    }
    if (g_thumbs.size() > 80) {  // keep the cache to about two pages of tiles
        auto oldest = g_thumbs.begin();
        for (auto it = g_thumbs.begin(); it != g_thumbs.end(); ++it)
            if (it->second.used < oldest->second.used) oldest = it;
        if (&oldest->second != &t) g_thumbs.erase(oldest);
    }
    return g_thumbs[{cv.titleId, box}];
}

// Draws a ready cover centred in a box x box square (scaled down when larger).
static void blitCover(gfx::Canvas& c, const CoverView& cv, int x, int y, int box) {
    const uint32_t* px = cv.px;
    int w = cv.w, h = cv.h;
    if (w > box || h > box) {
        const Thumb& t = thumbFor(cv, box);
        px = t.px.data(); w = t.w; h = t.h;
    }
    int ox = x + (box - w) / 2, oy = y + (box - h) / 2;
    for (int yy = 0; yy < h; yy++) {
        int py = oy + yy;
        if (py < 0 || py >= c.h) continue;
        uint32_t* row = c.px + (size_t)py * c.stride;
        const uint32_t* src = px + (size_t)yy * w;
        for (int xx = 0; xx < w; xx++) {
            int pxx = ox + xx;
            if (pxx >= 0 && pxx < c.w) row[pxx] = src[xx];
        }
    }
}

static std::string dateText(uint32_t d) {
    if (!d) return "";
    char b[16];
    snprintf(b, sizeof(b), "%04u-%02u-%02u", d / 10000, d / 100 % 100, d % 100);
    return b;
}

static void drawCheck(gfx::Canvas& c, int bx, int by, bool on, uint32_t back) {
    if (on) {
        gfx::fillRoundRect(c, bx, by, 18, 18, 4, col::accent);
        thickLine(c, bx + 4, by + 9, bx + 8, by + 13, 3, col::bg);
        thickLine(c, bx + 8, by + 13, bx + 14, by + 5, 3, col::bg);
    } else {
        gfx::fillRoundRect(c, bx, by, 18, 18, 4, col::dim);
        gfx::fillRoundRect(c, bx + 2, by + 2, 14, 14, 3, back);
    }
}

// Colour of the status dot on a cover wall tile (0 = no dot).
static uint32_t badgeDot(Badge b) {
    switch (b) {
        case Badge::Installed: return rgb(120, 128, 150);
        case Badge::Update: return rgb(40, 168, 92);
        case Badge::Dlc: return rgb(214, 162, 40);
        case Badge::Reinstall: return rgb(200, 60, 60);
        case Badge::Queued: return rgb(110, 124, 200);
        case Badge::Installing: return col::accent;
        default: return 0;
    }
}

// Side panel of the cover wall: the game under the cursor, in a narrow column.
static void drawGridPanel(gfx::Canvas& c, gfx::Font& f, const ListState& s, int x, int y, int w, int h) {
    gfx::fillRoundRect(c, x, y, w, h, 12, col::panel);
    if (s.items.empty()) return;
    const ListItem& it = s.items[s.cursor];
    const int box = w - 24;
    int bx = x + 12, by = y + 12;
    gfx::fillRoundRect(c, bx, by, box, box, 8, col::row);
    if (s.cover.state == CoverState::Ready && s.cover.px) {
        blitCover(c, s.cover, bx, by, box);
    } else {
        std::string msg = s.cover.titleId == 0 ? "檔名中沒有遊戲 ID" : s.cover.state == CoverState::Missing ? "找不到封面" : "封面載入中…";
        int mw = f.measure(msg, 16);
        f.draw(c, bx + (box - mw) / 2, by + box / 2 - 10, msg, 16, col::dim);
    }
    std::string title = it.name.substr(it.name.rfind('/') == std::string::npos ? 0 : it.name.rfind('/') + 1);
    std::string fileTitle, details;
    splitName(it.file.empty() ? it.name : it.file, fileTitle, details);
    int ty = by + box + 10;
    std::vector<std::string> lines = wrap(f, title, 17, box);
    for (size_t i = 0; i < lines.size() && i < 2; i++) {
        std::string l = lines[i];
        if (i == 1 && lines.size() > 2) l += "…";
        f.draw(c, bx, ty + (int)i * 22, l, 17, col::text, box);
    }
    ty += 48;
    f.draw(c, bx, ty, details, 13, col::dim, box);
    ty += 22;
    std::string date = it.releaseDate ? "發售日 " + dateText(it.releaseDate) : "發售日 不明";
    f.draw(c, bx, ty, date, 15, it.releaseDate ? col::text : col::dim, box);
    ty += 26;
    f.draw(c, bx, ty, humanSize(it.size), 20, col::accent);
    int bw = 0;
    drawBadge(c, f, bx + box, ty + 1, it.badge, bw);
    ty += 32;
    if (!it.versionLine.empty()) { f.draw(c, bx, ty, it.versionLine, 14, rgb(120, 236, 160), box); ty += 22; }
    if (!it.contentsLine.empty()) {
        std::vector<std::string> cl = wrap(f, it.contentsLine, 13, box);
        for (size_t i = 0; i < cl.size() && i < 2 && ty + 18 < y + h; i++) { f.draw(c, bx, ty, cl[i], 13, col::dim, box); ty += 19; }
    }
}

// Cover wall: kGridCols x kGridRows tiles + the side panel.
static void drawGridBody(gfx::Canvas& c, gfx::Font& f, ListState& s, int left, int top, int height) {
    const int cw = 116, gap = 8;
    const int gridW = kGridCols * (cw + gap) - gap;
    const int gridH = kGridRows * (cw + gap) - gap;
    const int gy = top + (height - gridH) / 2;
    const size_t per = pageSize(kViewGrid);
    size_t n = s.items.size();
    for (size_t i = 0; i < per; i++) {
        size_t idx = s.scroll + i;
        if (idx >= n) break;
        const ListItem& it = s.items[idx];
        int x = left + (int)(i % kGridCols) * (cw + gap), y = gy + (int)(i / kGridCols) * (cw + gap);
        bool cur = idx == s.cursor;
        gfx::fillRoundRect(c, x, y, cw, cw, 6, col::row);
        const CoverView* cv = i < s.covers.size() ? &s.covers[i] : nullptr;
        if (cv && cv->state == CoverState::Ready && cv->px) {
            blitCover(c, *cv, x, y, cw);
        } else {  // no picture (yet): the game's name, so the tile still says what it is
            std::string t, d;
            splitName(it.file.empty() ? it.name : it.file, t, d);
            std::vector<std::string> lines = wrap(f, t, 14, cw - 14);
            for (size_t k = 0; k < lines.size() && k < 4; k++) {
                std::string l = lines[k];
                if (k == 3 && lines.size() > 4) l += "…";
                f.draw(c, x + 7, y + 30 + (int)k * 19, l, 14, col::dim, cw - 14);
            }
            if (cv && cv->state == CoverState::Loading) f.draw(c, x + 8, y + cw - 24, "載入中…", 13, col::dim);
        }
        drawCheck(c, x + 6, y + 6, it.selected, col::row);
        if (uint32_t dot = badgeDot(it.badge)) {
            gfx::fillRoundRect(c, x + cw - 22, y + cw - 22, 16, 16, 8, col::bg);
            gfx::fillRoundRect(c, x + cw - 20, y + cw - 20, 12, 12, 6, dot);
        }
        if (cur) {
            gfx::fillRect(c, x - 4, y - 4, cw + 8, 4, col::accent);
            gfx::fillRect(c, x - 4, y + cw, cw + 8, 4, col::accent);
            gfx::fillRect(c, x - 4, y, 4, cw, col::accent);
            gfx::fillRect(c, x + cw, y, 4, cw, col::accent);
        }
        s.hits.push_back({x, y, cw, cw, 1000 + (int)idx});
    }
    int px = left + gridW + 14;
    drawGridPanel(c, f, s, px, top, kWidth - 24 - px, height);
}

// Compact list: a table without covers, kListRows games a page.
static void drawCompactBody(gfx::Canvas& c, gfx::Font& f, ListState& s, int left, int top, int height, size_t pages,
                            size_t page) {
    const int W = kWidth - 48 - 10;
    const int headH = 26, rowH = (height - headH) / kListRows;
    const int sizeR = left + W - 12, statusR = left + W - 118, verX = left + W - 330, dateX = left + W - 460;
    f.draw(c, left + 44, top + 2, "檔名", 15, col::dim);
    f.draw(c, dateX, top + 2, "發售日", 15, col::dim);
    f.draw(c, verX, top + 2, "版本", 15, col::dim);
    f.draw(c, statusR - 70, top + 2, "狀態", 15, col::dim);
    f.draw(c, sizeR - f.measure("大小", 15), top + 2, "大小", 15, col::dim);
    size_t n = s.items.size();
    for (int i = 0; i < kListRows; i++) {
        size_t idx = s.scroll + i;
        if (idx >= n) break;
        const ListItem& it = s.items[idx];
        int y = top + headH + i * rowH;
        bool cur = idx == s.cursor;
        gfx::fillRect(c, left, y, W, rowH - 2, cur ? col::cursor : col::row);
        s.hits.push_back({left, y, W, rowH - 2, 1000 + (int)idx});
        if (cur) gfx::fillRect(c, left, y, 4, rowH - 2, col::accent);
        drawCheck(c, left + 14, y + (rowH - 2 - 18) / 2, it.selected, cur ? col::cursor : col::row);
        int ty = y + (rowH - 2 - 26) / 2 + 2;
        std::string base = it.name.substr(it.name.rfind('/') == std::string::npos ? 0 : it.name.rfind('/') + 1);
        int nameW = dateX - 16 - (left + 44);
        if (cur) { if (marquee(c, f, left + 44, ty, base, 19, col::text, nameW, s.marqueeMs)) s.animating = true; }
        else f.draw(c, left + 44, ty, base, 19, col::text, nameW);
        f.draw(c, dateX, ty + 1, it.releaseDate ? dateText(it.releaseDate) : "—", 18, it.releaseDate ? col::text : col::dim);
        f.draw(c, verX, ty + 1, it.version.empty() ? "—" : it.version, 18, col::text, statusR - 90 - verX);
        int bw = 0;
        drawBadge(c, f, statusR, y + (rowH - 2 - 24) / 2, it.badge, bw);
        std::string size = humanSize(it.size);
        f.draw(c, sizeR - f.measure(size, 18), ty + 1, size, 18, col::dim);
    }
    if (pages > 1) {
        int trackY = top + headH, trackH = rowH * kListRows - 2;
        int thumbH = std::max(16, trackH / (int)pages);
        int thumbY = trackY + (int)((trackH - thumbH) * page / (pages - 1));
        gfx::fillRect(c, left + W + 4, trackY, 4, trackH, col::row);
        gfx::fillRect(c, left + W + 4, thumbY, 4, thumbH, col::dim);
    }
}

// Pop-up menu on the right (drawn over the list or the manage screen).
static void drawMenuOverlay(gfx::Canvas& c, gfx::Font& f, MenuState& menu, std::vector<HitBox>& hits) {
    if (!menu.open) return;
    {
        darken(c, 2, 5);
        drawSysStatus(c, f);  // clock / network stay readable over the menu
        const int mw = 470, mx = kWidth - mw, my = 74, mh = kHeight - 52 - 74;
        gfx::fillRect(c, mx, my, mw, mh, col::panel);
        gfx::fillRect(c, mx, my, 2, mh, col::accent);
        int hy = my + 20;
        if (menu.child) {
            f.draw(c, mx + 28, hy + 4, "‹", 26, col::accent);
            hits.push_back({mx, my, mw, 64, 300});
            f.draw(c, mx + 52, hy, menu.title, 28, col::text);
        } else {
            f.draw(c, mx + 28, hy, menu.title, 28, col::text);
        }
        gfx::fillRect(c, mx + 20, my + 72, mw - 40, 1, col::line);
        const int ih = 60;
        // more items than fit: scroll so the cursor stays visible
        const size_t rows = (size_t)((my + mh - 8 - (my + 84)) / ih);
        size_t n = menu.items.size();
        if (menu.cursor < menu.scroll) menu.scroll = menu.cursor;
        if (menu.cursor >= menu.scroll + rows) menu.scroll = menu.cursor - rows + 1;
        if (menu.scroll + rows > n) menu.scroll = n > rows ? n - rows : 0;
        if (n > rows) {
            int trackY = my + 84, trackH = (int)rows * ih - 6;
            int thumbH = std::max(24, trackH * (int)rows / (int)n);
            int thumbY = trackY + (trackH - thumbH) * (int)menu.scroll / (int)(n - rows);
            gfx::fillRect(c, mx + mw - 10, trackY, 4, trackH, col::row);
            gfx::fillRect(c, mx + mw - 10, thumbY, 4, thumbH, col::dim);
        }
        for (size_t i = menu.scroll; i < n && i < menu.scroll + rows; i++) {
            const MenuItem& it = menu.items[i];
            int iy = my + 84 + (int)(i - menu.scroll) * ih;
            bool cur = i == menu.cursor;
            if (cur) {
                gfx::fillRoundRect(c, mx + 16, iy, mw - 32, ih - 6, 10, col::cursor);
                gfx::fillRect(c, mx + 16, iy + 10, 4, ih - 26, col::accent);
            }
            int lx = mx + 36;
            if (menu.child) {
                if (it.checked) {
                    thickLine(c, lx, iy + 27, lx + 6, iy + 33, 3, col::accent);
                    thickLine(c, lx + 6, iy + 33, lx + 16, iy + 19, 3, col::accent);
                }
                lx += 30;
            }
            int rx = mx + mw - 40;
            if (it.submenu) { f.draw(c, rx, iy + 11, "›", 26, col::dim); rx -= 20; }
            int vw = 0;
            if (!it.value.empty()) {
                vw = std::min(f.measure(it.value, 20), 210);
                f.draw(c, rx - vw, iy + 16, it.value, 20, col::dim, 210);
            }
            f.draw(c, lx, iy + 13, it.label, 24, it.enabled ? col::text : col::dim, rx - vw - 16 - lx);
            hits.push_back({mx + 16, iy, mw - 32, ih - 6, 400 + (int)i});
        }
        // footer hints for the menu replace the list's
        gfx::fillRect(c, 0, kHeight - 52, kWidth, 52, col::panel);
        gfx::fillRect(c, 0, kHeight - 52, kWidth, 1, col::line);
        drawHints(c, f, 32, kHeight - 40, {{"A", "確定"}, {"B", menu.child ? "上一層" : "關閉選單"}, {"Y", "關閉"}});
    }
}

void drawList(gfx::Canvas& c, gfx::Font& f, ListState& s) {
    gfx::clear(c, col::bg);
    drawHeader(c, f, "遊戲書架  " + s.version, "", col::text);
    {
        // server in use, next to the title: tap to switch / manage servers
        std::string t = (s.serverName.empty() ? std::string("尚未設定伺服器") : s.serverName) + "  ›";
        int tx = 32 + f.measure("遊戲書架  " + s.version, 32) + 18;
        int tw = std::min(f.measure(t, 20), 230) + 28;
        gfx::fillRoundRect(c, tx, 18, tw, 36, 18, col::bg);
        f.draw(c, tx + 14, 24, t, 20, s.serverName.empty() ? col::yellow : col::accent, tw - 28);
    }
    {
        // right side of the header: SD card free space
        s.hits.clear();
        {
            std::string t = (s.serverName.empty() ? std::string("尚未設定伺服器") : s.serverName) + "  ›";
            int tx = 32 + f.measure("遊戲書架  " + s.version, 32) + 18;
            int tw = std::min(f.measure(t, 20), 230) + 28;
            s.hits.push_back({tx, 18, tw, 36, 212});
        }
        std::string t = s.sdFree ? "SD 卡剩餘 " + humanSize(s.sdFree) : "安裝到 SD 卡";
        int w = f.measure(t, 20) + 28;
        const int sdX = kWidth - kStatusW - 16 - w;
        gfx::fillRoundRect(c, sdX, 18, w, 36, 18, col::bg);
        f.draw(c, sdX + 14, 24, t, 20, col::dim);
        int leftEdge = sdX;
        if (!s.installSummary.empty()) {  // installs running in the background: tap (or -) for details
            std::string it = s.installSummary + "  ›";
            int iw = f.measure(it, 20) + 32;
            int ix = leftEdge - 12 - iw;
            gfx::fillRoundRect(c, ix, 16, iw, 40, 20, col::accent);
            f.draw(c, ix + 16, 23, it, 20, col::bg);
            s.hits.push_back({ix, 16, iw, 40, 210});
            leftEdge = ix;
        }
        if (!s.notice.empty()) {  // e.g. started from the album: less memory (tap for details)
            int nw = f.measure(s.notice, 18) + 28;
            int nx = leftEdge - 12 - nw;
            gfx::fillRoundRect(c, nx, 20, nw, 32, 16, col::yellow);
            f.draw(c, nx + 14, 25, s.notice, 18, col::bg);
            s.hits.push_back({nx, 20, nw, 32, 211});
        }
    }

    const int top = 124, rowH = 36, left = 24;
    const int panelW = 380, gap = 16;
    const int width = kWidth - 48 - panelW - gap;
    size_t n = s.items.size();
    if (n && s.cursor >= n) s.cursor = n - 1;
    const size_t per = pageSize(s.view);
    size_t pages = n ? (n + per - 1) / per : 1;
    size_t page = n ? s.cursor / per : 0;
    s.scroll = page * per;

    s.animating = false;
    // tab bar (left) + sort / search chips (right). Tabs: ZL/ZR or touch.
    {
        struct Tab { const char* label; uint32_t on, onText, hot; };
        const Tab tabsDef[4] = {{"全部", col::accent, col::bg, col::text},
                                {"可更新", rgb(40, 168, 92), rgb(10, 30, 18), rgb(120, 236, 160)},
                                {"可裝 DLC", rgb(214, 162, 40), rgb(40, 28, 4), rgb(255, 214, 110)},
                                {"已安裝", rgb(120, 128, 150), col::bg, col::text}};
        int x = 24;
        for (int i = 0; i < 4; i++) {
            const Tab& t = tabsDef[i];
            std::string label = std::string(t.label) + "  " + std::to_string(s.tabCounts[i]);
            int w = f.measure(label, 20) + 32;
            bool active = s.category == i;
            bool hot = (i == 1 || i == 2) && s.tabCounts[i] > 0;  // something to install: stands out
            uint32_t bg = active ? t.on : col::row;
            uint32_t fg = active ? t.onText : hot ? t.hot : (s.tabCounts[i] ? col::text : col::dim);
            gfx::fillRoundRect(c, x, 80, w, 36, 18, bg);
            if (hot && !active) {  // coloured outline
                gfx::fillRoundRect(c, x, 80, w, 36, 18, t.on);
                gfx::fillRoundRect(c, x + 2, 82, w - 4, 32, 16, col::row);
            }
            f.draw(c, x + 16, 84, label, 20, fg);
            s.hits.push_back({x, 80, w, 36, 100 + i});
            x += w + 10;
        }
        int tx = kWidth - 32;
        auto chip = [&](const std::string& label, uint32_t bg, uint32_t fg, int maxW, int id) {
            int w = std::min(f.measure(label, 18) + 24, maxW);
            tx -= w;
            gfx::fillRoundRect(c, tx, 83, w, 30, 15, bg);
            f.draw(c, tx + 12, 86, label, 18, fg, w - 24);
            s.hits.push_back({tx, 83, w, 30, id});
            tx -= 8;
        };
        if (!s.sortLabel.empty()) chip("排序：" + s.sortLabel + "  ›", col::row, col::dim, 280, 200);
        if (!s.search.empty()) chip("搜尋：" + s.search, rgb(70, 56, 110), col::text, 260, 201);
    }
    std::string pageText = n ? "第 " + std::to_string(page + 1) + " / " + std::to_string(pages) + " 頁" : "";

    if (n == 0) {
        std::string msg = s.server.empty() ? "請按 Y 開啟選單 →「伺服器設定」輸入伺服器位址"
                        : !s.search.empty() ? "找不到符合搜尋的檔案（按 B 清除搜尋）"
                        : s.category ? "這個分頁沒有檔案（按 B 回到全部）"
                        : "伺服器上沒有可安裝的檔案";
        int w = f.measure(msg, 24);
        f.draw(c, (kWidth - w) / 2, top + rowH * kListRows / 2 - 20, msg, 24, col::dim);
    }
    size_t selected = 0;
    uint64_t selectedBytes = 0;
    for (const auto& it : s.items)
        if (it.selected) { selected++; selectedBytes += it.size; }

    if (s.view == kViewGrid) {
        drawGridBody(c, f, s, left, top, rowH * kListRows - 2);
    } else if (s.view == kViewCompact) {
        drawCompactBody(c, f, s, left, top, rowH * kListRows - 2, pages, page);
    } else {
        for (int i = 0; i < kListRows; i++) {
            size_t idx = s.scroll + i;
            if (idx >= n) break;
            const ListItem& it = s.items[idx];
            int y = top + i * rowH;
            bool cur = idx == s.cursor;
            gfx::fillRect(c, left, y, width, rowH - 2, cur ? col::cursor : col::row);
            s.hits.push_back({left, y, width, rowH - 2, 1000 + (int)idx});
            if (cur) gfx::fillRect(c, left, y, 4, rowH - 2, col::accent);
            int bx = left + 14, by = y + 8;
            if (it.selected) {
                gfx::fillRoundRect(c, bx, by, 18, 18, 4, col::accent);
                thickLine(c, bx + 4, by + 9, bx + 8, by + 13, 3, col::bg);
                thickLine(c, bx + 8, by + 13, bx + 14, by + 5, 3, col::bg);
            } else {
                gfx::fillRoundRect(c, bx, by, 18, 18, 4, col::dim);
                gfx::fillRoundRect(c, bx + 2, by + 2, 14, 14, 3, cur ? col::cursor : col::row);
            }
            std::string size = humanSize(it.size);
            int sw = f.measure(size, 18);
            f.draw(c, left + width - 12 - sw, y + 8, size, 18, col::dim);
            int bw = 0;
            drawBadge(c, f, left + width - 12 - sw - 10, y + 5, it.badge, bw);
            std::string base = it.name.substr(it.name.rfind('/') == std::string::npos ? 0 : it.name.rfind('/') + 1);
            int nameW = width - 44 - sw - 28 - (bw ? bw + 10 : 0);
            if (cur) { if (marquee(c, f, left + 44, y + 6, base, 20, col::text, nameW, s.marqueeMs)) s.animating = true; }
            else f.draw(c, left + 44, y + 6, base, 20, col::text, nameW);
        }
        // page position bar
        if (pages > 1) {
            int trackH = rowH * kListRows - 2;
            int thumbH = trackH / (int)pages;
            if (thumbH < 16) thumbH = 16;
            int thumbY = top + (int)((trackH - thumbH) * page / (pages - 1));
            gfx::fillRect(c, left + width + 4, top, 4, trackH, col::row);
            gfx::fillRect(c, left + width + 4, thumbY, 4, thumbH, col::dim);
        }

        drawCoverPanel(c, f, s, left + width + gap, top, panelW, rowH * kListRows - 2);
    }

    // status line
    int sy = top + rowH * kListRows + 8;
    std::string right = n ? (std::to_string(n) + " 個檔案  /  " + pageText) : "";
    if (selected) {
        std::string sel = "已選 " + std::to_string(selected) + " 個（" + humanSize(s.selNeed ? s.selNeed : selectedBytes) + "）";
        if (s.selAvail >= 0) sel += " · 剩餘可用 " + humanSize((uint64_t)s.selAvail);
        right = sel + "  /  " + right;
    }
    int rw = f.measure(right, 20);
    f.draw(c, kWidth - 32 - rw, sy, right, 20, col::dim);
    f.draw(c, 32, sy, s.status, 20, col::text, kWidth - 96 - rw);

    // B only goes back (search / another tab → the start view); reading the server again is Y → 重新整理
    bool initial = s.search.empty() && s.category == 0;
    std::vector<std::pair<std::string, std::string>> hints;
    if (s.installSummary.empty())
        hints = {{"A", "選取"}, {"X", "安裝"}, {"Y", "選單"}, {"ZL/ZR", "切換分頁"}, {"L/R", "翻頁"}};
    else
        hints = {{"A", "選取"}, {"X", "加入安裝"}, {"-", "安裝進度"}, {"Y", "選單"}, {"ZL/ZR", "分頁"}, {"L/R", "翻頁"}};
    if (!initial) hints.push_back({"B", "返回"});
    hints.push_back({"+", "離開"});
    drawFooter(c, f, hints);

    drawMenuOverlay(c, f, s.menu, s.hits);
}

// ------------------------------------------------------------------ 詳細管理遊戲內容
void drawManage(gfx::Canvas& c, gfx::Font& f, ManageState& s) {
    gfx::clear(c, col::bg);
    s.hits.clear();
    s.animating = false;
    std::string head = "遊戲書架  " + s.version;
    drawHeader(c, f, head, "", col::text);
    int hx = 32 + f.measure(head, 32) + 24;
    f.draw(c, hx, 22, "›  詳細管理遊戲內容", 28, col::accent);

    const int top = 124, rowH = 36, left = 24, panelW = 380, gap = 16;
    const int width = kWidth - 48 - panelW - gap;
    size_t n = s.rows.size();
    if (n && s.cursor >= n) s.cursor = n - 1;
    size_t per = (size_t)kListRows;
    size_t pages = n ? (n + per - 1) / per : 1, page = n ? s.cursor / per : 0;
    s.scroll = page * per;

    // summary line + sort chip
    std::string summary = "主機上已安裝 " + std::to_string(n) + " 款遊戲，共 " + humanSize(s.total);
    f.draw(c, 24, 84, summary, 22, col::text);
    bool paused = s.checkText.compare(0, strlen("伺服器沒有回應"), "伺服器沒有回應") == 0;
    if (!s.checkText.empty())
        f.draw(c, 24 + f.measure(summary, 22) + 20, 87, paused ? s.checkText : "比對伺服器檔案：" + s.checkText, 18,
               paused ? rgb(250, 200, 90) : col::dim);
    {
        std::string chip = "排序：" + s.sortLabel + "  ›";
        int w = f.measure(chip, 18) + 24, x = kWidth - 32 - w;
        gfx::fillRoundRect(c, x, 83, w, 30, 15, col::row);
        f.draw(c, x + 12, 86, chip, 18, col::dim);
        s.hits.push_back({x, 83, w, 30, 200});
        if (s.sdFree) {
            std::string sd = "SD 卡剩餘 " + humanSize(s.sdFree);
            f.draw(c, x - 24 - f.measure(sd, 20), 86, sd, 20, col::dim);
        }
    }

    if (n == 0) {
        std::string msg = "主機上沒有已安裝的遊戲";
        f.draw(c, (kWidth - f.measure(msg, 24)) / 2, top + rowH * kListRows / 2 - 20, msg, 24, col::dim);
    }
    for (int i = 0; i < kListRows; i++) {
        size_t idx = s.scroll + (size_t)i;
        if (idx >= n) break;
        const ManageRow& r = s.rows[idx];
        int y = top + i * rowH;
        bool cur = idx == s.cursor;
        gfx::fillRect(c, left, y, width, rowH - 2, cur ? col::cursor : col::row);
        if (cur) gfx::fillRect(c, left, y, 4, rowH - 2, col::accent);
        s.hits.push_back({left, y, width, rowH - 2, 1000 + (int)idx});
        std::string size = humanSize(r.size);
        int sw = f.measure(size, 18);
        f.draw(c, left + width - 12 - sw, y + 8, size, 18, col::dim);
        int right = left + width - 12 - sw - 12;
        if (r.notOnNas) {
            std::string t = r.checking ? std::string(paused ? "未確認" : "比對中…") : std::string("伺服器上沒有");
            int bw = f.measure(t, 16) + 16;
            gfx::fillRoundRect(c, right - bw, y + 5, bw, 24, 12, r.checking ? col::key : rgb(96, 62, 24));
            f.draw(c, right - bw + 8, y + 7, t, 16, r.checking ? col::dim : rgb(250, 200, 130));
            right -= bw + 12;
        }
        int nameW = std::min(f.measure(r.name, 20), (right - left - 16) * 2 / 3);
        if (cur) { if (marquee(c, f, left + 16, y + 6, r.name, 20, col::text, nameW, s.marqueeMs)) s.animating = true; }
        else f.draw(c, left + 16, y + 6, r.name, 20, col::text, nameW);
        int cx = left + 16 + nameW + 14;
        if (cx < right - 40) f.draw(c, cx, y + 9, r.contents, 16, r.noBase ? col::yellow : col::dim, right - cx - 8);
    }
    if (pages > 1) {
        int trackH = rowH * kListRows - 2, thumbH = std::max(16, trackH / (int)pages);
        int thumbY = top + (int)((trackH - thumbH) * page / (pages - 1));
        gfx::fillRect(c, left + width + 4, top, 4, trackH, col::row);
        gfx::fillRect(c, left + width + 4, thumbY, 4, thumbH, col::dim);
    }

    // side panel: cover, name, id, each part's size
    int px = left + width + gap, ph = rowH * kListRows - 2;
    gfx::fillRoundRect(c, px, top, panelW, ph, 12, col::panel);
    if (n) {
        const ManageRow& r = s.rows[s.cursor];
        const int box = 260;
        int bx = px + (panelW - box) / 2, by = top + 16;
        gfx::fillRoundRect(c, bx, by, box, box, 10, col::row);
        if (s.cover.state == CoverState::Ready && s.cover.px) {
            blitCover(c, s.cover, bx, by, box);
        } else {
            std::string msg = s.cover.state == CoverState::Missing ? "沒有封面" : "封面載入中…";
            f.draw(c, bx + (box - f.measure(msg, 20)) / 2, by + box / 2 - 12, msg, 20, col::dim);
        }
        int ty = by + box + 12;
        if (marquee(c, f, px + 20, ty, r.name, 22, col::text, panelW - 40, s.marqueeMs)) s.animating = true;
        char id[24];
        snprintf(id, sizeof id, "%016llX", (unsigned long long)r.titleId);
        f.draw(c, px + 20, ty + 32, id, 16, col::dim);
        std::string total = humanSize(r.size);
        f.draw(c, px + panelW - 20 - f.measure(total, 22), ty + 28, total, 22, col::accent);
        int ly = ty + 60;
        if (r.noBase) {
            f.draw(c, px + 20, ly, "缺本體，無法遊玩", 18, col::yellow);
            ly += 24;
        }
        for (size_t k = 0; k < r.parts.size() && ly < top + ph - 22; k++, ly += 24) {
            std::string sz = humanSize(r.parts[k].second);
            int szw = f.measure(sz, 18);
            f.draw(c, px + 20, ly, r.parts[k].first, 18, col::text, panelW - 60 - szw);
            f.draw(c, px + panelW - 20 - szw, ly, sz, 18, col::dim);
        }
    }

    int sy = top + rowH * kListRows + 8;
    std::string right = n ? std::to_string(n) + " 款  /  第 " + std::to_string(page + 1) + " / " + std::to_string(pages) + " 頁" : "";
    int rw = f.measure(right, 20);
    f.draw(c, kWidth - 32 - rw, sy, right, 20, col::dim);
    f.draw(c, 32, sy, s.status, 20, col::text, kWidth - 96 - rw);
    drawFooter(c, f, {{"A", "管理內容"}, {"Y", "排序"}, {"L/R", "翻頁"}, {"B", "返回"}});
    drawMenuOverlay(c, f, s.menu, s.hits);
}

// ------------------------------------------------------------------ progress
void drawProgress(gfx::Canvas& c, gfx::Font& f, ProgressState& s) {
    gfx::clear(c, col::bg);
    s.hits.clear();
    s.animating = false;
    size_t running = 0;
    double total = 0;
    for (const auto& l : s.lanes)
        if (l.active) { running++; total += l.speedMBs; }
    char head[96];
    snprintf(head, sizeof(head), "安裝進度  已完成 %zu / %zu", s.finished, s.count);
    char right[64];
    snprintf(right, sizeof(right), "同時 %zu 個  ·  %.1f MB/s", running, total);
    drawHeader(c, f, head, right, col::accent);
    if (s.background && running) {  // turn the screen off while installing (any button turns it back on)
        int tx = 32 + f.measure(head, 32) + 24;
        const char* t = "關閉螢幕";
        int cw = f.measure(t, 18) + 28;
        gfx::fillRoundRect(c, tx, 20, cw, 32, 16, col::key);
        f.draw(c, tx + 14, 25, t, 18, col::text);
        s.hits.push_back({tx, 20, cw, 32, 530});
    }

    const bool lists = !s.groups.empty() || !s.recent.empty();
    const int x0 = 40, w = kWidth - 80;
    // compact lanes when the lists are shown: name + numbers on one line, stage, bar
    const int laneH = lists ? 84 : 158;
    int y = 88;
    for (size_t i = 0; i < s.lanes.size(); i++, y += laneH + 8) {
        const ProgressLane& l = s.lanes[i];
        gfx::fillRoundRect(c, x0, y, w, laneH, 12, col::panel);
        if (!l.active) {
            f.draw(c, x0 + 24, y + laneH / 2 - 13, s.cancelling ? "正在停止…" : "閒置", 20, col::dim);
            continue;
        }
        double frac = l.total ? (double)l.done / (double)l.total : 0.0;
        if (frac > 1) frac = 1;
        char pct[16];
        snprintf(pct, sizeof(pct), "%.1f%%", frac * 100.0);
        std::string info = humanSize(l.done) + " / " + humanSize(l.total);
        char speed[64];
        snprintf(speed, sizeof(speed), "   %.1f MB/s", l.speedMBs);
        info += speed;
        if (l.speedMBs > 0.1 && l.total > l.done) {
            unsigned long long secs = (unsigned long long)((double)(l.total - l.done) / 1048576.0 / l.speedMBs);
            char eta[48];
            snprintf(eta, sizeof(eta), "   剩餘 %llu:%02llu:%02llu", secs / 3600, secs / 60 % 60, secs % 60);
            info += eta;
        }
        const int bx = x0 + 24, bw = w - 48;
        if (lists) {
            std::string r = std::string(pct) + "   " + info;
            int rw = f.measure(r, 17);
            f.draw(c, bx + bw - rw, y + 12, r, 17, col::dim);
            f.draw(c, bx, y + 9, l.file, 20, col::text, bw - rw - 24);
            f.draw(c, bx, y + 38, l.stage, 16, col::dim, bw);
            gfx::fillRoundRect(c, bx, y + 64, bw, 10, 5, col::barBg);
            if (frac > 0) gfx::fillRoundRect(c, bx, y + 64, (int)(bw * frac) < 10 ? 10 : (int)(bw * frac), 10, 5, col::accent);
        } else {
            f.draw(c, bx, y + 16, l.file, 22, col::text, bw);
            f.draw(c, bx, y + 48, l.stage, 17, col::dim, bw);
            const int by = y + 76, bh = 16;
            gfx::fillRoundRect(c, bx, by, bw, bh, 8, col::barBg);
            if (frac > 0) gfx::fillRoundRect(c, bx, by, (int)(bw * frac) < bh ? bh : (int)(bw * frac), bh, 8, col::accent);
            f.draw(c, bx, by + 22, pct, 20, col::text);
            int iw = f.measure(info, 18);
            f.draw(c, bx + bw - iw, by + 24, info, 18, col::dim);
        }
    }
    if (lists) {
        const int ly = y + 2, colW = (w - 16) / 2, boxH = kHeight - 60 - ly, bottom = ly + boxH - 8;
        // ---- waiting games: select, move, remove
        {
            const int bx = x0;
            gfx::fillRoundRect(c, bx, ly, colW, boxH, 12, col::panel);
            std::string title = "排隊中（" + std::to_string(s.groups.size()) + " 款）";
            f.draw(c, bx + 18, ly + 10, title, 20, col::dim);
            bool sel = s.groupCursor >= 0 && (size_t)s.groupCursor < s.groups.size();
            if (sel) {  // action chips for the selected game
                const char* labels[] = {"置頂", "上移", "下移", "移除"};
                int cx = bx + colW - 14;
                for (int k = 3; k >= 0; k--) {
                    int cw = f.measure(labels[k], 17) + 22;
                    cx -= cw;
                    gfx::fillRoundRect(c, cx, ly + 8, cw, 30, 15, k == 3 ? col::red : col::key);
                    f.draw(c, cx + 11, ly + 12, labels[k], 17, k == 3 ? col::bg : col::text);
                    s.hits.push_back({cx, ly + 8, cw, 30, 520 + k});
                    cx -= 8;
                }
            }
            const int rowH = 32, top = ly + 46;
            size_t rows = (size_t)std::max(1, (bottom - top) / rowH);
            if (sel) {
                size_t cur = (size_t)s.groupCursor;
                if (cur < s.groupScroll) s.groupScroll = cur;
                if (cur >= s.groupScroll + rows) s.groupScroll = cur - rows + 1;
            }
            if (s.groupScroll + rows > s.groups.size()) s.groupScroll = s.groups.size() > rows ? s.groups.size() - rows : 0;
            for (size_t k = s.groupScroll, r = 0; k < s.groups.size() && r < rows; k++, r++) {
                const auto& g = s.groups[k];
                int ry = top + (int)r * rowH;
                bool isCur = sel && (size_t)s.groupCursor == k;
                if (isCur) {
                    gfx::fillRoundRect(c, bx + 8, ry - 3, colW - 16, rowH - 2, 8, col::cursor);
                    gfx::fillRect(c, bx + 8, ry - 3, 4, rowH - 2, col::accent);
                }
                char num[24];
                snprintf(num, sizeof(num), "%zu.", k + 1);
                f.draw(c, bx + 20, ry + 2, num, 17, col::dim);
                std::string tail = std::to_string(g.files) + " 個檔案" + (g.started ? "・安裝中" : "");
                int tw = f.measure(tail, 16);
                f.draw(c, bx + colW - 20 - tw, ry + 4, tail, 16, g.started ? col::accent : col::dim);
                f.draw(c, bx + 60, ry + 1, g.label, 18, col::text, colW - 100 - tw);
                s.hits.push_back({bx + 8, ry - 3, colW - 16, rowH - 2, 600 + (int)k});
            }
            if (s.groups.empty()) f.draw(c, bx + 18, top, "（沒有）", 18, col::dim);
            else if (s.groups.size() > rows) {  // scroll bar
                int trackH = bottom - top;
                int thumbH = std::max(16, (int)(trackH * rows / s.groups.size()));
                int thumbY = top + (int)((trackH - thumbH) * s.groupScroll / (s.groups.size() - rows));
                gfx::fillRect(c, bx + colW - 8, top, 3, trackH, col::row);
                gfx::fillRect(c, bx + colW - 8, thumbY, 3, thumbH, col::dim);
            }
        }
        // ---- recently finished, newest first; the reason gets its own wrapped lines
        {
            const int bx = x0 + colW + 16;
            gfx::fillRoundRect(c, bx, ly, colW, boxH, 12, col::panel);
            f.draw(c, bx + 18, ly + 10, "最近完成", 20, col::dim);
            int iy = ly + 46;
            for (size_t k = 0; k < s.recent.size() && iy < bottom - 18; k++) {
                uint32_t colr = (k < s.recentOk.size() && s.recentOk[k]) ? col::green : col::red;
                f.draw(c, bx + 18, iy, s.recent[k], 18, colr, colW - 36);
                iy += 26;
                if (k < s.recentDetail.size() && !s.recentDetail[k].empty()) {
                    bool warn = k < s.recentWarn.size() && s.recentWarn[k];
                    uint32_t dc = (colr == col::red || warn) ? col::yellow : col::dim;
                    std::vector<std::string> lines = wrap(f, s.recentDetail[k], 16, colW - 56);
                    for (size_t li = 0; li < lines.size() && li < 3 && iy < bottom - 14; li++, iy += 22)
                        f.draw(c, bx + 34, iy, lines[li], 16, dc, colW - 56);
                }
                iy += 6;
            }
            if (s.recent.empty()) f.draw(c, bx + 18, ly + 46, "（沒有）", 18, col::dim);
        }
    } else {
        f.draw(c, 40, kHeight - 90, s.cancelling ? (s.discarding ? "正在取消並刪除已下載的部分，請稍候…" : "正在取消，請稍候…") : "安裝期間主機不會進入休眠，HOME 鍵暫時停用。",
               20, s.cancelling ? col::yellow : col::dim);
    }
    if (s.background) {
        if (!s.groups.empty())
            drawFooter(c, f, {{"↑↓", "選擇排隊遊戲"}, {"ZL/ZR", "上移／下移"}, {"X", "移除"}, {"R", "關閉螢幕"},
                              {"B", "回到清單"}, {"Y", "按住：全部取消並清除"}});
        else
            drawFooter(c, f, {{"B", "回到清單（繼續在背景安裝）"}, {"R", "關閉螢幕"}, {"Y", "按住：全部取消並清除"}});
    } else {
        drawFooter(c, f, {{"B", "按住取消"}});
    }
}

// ------------------------------------------------------------------ search keyboard (注音 / 倉頡 / 英數)
void drawIme(gfx::Canvas& c, gfx::Font& f, ImeState& s) {
    gfx::clear(c, col::bg);
    s.hits.clear();
    const char* modeName = s.mode == ime::Mode::Zhuyin ? "注音" : s.mode == ime::Mode::Cangjie ? "倉頡" : "英數";
    char right[64];
    snprintf(right, sizeof(right), "符合 %zu / %zu 款", s.matches, s.total);
    drawHeader(c, f, std::string("搜尋：") + modeName + "輸入", right, s.matches ? col::accent : col::yellow);

    // input box: text + what is being typed (underlined) + caret
    const int bx = 40, by = 92, bw = kWidth - 80, bh = 64;
    gfx::fillRoundRect(c, bx, by, bw, bh, 12, col::panel);
    gfx::fillRect(c, bx, by + bh - 3, bw, 3, col::accent);
    std::string comp = s.mode == ime::Mode::Cangjie ? ime::cangjieRadicals(s.composing) : s.composing;
    int x = bx + 20;
    if (s.text.empty() && comp.empty()) {
        f.draw(c, x, by + 16, "輸入遊戲名稱的一部分（例如「瑪利歐」或遊戲 ID）", 24, col::dim, bw - 40);
    } else {
        int tw = f.measure(s.text, 28);
        int cw = comp.empty() ? 0 : f.measure(comp, 28);
        int shift = std::max(0, tw + cw + 24 - (bw - 40));  // keep the end visible
        f.drawClipped(c, x, by + 14, s.text, 28, col::text, bw - 40, shift);
        int cx = x + tw - shift;
        if (!comp.empty()) {
            f.draw(c, cx, by + 14, comp, 28, col::accent);
            gfx::fillRect(c, cx, by + 50, cw, 2, col::accent);
            cx += cw;
        }
        gfx::fillRect(c, cx + 3, by + 14, 3, 34, col::text);
    }

    // candidates
    const int cy = by + bh + 12, ch = 60;
    gfx::fillRoundRect(c, bx, cy, bw, ch, 12, col::panel);
    if (s.cands.empty()) {
        const char* msg = s.composing.empty()
                              ? (s.mode == ime::Mode::Latin ? "英數模式直接輸入" : "輸入注音或倉頡字根，這裡會列出檔名裡有的字")
                              : "檔名裡沒有符合的字";
        f.draw(c, bx + 20, cy + 16, msg, 22, col::dim, bw - 40);
    } else {
        const int cell = 64;
        size_t per = (size_t)((bw - 40) / cell);
        size_t first = s.candSel >= per ? s.candSel - per + 1 : 0;
        for (size_t i = first, k = 0; i < s.cands.size() && k < per; i++, k++) {
            int qx = bx + 20 + (int)k * cell;
            bool sel = i == s.candSel;
            if (sel) gfx::fillRoundRect(c, qx, cy + 6, cell - 6, ch - 12, 10, col::cursor);
            int w = f.measure(s.cands[i], 30);
            f.draw(c, qx + (cell - 6 - w) / 2, cy + 12, s.cands[i], 30, sel ? col::accent : col::text);
            s.hits.push_back({qx, cy + 6, cell - 6, ch - 12, 900 + (int)i});
        }
        if (s.cands.size() > per) {
            std::string more = std::to_string(s.candSel + 1) + " / " + std::to_string(s.cands.size());
            int mw = f.measure(more, 16);
            f.draw(c, bx + bw - 16 - mw, cy + ch - 22, more, 16, col::dim);
        }
    }

    // keys
    const auto& rows = ime::layout(s.mode);
    const int ky0 = cy + ch + 16, gap = 8;
    const int kh = (kHeight - 60 - ky0 - gap * ((int)rows.size() - 1)) / (int)rows.size();
    for (size_t r = 0; r < rows.size(); r++) {
        int units = 0;
        for (const auto& k : rows[r]) units += k.width;
        // a key `width` units wide also covers the gaps it spans
        int unit = std::min(104, (bw - gap * (std::max(units, 11) - 1)) / std::max(units, 11));
        int rowW = units * unit + gap * (units - 1);
        int kx = bx + (bw - rowW) / 2;
        int ky = ky0 + (int)r * (kh + gap);
        for (size_t k = 0; k < rows[r].size(); k++) {
            const ime::Key& key = rows[r][k];
            int kw = key.width * unit + gap * (key.width - 1);
            bool cur = (int)r == s.row && (int)k == s.col;
            uint32_t bg = key.action == ime::kDone ? rgb(36, 150, 82) : key.action ? col::key : col::panel;
            gfx::fillRoundRect(c, kx, ky, kw, kh, 10, bg);
            if (cur) {
                gfx::fillRoundRect(c, kx - 3, ky - 3, kw + 6, 4, 2, col::accent);
                gfx::fillRoundRect(c, kx - 3, ky + kh - 1, kw + 6, 4, 2, col::accent);
                gfx::fillRect(c, kx - 3, ky - 3, 4, kh + 6, col::accent);
                gfx::fillRect(c, kx + kw - 1, ky - 3, 4, kh + 6, col::accent);
            }
            std::string label = key.label;
            if (s.mode == ime::Mode::Zhuyin && !s.zhuyinGlyphs && !key.action) label = key.value;  // fallback
            int px = key.action ? 24 : 32;
            int lw = f.measure(label, px);
            f.draw(c, kx + (kw - lw) / 2, ky + (kh - px) / 2 - 2, label, px, cur ? col::accent : col::text);
            if (!key.hint.empty()) f.draw(c, kx + 8, ky + 4, key.hint, 15, col::dim);
            s.hits.push_back({kx, ky, kw, kh, 700 + (int)r * 20 + (int)k});
            kx += kw + gap;
        }
    }
    drawFooter(c, f, {{"A", "輸入"}, {"B", "刪除"}, {"L/R", "選字"}, {"Y", "用這個字"}, {"X", "切換注音／倉頡／英數"},
                      {"+", "完成"}, {"-", "取消"}});
}

// ------------------------------------------------------------------ choice
void drawChoice(gfx::Canvas& c, gfx::Font& f, const std::string& title, const std::vector<std::string>& lines,
                const std::vector<ChoiceOption>& options, std::vector<HitBox>& hits) {
    gfx::clear(c, col::bg);
    drawHeader(c, f, "遊戲書架", "", col::text);
    hits.clear();
    int tw = f.measure(title, 34);
    f.draw(c, (kWidth - tw) / 2, 120, title, 34, col::text);
    int y = 190;
    for (const auto& line : lines) {
        // "!" = warning line (yellow), "#" = section heading (white)
        std::string l = line;
        uint32_t colr = col::dim;
        if (!l.empty() && l[0] == '!') { colr = col::yellow; l.erase(0, 1); }
        else if (!l.empty() && l[0] == '#') { colr = col::text; l.erase(0, 1); }
        if (!l.empty()) {
            int lw = std::min(f.measure(l, 22), kWidth - 120);
            f.draw(c, (kWidth - lw) / 2, y, l, 22, colr, kWidth - 120);
        }
        y += 36;
    }
    // big buttons, one per option
    const int bw = 330, bh = 96, gap = 28;
    int total = (int)options.size() * bw + ((int)options.size() - 1) * gap;
    int bx = (kWidth - total) / 2, by = std::max(y + 30, 470);
    for (const auto& o : options) {
        gfx::fillRoundRect(c, bx, by, bw, bh, 18, o.color);
        // key "coin"
        gfx::fillRoundRect(c, bx + 22, by + (bh - 56) / 2, 56, 56, 28, rgb(250, 250, 252));
        int kw = f.measure(o.key, 30);
        f.draw(c, bx + 22 + (56 - kw) / 2, by + (bh - 56) / 2 + 9, o.key, 30, o.color);
        f.draw(c, bx + 96, by + (bh - 34) / 2, o.label, 30, rgb(255, 255, 255), bw - 110);
        hits.push_back({bx, by, bw, bh, o.id});
        bx += bw + gap;
    }
    f.draw(c, (kWidth - f.measure("也可以直接點按鈕", 18)) / 2, by + bh + 26, "也可以直接點按鈕", 18, col::dim);
}


// ------------------------------------------------------------------ result
void drawResult(gfx::Canvas& c, gfx::Font& f, const ResultState& s) {
    gfx::clear(c, col::bg);
    drawHeader(c, f, "安裝結果", "", col::text);

    int x = 64, y = 108;
    int w = f.draw(c, x, y, "成功 " + std::to_string(s.ok) + " 個", 32, col::green);
    f.draw(c, x + w + 40, y, "失敗 " + std::to_string(s.failures.size()) + " 個", 32,
           s.failures.empty() ? col::dim : col::red);
    y += 70;

    auto block = [&](const std::vector<std::string>& lines, uint32_t color) {
        for (const auto& l : lines) {
            bool first = true;
            for (const auto& part : wrap(f, l, 22, kWidth - 128 - 30)) {
                if (y > kHeight - 100) return;
                if (first) gfx::fillRoundRect(c, x + 4, y + 10, 12, 12, 6, color);
                f.draw(c, x + 30, y, part, 22, color);
                y += 32;
                first = false;
            }
            y += 10;
        }
    };
    block(s.failures, col::red);
    block(s.notes, col::yellow);
    if (s.ok && s.failures.empty() && s.notes.empty()) {
        f.draw(c, x, y, "已安裝的遊戲會出現在 HOME 選單。", 22, col::text);
    } else if (s.ok) {
        f.draw(c, x, y, "已安裝的遊戲會出現在 HOME 選單。", 22, col::dim);
    }
    drawFooter(c, f, {{"A", "繼續"}});
}

// ------------------------------------------------------------------ message
void drawConnect(gfx::Canvas& c, gfx::Font& f, const ConnectState& s) {
    gfx::clear(c, col::bg);
    drawHeader(c, f, "遊戲書架  " + s.version, "", col::text);
    std::string title = s.failed ? "無法連上伺服器" : s.stage >= 4 ? "連線完成" : "正在連線伺服器…";
    if (s.failed && s.stage >= 2) title = "無法讀取檔案清單";
    int tw = f.measure(title, 34);
    f.draw(c, (kWidth - tw) / 2, 150, title, 34, s.failed ? col::red : col::text);
    if (!s.server.empty()) {
        int sw = f.measure(s.server, 24);
        f.draw(c, (kWidth - sw) / 2, 205, s.server, 24, col::accent);
    }
    int bx = 240, bw = kWidth - 480, by = 270, bh = 22;
    gfx::fillRoundRect(c, bx, by, bw, bh, 11, col::barBg);
    double p = s.percent < 0 ? 0 : s.percent > 1 ? 1 : s.percent;
    int fw = (int)(bw * p);
    if (fw > 0)
        gfx::fillRoundRect(c, bx, by, std::max(fw, bh), bh, 11, s.failed ? col::red : s.stage >= 4 ? col::green : col::accent);
    char pct[16];
    snprintf(pct, sizeof pct, "%d%%", (int)(p * 100 + 0.5));
    f.draw(c, bx + bw + 16, by - 3, pct, 22, col::dim);
    static const char* names[] = {"檢查主機上的遊戲紀錄", "連線伺服器（查網址、連線、登入）", "讀取伺服器檔案清單",
                                  "比對主機上已安裝的遊戲"};
    int y = 330;
    for (int k = 0; k < 4; k++) {
        bool done = k < s.stage, cur = k == s.stage;
        uint32_t colr = done ? col::green : cur ? (s.failed ? col::red : col::text) : col::dim;
        // marks drawn, not typed: the console's fonts have no check mark (it showed as "?")
        if (done) gfx::fillRoundRect(c, 422, y + 5, 18, 18, 9, col::green);           // finished
        else if (!cur) gfx::fillRoundRect(c, 427, y + 10, 8, 8, 4, col::dim);         // still to come
        else f.draw(c, 420, y, s.failed ? "×" : "▶", 24, colr);                       // now / failed here
        f.draw(c, 460, y, names[k], 24, colr);
        y += 44;
    }
    y += 14;
    for (const auto& part : wrap(f, s.detail, 20, kWidth - 256)) {
        int dw = f.measure(part, 20);
        f.draw(c, (kWidth - dw) / 2, y, part, 20, s.failed ? col::yellow : col::dim);
        y += 30;
    }
    // while connecting there is nothing to press: it ends by itself (each step has a time limit)
    if (s.failed) drawFooter(c, f, {{"A", "重試"}, {"Y", "伺服器設定"}, {"B", "先不連線，直接進入"}});
}

void drawMessage(gfx::Canvas& c, gfx::Font& f, const std::string& title, const std::vector<std::string>& lines,
                 const std::string& hint) {
    gfx::clear(c, col::bg);
    drawHeader(c, f, "遊戲書架", "", col::text);
    int y = 200;
    int tw = f.measure(title, 32);
    f.draw(c, (kWidth - tw) / 2, y, title, 32, col::text);
    y += 70;
    for (const auto& l : lines) {
        for (const auto& part : wrap(f, l, 22, kWidth - 256)) {
            int w = f.measure(part, 22);
            f.draw(c, (kWidth - w) / 2, y, part, 22, col::dim);
            y += 34;
        }
    }
    if (!hint.empty()) {
        // "A 繼續　X 放棄" (pairs separated by an ideographic space) shows one key per pair;
        // a plain text like "離開" is the A button.
        std::vector<std::pair<std::string, std::string>> keys;
        const std::string sep = "\xE3\x80\x80";  // U+3000
        size_t pos = 0;
        while (pos <= hint.size()) {
            size_t next = hint.find(sep, pos);
            std::string part = hint.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
            size_t sp = part.find(' ');
            if (sp != std::string::npos && sp > 0 && sp <= 2) keys.push_back({part.substr(0, sp), part.substr(sp + 1)});
            else if (!part.empty()) keys.push_back({"A", part});
            if (next == std::string::npos) break;
            pos = next + sep.size();
        }
        drawFooter(c, f, keys);
    }
}

}  // namespace screens
