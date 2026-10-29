// 遊戲書架 (GameShelf) - install your own NSP / NSZ / XCI / XCZ backups from your own PC / NAS.
// UI is drawn into a framebuffer with the console's built-in Traditional Chinese system font.
#include <switch.h>

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <functional>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <climits>
#include <cmath>
#include <string>
#include <vector>

#include "badfile.hpp"
#include "container.hpp"
#include "covers.hpp"
#include "gfx.hpp"
#include "http.hpp"
#include "install.hpp"
#include "remote.hpp"
#include "resume.hpp"
#include "servers.hpp"
#include "reports.hpp"
#include "shortcut.hpp"
#include "tsconv.hpp"
// The HOME shortcut's loader and icon are built by CI into switch/data (see switch/forwarder).
#if __has_include("forwarder_main_bin.h") && __has_include("forwarder_npdm_bin.h") && __has_include("shortcut_icon_bin.h")
#include "forwarder_main_bin.h"
#include "forwarder_npdm_bin.h"
#include "shortcut_icon_bin.h"
#define HAVE_SHORTCUT 1
#endif
#include "update.hpp"
#include "ime.hpp"
#include "screens.hpp"
#include "titledb.hpp"
#include "titles.hpp"

// libnx: 1 = on exit, ask the system to close the application properly (ISelfController::Exit)
// instead of returning to the homebrew loader.
extern "C" u32 __nx_applet_exit_mode;

#define APP_VERSION "v3.0.12"
static const char* kConfigPath = "sdmc:/config/lan-installer/config.ini";
static const char* kCoverDir = "sdmc:/config/lan-installer/covers";
// "{id}" = 16-digit base title id. Can be changed with a "cover=" line in config.ini (empty = off).
static const char* kDefaultCoverUrl = "https://tinfoil.media/ti/{id}/256/256/";
static const int kCoverBox = 340;
// Atmosphère's hosts file sends every Nintendo address to the console itself (so it never talks to
// Nintendo), so eShop icons are fetched through a public image proxy that also shrinks them.
// "{url}" = the icon address, encoded. Change with "icon_proxy=" in config.ini; empty = direct.
static const char* kDefaultIconProxy = "https://wsrv.nl/?url={url}&w=256&h=256&fit=inside&output=jpg&q=85";


static const char* kTitleDbCache = "sdmc:/config/lan-installer/titledb.tsv";
// Game notes (ID, release date, cover URL), rebuilt every week on GitHub from the Hong Kong, Korea,
// Japan and US titledb files (tools/make_titledb_notes.py); about 3 MB. If it cannot be reached,
// the Hong Kong file itself (about 50 MB). Override with "titledb=url1,url2" in config.ini.
static const char* kDefaultTitleDb =
    "https://github.com/Antaite/GameShelf-releases/releases/download/titledb/titledb-mini.tsv,"
    "https://raw.githubusercontent.com/blawar/titledb/master/HK.zh.json";
// The v3.0.0 default (HK, TW, US): replaced by the new default when found in config.ini.
static const char* kOldTitleDb =
    "https://raw.githubusercontent.com/blawar/titledb/master/HK.zh.json,"
    "https://raw.githubusercontent.com/blawar/titledb/master/TW.zh.json,"
    "https://raw.githubusercontent.com/blawar/titledb/master/US.en.json";

struct App {
    PadState pad;
    Framebuffer fb;
    gfx::Font font;
    std::vector<remote::RemoteFile> files;   // everything on the server (merged packages split into their titles)
    std::vector<remote::RemoteFile> raw;     // the server's list as it is
    std::map<std::string, size_t> urlIndex;  // url -> index in files
    std::string greeting;                    // the server's greeting from its list ("" = none)
    bool openServerMenu = false;             // connecting failed and the player chose 伺服器設定
    std::vector<titles::FileInfo> info;      // per file: title id / version / kind
    std::vector<titles::Status> status;      // per file: compared with the console
    std::vector<bool> selected;              // per file
    std::vector<size_t> view;                // files shown (category + search filter)
    titles::Installed installed;
    bool installedOk = false;
    std::set<u64> cardBases;  // installed as unconverted cartridge dumps
    int category = 0;                        // tab: 0 全部, 1 可更新, 2 可裝 DLC, 3 已安裝
    int parallel = 0;                        // installs at the same time: 1-3, 0 = automatic
    int menuPage = 0;                        // see MenuPage
    servers::Store store;                    // the user's servers and which one is in use
    int menuServer = -1;                     // server shown in the "one server" menu page
    std::map<u64, std::string> dispVer;      // installed display version per game ("" = unknown)
    std::map<std::string, uint64_t> instSize;  // url -> size once installed (compressed files measured from their headers)
    // One row per game: the [BASE] file stands for the game, its update / DLC files install with it.
    struct Row {
        size_t rep = 0;                 // file shown in the list (the base file)
        std::vector<size_t> members;    // base, newest update, newest of each DLC
        std::vector<size_t> needed;     // what X installs now (missing parts, or all when the base is missing)
        int update = -1;                // newest update file, -1 = none on the server
        size_t dlcCount = 0;
        bool baseInstalled = false, updateAvailable = false, dlcAvailable = false, card = false;
        uint64_t total = 0, neededSize = 0;
        u64 app = 0;                              // the game (0 = a file without a title id)
        std::map<size_t, std::set<u64>> skip;     // per needed file: titles in it not to install (older / not needed)
        size_t dlcHave = 0, dlcNeed = 0;          // DLC titles already on the console / to install
        bool merged = false;                      // a needed file holds several titles (base + update / DLC)
        std::string updateText;                   // newest update on the server, e.g. "v1.1.0"
    };
    std::vector<Row> rows;
    std::map<size_t, size_t> rowOf;          // rep file -> index in rows
    // background installs
    bool showProgress = false;               // progress view on top of the list
    std::vector<std::string> recent;         // finished files of this session, newest first
    std::vector<bool> recentOk;
    std::vector<std::string> recentDetail;   // full reason / note per finished file
    std::vector<bool> recentWarn;            // installed with a problem: detail in yellow
    u64 qSel = 0;          // selected waiting game on the progress screen (group key, 0 = none)
    size_t qScroll = 0;    // its list's scroll position
    u64 progressSince = 0; // when the progress screen opened (drives the marquee)
    bool boost = true;     // raise the CPU clock while installing (setting "boost")
    bool screenOff = false;  // backlight switched off from the progress screen
    std::vector<resume::QueueEntry> retryLater;  // files that failed on the network, offered again
    std::vector<screens::HitBox> progressHits;
    size_t sessionOk = 0, sessionFail = 0;
    u64 yHeld = 0;                           // progress view: hold Y to cancel all
    bool touching = false;                   // touch screen: finger down
    int touchX = 0, touchY = 0, touchStartX = 0, touchStartY = 0;
    int sort = 0;                            // see kSorts
    std::vector<std::string> names;          // per file: display name (titledb title or file name)
    std::vector<uint32_t> dates;             // per file: release date of the game, 0 = unknown
    std::vector<std::string> searchKeys;     // per file: search key of its file name ("" = not made yet)
    std::unique_ptr<titledb::Db> db;
    uint32_t dbGen = 0;          // titledb data last applied
    uint32_t tidGen = 0;         // real-TID scan results last applied
    std::string dbShown;         // titledb message last shown
    std::string search;
    u64 lastMove = 0;                        // ms, for the marquee
    screens::ListState list;
    std::unique_ptr<covers::Cache> covers;
    std::shared_ptr<const covers::Image> shownCover;  // keeps pixels alive while drawn
    std::vector<std::shared_ptr<const covers::Image>> shownCovers;  // cover wall: the page's pictures
};

// ------------------------------------------------------------------ drawing helpers
// Started from the album (applet mode): far less memory than when started through a game.
static bool g_appletMode = false;

// Like framebufferBegin, but a failed buffer request skips this frame instead of ending the app
// (libnx aborts there, e.g. while memory is short during big installs).
static bool updateSysStatus();
static void present(App& a, const std::function<void(gfx::Canvas&)>& draw) {
    updateSysStatus();  // header clock / network on every screen
    s32 slot = -1;
    Result rc = nwindowDequeueBuffer(a.fb.win, &slot, nullptr);
    if (R_FAILED(rc)) {
        static int logged = 0;
        if (logged++ < 5) install::log("screen: frame skipped (buffer request 0x%X)", rc);
        svcSleepThread(16000000ULL);
        return;
    }
    u32* px = (u32*)(a.fb.buf_linear ? a.fb.buf_linear : (u8*)a.fb.buf + slot * a.fb.fb_size);
    gfx::Canvas c{px, screens::kWidth, screens::kHeight, (int)(a.fb.stride / sizeof(u32))};
    draw(c);
    framebufferEnd(&a.fb);
}

static void showMessage(App& a, const std::string& title, const std::vector<std::string>& lines, const std::string& hint) {
    present(a, [&](gfx::Canvas& c) { screens::drawMessage(c, a.font, title, lines, hint); });
}

static bool installsActive();  // below
// Updates the cover shown for the item under the cursor. Returns true if it changed.
static bool updateCover(App& a) {
    screens::CoverView v;
    std::shared_ptr<const covers::Image> img;
    if (!a.list.items.empty() && a.list.cursor < a.view.size()) {
        uint64_t id = a.info[a.view[a.list.cursor]].baseId;
        v.titleId = id;
        if (id && a.covers) {
            if (a.list.view != screens::kViewGrid) a.covers->request(id);  // the wall asks for its whole page below
            switch (a.covers->get(id, img)) {
                case covers::State::Ready: v.state = screens::CoverState::Ready; break;
                case covers::State::Missing: v.state = screens::CoverState::Missing; break;
                default: v.state = screens::CoverState::Loading; break;
            }
        }
    }
    if (img) { v.w = img->w; v.h = img->h; v.px = img->px.data(); }
    if (a.covers && !a.list.items.empty() && !installsActive()) {
        // covers likely next: around the cursor in the list, the previous / next page on the wall
        // (not while installing: downloads keep the bandwidth)
        static size_t lastKey = (size_t)-1;
        size_t n = a.list.items.size();
        bool grid = a.list.view == screens::kViewGrid;
        const size_t per = grid ? screens::pageSize(screens::kViewGrid) : 6;
        size_t key = (grid ? a.list.cursor / per : a.list.cursor) * 4 + (size_t)a.list.view;
        if (key != lastKey) {
            lastKey = key;
            std::vector<uint64_t> ids;
            if (grid) {
                size_t first = a.list.cursor / per * per;
                for (size_t k = first + per; k < std::min(n, first + 2 * per); k++) ids.push_back(a.list.items[k].titleId);
                for (size_t k = first >= per ? first - per : 0; k < first; k++) ids.push_back(a.list.items[k].titleId);
            } else {
                for (size_t d = 1; d <= per; d++) {
                    if (a.list.cursor + d < n) ids.push_back(a.list.items[a.list.cursor + d].titleId);
                    if (a.list.cursor >= d) ids.push_back(a.list.items[a.list.cursor - d].titleId);
                }
            }
            a.covers->prefetch(ids);
        }
    }
    const auto& o = a.list.cover;
    bool changed = o.state != v.state || o.titleId != v.titleId || o.px != v.px;
    if (a.list.view == screens::kViewGrid && a.covers) {
        // cover wall: every game on the cursor's page. Requested last-to-first, the cursor's game
        // after them, so it is fetched first and the rest of the page in order.
        const size_t per = screens::pageSize(screens::kViewGrid);
        size_t first = a.list.items.empty() ? 0 : a.list.cursor / per * per;
        size_t count = a.list.items.empty() ? 0 : std::min(per, a.list.items.size() - first);
        for (size_t k = count; k-- > 0;) a.covers->request(a.list.items[first + k].titleId, per + 8);
        if (v.titleId) a.covers->request(v.titleId, per + 8);
        std::vector<screens::CoverView> cvs(count);
        std::vector<std::shared_ptr<const covers::Image>> keep(count);
        for (size_t k = 0; k < count; k++) {
            screens::CoverView& cv = cvs[k];
            cv.titleId = a.list.items[first + k].titleId;
            if (!cv.titleId) continue;
            switch (a.covers->get(cv.titleId, keep[k])) {
                case covers::State::Ready: cv.state = screens::CoverState::Ready; break;
                case covers::State::Missing: cv.state = screens::CoverState::Missing; break;
                default: cv.state = screens::CoverState::Loading; break;
            }
            if (keep[k]) { cv.w = keep[k]->w; cv.h = keep[k]->h; cv.px = keep[k]->px.data(); }
        }
        if (cvs.size() != a.list.covers.size()) changed = true;
        for (size_t k = 0; k < cvs.size() && !changed; k++)
            changed = cvs[k].state != a.list.covers[k].state || cvs[k].titleId != a.list.covers[k].titleId || cvs[k].px != a.list.covers[k].px;
        a.list.covers = std::move(cvs);
        a.shownCovers = std::move(keep);
    } else if (!a.list.covers.empty()) {
        a.list.covers.clear();
        a.shownCovers.clear();
        changed = true;
    }
    a.list.cover = v;
    a.shownCover = img;
    return changed;
}

static void showList(App& a) {
    updateCover(a);
    present(a, [&](gfx::Canvas& c) { screens::drawList(c, a.font, a.list); });
}

// A core for background work, never the one drawing the screen (k: which worker, spreads them).
static int bgCore(int k) {
    static int self = -1;
    static std::vector<int> others;
    if (self < 0) {
        self = (int)svcGetCurrentProcessorNumber();  // first called on the UI thread
        u64 mask = 0;
        svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
        for (int c = 0; c < 4; c++)
            if (((mask >> c) & 1) && c != self) others.push_back(c);
    }
    return others.empty() ? -2 : others[(size_t)k % others.size()];
}

static bool netLinkUp() {  // has the console an address on some network? (see netUp below)
    u32 ip = 0;
    return R_SUCCEEDED(nifmGetCurrentIpAddress(&ip)) && ip != 0;
}
static u64 buttonsDown(App& a) {
    padUpdate(&a.pad);
    return padGetButtonsDown(&a.pad);
}

static void waitForConfirm(App& a) {
    while (appletMainLoop()) {
        if (buttonsDown(a) & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus)) break;
        svcSleepThread(16000000);
    }
}

// ------------------------------------------------------------------ server / list
static const char* kServersPath = "sdmc:/config/lan-installer/servers.ini";
static const uint32_t kGo = gfx::rgb(36, 150, 82), kStop = gfx::rgb(196, 58, 58), kLater = gfx::rgb(84, 92, 112);
static int askChoice(App& a, const std::string& title, const std::vector<std::string>& lines,
                     const std::vector<screens::ChoiceOption>& opts);

// Software keyboard. Returns false when cancelled. `password` hides what is typed.
static bool askText(App& a, const std::string& header, const std::string& guide, const std::string& initial,
                    bool password, std::string& out, size_t maxLen = 255, bool chinese = false) {
    (void)a;
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header.c_str());
    swkbdConfigSetGuideText(&kbd, guide.c_str());
    swkbdConfigSetInitialText(&kbd, initial.c_str());
    swkbdConfigSetStringLenMax(&kbd, (u32)maxLen);
    if (chinese) swkbdConfigSetType(&kbd, SwkbdType_All);  // the preset is English-only (QWERTY)
    if (password) swkbdConfigSetPasswordFlag(&kbd, 1);
    std::vector<char> buf(maxLen * 4 + 4, 0);
    Result rc = swkbdShow(&kbd, buf.data(), buf.size());
    swkbdClose(&kbd);
    if (R_FAILED(rc)) return false;
    out = buf.data();
    return true;
}

// The server in use becomes the address everything downloads from (account / password included).
static void applyCurrentServer(App& a) {
    const servers::Server* s = a.store.active();
    a.list.server = s ? servers::fullUrl(*s) : "";
    a.list.serverName = s ? s->name : "";
}

static void saveServers(App& a) {
    if (!servers::save(kServersPath, a.store)) {
        install::log("servers: could not save %s", kServersPath);
        a.list.status = "無法儲存伺服器設定到 SD 卡。";
    }
}

static u64 nowMs() { return armTicksToNs(armGetSystemTick()) / 1000000ULL; }

static std::string lower(std::string t) {
    for (auto& ch : t) ch = (char)tolower((unsigned char)ch);
    return t;
}

static const char* kSorts[] = {"名稱", "發售日 新到舊", "發售日 舊到新", "大小 大到小", "大小 小到大"};
static const char* kSortMenu[] = {"名稱", "發售日（新 → 舊）", "發售日（舊 → 新）", "檔案大小（大 → 小）", "檔案大小（小 → 大）"};
static const int kSortCount = 5;

static std::string baseName(const std::string& path) {
    size_t p = path.rfind('/');
    return p == std::string::npos ? path : path.substr(p + 1);
}

// Display names and release dates from the titledb (falls back to the file name).
static void applyTitleDb(App& a) {
    a.names.assign(a.files.size(), "");
    a.dates.assign(a.files.size(), 0);
    a.searchKeys.assign(a.files.size(), "");
    for (size_t i = 0; i < a.files.size(); i++) {
        const titles::FileInfo& fi = a.info[i];
        titledb::Entry e;
        bool base = a.db && fi.baseId && a.db->find(fi.baseId, e) && e.releaseDate;
        if (!base && a.db && fi.titleId && fi.titleId != fi.baseId) a.db->find(fi.titleId, e);
        // The list shows the file names from the server; titledb only supplies release dates and covers.
        a.names[i] = baseName(a.files[i].displayName);
        a.dates[i] = e.releaseDate;
    }
}

// Files of one game (base, update, DLC) stay together. Games are ordered by the chosen sort;
// inside a game: base, update, DLC.
static u64 groupKey(const App& a, size_t i) { return a.info[i].baseId ? a.info[i].baseId : (0x8000000000000000ULL | i); }

struct GroupInfo {
    std::string name;
    uint32_t date = 0;
    uint64_t size = 0;
    bool hasBase = false;
};

// Everything the comparison needs is worked out once per file first (group key, the group's
// name / date / size as pointers into one table), so sorting 5,000 files does no map lookups.
static void orderFiles(const App& a, std::vector<size_t>& idx) {
    std::map<u64, size_t> slot;
    std::vector<GroupInfo> groups;
    struct Key { u64 group; const GroupInfo* g; };
    std::vector<size_t> gOf(idx.size());
    for (size_t k = 0; k < idx.size(); k++) {
        size_t i = idx[k];
        auto ins = slot.emplace(groupKey(a, i), groups.size());
        if (ins.second) groups.emplace_back();
        gOf[k] = ins.first->second;
        GroupInfo& g = groups[gOf[k]];
        auto row = a.rowOf.find(i);
        g.size += row != a.rowOf.end() ? a.rows[row->second].total : a.files[i].size;
        g.date = std::max(g.date, a.dates[i]);
        bool base = a.info[i].kind == titles::Kind::Base;
        if (base && !g.hasBase) { g.name = a.names[i]; g.hasBase = true; }
        else if (!g.hasBase && (g.name.empty() || a.names[i] < g.name)) g.name = a.names[i];
    }
    std::vector<Key> keys(a.files.size(), Key{0, nullptr});
    for (size_t k = 0; k < idx.size(); k++) keys[idx[k]] = Key{groupKey(a, idx[k]), &groups[gOf[k]]};
    int sort = a.sort;
    std::stable_sort(idx.begin(), idx.end(), [&](size_t x, size_t y) {
        u64 gx = keys[x].group, gy = keys[y].group;
        if (gx != gy) {
            const GroupInfo& X = *keys[x].g;
            const GroupInfo& Y = *keys[y].g;
            switch (sort) {
                case 1:
                case 2:
                    if (X.date != Y.date) {
                        if (!X.date) return false;  // unknown dates last
                        if (!Y.date) return true;
                        return sort == 1 ? X.date > Y.date : X.date < Y.date;
                    }
                    break;
                case 3:
                case 4:
                    if (X.size != Y.size) return sort == 3 ? X.size > Y.size : X.size < Y.size;
                    break;
                default:
                    break;
            }
            if (X.name != Y.name) return X.name < Y.name;
            return gx < gy;
        }
        int rx = titles::kindRank(a.info[x].kind), ry = titles::kindRank(a.info[y].kind);
        if (rx != ry) return rx < ry;
        if (a.info[x].titleId != a.info[y].titleId) return a.info[x].titleId < a.info[y].titleId;
        if (a.info[x].version != a.info[y].version) return a.info[x].version > a.info[y].version;
        return a.files[x].displayName < a.files[y].displayName;
    });
}

static std::string verText(const std::string& disp, uint32_t ncm);

// Groups the server's files into one row per game (see App::Row).
static void itemsOfAll(const App& a, std::vector<std::vector<container::Meta>>& items);  // below
static std::string verText(const std::string& disp, uint32_t ncm);     // below

// One row per game. What each file holds comes from the package itself when it was read (merged
// packages: base + update + DLC in one file), else from the file name. For each title of the game
// (base, update, every DLC) the newest version on the server is the target; what the console lacks
// (or has older) is installed, from as few files as possible (a merged package that carries the
// newest update is preferred); titles inside a chosen file that are not wanted are skipped.
static void buildRows(App& a) {
    a.rows.clear();
    a.rowOf.clear();
    struct Src { size_t file; uint32_t version; };
    struct G {
        std::set<size_t> files;
        std::map<u64, std::vector<Src>> titles;  // title id -> where it is (file, version)
    };
    std::map<u64, G> groups;
    std::vector<std::vector<container::Meta>> items(a.files.size());
    std::vector<size_t> loose;  // nothing known about it: shown as it is
    itemsOfAll(a, items);
    for (size_t i = 0; i < a.files.size(); i++) {
        if (items[i].empty()) { loose.push_back(i); continue; }
        for (const auto& m : items[i]) {
            u64 app = covers::baseTitleId(m.titleId);
            G& g = groups[app];
            g.files.insert(i);
            g.titles[m.titleId].push_back({i, m.version});
        }
    }
    auto holds = [&](size_t f, u64 tid, uint32_t ver) {
        for (const auto& m : items[f])
            if (m.titleId == tid && m.version == ver) return true;
        return false;
    };
    for (auto& kv : groups) {
        u64 app = kv.first;
        G& g = kv.second;
        App::Row r;
        r.app = app;
        bool baseOnConsole = a.installed.count(app) > 0;
        bool baseOnServer = g.titles.count(app) > 0;
        if (!baseOnServer && !baseOnConsole) continue;  // update / DLC without its game: hidden, would not start
        r.baseInstalled = baseOnConsole;
        r.card = a.cardBases.count(app) > 0;
        bool fresh = !baseOnConsole || r.card;  // (re)install the whole game
        // target version of every title, and whether the console needs it
        std::map<u64, uint32_t> target;
        std::set<u64> need;
        for (auto& t : g.titles) {
            uint32_t best = 0;
            for (const auto& sv : t.second) best = std::max(best, sv.version);
            target[t.first] = best;
            auto in = a.installed.find(t.first);
            bool isBase = t.first == app;
            if (fresh || (!isBase && (in == a.installed.end() || in->second < best))) {
                if (isBase && !baseOnServer) continue;
                need.insert(t.first);
            }
            if ((t.first & 0xFFF) != 0 && (t.first & 0xFFF) != 0x800) {
                r.dlcCount++;
                if (in != a.installed.end() && in->second >= best) r.dlcHave++;
            }
        }
        if (!baseOnServer && fresh) need.clear();  // nothing to install it on
        // representative / display files: the source of each title's newest version
        std::map<u64, size_t> bestFile;
        for (auto& t : g.titles)
            for (const auto& sv : t.second)
                if (sv.version == target[t.first] && !bestFile.count(t.first)) bestFile[t.first] = sv.file;
        // choose files: each time the one that brings the most still-needed titles at their target
        std::set<u64> left = need;
        std::vector<size_t> chosen;
        while (!left.empty()) {
            int pick = -1;
            size_t bestCount = 0;
            for (size_t f : g.files) {
                if (std::find(chosen.begin(), chosen.end(), f) != chosen.end()) continue;
                size_t n = 0;
                for (u64 tid : left) n += holds(f, tid, target[tid]);
                if (n > bestCount || (n == bestCount && n && pick >= 0 && a.files[f].size < a.files[(size_t)pick].size)) {
                    bestCount = n;
                    pick = (int)f;
                }
            }
            if (pick < 0 || !bestCount) break;
            chosen.push_back((size_t)pick);
            for (auto it = left.begin(); it != left.end();)
                it = holds((size_t)pick, *it, target[*it]) ? left.erase(it) : std::next(it);
        }
        for (size_t f : chosen) {
            std::set<u64> skip;
            for (const auto& m : items[f])
                if (!need.count(m.titleId) || m.version != target[m.titleId]) skip.insert(m.titleId);
            if (!skip.empty()) r.skip[f] = skip;
            if (items[f].size() > 1) r.merged = true;
            r.needed.push_back(f);
            r.neededSize += a.files[f].size;
        }
        // flags
        u64 upd = app | 0x800;
        r.updateAvailable = baseOnConsole && need.count(upd);
        for (u64 tid : need)
            if ((tid & 0xFFF) != 0 && (tid & 0xFFF) != 0x800) { r.dlcAvailable = baseOnConsole; r.dlcNeed++; }
        if (bestFile.count(upd)) {
            r.update = (int)bestFile[upd];
            std::string v = titles::displayVersion(a.files[bestFile[upd]].displayName);
            r.updateText = v.empty() ? verText("", target[upd]) : "v" + v;
        }
        // the row's file: a chosen file with the base, else the base file holding the most, else any
        int rep = -1;
        for (size_t f : chosen)
            if (holds(f, app, target.count(app) ? target[app] : 0) && rep < 0) rep = (int)f;
        if (rep < 0 && bestFile.count(app)) {
            size_t most = 0;
            for (const auto& sv : g.titles[app])
                if (rep < 0 || items[sv.file].size() > most) { rep = (int)sv.file; most = items[sv.file].size(); }
        }
        std::set<size_t> mem;
        if (rep >= 0) mem.insert((size_t)rep);
        for (auto& bf : bestFile) mem.insert(bf.second);
        if (rep < 0) rep = (int)*mem.begin();
        r.rep = (size_t)rep;
        r.members.push_back(r.rep);
        for (size_t m : mem)
            if (m != r.rep) r.members.push_back(m);
        for (size_t m : r.members) r.total += a.files[m].size;
        if (a.rowOf.count(r.rep)) continue;  // one file, two games (rare): first one wins
        a.rowOf[r.rep] = a.rows.size();
        a.rows.push_back(r);
    }
    for (size_t i : loose) {
        if (a.rowOf.count(i)) continue;
        App::Row r;
        r.rep = i;
        r.members = r.needed = {i};
        r.total = r.neededSize = a.files[i].size;
        a.rowOf[i] = a.rows.size();
        a.rows.push_back(r);
    }
}

static bool rowInTab(const App::Row& r, int tab) {
    if (tab == 1) return r.baseInstalled && r.updateAvailable;
    if (tab == 2) return r.baseInstalled && r.dlcAvailable;
    if (tab == 3) return r.baseInstalled;
    return true;
}

static screens::Badge rowBadge(const App::Row& r) {
    if (r.card) return screens::Badge::Reinstall;
    if (r.baseInstalled && r.updateAvailable) return screens::Badge::Update;
    if (r.baseInstalled && r.dlcAvailable) return screens::Badge::Dlc;
    if (r.baseInstalled) return screens::Badge::Installed;
    return screens::Badge::None;
}

// "包含：本體、更新 v1.7.0、DLC 12 個  ·  將安裝 3 個檔案"
static std::string contentsLine(const App& a, const App::Row& r) {
    std::string t = "包含：";
    bool first = true;
    if (r.app) { t += "本體"; first = false; }
    if (!r.updateText.empty()) { t += std::string(first ? "" : "、") + "更新 " + r.updateText; first = false; }
    if (r.dlcCount) {
        t += std::string(first ? "" : "、") + "DLC " + std::to_string(r.dlcCount) + " 個";
        if (r.baseInstalled && !r.card) t += "（主機已有 " + std::to_string(r.dlcHave) + " 個）";
        first = false;
    }
    if (first) t += "—";
    if (r.needed.empty()) t += "  ·  已全部安裝";
    else if (r.baseInstalled && !r.card) {
        t += "  ·  將安裝 " + std::to_string(r.needed.size()) + " 個檔案";
        if (r.dlcNeed) t += "（DLC " + std::to_string(r.dlcNeed) + " 個）";
    }
    bool fromPack = false;
    for (size_t m : r.needed.empty() ? r.members : r.needed) fromPack = fromPack || a.files[m].onlyTitle;
    if ((r.merged && !r.needed.empty()) || fromPack) t += "  ·  來自整合版";
    return t;
}

static std::map<std::string, int> queuedUrls();  // background installs, see below

// Search: traditional and simplified characters match each other, letters ignore case.
// `needle` is already a tsconv::searchKey.
// The keys are made once per file and kept (searching again, or typing one more letter, reuses them).
static const std::string& searchKeyOf(App& a, size_t i) {
    if (a.searchKeys.size() != a.files.size()) a.searchKeys.assign(a.files.size(), "");
    std::string& k = a.searchKeys[i];
    if (k.empty()) {
        k = tsconv::searchKey(a.names[i]) + "\n" + tsconv::searchKey(a.files[i].displayName);
        if (k.empty()) k = "\n";
    }
    return k;
}

static bool rowMatches(App& a, const App::Row& r, const std::string& needle) {
    if (searchKeyOf(a, r.rep).find(needle) != std::string::npos) return true;
    for (size_t m : r.members)
        if (searchKeyOf(a, m).find(needle) != std::string::npos) return true;
    return false;
}

// Rebuilds the visible list from tab + search + sort, keeping the cursor on the same game.
static void updateSelInfo(App& a);  // below: "已選 … · 剩餘可用 …"
// `rows`: work out the games again (data changed); false = only another tab / sort / search of the
// same games (fast: ZL / ZR do not recompute 5,000 files).
static void rebuildView(App& a, bool keepCursor = true, bool rows = true) {
    size_t keep = (keepCursor && a.list.cursor < a.view.size()) ? a.view[a.list.cursor] : (size_t)-1;
    if (rows || a.rows.empty()) buildRows(a);
    std::string needle = tsconv::searchKey(a.search);
    size_t counts[4] = {0, 0, 0, 0};
    a.view.clear();
    a.list.items.clear();
    for (const App::Row& r : a.rows) {
        if (!needle.empty() && !rowMatches(a, r, needle)) continue;
        for (int t = 0; t < 4; t++)
            if (rowInTab(r, t)) counts[t]++;
        if (!rowInTab(r, a.category)) continue;
        a.view.push_back(r.rep);
    }
    orderFiles(a, a.view);
    std::map<std::string, int> queued = queuedUrls();
    for (size_t i : a.view) {
        const App::Row& r = a.rows[a.rowOf[i]];
        uint64_t size = r.needed.empty() ? r.total : r.neededSize;
        screens::ListItem it{a.names[i], size, a.selected[i], rowBadge(r)};
        int q = 0;
        for (size_t m : r.members) {
            auto f = queued.find(a.files[m].url);
            if (f != queued.end()) q = std::max(q, f->second);
        }
        if (q) it.badge = q == 2 ? screens::Badge::Installing : screens::Badge::Queued;
        it.file = a.files[i].displayName;
        it.releaseDate = a.dates[i];
        it.contentsLine = contentsLine(a, r);
        it.titleId = a.info[i].baseId;
        if (!r.updateText.empty()) it.version = r.updateText;
        else {
            std::string v = titles::displayVersion(a.files[i].displayName);
            it.version = v.empty() ? "v0" : "v" + v;
        }
        a.list.items.push_back(it);
    }
    a.list.cursor = 0;
    for (size_t v = 0; v < a.view.size(); v++)
        if (a.view[v] == keep) { a.list.cursor = v; break; }
    a.list.updates = counts[1];
    a.list.dlcs = counts[2];
    for (int t = 0; t < 4; t++) a.list.tabCounts[t] = counts[t];
    a.list.category = a.category;
    a.list.sortLabel = kSorts[a.sort];
    a.list.search = a.search;
    a.lastMove = nowMs();
    updateSelInfo(a);
}

// "主機 v1.0.0 → 伺服器 v1.0.3" for the file under the cursor (computed on demand: it asks the system).
static std::string verText(const std::string& disp, uint32_t ncm) {
    if (!disp.empty()) return "v" + disp;
    return ncm ? "第 " + std::to_string(ncm >> 16) + " 版" : "v0";
}

static void updateVersionLine(App& a) {
    if (a.list.cursor >= a.view.size()) return;
    screens::ListItem& it = a.list.items[a.list.cursor];
    if (!it.versionLine.empty()) return;
    size_t i = a.view[a.list.cursor];
    const titles::FileInfo& fi = a.info[i];
    if (!fi.titleId || !a.installedOk) return;
    auto row = a.rowOf.find(i);
    if (row == a.rowOf.end()) return;
    const App::Row& r = a.rows[row->second];
    if (!r.baseInstalled) { it.versionLine = "主機尚未安裝這款遊戲"; return; }
    auto dv = a.dispVer.find(fi.baseId);
    if (dv == a.dispVer.end()) dv = a.dispVer.emplace(fi.baseId, install::installedDisplayVersion(fi.baseId)).first;
    uint32_t baseV = 0, patchV = 0;
    auto b = a.installed.find(fi.baseId);
    if (b != a.installed.end()) baseV = b->second;
    auto pt = a.installed.find(fi.baseId | 0x800);
    if (pt != a.installed.end()) patchV = pt->second;
    std::string now = verText(dv->second, pt != a.installed.end() ? patchV : baseV);
    if (r.updateAvailable) {
        it.versionLine = "主機 " + now + "  →  伺服器 " + r.updateText;
    } else {
        it.versionLine = "主機已安裝 " + now;
    }
}

// The sort order is a saved setting, so B only clears the search and goes back to the first tab.
static bool isInitialView(const App& a) { return a.search.empty() && a.category == 0; }

static void updateSdFree(App& a) { a.list.sdFree = install::sdFreeBytes(); }

static void classifyAll(App& a) {
    a.installedOk = install::scanInstalled(a.installed);
    install::scanGamecardInstalls(a.installed, a.cardBases);
    a.dispVer.clear();
    updateSdFree(a);
    a.status.assign(a.files.size(), titles::Status::Unknown);
    for (size_t i = 0; i < a.files.size(); i++) a.status[i] = titles::classify(a.info[i], a.installed);
}


// ---- Real TIDs of the server's files, read in the background from each package's content meta
// header (decrypted with the user's header key): a file whose name has no TID, or a wrong one, is
// still recognised, and merged packages (base + update + DLC in one file) are split into their
// titles. Results are kept on the SD card, one line per file as soon as it is read, so nothing is
// read twice (leaving a screen, reloading the list or closing the app keeps what was read).
static const char* kNasTidsPath = "sdmc:/config/lan-installer/server-tids.txt";
static const char* kOldTidsPath = "sdmc:/config/lan-installer/nas-tids.txt";  // name up to v3.0.12 test builds
static const int kTidWorkers = 4;  // files read at the same time (fewer when the NAS is slow / installing)
// What one server file holds: how many content metas, and (read for packages with several, or with no
// TID in their name) each one's title id, version, type and installed size. Empty `items`: the name tells it.
struct FileScan {
    int metas = 0;  // -2: this file could not be read (its name decides; read again if it changes)
    std::vector<container::Meta> items;
};

// A failure of the network / NAS (worth trying again, counts toward "the NAS is not answering"), not
// of this one file (the server refuses parts of it, the package is damaged…).
static bool networkTrouble(const std::string& e) {
    static const char* kNet[] = {"逾時", "無法連線", "連線中斷", "接收資料失敗", "傳送資料失敗", "找不到主機", "沒有連上網路",
                                 "交握", "沒有回應", "已停止", "無法建立網路連線", "超過 20 秒"};
    for (const char* k : kNet)
        if (e.find(k) != std::string::npos) return true;
    return false;
}
struct TidScan {
    Mutex m = 0;
    Thread threads[kTidWorkers];
    bool used[kTidWorkers] = {false, false, false, false};
    http::Session* ses[kTidWorkers] = {nullptr, nullptr, nullptr, nullptr};  // to interrupt reads at once
    bool stop = false;
    bool done = true;
    bool paused = false;    // the NAS stopped answering: given up until the next start / manage screen
    int alive = 0;
    uint32_t gen = 0;       // goes up whenever results arrive (and when a scan ends)
    uint32_t structGen = 0; // goes up when a result changes what a file holds (merged / no TID in the name)
    struct Item { std::string key, url, name; uint64_t size; bool nameHasTid; bool retried; };
    std::vector<Item> todo;
    std::vector<Item> retry;          // failed once: tried again after the rest
    std::set<std::string> queued;     // keys in todo / retry
    size_t next = 0, finished = 0;
    int failures = 0, inARow = 0;     // failures this scan / in a row (10 in a row: the NAS is not answering)
    bool slow = false;                // answers take long or fail: 2 at a time
    int limit = kTidWorkers;          // workers allowed now (set by the UI: installing -> 1)
    double avgMs = 0;                 // time per file
    std::set<std::string> current;    // keys of the files on the server now
    size_t knownCurrent = 0;          // how many of them are known (progress)
    std::string server;               // "host:port" of the server being scanned
    bool rewrite = false;             // the file holds lines to drop (older format, gone files)
    std::map<std::string, FileScan> known;  // file key -> what it holds
    std::vector<std::string> pending; // lines not written to the SD card yet
    u64 lastFlush = 0;
    uint8_t key[0x20];
    uint8_t kaek[32][0x10];  // key_area_key_application_00..1f (from the user's prod.keys)
    bool kaekHave[32] = {false};
};
static TidScan g_tid;
static Mutex g_tidFile = 0;  // writing nas-tids.txt (never while g_tid.m is held)

static u64 msNow() { return armTicksToNs(armGetSystemTick()) / 1000000ULL; }

// "host:port<TAB>size<TAB>path": per server, so switching servers keeps each server's results.
static std::string tidServer(const std::string& url) {
    // called for every file of the list, many times: the same answer as http::parseUrl, without
    // decoding the password into an Authorization header each time
    size_t p = url.find("://");
    bool tls = p != std::string::npos && p == 5;
    size_t a = p == std::string::npos ? 0 : p + 3;
    size_t e = url.find_first_of("/?#", a);
    std::string auth = url.substr(a, e == std::string::npos ? std::string::npos : e - a);
    size_t at = auth.rfind('@');
    if (at != std::string::npos) auth = auth.substr(at + 1);
    if (auth.empty()) return "?";
    size_t colon = auth.rfind(':');
    if (colon != std::string::npos) return auth;
    return auth + (tls ? ":443" : ":80");
}
static std::string tidCacheKey(const remote::RemoteFile& f) {
    if (!f.pkgUrl.empty()) return tidServer(f.pkgUrl) + "\t" + std::to_string(f.pkgSize) + "\t" + f.pkgName;
    return tidServer(f.url) + "\t" + std::to_string(f.size) + "\t" + f.displayName;
}

// "<key><TAB>M<metas><TAB>TID:version:type:known:size,..." (version / type / size in decimal / hex / decimal)
static std::string tidLine(const std::string& key, const FileScan& fs) {
    std::string l = key + "\tM" + std::to_string(fs.metas) + "\t";
    for (size_t j = 0; j < fs.items.size(); j++) {
        char b[96];
        snprintf(b, sizeof b, "%s%016llX:%u:%02X:%d:%llu", j ? "," : "", (unsigned long long)fs.items[j].titleId, fs.items[j].version,
                 fs.items[j].type, fs.items[j].versionKnown ? 1 : 0, (unsigned long long)fs.items[j].size);
        l += b;
    }
    return l;
}

// false: not a line of this format. `old`: written by v3.0.11 (no sizes: a merged package is read again).
static bool parseTidLine(const std::string& l, std::string& key, FileScan& fs, bool& old) {
    old = false;
    size_t t2 = l.rfind('\t');
    if (t2 == std::string::npos || t2 == 0) return false;
    size_t t1 = l.rfind('\t', t2 - 1);
    if (t1 == std::string::npos || l.compare(t1 + 1, 1, "M") != 0) return false;
    key = l.substr(0, t1);
    if (std::count(key.begin(), key.end(), '\t') != 2) return false;
    fs.metas = atoi(l.c_str() + t1 + 2);
    fs.items.clear();
    std::string list = l.substr(t2 + 1);
    for (size_t p = 0; p < list.size();) {
        size_t q = list.find(',', p);
        if (q == std::string::npos) q = list.size();
        unsigned long long tid = 0, size = 0;
        unsigned ver = 0, type = 0;
        int known = 0;
        int n = sscanf(list.substr(p, q - p).c_str(), "%llx:%u:%x:%d:%llu", &tid, &ver, &type, &known, &size);
        if (n >= 4 && tid) {
            if (n == 4) old = true;
            container::Meta m;
            m.titleId = tid;
            m.version = ver;
            m.type = (uint8_t)type;
            m.versionKnown = known != 0;
            m.size = size;
            fs.items.push_back(m);
        }
        p = q + 1;
    }
    return fs.metas > 0 || fs.metas == -2;  // (-1 came from a test build that misread small files: read again)
}

// Reads what one server file holds: its directory (1 read) tells how many content metas it has;
// packages with several (base + update / DLC merged) and files whose name has no TID are opened
// further for each one's title id and version. Reads come in 64 KB pieces over a kept connection.
// More than 20 s on one file: given up.
static bool scanFile(http::Session& ses, const std::string& url, uint64_t size, bool nameHasTid, FileScan& fs, std::string& err) {
    std::map<uint64_t, std::vector<uint8_t>> blocks;
    u64 start = armTicksToNs(armGetSystemTick());
    container::ReadFn rd = [&](uint64_t o, uint64_t n, std::vector<uint8_t>& out, std::string& e) {
        if (armTicksToNs(armGetSystemTick()) - start > 20000000000ULL) { e = "超過 20 秒沒讀完，略過"; return false; }
        const uint64_t B = 64 * 1024;
        out.clear();
        for (uint64_t p = o; p < o + n;) {
            uint64_t b = p / B * B;
            auto c = blocks.find(b);
            if (c == blocks.end()) {
                uint64_t len = std::min<uint64_t>(std::max<uint64_t>(B, o + n - b), size > b ? size - b : 0);
                if (!len) { e = "讀取超出檔案大小"; return false; }
                std::vector<uint8_t> data;
                if (!ses.getRange(url, b, len, data, e)) return false;
                for (uint64_t q = 0; q < len; q += B)  // a big read fills several blocks
                    blocks[b + q].assign(data.begin() + (long)q, data.begin() + (long)std::min<uint64_t>(len, q + B));
                c = blocks.find(b);
            }
            uint64_t from = p - b;
            if (c->second.size() <= from) { e = "讀取超出檔案大小"; return false; }
            uint64_t take = std::min<uint64_t>(c->second.size() - from, o + n - p);
            out.insert(out.end(), c->second.begin() + (long)from, c->second.begin() + (long)(from + take));
            p += take;
        }
        return true;
    };
    container::KeyAreaKeyFn kaek = [](int idx, uint8_t out[0x10]) {
        if (idx < 0 || idx >= 32 || !g_tid.kaekHave[idx]) return false;
        memcpy(out, g_tid.kaek[idx], 0x10);
        return true;
    };
    int metas = 0;
    std::vector<container::Meta> items;
    if (!container::readMetas(rd, g_tid.key, kaek, false, metas, items, err)) return false;
    fs.metas = metas;
    fs.items.clear();
    if (metas >= 2 || !nameHasTid) {
        if (!container::readMetas(rd, g_tid.key, kaek, true, metas, items, err)) return false;
        fs.items = items;
    }
    return true;
}

static void tidWrite(const std::vector<std::string>& lines, bool whole) {
    if (lines.empty() && !whole) return;
    mutexLock(&g_tidFile);
    if (FILE* f = fopen(kNasTidsPath, whole ? "w" : "a")) {
        for (const auto& l : lines) fprintf(f, "%s\n", l.c_str());
        fclose(f);
    }
    mutexUnlock(&g_tidFile);
}

// Lines waiting to be written (called with g_tid.m held; write them after unlocking).
static std::vector<std::string> tidTakeLocked() {
    std::vector<std::string> out;
    out.swap(g_tid.pending);
    g_tid.lastFlush = msNow();
    return out;
}

// The scan is over: keep only the files still on the server (the file never grows without end).
// Called with g_tid.m held; returns the whole file to write ("" lines = nothing to rewrite).
static bool tidPruneLocked(std::vector<std::string>& whole) {
    size_t before = g_tid.known.size();
    std::string mine = g_tid.server + "\t";  // only this server's entries; other servers' stay
    for (auto it = g_tid.known.begin(); it != g_tid.known.end();)
        it = (it->first.compare(0, mine.size(), mine) != 0 || g_tid.current.count(it->first)) ? std::next(it) : g_tid.known.erase(it);
    if (before == g_tid.known.size() && !g_tid.rewrite) return false;  // nothing gone: the file stays as it is
    g_tid.rewrite = false;
    whole.clear();
    for (const auto& kv : g_tid.known) whole.push_back(tidLine(kv.first, kv.second));
    if (before != g_tid.known.size()) install::log("real TIDs: %zu old entr(ies) removed", before - g_tid.known.size());
    return true;
}

static void tidScanThread(void* arg) {
    const int idx = (int)(intptr_t)arg;
    http::Session ses;  // one kept-alive connection per worker
    http::setThreadTimeout(8000);
    mutexLock(&g_tid.m);
    g_tid.ses[idx] = &ses;
    mutexUnlock(&g_tid.m);
    for (;;) {
        mutexLock(&g_tid.m);
        if (g_tid.stop) break;  // leaves the loop locked
        int allowed = std::min(g_tid.limit, g_tid.slow ? 2 : kTidWorkers);
        if (idx >= allowed) {  // not needed now (installing / slow NAS): wait
            mutexUnlock(&g_tid.m);
            svcSleepThread(250000000ULL);
            continue;
        }
        if (g_tid.next >= g_tid.todo.size()) {
            if (g_tid.retry.empty()) break;
            g_tid.todo.insert(g_tid.todo.end(), g_tid.retry.begin(), g_tid.retry.end());  // second try, after the rest
            g_tid.retry.clear();
        }
        TidScan::Item it = g_tid.todo[g_tid.next++];
        mutexUnlock(&g_tid.m);
        FileScan fs;
        std::string err;
        u64 t0 = msNow();
        bool ok = scanFile(ses, it.url, it.size, it.nameHasTid, fs, err);
        if (!ok) ses.close();  // a broken connection is not used for the next file
        u64 took = msNow() - t0;
        std::vector<std::string> lines;
        mutexLock(&g_tid.m);
        if (g_tid.stop && !ok) break;  // interrupted on purpose: not a failure
        g_tid.finished++;
        g_tid.avgMs = g_tid.avgMs <= 0 ? (double)took : g_tid.avgMs * 0.8 + took * 0.2;
        if (ok) {
            g_tid.inARow = 0;
            if (!g_tid.known.count(it.key) && g_tid.current.count(it.key)) g_tid.knownCurrent++;
            g_tid.known[it.key] = fs;
            g_tid.queued.erase(it.key);
            g_tid.pending.push_back(tidLine(it.key, fs));
            if (!fs.items.empty()) g_tid.structGen++;
        } else if (!networkTrouble(err)) {
            // this file itself cannot be read (e.g. the server ignores Range for it): remembered, so it
            // is not read again at every start; the NAS is answering, so it does not count as trouble
            g_tid.inARow = 0;
            if (g_tid.failures++ < 20) install::log("real TID %s: %s", it.name.c_str(), err.c_str());
            FileScan bad;
            bad.metas = -2;
            if (!g_tid.known.count(it.key) && g_tid.current.count(it.key)) g_tid.knownCurrent++;
            g_tid.known[it.key] = bad;
            g_tid.queued.erase(it.key);
            g_tid.pending.push_back(tidLine(it.key, bad));
        } else {
            g_tid.inARow++;
            if (g_tid.failures++ < 20) install::log("real TID %s: %s", it.name.c_str(), err.c_str());
            if (!it.retried) {
                it.retried = true;
                g_tid.retry.push_back(it);
            } else {
                g_tid.queued.erase(it.key);  // given up for this time (tried again at the next start)
            }
            if (g_tid.inARow >= 10 && !g_tid.paused) {
                g_tid.paused = true;
                g_tid.stop = true;  // every worker leaves
                install::log("real TID scan: 10 files in a row failed, server not answering: paused");
            }
        }
        g_tid.slow = g_tid.inARow >= 2 || g_tid.avgMs > 3000;
        g_tid.gen++;
        if (g_tid.pending.size() >= 16 || msNow() - g_tid.lastFlush >= 1000) lines = tidTakeLocked();
        mutexUnlock(&g_tid.m);
        tidWrite(lines, false);
    }
    // (locked here)
    g_tid.ses[idx] = nullptr;
    std::vector<std::string> lines = tidTakeLocked(), whole;
    bool last = --g_tid.alive == 0, rewrite = false;
    if (last) {
        if (!g_tid.stop) {
            rewrite = tidPruneLocked(whole);
            install::log("real TID scan: done, %zu file(s) read, %d failed", g_tid.finished, g_tid.failures);
        }
        g_tid.done = true;
        g_tid.gen++;
    }
    mutexUnlock(&g_tid.m);
    tidWrite(lines, false);
    if (rewrite) tidWrite(whole, true);
}

// Stops the workers (a read in progress is interrupted) and waits for them.
static void stopTidScan() {
    mutexLock(&g_tid.m);
    g_tid.stop = true;
    for (int k = 0; k < kTidWorkers; k++)
        if (g_tid.ses[k]) g_tid.ses[k]->interrupt();
    mutexUnlock(&g_tid.m);
    for (int k = 0; k < kTidWorkers; k++) {
        if (!g_tid.used[k]) continue;
        threadWaitForExit(&g_tid.threads[k]);
        threadClose(&g_tid.threads[k]);
        g_tid.used[k] = false;
    }
    mutexLock(&g_tid.m);
    g_tid.stop = false;
    mutexUnlock(&g_tid.m);
}

// Installing: the scan steps aside (1 file at a time) so downloads keep the bandwidth.
static void tidSetInstalling(bool installing) {
    mutexLock(&g_tid.m);
    g_tid.limit = installing ? 1 : g_appletMode ? 2 : kTidWorkers;  // album mode: less memory, fewer at a time
    mutexUnlock(&g_tid.m);
}

// A name that looks like a merged package ("2G+2U+10D", "1G+1U", "+12D"…): read early.
static bool looksMerged(const std::string& n) {
    for (size_t i = 0; i + 1 < n.size(); i++) {
        char c = n[i], d = n[i + 1];
        if (isdigit((unsigned char)c) && (d == 'G' || d == 'U' || d == 'D' || d == 'g' || d == 'u' || d == 'd') &&
            ((i + 2 < n.size() && n[i + 2] == '+') || (i > 0 && n[i - 1] == '+')))
            return true;
    }
    return false;
}

// Starts (or continues) reading the files not read before: names without a TID first, then names
// that look merged, then the rest. Called again for the same server (list reloaded, manage screen
// opened) it only adds what is new and keeps going; after a pause (NAS not answering) it tries again.
static void startTidScan(App& a) {
    const uint8_t* hk = install::headerKey();
    const std::vector<remote::RemoteFile>& files = a.raw;
    std::string server = files.empty() ? std::string() : tidServer(files[0].url);
    {   // the old file name: renamed once (SD card only), so nothing is read again
        struct stat st;
        if (stat(kNasTidsPath, &st) != 0 && stat(kOldTidsPath, &st) == 0) {
            int rc = rename(kOldTidsPath, kNasTidsPath);
            install::log("TID cache: nas-tids.txt renamed to server-tids.txt (%s)", rc == 0 ? "ok" : "failed");
        }
    }
    mutexLock(&g_tid.m);
    if (g_tid.known.empty()) {
        if (FILE* f = fopen(kNasTidsPath, "r")) {
            char line[8192];
            while (fgets(line, sizeof line, f)) {
                std::string l(line);
                while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
                std::string k;
                FileScan fs;
                bool old = false;
                if (parseTidLine(l, k, fs, old) && !(old && fs.metas >= 2)) g_tid.known[k] = fs;  // merged without sizes: read again
                else if (!l.empty()) g_tid.rewrite = true;  // older format: read again
            }
            fclose(f);
        }
    }
    g_tid.current.clear();
    for (const auto& f : files) g_tid.current.insert(tidCacheKey(f));
    g_tid.knownCurrent = 0;
    for (const auto& k : g_tid.current) g_tid.knownCurrent += g_tid.known.count(k);
    if (hk) {
        memcpy(g_tid.key, hk, 0x20);
        for (int k = 0; k < 32; k++) {  // the key area keys, to read versions out of the packages
            char name[48];
            snprintf(name, sizeof name, "key_area_key_application_%02x", k);
            g_tid.kaekHave[k] = install::prodKey(name, g_tid.kaek[k], 0x10);
        }
    }
    std::vector<TidScan::Item> first, merged, later;
    if (hk)
        for (const auto& f : files) {
            std::string k = tidCacheKey(f);
            if (g_tid.known.count(k) || g_tid.queued.count(k)) continue;
            bool named = covers::titleIdFromName(f.displayName) != 0;
            TidScan::Item it{k, f.url, baseName(f.displayName), f.size, named, false};
            (!named ? first : looksMerged(f.displayName) ? merged : later).push_back(it);
        }
    bool running = !g_tid.done && !g_tid.stop && !g_tid.paused;
    if (running && server == g_tid.server) {  // same server, still reading: only add what is new
        for (auto* v : {&first, &merged, &later})
            for (auto& it : *v) { g_tid.queued.insert(it.key); g_tid.todo.push_back(it); }
        g_tid.gen++;
        mutexUnlock(&g_tid.m);
        return;
    }
    mutexUnlock(&g_tid.m);
    stopTidScan();
    mutexLock(&g_tid.m);
    g_tid.server = server;
    g_tid.todo.clear();
    g_tid.retry.clear();
    g_tid.queued.clear();
    for (auto* v : {&first, &merged, &later})
        for (auto& it : *v) { g_tid.queued.insert(it.key); g_tid.todo.push_back(it); }
    g_tid.next = g_tid.finished = 0;
    g_tid.failures = g_tid.inARow = 0;
    g_tid.paused = g_tid.slow = false;
    g_tid.avgMs = 0;
    g_tid.done = true;
    g_tid.gen++;
    g_tid.structGen++;
    size_t n = g_tid.todo.size();
    std::vector<std::string> whole;
    bool rewrite = false;
    if (!n) {
        if (hk && !files.empty()) rewrite = tidPruneLocked(whole);  // nothing new: still drop files that are gone
        mutexUnlock(&g_tid.m);
        if (rewrite) tidWrite(whole, true);
        return;
    }
    g_tid.done = false;
    g_tid.alive = 0;
    g_tid.lastFlush = msNow();
    mutexUnlock(&g_tid.m);
    install::log("real TID scan: %zu file(s) to read, up to %d at a time", n, kTidWorkers);
    for (int k = 0; k < kTidWorkers && (size_t)k < n; k++) {
        mutexLock(&g_tid.m);
        g_tid.alive++;
        mutexUnlock(&g_tid.m);
        if (R_SUCCEEDED(threadCreate(&g_tid.threads[k], tidScanThread, (void*)(intptr_t)k, nullptr, 0x40000, 0x30, bgCore(k))) &&
            R_SUCCEEDED(threadStart(&g_tid.threads[k]))) {
            g_tid.used[k] = true;
            continue;
        }
        if (g_tid.used[k]) threadClose(&g_tid.threads[k]);
        mutexLock(&g_tid.m);
        if (--g_tid.alive == 0) { g_tid.done = true; g_tid.gen++; }
        mutexUnlock(&g_tid.m);
    }
}

// "已比對 2,310 / 5,590" while reading (everything read before counts), "" when done.
static std::string groupThousands(size_t n) {
    std::string s = std::to_string(n);
    for (int p = (int)s.size() - 3; p > 0; p -= 3) s.insert((size_t)p, ",");
    return s;
}
static std::string tidProgress() {
    mutexLock(&g_tid.m);
    std::string t;
    if (g_tid.paused) t = "伺服器沒有回應，比對暫停（B 返回後再進來會重試）";
    else if (!g_tid.done)
        t = "已比對 " + groupThousands(g_tid.knownCurrent) + " / " + groupThousands(g_tid.current.size());
    mutexUnlock(&g_tid.m);
    return t;
}

static std::vector<uint64_t> realIds(const App& a, size_t i) {
    std::vector<uint64_t> ids;
    if (a.files[i].onlyTitle) return {a.files[i].onlyTitle};
    mutexLock(&g_tid.m);
    auto it = g_tid.known.find(tidCacheKey(a.files[i]));
    if (it != g_tid.known.end())
        for (const auto& m : it->second.items) ids.push_back(m.titleId);
    mutexUnlock(&g_tid.m);
    return ids;
}

// Every file of the server has been looked at (or the scan is not possible): "NAS 上沒有" is final.
static bool tidScanDone() {
    mutexLock(&g_tid.m);
    bool d = g_tid.done && !g_tid.paused;
    mutexUnlock(&g_tid.m);
    return d;
}
static bool tidScanPaused() {
    mutexLock(&g_tid.m);
    bool p = g_tid.paused;
    mutexUnlock(&g_tid.m);
    return p;
}

// ---- Before installing, every file of the chosen games (merged packages included) is looked into,
// so the newest update / DLC is picked. This runs in the background: X queues the games at once
// ("確認內容中"), their files start once the check is done.
struct ConfirmReq {
    std::set<u64> apps;                 // the games waiting for it
    std::vector<TidScan::Item> items;   // files not read yet
};
struct ConfirmRun {
    Mutex m = 0;
    Thread th;
    bool used = false, running = false;
    std::deque<ConfirmReq> todo;
    std::vector<ConfirmReq> done;
    http::Session* ses = nullptr;
    bool stop = false;
};
static ConfirmRun g_confirm;

static void confirmThread(void*) {
    http::Session ses;
    http::setThreadTimeout(8000);
    mutexLock(&g_confirm.m);
    g_confirm.ses = &ses;
    mutexUnlock(&g_confirm.m);
    for (;;) {
        mutexLock(&g_confirm.m);
        if (g_confirm.todo.empty() || g_confirm.stop) {
            g_confirm.running = false;
            g_confirm.ses = nullptr;
            mutexUnlock(&g_confirm.m);
            return;
        }
        ConfirmReq req = g_confirm.todo.front();
        g_confirm.todo.pop_front();
        mutexUnlock(&g_confirm.m);
        int failed = 0;
        for (const auto& it : req.items) {
            mutexLock(&g_tid.m);
            bool known = g_tid.known.count(it.key) > 0;  // the background scan got there first
            mutexUnlock(&g_tid.m);
            if (known) continue;
            if (failed >= 2) break;  // the NAS is not answering: the names decide
            FileScan fs;
            std::string err;
            if (!scanFile(ses, it.url, it.size, it.nameHasTid, fs, err)) {
                ses.close();
                install::log("check before install %s: %s", it.name.c_str(), err.c_str());
                if (networkTrouble(err)) { failed++; continue; }
                fs = FileScan();
                fs.metas = -2;  // this file cannot be read: its name decides
            }
            std::vector<std::string> lines;
            mutexLock(&g_tid.m);
            if (!g_tid.known.count(it.key) && g_tid.current.count(it.key)) g_tid.knownCurrent++;
            g_tid.known[it.key] = fs;
            g_tid.pending.push_back(tidLine(it.key, fs));
            if (!fs.items.empty()) g_tid.structGen++;
            g_tid.gen++;
            lines = tidTakeLocked();
            mutexUnlock(&g_tid.m);
            tidWrite(lines, false);
        }
        mutexLock(&g_confirm.m);
        g_confirm.done.push_back(req);
        mutexUnlock(&g_confirm.m);
    }
}

static void startConfirm(const ConfirmReq& req) {
    mutexLock(&g_confirm.m);
    g_confirm.todo.push_back(req);
    bool start = !g_confirm.running;
    if (start) g_confirm.running = true;
    mutexUnlock(&g_confirm.m);
    if (!start) return;
    if (g_confirm.used) {
        threadWaitForExit(&g_confirm.th);
        threadClose(&g_confirm.th);
        g_confirm.used = false;
    }
    if (R_SUCCEEDED(threadCreate(&g_confirm.th, confirmThread, nullptr, nullptr, 0x40000, 0x2D, bgCore(0))) &&
        R_SUCCEEDED(threadStart(&g_confirm.th))) {
        g_confirm.used = true;
        return;
    }
    confirmThread(nullptr);  // no thread: check here
}

static void stopConfirm() {
    mutexLock(&g_confirm.m);
    g_confirm.stop = true;
    if (g_confirm.ses) g_confirm.ses->interrupt();
    mutexUnlock(&g_confirm.m);
    if (g_confirm.used) {
        threadWaitForExit(&g_confirm.th);
        threadClose(&g_confirm.th);
        g_confirm.used = false;
    }
}

// What a server file holds: read from the package when known, else from its name.
// (g_tid.m held by the caller: lists of 5,000 files take the lock once, not once per file)
static std::vector<container::Meta> itemsOfLocked(const App& a, size_t i) {
    {
        auto it = g_tid.known.find(tidCacheKey(a.files[i]));
        std::vector<container::Meta> v;
        if (it != g_tid.known.end())
            for (const auto& m : it->second.items)
                if (!a.files[i].onlyTitle || m.titleId == a.files[i].onlyTitle) v.push_back(m);  // one title of a merged package
        if (!v.empty()) return v;
    }
    const titles::FileInfo& fi = a.info[i];
    if (!fi.titleId) return {};
    container::Meta m;
    m.titleId = fi.titleId;
    m.version = fi.version;
    m.versionKnown = fi.hasVersion;
    m.type = fi.kind == titles::Kind::Base ? 0x80 : fi.kind == titles::Kind::Update ? 0x81 : 0x82;
    return {m};
}

static void itemsOfAll(const App& a, std::vector<std::vector<container::Meta>>& items) {
    items.assign(a.files.size(), {});
    mutexLock(&g_tid.m);
    for (size_t i = 0; i < a.files.size(); i++) items[i] = itemsOfLocked(a, i);
    mutexUnlock(&g_tid.m);
}

// Files whose name has no TID take the real one (the base game's when the package holds several).
static size_t applyRealTids(App& a) {
    size_t n = 0;
    for (size_t i = 0; i < a.files.size() && i < a.info.size(); i++) {
        if (a.info[i].titleId) continue;
        std::vector<uint64_t> ids = realIds(a, i);
        if (ids.empty()) continue;
        uint64_t pick = ids[0];
        for (uint64_t id : ids)
            if ((id & 0xFFF) == 0) { pick = id; break; }
        a.info[i] = titles::fromTitleId(pick);
        n++;
    }
    return n;
}

// ---- Merged packages listed as one file per title (base, update, each DLC): every rule for single
// files (newest wins, no downgrade, DLC by TID, space) then applies to them. Nothing changes on the
// server: installing one reads only that title's part of the package.
static std::string tagOf(const container::Meta& m) {
    char b[64];
    snprintf(b, sizeof b, "[%016llX][v%u][%s]", (unsigned long long)m.titleId, m.version,
             m.type == 0x81 ? "UPD" : m.type == 0x82 ? "DLC" : "BASE");
    return b;
}
// "洛克人EXE合集 第一卷 v1.0.3[0100…][v196608][Base] US 2G+2U+10D.xci" -> "洛克人EXE合集 第一卷 v1.0.3 US 2G+2U+10D"
static std::string stemWithoutTags(const std::string& name) {
    std::string n = name;
    size_t dot = n.rfind('.');
    if (dot != std::string::npos && n.size() - dot <= 5) n.resize(dot);
    std::string out;
    int depth = 0;
    for (char c : n) {
        if (c == '[') { depth++; continue; }
        if (c == ']') { if (depth) depth--; continue; }
        if (!depth) out += c;
    }
    std::string t;
    for (char c : out)
        if (!(c == ' ' && (t.empty() || t.back() == ' '))) t += c;
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t;
}

static std::vector<remote::RemoteFile> splitMerged(const App& a) {
    std::vector<remote::RemoteFile> out;
    out.reserve(a.raw.size());
    std::map<size_t, std::vector<container::Meta>> mergedOf;  // raw index -> its titles (merged packages only)
    mutexLock(&g_tid.m);
    for (size_t r = 0; r < a.raw.size(); r++) {
        auto it = g_tid.known.find(tidCacheKey(a.raw[r]));
        if (it != g_tid.known.end() && it->second.items.size() >= 2) mergedOf[r] = it->second.items;
    }
    mutexUnlock(&g_tid.m);
    for (size_t r = 0; r < a.raw.size(); r++) {
        const auto& f = a.raw[r];
        std::vector<container::Meta> items;
        auto mo = mergedOf.find(r);
        if (mo != mergedOf.end()) items = mo->second;
        bool usable = items.size() >= 2;
        for (const auto& m : items) usable = usable && m.titleId && m.versionKnown;
        if (!usable) { out.push_back(f); continue; }
        std::string dir, file = f.displayName;
        size_t slash = file.rfind('/');
        if (slash != std::string::npos) { dir = file.substr(0, slash + 1); file = file.substr(slash + 1); }
        std::string ext;
        size_t dot = file.rfind('.');
        if (dot != std::string::npos) ext = file.substr(dot);
        std::string stem = stemWithoutTags(file);
        std::set<u64> games;
        for (const auto& m : items) games.insert(covers::baseTitleId(m.titleId));
        std::map<u64, int> gameNo;
        for (u64 g : games) gameNo[g] = (int)gameNo.size() + 1;
        for (const auto& m : items) {
            u64 base = covers::baseTitleId(m.titleId);
            std::string name;
            titledb::Entry e;
            if (a.db && m.type == 0x82 && a.db->find(m.titleId, e) && !e.name.empty()) name = e.name;
            else if (a.db && a.db->find(base, e) && !e.name.empty()) name = e.name + (m.type == 0x82 ? " DLC" : "");
            else name = stem + (games.size() > 1 ? "（第 " + std::to_string(gameNo[base]) + " 款）" : "");
            char hex[20];
            snprintf(hex, sizeof hex, "%016llX", (unsigned long long)m.titleId);
            remote::RemoteFile v;
            v.url = f.url + "#gs-title=" + hex;
            v.displayName = dir + name + " " + tagOf(m) + ext;
            v.size = m.size ? m.size : f.size;
            v.pkgUrl = f.url;
            v.pkgName = f.displayName;
            v.pkgSize = f.size;
            v.onlyTitle = m.titleId;
            out.push_back(v);
        }
    }
    return out;
}

static void classifyAll(App& a);  // below
static void applyTitleDb(App& a);
// Builds the file list from the server's list and what is known about its packages, keeping the
// selection and the cursor (by URL). Called after the list is read and when merged packages are found.
static void reloadFiles(App& a, bool keepView) {
    std::set<std::string> sel;
    for (size_t i = 0; i < a.files.size() && i < a.selected.size(); i++)
        if (a.selected[i]) sel.insert(a.files[i].url);
    std::string curUrl = keepView && a.list.cursor < a.view.size() ? a.files[a.view[a.list.cursor]].url : std::string();
    a.files = splitMerged(a);
    a.info.clear();
    for (const auto& f : a.files) a.info.push_back(titles::parse(f.displayName));
    applyRealTids(a);
    a.selected.assign(a.files.size(), false);
    for (size_t i = 0; i < a.files.size(); i++) a.selected[i] = sel.count(a.files[i].url) > 0;
    a.urlIndex.clear();
    for (size_t i = 0; i < a.files.size(); i++) a.urlIndex[a.files[i].url] = i;
    applyTitleDb(a);
    a.status.assign(a.files.size(), titles::Status::Unknown);
    for (size_t i = 0; i < a.files.size(); i++) a.status[i] = titles::classify(a.info[i], a.installed);
    if (!keepView) return;
    rebuildView(a, false);
    for (size_t v = 0; v < a.view.size(); v++)
        if (a.files[a.view[v]].url == curUrl) { a.list.cursor = v; break; }
    for (size_t v = 0; v < a.view.size() && v < a.list.items.size(); v++) a.list.items[v].selected = a.selected[a.view[v]];
}

// ------------------------------------------------------------------ connecting (start / 重新整理)
// The server's list is read on a thread while a bar shows how far it got: connecting (address,
// connection, login), reading the list ("已讀到 N 個檔案"), comparing with the console. It cannot be
// cancelled: every step has its own time limit (5 s to connect, 8 s without data), and a failure
// offers A retry / Y server settings / B go on without the server.
static std::string groupDigits(size_t n) {
    std::string t = std::to_string(n);
    for (int k = (int)t.size() - 3; k > 0; k -= 3) t.insert((size_t)k, ",");
    return t;
}

static void showConnect(App& a, int stage, double pct, const std::string& detail, bool failed = false) {
    screens::ConnectState s;
    s.server = a.list.serverName;
    s.version = APP_VERSION;
    s.stage = stage;
    s.percent = pct;
    s.detail = detail;
    s.failed = failed;
    present(a, [&](gfx::Canvas& c) { screens::drawConnect(c, a.font, s); });
}

struct ListJob {
    std::string url;
    std::vector<remote::RemoteFile> files;
    remote::ServerKind kind = remote::ServerKind::None;
    std::string err, greeting;
    bool ok = false;
    remote::ListProgress progress;
    std::atomic<bool> done{false};
};
static Thread g_listThread;
static std::shared_ptr<ListJob> g_listJob;
static void listThread(void*) {
    std::shared_ptr<ListJob> j = g_listJob;
    remote::setListProgress(&j->progress);
    http::setThreadTimeout(8000);  // the list: no answer within 8 s is an error, not a long wait
    j->ok = remote::fetchList(j->url, j->files, j->kind, j->err);
    j->greeting = j->ok ? remote::lastListMessage() : std::string();
    http::setThreadTimeout(0);
    remote::setListProgress(nullptr);
    j->done = true;
}

static bool fetchWithProgress(App& a, remote::ServerKind& kind, std::string& err, bool& connected, double& pct) {
    auto job = std::make_shared<ListJob>();
    job->url = a.list.server;
    g_listJob = job;
    u64 t0 = nowMs();
    pct = 0.10;
    showConnect(a, 1, pct, "");
    bool created = R_SUCCEEDED(threadCreate(&g_listThread, listThread, nullptr, nullptr, 0x40000, 0x2C, bgCore(1)));
    bool started = created && R_SUCCEEDED(threadStart(&g_listThread));
    if (created && !started) threadClose(&g_listThread);
    if (!started) listThread(nullptr);  // no thread: read it here (the bar just jumps)
    while (!job->done) {
        double t = (double)(nowMs() - t0);
        if (!job->progress.connected) {
            pct = 0.10 + 0.24 * (1.0 - std::exp(-t / 4000.0));
            int sec = (int)(t / 1000);
            showConnect(a, 1, pct, sec ? "已等 " + std::to_string(sec) + " 秒" : std::string());
        } else {
            size_t n = job->progress.files;
            pct = std::max(pct, 0.35 + 0.45 * (1.0 - std::exp(-(double)n / 1500.0)));
            showConnect(a, 2, pct, n ? "已讀到 " + groupDigits(n) + " 個檔案" : std::string("伺服器已回應，正在讀取清單…"));
        }
        appletMainLoop();  // keeps HOME / sleep working; the read ends by itself within its time limits
        svcSleepThread(33000000ULL);
    }
    if (started) {
        threadWaitForExit(&g_listThread);
        threadClose(&g_listThread);
    }
    g_listJob.reset();
    connected = job->progress.connected;
    kind = job->kind;
    err = job->err;
    a.greeting = job->greeting;
    if (!job->ok) return false;
    a.raw = std::move(job->files);
    pct = 0.80;
    showConnect(a, 2, pct, "已讀到 " + groupDigits(a.raw.size()) + " 個檔案");
    return true;
}

// The bar turns red with the reason. 1 = retry (A), 2 = server settings (Y), 0 = go on without (B).
static int connectFailed(App& a, int stage, double pct, const std::string& err) {
    showConnect(a, stage, pct, err, true);
    std::vector<screens::HitBox> none;
    while (appletMainLoop()) {
        u64 d = buttonsDown(a);
        if (d & HidNpadButton_A) return 1;
        if (d & HidNpadButton_Y) return 2;
        if (d & (HidNpadButton_B | HidNpadButton_Plus)) return 0;
        svcSleepThread(16000000);
    }
    return 0;
}

static void refresh(App& a) {
    a.files.clear();
    a.raw.clear();
    a.urlIndex.clear();
    a.info.clear();
    a.status.clear();
    a.selected.clear();
    a.names.clear();
    a.dates.clear();
    a.view.clear();
    a.list.items.clear();
    a.list.cursor = a.list.scroll = 0;
    if (a.list.server.empty()) {
        a.list.status = "尚未設定伺服器：按 Y → 伺服器設定 → 新增伺服器。";
        rebuildView(a);
        return;
    }
    while (!netLinkUp()) {
        int pick = askChoice(a, "Switch 沒有連上網路",
                             {"!目前沒有網路連線，無法連到伺服器。",
                              "請到 HOME →「設定」→「網際網路」確認 Wi-Fi 或有線網路已連上，再回來按 A 重試。",
                              "已在主機上的遊戲照常可以開，也可以用 Y 選單「詳細管理遊戲內容」。"},
                             {{"A", "重試", kGo, 1}, {"B", "先不連線", kLater, 0}});
        if (pick == 1) continue;
        a.list.status = "沒有網路：連上網路後按 Y → 重新整理。";
        rebuildView(a);
        return;
    }
    remote::ServerKind kind = remote::ServerKind::None;
    for (;;) {
        std::string err;
        bool connected = false;
        double pct = 0;
        if (fetchWithProgress(a, kind, err, connected, pct)) break;
        install::log("connect: %s", err.c_str());
        int pick = connectFailed(a, connected ? 2 : 1, pct, err);
        if (pick == 1) continue;  // A: again
        a.list.status = "錯誤：" + err + "（按 Y → 重新整理 可再試一次）";
        a.raw.clear();
        a.files.clear();
        a.greeting.clear();
        rebuildView(a);
        if (pick == 2) a.openServerMenu = true;  // Y: straight to the server settings
        return;
    }
    startTidScan(a);   // real TIDs in the background (known ones apply right away)
    mutexLock(&g_tid.m);
    a.tidGen = g_tid.structGen;
    mutexUnlock(&g_tid.m);
    showConnect(a, 3, 0.85, "伺服器上 " + groupDigits(a.raw.size()) + " 個檔案，正在和主機上的遊戲比對");
    u64 t0 = nowMs();
    // The game database (names / dates) is read from the SD card in the background at start. Waited
    // for here (a few seconds at most) and applied now: otherwise the list was built a second time
    // right after the home screen appeared, which froze it for seconds.
    if (a.db) {
        while (a.db->generation() == 0 && nowMs() - t0 < 3000) {
            titledb::Db::State st = a.db->state();
            if (st != titledb::Db::State::Loading && st != titledb::Db::State::Idle) break;
            svcSleepThread(20000000ULL);
        }
        a.dbGen = a.db->generation();  // what reloadFiles applies now (a newer one is applied later)
    }
    u64 t1 = nowMs();
    classifyAll(a);
    u64 t2 = nowMs();
    mutexLock(&g_tid.m);
    a.tidGen = g_tid.structGen;  // results known now are in this list
    mutexUnlock(&g_tid.m);
    reloadFiles(a, false);  // merged packages already known are split into their titles
    rebuildView(a);
    install::log("start: game database %llu ms, installed games %llu ms, list %llu ms (%zu files)",
                 (unsigned long long)(t1 - t0), (unsigned long long)(t2 - t1), (unsigned long long)(nowMs() - t2), a.files.size());
    showConnect(a, 4, 1.0, "伺服器回應正常：" + groupDigits(a.files.size()) + " 個檔案（" + groupDigits(a.rows.size()) + " 款遊戲）");
    svcSleepThread(400000000ULL);  // the full bar for a moment, then the list
    size_t upd = a.list.updates, dlc = a.list.dlcs;
    a.list.status = (a.list.serverName.empty() ? std::string(remote::kindName(kind)) : a.list.serverName) + "：" +
                    std::to_string(a.files.size()) + " 個檔案";
    a.list.status += "（" + std::to_string(a.rows.size()) + " 款遊戲）";
    if (a.installedOk) a.list.status += "，" + std::to_string(upd) + " 款可更新、" + std::to_string(dlc) + " 款可裝 DLC。";
    else a.list.status += "（無法讀取主機已安裝的內容）。";
    if (!a.cardBases.empty())
        a.list.status += std::to_string(a.cardBases.size()) + " 款遊戲是卡帶版原樣安裝（標示「需重裝」），請重裝本體。";
}

static bool askSearch(App& a) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetType(&kbd, SwkbdType_All);  // every keyboard the console offers (the preset is English only)
    swkbdConfigSetHeaderText(&kbd, "搜尋檔名");
    swkbdConfigSetGuideText(&kbd, "輸入遊戲名稱或遊戲 ID 的一部分，留空則清除搜尋");
    swkbdConfigSetInitialText(&kbd, a.search.c_str());
    char out[128] = {0};
    Result rc = swkbdShow(&kbd, out, sizeof(out));
    swkbdClose(&kbd);
    if (R_FAILED(rc)) return false;  // cancelled
    a.search = out;
    return true;
}

// ------------------------------------------------------------------ install
// Installs run in the background on up to 3 worker threads while the list stays usable: X adds to
// the queue, "-" (or the blue pill at the top) shows the progress. Files of the same game wait for
// each other (base, then update, then DLC) unless only that game is left in the queue.
namespace {
struct Job {
    std::string url, name;
    u64 group = 0;
    int state = 0;  // 0 waiting, 1 running, 2 done
    bool alone = false;  // retry without anything else running (it ran out of memory next to others)
    bool ok = false, reported = false, skipped = false;
    int64_t added = 0;  // unix time it was queued
    int64_t seq = 0;    // install order (lower first); the queue screen can change it
    bool removed = false;  // taken out of the queue by the user: not reported as a result
    bool warn = false;     // installed, but with a problem the user should see (yellow)
    bool retryWithBase = false;  // not installed because its base hit a network problem: retried with it
    std::set<u64> skipTitles;    // titles inside this package not to install (older than wanted / not needed)
    bool confirm = false;        // its game's files are still being checked: waits, replaced by the final plan
    std::string err, note;
};

const int kMaxLanes = 3;

struct InstallRun {
    Mutex m = 0;
    std::deque<Job> jobs;  // deque: workers keep references while the UI appends
    screens::ProgressLane lanes[kMaxLanes];
    install::Options opt;
    bool cancel = false;
    bool discard = false;  // the cancel also deletes what was downloaded (Y held), not "continue later"
    int alive = 0;
    bool laneUsed[kMaxLanes] = {false, false, false};
    bool laneExited[kMaxLanes] = {false, false, false};
    Thread threads[kMaxLanes];
    int allowed = 1;         // lanes that may start another file (lower lanes first)
    uint64_t received = 0;   // bytes received by all installs (the auto setting measures this)
};
InstallRun g_run;

struct WorkerArg {
    InstallRun* run;
    int lane;
    int delayMs;
};
WorkerArg g_args[kMaxLanes];

bool retryable(const std::string& e) {
    const char* keys[] = {"無法連線", "HTTPS 連線建立失敗", "HTTPS 交握失敗", "逾時", "中斷", "接收資料失敗",
                          "傳送資料失敗", "無法建立網路連線", "雜湊值不符", "大小不符", "HTTP 5", "無法解析主機", "連線卡住"};
    for (const char* k : keys)
        if (e.find(k) != std::string::npos) return true;
    return false;
}

// Sleeps up to `ms`, returns false early when the run is cancelled.
bool sleepUnlessCancelled(InstallRun& r, int ms) {
    for (int t = 0; t < ms; t += 100) {
        mutexLock(&r.m);
        bool c = r.cancel;
        mutexUnlock(&r.m);
        if (c) return false;
        svcSleepThread(100000000ULL);
    }
    return true;
}

void workerExit(InstallRun& r, int lane) {
    mutexLock(&r.m);
    r.lanes[lane].active = false;
    r.alive--;
    r.laneExited[lane] = true;
    mutexUnlock(&r.m);
}

// Indexes of the waiting files, in install order. Call with r.m locked.
std::vector<size_t> waitingOrder(const InstallRun& r) {
    std::vector<size_t> v;
    for (size_t j = 0; j < r.jobs.size(); j++)
        if (r.jobs[j].state == 0) v.push_back(j);
    std::stable_sort(v.begin(), v.end(), [&](size_t a, size_t b) { return r.jobs[a].seq < r.jobs[b].seq; });
    return v;
}

// Is the console connected to a network right now?
bool netUp() {
    NifmInternetConnectionType type;
    u32 strength = 0;
    NifmInternetConnectionStatus st;
    if (R_FAILED(nifmGetInternetConnectionStatus(&type, &strength, &st))) return false;
    return st == NifmInternetConnectionStatus_Connected;
}

// CPU cores this app may use, the UI thread's core first. Installs are spread over all of them
// (they all used to share the UI core, which capped the total speed).
std::vector<int> g_cores;
void initCores() {
    if (!g_cores.empty()) return;
    u64 mask = 0;
    int self = (int)svcGetCurrentProcessorNumber();
    if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || !mask) mask = 1ULL << self;
    g_cores.push_back(self);
    for (int k = 0; k < 4; k++)
        if ((mask & (1ULL << k)) && k != self) g_cores.push_back(k);
}
int coreFor(int slot) {  // slot 1.. = background threads; 0 would be the UI core
    if (g_cores.empty()) return -2;
    return g_cores[(size_t)slot % g_cores.size()];
}

// ------------------------------------------------------------------ notes to the author (ntfy)
// Every note goes through one outbox kept on the SD card until ntfy has taken it: a note that cannot
// be sent (no internet, ntfy busy) is tried again after 10 s and after 1 minute, then at the next
// start, without asking the player again. Sent one at a time, a second apart, by one thread.
static const char* kOutboxPath = "sdmc:/config/lan-installer/report-outbox.txt";
struct OutNote {
    uint32_t id = 0;
    std::string what;      // for the log ("server check", "file report: x.nsp")
    std::string body;      // the ntfy JSON (one line: jsonEscape leaves no line break or tab)
    bool notify = false;   // the player asked for it: the list shows how it went
    int tries = 0;
    u64 due = 0;           // ms; ~0 = parked until the next start
};
static struct {
    Mutex m;
    CondVar cv;
    Thread th;
    bool started = false, stop = false;
    std::deque<OutNote> q;
    uint32_t nextId = 1;
    int result = 0;        // for the list: 1 sent, 2 not sent yet (kept), 0 nothing new
    std::string why;
} g_out;

static void outboxSaveLocked() {
    if (g_out.q.empty()) { unlink(kOutboxPath); return; }
    if (FILE* f = fopen(kOutboxPath, "w")) {
        for (const OutNote& n : g_out.q) {
            std::string w = n.what;
            for (auto& ch : w) if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
            fprintf(f, "%s\t%s\n", w.c_str(), n.body.c_str());
        }
        fclose(f);
    }
}

static bool sendNtfy(const std::string& body, std::string& why) {
    http::Request req;
    req.method = "POST";
    req.headers.push_back("Content-Type: application/json; charset=utf-8");
    req.body = body;
    std::vector<uint8_t> out;
    http::Response res;
    std::string e;
    bool ok = http::requestToMemory(badfile::kNtfyUrl, req, out, res, e) && res.status >= 200 && res.status < 300;
    if (ok) return true;
    why = !e.empty() ? e : "HTTP " + std::to_string(res.status);
    if (e.empty() && !out.empty()) {  // ntfy says why, e.g. {"code":42901,"error":"limit reached"}
        std::string t(out.begin(), out.begin() + (long)std::min<size_t>(out.size(), 200));
        for (auto& ch : t) if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
        why += "：" + t;
    }
    return false;
}

static void outboxThread(void*) {
    http::setThreadTimeout(8000);
    mutexLock(&g_out.m);
    while (!g_out.stop) {
        u64 now = nowMs(), wait = 0;
        OutNote* pick = nullptr;
        for (OutNote& n : g_out.q) {
            if (n.due == ~0ULL) continue;
            if (n.due <= now) { pick = &n; break; }
            u64 w = n.due - now;
            if (!wait || w < wait) wait = w;
        }
        if (!pick) {  // nothing due: sleep until the next one, or until a note is added
            condvarWaitTimeout(&g_out.cv, &g_out.m, wait ? wait * 1000000ULL : UINT64_MAX);
            continue;
        }
        OutNote n = *pick;
        mutexUnlock(&g_out.m);
        std::string why;
        bool ok = sendNtfy(n.body, why);
        install::log("%s -> %s (try %d)", n.what.c_str(), ok ? "sent" : why.c_str(), n.tries + 1);
        mutexLock(&g_out.m);
        for (auto it = g_out.q.begin(); it != g_out.q.end(); ++it) {
            if (it->id != n.id) continue;
            if (ok) {
                if (it->notify) { g_out.result = 1; g_out.why.clear(); }
                g_out.q.erase(it);
            } else {
                it->tries++;
                it->due = it->tries == 1 ? nowMs() + 10000 : it->tries == 2 ? nowMs() + 60000 : ~0ULL;
                if (it->notify) { g_out.result = 2; g_out.why = why; }
            }
            break;
        }
        outboxSaveLocked();
        mutexUnlock(&g_out.m);
        svcSleepThread(1000000000ULL);  // a second between notes (ntfy limits bursts)
        mutexLock(&g_out.m);
    }
    mutexUnlock(&g_out.m);
}

// At start: notes left from earlier runs are sent again (silently).
static void outboxStart() {
    mutexLock(&g_out.m);
    if (FILE* f = fopen(kOutboxPath, "r")) {
        std::string line;
        char buf[4096];
        while (fgets(buf, sizeof buf, f)) {
            line += buf;
            if (line.empty() || line.back() != '\n') continue;  // a long line: read on
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            size_t t = line.find('\t');
            if (t != std::string::npos && line.size() > t + 2 && line[t + 1] == '{') {
                OutNote n;
                n.id = g_out.nextId++;
                n.what = line.substr(0, t) + " (from an earlier start)";
                n.body = line.substr(t + 1);
                g_out.q.push_back(n);
            }
            line.clear();
        }
        fclose(f);
        if (!g_out.q.empty()) install::log("report outbox: %zu note(s) left from an earlier start", g_out.q.size());
    }
    if (!g_out.started &&
        R_SUCCEEDED(threadCreate(&g_out.th, outboxThread, nullptr, nullptr, 0x40000, 0x2F, bgCore(1)))) {
        if (R_SUCCEEDED(threadStart(&g_out.th))) g_out.started = true;
        else threadClose(&g_out.th);
    }
    mutexUnlock(&g_out.m);
}

// Any thread: the notes are kept on the SD card at once and sent in the background.
static void outboxAdd(const std::vector<std::string>& bodies, const std::string& what, bool notify) {
    if (bodies.empty()) return;
    mutexLock(&g_out.m);
    for (size_t k = 0; k < bodies.size(); k++) {
        OutNote n;
        n.id = g_out.nextId++;
        n.what = bodies.size() > 1 ? what + " (" + std::to_string(k + 1) + "/" + std::to_string(bodies.size()) + ")" : what;
        n.body = bodies[k];
        n.notify = notify && k + 1 == bodies.size();  // the list reports once, on the last part
        g_out.q.push_back(n);
    }
    outboxSaveLocked();
    condvarWakeAll(&g_out.cv);
    mutexUnlock(&g_out.m);
}

static void outboxStop() {
    if (!g_out.started) return;
    mutexLock(&g_out.m);
    g_out.stop = true;
    condvarWakeAll(&g_out.cv);
    mutexUnlock(&g_out.m);
    threadWaitForExit(&g_out.th);  // a note being sent ends within its time limits; the rest stay on the SD card
    threadClose(&g_out.th);
    g_out.started = false;
}

// Main loop: how the player's report went, on the list's status line.
static void pollOutbox(App& a) {
    mutexLock(&g_out.m);
    int r = g_out.result;
    std::string why = g_out.why;
    g_out.result = 0;
    mutexUnlock(&g_out.m);
    if (r == 1) a.list.status = "已回報給作者，謝謝！";
    else if (r == 2) a.list.status = "回報還沒送出（" + why + "），會自動再試；仍不行的話下次開啟會自動補送。";
}

// 壞檔自動回報 (see badfile.hpp): runs on the install thread, never blocks the screen.
// `kinds`: (kind code, what to write). Each kind of each file is reported at most twice.
const char* kReportedPath = "sdmc:/config/lan-installer/reported.txt";
void sendFileReport(const std::string& url, const std::string& name,
                    const std::vector<std::pair<const char*, std::string>>& kinds) {
    if (!badfile::fromReportServer(url)) return;
    std::string file = badfile::fileName(url);
    if (file.empty()) return;
    std::vector<std::string> keys;
    std::string text;
    for (const auto& k : kinds) {
        std::string key = badfile::fileKey(file, k.first);
        if (badfile::reportedTimes(kReportedPath, key) >= badfile::kMaxReports) continue;
        keys.push_back(key);
        text += (text.empty() ? "" : "；") + k.second;
    }
    if (keys.empty()) return;
    // counted now: the outbox keeps the note until it is sent (also across starts)
    badfile::bumpReported(kReportedPath, keys);
    outboxAdd({badfile::ntfyJson(badfile::kNtfyTopic, file, titles::parse(name).titleId, text)},
              "file report: " + file + " (" + text + ")", false);
}

void reportBadFile(const std::string& url, const std::string& name, const std::string& err) {
    std::string kind;
    if (badfile::isBadFile(err, kind)) sendFileReport(url, name, {{badfile::kKindBad, kind}});
}

void installWorker(void* p) {
    WorkerArg* w = (WorkerArg*)p;
    InstallRun& r = *w->run;
    install::setLogTag(w->lane + 1);
    install::setReaderCore(coreFor(w->lane + 2));  // its download thread runs on another core
    // Workers start 2 s apart: some NAS reject several HTTPS connections opened at the same moment.
    if (w->delayMs > 0 && !sleepUnlessCancelled(r, w->delayMs)) { workerExit(r, w->lane); return; }
    for (;;) {
        mutexLock(&r.m);
        if (r.cancel) { mutexUnlock(&r.m); break; }
        if (w->lane >= r.allowed) { mutexUnlock(&r.m); break; }  // fewer installs at a time now
        // Pick the next file:
        //  1. a file of a game nothing else is installing (different games run side by side);
        //  2. when only games already installing are left, more files of those games;
        //  a file marked `alone` waits until nothing else runs, and runs by itself.
        int pick = -1;
        bool waiting = false, aloneRunning = false;
        size_t running = 0;
        for (const Job& o : r.jobs)
            if (o.state == 1) { running++; aloneRunning = aloneRunning || o.alone; }
        auto groupBusy = [&](u64 g) {
            for (const Job& o : r.jobs)
                if (o.state == 1 && o.group == g) return true;
            return false;
        };
        std::vector<size_t> order = waitingOrder(r);  // waiting files, install order
        std::set<u64> seen;  // games whose first waiting file came earlier in the order
        for (size_t j : order) {
            waiting = true;
            if (aloneRunning) break;
            bool earlier = !seen.insert(r.jobs[j].group).second;  // keep a game's order
            if (r.jobs[j].confirm) continue;  // its game is still being checked
            if (r.jobs[j].alone) {
                if (running == 0 && !earlier) { pick = (int)j; break; }
                continue;
            }
            if (!earlier && !groupBusy(r.jobs[j].group)) { pick = (int)j; break; }
        }
        if (pick < 0 && waiting && !aloneRunning) {  // 2. same game in parallel
            for (size_t j : order)
                if (!r.jobs[j].alone && !r.jobs[j].confirm) { pick = (int)j; break; }
        }
        if (pick < 0) {
            mutexUnlock(&r.m);
            if (!waiting) break;   // nothing left
            svcSleepThread(50000000);
            continue;
        }
        Job& job = r.jobs[pick];
        job.state = 1;
        const std::string jobUrl = job.url, jobName = job.name;
        const std::set<u64> jobSkip = job.skipTitles;
        screens::ProgressLane& lane = r.lanes[w->lane];
        lane = screens::ProgressLane();
        lane.active = true;
        lane.file = baseName(jobName);  // the NAS file name, unchanged
        lane.stage = "連線中…";
        install::Options opt = r.opt;
        opt.displayName = jobName;
        opt.skipTitles = std::set<uint64_t>(jobSkip.begin(), jobSkip.end());
        for (size_t k = 0; k < r.jobs.size(); k++)
            if ((int)k != pick && r.jobs[k].group == job.group && r.jobs[k].state != 2) opt.siblingsBusy = true;
        bool hadCompany = running > 0;
        mutexUnlock(&r.m);

        std::string err, note;
        bool ok = false;
        install::Findings found;
        opt.findings = &found;
        for (int attempt = 0;;) {
            err.clear();
            note.clear();
            found = install::Findings();
            uint64_t lastRx = 0;
            ok = install::installPackage(jobUrl, opt,
                [&](const install::Progress& pr) {
                    mutexLock(&r.m);
                    if (pr.received > lastRx) { r.received += pr.received - lastRx; lastRx = pr.received; }
                    screens::ProgressLane& l = r.lanes[w->lane];
                    l.stage = pr.stage;
                    l.done = pr.done;
                    l.total = pr.total;
                    l.speedMBs = pr.speedMBs;
                    bool stop = r.cancel;
                    mutexUnlock(&r.m);
                    return !stop;
                },
                err, note);
            // Network hiccups (the NAS refusing a connection, a dropped transfer) are retried;
            // the next attempt continues where this one stopped.
            mutexLock(&r.m);
            bool stopNow = r.cancel;
            mutexUnlock(&r.m);
            static const int kDelays[] = {5, 15, 30, 30};  // seconds before retry 1..4
            if (ok || stopNow) break;
            if (!netUp()) {
                // The console lost its network (Wi-Fi drop, sleep, out of range): wait for it instead
                // of using up the retries, then continue where the download stopped.
                install::log("network down, waiting for it: %s", err.c_str());
                mutexLock(&r.m);
                r.lanes[w->lane].stage = "網路中斷，等待網路恢復…（已下載的部分會保留）";
                r.lanes[w->lane].speedMBs = 0;
                mutexUnlock(&r.m);
                bool back = true;
                while (!netUp())
                    if (!sleepUnlessCancelled(r, 1000)) { back = false; break; }
                if (!back) break;
                if (!sleepUnlessCancelled(r, 2000)) break;  // let the connection settle
                install::log("network back, continuing");
                continue;
            }
            if (!retryable(err) || attempt >= 4) break;
            int delay = kDelays[attempt++];
            install::log("retry %d of 4 in %d s: %s", attempt, delay, err.c_str());
            mutexLock(&r.m);
            r.lanes[w->lane].stage = "連線問題，" + std::to_string(delay) + " 秒後重試（第 " + std::to_string(attempt) +
                                     " / 4 次，已下載的部分會接著下載）…";
            mutexUnlock(&r.m);
            if (!sleepUnlessCancelled(r, delay * 1000)) break;
        }

        {
            mutexLock(&r.m);
            bool cancelled = r.cancel;
            mutexUnlock(&r.m);
            if (!ok && !cancelled) reportBadFile(jobUrl, jobName, err);
            else if (ok && found.mismatched.empty() && found.missingTicket.empty() && badfile::fromReportServer(jobUrl)) {
                std::string f = badfile::fileName(jobUrl);  // installed cleanly: those problems start again from zero
                badfile::clearReported(kReportedPath, {f, badfile::fileKey(f, badfile::kKindBad), badfile::fileKey(f, badfile::kKindMismatch),
                                                       badfile::fileKey(f, badfile::kKindTicket)});
            }
        }
        bool warned = false;
        if (ok && (!found.mismatched.empty() || !found.missingTicket.empty())) {
            // installed, with something the user must know: yellow in 最近完成, and a note to the author
            std::string w;
            std::vector<std::pair<const char*, std::string>> kinds;
            if (!found.missingTicket.empty()) {
                w += "缺少 ticket（.tik），無法啟動，請換一個含 ticket 的檔案。";
                kinds.push_back({badfile::kKindTicket, "缺少 ticket（無法啟動）"});
            }
            if (!found.mismatched.empty()) {
                w += "已安裝，但檔案內容和描述檔不一致（可能是整合版改過）。如果不能啟動請通知作者。";
                kinds.push_back({badfile::kKindMismatch, "描述檔不一致（已照樣安裝，如果不能啟動請通知作者）"});
            }
            note = note.empty() ? w : w + " " + note;
            warned = true;
            sendFileReport(jobUrl, jobName, kinds);
        }
        mutexLock(&r.m);
        r.lanes[w->lane].active = false;
        bool oom = !ok && err.find("記憶體不足") != std::string::npos;
        if (oom && !r.cancel && !job.alone) {
            // Out of memory (next to other installs, or memory still in pieces): try again later,
            // by itself: nothing else starts until it is done.
            (void)hadCompany;
            install::log("retry later alone (out of memory): %s", jobName.c_str());
            job.state = 0;
            job.alone = true;
            mutexUnlock(&r.m);
            continue;
        }
        if (oom && g_appletMode)
            err += "。相簿模式記憶體較少：請按住 R 開任一遊戲進 hbmenu 再開本程式";
        job.state = 2;
        job.ok = ok;
        job.err = (!ok && r.cancel) ? std::string("已取消") : err;
        job.note = note;
        job.warn = warned;
        if (!ok && !r.cancel && titles::parse(jobName).kind == titles::Kind::Base) {
            // without its base game an update / DLC cannot start: do not install them alone
            int skipped = 0;
            bool net = retryable(job.err);
            for (Job& o : r.jobs)
                if (o.state == 0 && o.group == job.group) {
                    o.state = 2;
                    o.skipped = true;
                    o.retryWithBase = net;
                    o.err = net ? "本體因連線問題沒裝完，這款的更新／DLC 稍後跟本體一起再試" : "本體沒裝成功，這款的更新／DLC 先不裝";
                    skipped++;
                }
            if (skipped) install::log("base failed: %d update / DLC file(s) of the same game not installed", skipped);
        }
        mutexUnlock(&r.m);
    }
    workerExit(r, w->lane);
}
}  // namespace

static bool g_installUiLocked = false;  // HOME button blocked + auto sleep off while installing

// ---- CPU boost while installing: HTTPS decryption, decompression and hashing are CPU work, so a
// faster CPU means faster installs. The clocks go back to what they were when the installs end.
static bool g_boostOn = false;
static u32 g_boostPrev[3] = {0, 0, 0};  // CPU, GPU, memory clocks before
static const u32 kBoostHz[3] = {1785000000, 76800000, 1600000000};  // the UI is drawn by the CPU: GPU can idle

static bool setClock(int which, u32 hz, u32* before) {
    static const PcvModule mods[3] = {PcvModule_CpuBus, PcvModule_GPU, PcvModule_EMC};
    Result rc;
    if (hosversionAtLeast(8, 0, 0)) {
        PcvModuleId id;
        if (R_FAILED(pcvGetModuleId(&id, mods[which]))) return false;
        ClkrstSession ses;
        if (R_FAILED(clkrstOpenSession(&ses, id, 3))) return false;
        if (before) clkrstGetClockRate(&ses, before);
        rc = clkrstSetClockRate(&ses, hz);
        clkrstCloseSession(&ses);
    } else {
        if (before) pcvGetClockRate(mods[which], before);
        rc = pcvSetClockRate(mods[which], hz);
    }
    return R_SUCCEEDED(rc);
}

static void boostStart(App& a) {
    if (!a.boost || g_boostOn) return;
    static bool ready = false, tried = false;
    if (!tried) {
        tried = true;
        ready = R_SUCCEEDED(pcvInitialize());
        if (ready && hosversionAtLeast(8, 0, 0) && R_FAILED(clkrstInitialize())) { pcvExit(); ready = false; }
    }
    if (!ready) { install::log("cpu boost: clock services not available"); return; }
    bool ok = true;
    for (int k = 0; k < 3; k++) ok = setClock(k, kBoostHz[k], &g_boostPrev[k]) && ok;
    g_boostOn = true;
    install::log("cpu boost on: CPU %u -> %u MHz%s", g_boostPrev[0] / 1000000, kBoostHz[0] / 1000000, ok ? "" : " (partly refused)");
}

static void boostKeep() {  // docking / undocking resets the clocks: set them again now and then
    static u64 last = 0;
    if (!g_boostOn) return;
    u64 now = armTicksToNs(armGetSystemTick()) / 1000000ULL;
    if (now - last < 10000) return;
    last = now;
    for (int k = 0; k < 3; k++) setClock(k, kBoostHz[k], nullptr);
}

static void boostStop() {
    if (!g_boostOn) return;
    for (int k = 0; k < 3; k++)
        if (g_boostPrev[k]) setClock(k, g_boostPrev[k], nullptr);
    g_boostOn = false;
    install::log("cpu boost off: CPU back to %u MHz", g_boostPrev[0] / 1000000);
}

// ---- screen off while installing (handheld backlight); any button / touch turns it back on
static bool g_lblReady = false;
static void screenOff(App& a) {
    if (!g_lblReady) g_lblReady = R_SUCCEEDED(lblInitialize());
    if (!g_lblReady || a.screenOff) return;
    if (R_SUCCEEDED(lblSwitchBacklightOff(0))) a.screenOff = true;
}
static void screenOn(App& a) {
    if (!a.screenOff) return;
    lblSwitchBacklightOn(0);
    a.screenOff = false;
}
static bool g_exiting = false;          // stopping because the app closes: keep the saved queue
static const char* kQueuePath = "sdmc:/config/lan-installer/queue.txt";

// The queue (waiting + installing files) is mirrored on the SD card so it survives leaving the app.
// Files the user put off with "稍後再說": they stay in the saved queue until installed or discarded.
static std::vector<resume::QueueEntry> g_deferred;
static int64_t g_seq = 0;  // last install-order number handed out

static void saveQueueNow() {
    std::vector<resume::QueueEntry> q;
    std::set<std::string> urls;
    mutexLock(&g_run.m);
    for (const Job& j : g_run.jobs)  // installing now: first again next time
        if (j.state == 1) { q.push_back({j.url, j.name, j.group, j.added}); urls.insert(j.url); }
    for (size_t k : waitingOrder(g_run)) {
        const Job& j = g_run.jobs[k];
        q.push_back({j.url, j.name, j.group, j.added});
        urls.insert(j.url);
    }
    mutexUnlock(&g_run.m);
    for (const auto& d : g_deferred)
        if (!urls.count(d.url)) q.push_back(d);
    resume::saveQueue(kQueuePath, q);
}

static bool installsActive() {
    mutexLock(&g_run.m);
    bool active = g_run.alive > 0;
    for (const Job& j : g_run.jobs) active = active || j.state != 2;
    mutexUnlock(&g_run.m);
    return active;
}

// ---- "同時安裝數量：自動": start with one install; try one more and keep it only when the total speed
// really goes up. When the network is the limit, one game at full speed finishes (and can be played)
// sooner than three games sharing it.
struct AutoLanes {
    int lanes = 1;
    bool trial = false;     // just added a lane, measuring whether it helps
    u64 phaseStart = 0;     // ms
    uint64_t bytesAtMeasure = 0;
    bool measuring = false; // past the warm-up of this phase
    double baseRate = 0;    // MB/s before the trial
    u64 cooldownUntil = 0;  // ms: no new trial before this
};
static AutoLanes g_auto;
static const u64 kAutoWarmMs = 10000, kAutoMeasureMs = 25000, kAutoCooldownMs = 300000;

static int lanesNow(App& a) {
    if (a.parallel >= 1) return std::min(a.parallel, kMaxLanes);
    return g_auto.lanes;
}

static void autoReset() { g_auto = AutoLanes(); g_auto.phaseStart = armTicksToNs(armGetSystemTick()) / 1000000ULL; }

// Called from pollInstalls while installs run.
static void autoStep(App& a, size_t waiting) {
    if (a.parallel != 0) return;
    u64 now = armTicksToNs(armGetSystemTick()) / 1000000ULL;
    mutexLock(&g_run.m);
    uint64_t rx = g_run.received;
    mutexUnlock(&g_run.m);
    if (!g_auto.measuring) {
        if (now - g_auto.phaseStart < kAutoWarmMs) return;
        g_auto.measuring = true;
        g_auto.bytesAtMeasure = rx;
        g_auto.phaseStart = now;
        return;
    }
    if (now - g_auto.phaseStart < kAutoMeasureMs) return;
    double rate = (double)(rx - g_auto.bytesAtMeasure) / 1048576.0 / ((double)(now - g_auto.phaseStart) / 1000.0);
    int before = g_auto.lanes;
    if (g_auto.trial) {
        if (rate >= g_auto.baseRate * 1.15 + 0.3) {
            install::log("auto parallel: keep %d (%.1f MB/s, was %.1f with %d)", g_auto.lanes, rate, g_auto.baseRate, before - 1);
        } else {
            g_auto.lanes--;
            g_auto.cooldownUntil = now + kAutoCooldownMs;
            install::log("auto parallel: back to %d (%.1f MB/s with %d, %.1f with %d: no gain)", g_auto.lanes, rate, before,
                         g_auto.baseRate, g_auto.lanes);
        }
        g_auto.trial = false;
    } else if (g_auto.lanes < kMaxLanes && waiting > 0 && now >= g_auto.cooldownUntil) {
        g_auto.baseRate = rate;
        g_auto.lanes++;
        g_auto.trial = true;
        install::log("auto parallel: trying %d at a time (%.1f MB/s with %d)", g_auto.lanes, rate, before);
    }
    g_auto.measuring = false;
    g_auto.phaseStart = now;
}

// Starts workers while there is waiting work and fewer workers than the "同時安裝數量" setting.
static void ensureWorkers(App& a) {
    mutexLock(&g_run.m);
    if (g_run.cancel) { mutexUnlock(&g_run.m); return; }
    size_t open = 0;
    for (const Job& j : g_run.jobs) open += j.state != 2;
    g_run.allowed = lanesNow(a);
    int want = std::min(g_run.allowed, (int)open);
    int delay = 0;
    for (int lane = 0; lane < g_run.allowed && g_run.alive < want; lane++) {
        if (g_run.laneUsed[lane]) continue;
        g_args[lane] = {&g_run, lane, delay};
        g_run.laneUsed[lane] = true;
        g_run.laneExited[lane] = false;
        g_run.alive++;
        initCores();
        if (R_SUCCEEDED(threadCreate(&g_run.threads[lane], installWorker, &g_args[lane], nullptr, 0x80000, 0x2C, coreFor(lane + 1)))) {
            if (R_SUCCEEDED(threadStart(&g_run.threads[lane]))) { delay += 2000; continue; }
            threadClose(&g_run.threads[lane]);
        }
        g_run.laneUsed[lane] = false;
        g_run.alive--;
    }
    mutexUnlock(&g_run.m);
}

// Adds files to the queue (files already waiting or installing are skipped). Returns how many were added.
static size_t enqueue(App& a, const std::vector<Job>& jobs) {
    if (jobs.empty()) return 0;
    bool wasActive = installsActive();
    if (!wasActive) {
        install::prepare();
        mutexLock(&g_run.m);
        g_run.jobs.clear();  // a new session: forget the finished ones
        g_run.opt = install::Options();
        g_run.opt.storage = NcmStorageId_SdCard;
        g_run.opt.headerKey = install::headerKey();  // the user's own prod.keys on SD; null = install unconverted
        g_run.opt.convertGamecard = true;            // cartridge dumps become eShop type, so updates/DLC start
        mutexUnlock(&g_run.m);
        a.recent.clear();
        a.recentOk.clear();
        a.recentDetail.clear();
        a.recentWarn.clear();
        autoReset();
        a.sessionOk = a.sessionFail = 0;
    }
    if (!g_installUiLocked) {
        appletSetAutoSleepDisabled(true);
        appletBeginBlockingHomeButtonShortAndLongPressed(0);
        g_installUiLocked = true;
    }
    boostStart(a);
    size_t added = 0;
    mutexLock(&g_run.m);
    for (const Job& j : jobs) {
        bool dup = false;
        for (const Job& o : g_run.jobs) dup = dup || (o.url == j.url && o.state != 2);
        if (dup) continue;
        g_run.jobs.push_back(j);
        if (!g_run.jobs.back().added) g_run.jobs.back().added = (int64_t)time(nullptr);
        g_run.jobs.back().seq = ++g_seq;
        added++;
    }
    mutexUnlock(&g_run.m);
    for (const Job& j : jobs)  // queued now: no longer just "put off"
        g_deferred.erase(std::remove_if(g_deferred.begin(), g_deferred.end(),
                                        [&](const resume::QueueEntry& d) { return d.url == j.url; }),
                         g_deferred.end());
    saveQueueNow();
    install::log("queue: +%zu file(s), %s at a time, HTTPS via %s", added,
                 a.parallel ? std::to_string(a.parallel).c_str() : "auto", http::tlsBackend());
    ensureWorkers(a);
    return added;
}

// discard = true: what the cancelled files downloaded is deleted right away (Y held on the progress
// screen). Leaving the app keeps it so the files can continue later.
static void cancelAllInstalls(bool discard = false) {
    mutexLock(&g_run.m);
    g_run.cancel = true;
    g_run.discard = g_run.discard || discard;
    mutexUnlock(&g_run.m);
}

static std::string shortName(const std::string& n) {
    std::string b = n.substr(n.rfind('/') == std::string::npos ? 0 : n.rfind('/') + 1);
    return b;
}

// "cb89…3775.ncz：zstd 解壓失敗" -> "zstd 解壓失敗" (the content file name means nothing to the user)
static std::string plainReason(const std::string& r) {
    size_t p = r.find("：");
    if (p >= 36 && p <= 40) {
        bool hex = true;
        for (size_t i = 0; i < 32; i++) hex = hex && isxdigit((unsigned char)r[i]);
        if (hex && r.compare(32, 3, ".nc") == 0) return r.substr(p + strlen("："));
    }
    return r;
}


// Called every frame from the main loop: joins finished workers, reports finished files, refreshes
// what is installed, and unlocks HOME / sleep when everything is done.
static void pollInstalls(App& a) {
    std::vector<Job> finished;
    bool idle;
    mutexLock(&g_run.m);
    for (int lane = 0; lane < kMaxLanes; lane++) {
        if (!g_run.laneUsed[lane] || !g_run.laneExited[lane]) continue;
        mutexUnlock(&g_run.m);
        threadWaitForExit(&g_run.threads[lane]);  // it is returning right now
        threadClose(&g_run.threads[lane]);
        mutexLock(&g_run.m);
        g_run.laneUsed[lane] = false;
        g_run.laneExited[lane] = false;
    }
    bool queueChanged = false;
    std::vector<std::string> discardUrls;  // cancelled with Y: their downloaded parts are deleted
    if (g_run.cancel && g_run.alive == 0) {  // cancel finished: drop what never started
        for (Job& j : g_run.jobs) {
            if (j.state == 0) { j.state = 2; j.skipped = true; j.err = "已取消"; }
            else if (g_run.discard && !g_exiting && j.state == 2 && !j.ok && !j.skipped && j.err == "已取消")
                discardUrls.push_back(j.url);
        }
        g_run.cancel = false;
        g_run.discard = false;
        queueChanged = true;
    }
    for (Job& j : g_run.jobs)
        if (j.state == 2 && !j.reported) {
            j.reported = true;
            if (!j.removed) finished.push_back(j);  // taken out of the queue: nothing to report
        }
    idle = g_run.alive == 0;
    for (const Job& j : g_run.jobs) idle = idle && j.state == 2;
    mutexUnlock(&g_run.m);

    if ((queueChanged || !finished.empty()) && !g_exiting) saveQueueNow();  // finished / cancelled files leave it
    std::string cancelNote;
    if (!discardUrls.empty()) {
        uint64_t freed = 0;
        for (const std::string& u : discardUrls) freed += install::discardPendingUrl(u);
        updateSdFree(a);
        cancelNote = "已取消安裝，刪除已下載的部分，釋放了 " + screens::humanSize(freed) + "。";
        install::log("cancel: %zu file(s) discarded, %llu bytes freed", discardUrls.size(), (unsigned long long)freed);
    }
    if (!idle) {
        size_t waitingNow = 0;
        mutexLock(&g_run.m);
        for (const Job& j : g_run.jobs) waitingNow += j.state == 0;
        mutexUnlock(&g_run.m);
        autoStep(a, waitingNow);
        ensureWorkers(a);  // e.g. the parallel setting went up
    }
    for (const Job& j : finished)  // lost to the network after all retries: offer them again later
        if (!j.ok && !g_exiting && ((!j.skipped && retryable(j.err)) || j.retryWithBase))
            a.retryLater.push_back({j.url, j.name, j.group, j.added});
    boostKeep();
    for (const Job& j : finished) {
        std::string line = (j.ok ? "○ " : "× ") + shortName(j.name);
        a.recent.insert(a.recent.begin(), line);
        a.recentOk.insert(a.recentOk.begin(), j.ok);
        a.recentDetail.insert(a.recentDetail.begin(), plainReason(j.ok ? j.note : j.err));
        a.recentWarn.insert(a.recentWarn.begin(), j.warn);
        if (a.recent.size() > 30) { a.recent.pop_back(); a.recentOk.pop_back(); a.recentDetail.pop_back(); a.recentWarn.pop_back(); }
        if (j.ok) a.sessionOk++;
        else a.sessionFail++;
    }
    // What is installed has changed: rescan (at most every 3 s, a batch of DLC finishes quickly).
    static u64 lastScan = 0;
    static bool scanDue = false;
    if (!finished.empty()) scanDue = true;
    if (scanDue && (idle || nowMs() - lastScan > 3000)) {
        classifyAll(a);
        rebuildView(a);
        lastScan = nowMs();
        scanDue = false;
    }
    if (!finished.empty()) {
        const Job& last = finished.back();
        a.list.status = std::string(last.ok ? "安裝完成：" : "安裝失敗：") + shortName(last.name) + (last.ok ? "" : "（" + last.err + "）");
    }
    if (idle && g_installUiLocked) {
        appletEndBlockingHomeButtonShortAndLongPressed();
        appletSetAutoSleepDisabled(false);
        g_installUiLocked = false;
        boostStop();
        screenOn(a);  // the batch is done: show the result
        a.list.status = "全部安裝結束：成功 " + std::to_string(a.sessionOk) + " 個、失敗 " + std::to_string(a.sessionFail) + " 個。";
        if (a.sessionFail && !install::pendingInstalls().empty())
            a.list.status += "沒裝完的檔案已保留進度，再按一次安裝會接著下載。";
        rebuildView(a);
    }
    if (!cancelNote.empty()) a.list.status = cancelNote;
    // list badges follow the queue (排隊中 → 安裝中 → normal)
    if (!idle || !finished.empty()) {
        std::map<std::string, int> queued = queuedUrls();
        for (size_t v = 0; v < a.view.size() && v < a.list.items.size(); v++) {
            const App::Row& r = a.rows[a.rowOf[a.view[v]]];
            int q = 0;
            for (size_t m : r.members) {
                auto f = queued.find(a.files[m].url);
                if (f != queued.end()) q = std::max(q, f->second);
            }
            a.list.items[v].badge = q == 2 ? screens::Badge::Installing : q == 1 ? screens::Badge::Queued : rowBadge(r);
        }
    }
    // header pill
    if (idle) {
        a.list.installSummary.clear();
    } else {
        size_t running = 0, waiting = 0;
        std::set<u64> checking;
        double speed = 0;
        mutexLock(&g_run.m);
        for (const Job& j : g_run.jobs) {
            running += j.state == 1;
            waiting += j.state == 0;
            if (j.state == 0 && j.confirm) checking.insert(j.group);
        }
        for (int lane = 0; lane < kMaxLanes; lane++)
            if (g_run.lanes[lane].active) speed += g_run.lanes[lane].speedMBs;
        bool cancelling = g_run.cancel;
        mutexUnlock(&g_run.m);
        char b[96];
        if (cancelling) snprintf(b, sizeof(b), "正在取消…");
        else if (!checking.empty()) snprintf(b, sizeof(b), "安裝中 %zu · 確認內容中 %zu · %.1f MB/s", running, checking.size(), speed);
        else if (waiting) snprintf(b, sizeof(b), "安裝中 %zu · 排隊 %zu · %.1f MB/s", running, waiting, speed);
        else snprintf(b, sizeof(b), "安裝中 %zu · %.1f MB/s", running, speed);
        a.list.installSummary = b;
    }
}

// Games with waiting files, in install order (g_run.m must be locked).
static std::vector<u64> queueGroups() {
    std::vector<u64> keys;
    std::set<u64> seen;
    for (size_t k : waitingOrder(g_run))
        if (seen.insert(g_run.jobs[k].group).second) keys.push_back(g_run.jobs[k].group);
    return keys;
}

// Moves a waiting game: delta -1 = one up, +1 = one down, INT_MIN = to the top. Returns true if moved.
static bool moveQueuedGame(u64 g, int delta) {
    mutexLock(&g_run.m);
    std::vector<u64> keys = queueGroups();
    auto it = std::find(keys.begin(), keys.end(), g);
    bool moved = false;
    if (it != keys.end()) {
        size_t i = (size_t)(it - keys.begin());
        size_t to = delta == INT_MIN ? 0 : (delta < 0 ? (i ? i - 1 : 0) : std::min(keys.size() - 1, i + 1));
        if (to != i) {
            keys.erase(keys.begin() + (long)i);
            keys.insert(keys.begin() + (long)to, g);
            // renumber the waiting files game by game, keeping each game's own file order
            std::vector<size_t> order = waitingOrder(g_run);
            for (u64 key : keys)
                for (size_t k : order)
                    if (g_run.jobs[k].group == key) g_run.jobs[k].seq = ++g_seq;
            moved = true;
        }
    }
    mutexUnlock(&g_run.m);
    if (moved) saveQueueNow();
    return moved;
}

// Takes a game's waiting files out of the queue (a file already installing keeps going).
static size_t removeQueuedGame(u64 g) {
    size_t n = 0;
    mutexLock(&g_run.m);
    for (Job& j : g_run.jobs)
        if (j.group == g && j.state == 0) {
            j.state = 2;
            j.removed = j.skipped = true;
            j.err = "已從排隊移除";
            n++;
        }
    mutexUnlock(&g_run.m);
    saveQueueNow();
    return n;
}

// Clock + network for the header of every screen. Cheap: refreshed at most once a second.
// Returns true when what is shown changed.
static bool updateSysStatus() {
    static u64 last = 0;
    static screens::SysStatus shown;
    u64 now = armTicksToNs(armGetSystemTick()) / 1000000ULL;
    if (last && now - last < 1000) return false;
    last = now;
    screens::SysStatus st;
    u64 ts = 0;
    TimeCalendarTime ct;
    TimeCalendarAdditionalInfo info;
    if (R_SUCCEEDED(timeGetCurrentTime(TimeType_UserSystemClock, &ts)) &&
        R_SUCCEEDED(timeToCalendarTimeWithMyRule(ts, &ct, &info))) {
        char b[8];
        snprintf(b, sizeof(b), "%02d:%02d", ct.hour, ct.minute);
        st.clock = b;
    }
    NifmInternetConnectionType type;
    u32 strength = 0;
    NifmInternetConnectionStatus cs;
    if (R_FAILED(nifmGetInternetConnectionStatus(&type, &strength, &cs))) {
        st.net = -1;
    } else if (cs != NifmInternetConnectionStatus_Connected) {
        st.net = -1;
        st.connecting = true;
    } else {
        st.net = type == NifmInternetConnectionType_Ethernet ? 4 : (int)std::min<u32>(strength, 3);
    }
    bool changed = st.clock != shown.clock || st.net != shown.net || st.connecting != shown.connecting;
    shown = st;
    screens::setSysStatus(st);
    return changed;
}

static screens::ProgressState progressState(App& a) {
    screens::ProgressState ps;
    ps.background = true;
    mutexLock(&g_run.m);
    for (const Job& j : g_run.jobs) {
        if (j.removed) continue;
        ps.count++;
        if (j.state == 2) ps.finished++;
    }
    std::vector<size_t> order = waitingOrder(g_run);
    for (u64 g : queueGroups()) {
        screens::ProgressState::WaitGroup wg;
        std::string first, base;
        bool checking = false;
        for (size_t k : order) {
            const Job& j = g_run.jobs[k];
            if (j.group != g) continue;
            wg.files++;
            checking = checking || j.confirm;
            if (first.empty()) first = j.name;
        }
        for (const Job& j : g_run.jobs) {
            if (j.group != g) continue;
            if (j.state == 1) wg.started = true;
            std::string up = j.name;
            for (char& ch : up) ch = (char)toupper((unsigned char)ch);
            if (base.empty() && up.find("[BASE]") != std::string::npos) base = j.name;
        }
        wg.label = shortName(base.empty() ? first : base) + (checking ? " · 確認內容中…" : "");
        if (a.qSel == g) ps.groupCursor = (int)ps.groups.size();
        ps.groups.push_back(wg);
    }
    ps.groupScroll = a.qScroll;
    int lanes = lanesNow(a);
    for (int lane = 0; lane < kMaxLanes; lane++)
        if (lane < lanes || g_run.lanes[lane].active) ps.lanes.push_back(g_run.lanes[lane]);
    ps.cancelling = g_run.cancel;
    ps.discarding = g_run.discard;
    mutexUnlock(&g_run.m);
    ps.recent = a.recent;
    ps.recentOk = a.recentOk;
    ps.recentDetail = a.recentDetail;
    ps.recentWarn = a.recentWarn;
    ps.marqueeMs = nowMs() - a.progressSince;
    return ps;
}

// For the list badges: 2 = installing, 1 = waiting, 0 = not queued.
static std::map<std::string, int> queuedUrls() {
    std::map<std::string, int> m;
    mutexLock(&g_run.m);
    for (const Job& j : g_run.jobs)
        if (j.state != 2) m[j.url] = j.state == 1 ? 2 : 1;
    mutexUnlock(&g_run.m);
    return m;
}

// Big-button question. Returns the chosen option id (touch or button), or -1 when the app closes.
static int askChoice(App& a, const std::string& title, const std::vector<std::string>& lines,
                     const std::vector<screens::ChoiceOption>& opts) {
    std::vector<screens::HitBox> hits;
    present(a, [&](gfx::Canvas& c) { screens::drawChoice(c, a.font, title, lines, opts, hits); });
    bool touching = false;
    int tx = 0, ty = 0, sx = 0, sy = 0;
    while (appletMainLoop()) {
        u64 d = buttonsDown(a);
        for (const auto& o : opts) {
            u64 mask = o.key == "A" ? HidNpadButton_A : o.key == "B" ? HidNpadButton_B : o.key == "X" ? HidNpadButton_X
                     : o.key == "Y" ? HidNpadButton_Y : 0;
            if (d & mask) return o.id;
        }
        HidTouchScreenState ts = {0};
        if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
            tx = (int)ts.touches[0].x;
            ty = (int)ts.touches[0].y;
            if (!touching) { touching = true; sx = tx; sy = ty; }
        } else if (touching) {
            touching = false;
            if (std::abs(tx - sx) < 30 && std::abs(ty - sy) < 30)
                for (const auto& h : hits)
                    if (tx >= h.x && tx < h.x + h.w && ty >= h.y && ty < h.y + h.h) return h.id;
        }
        svcSleepThread(16000000);
    }
    return -1;
}


static bool isCompressed(const std::string& name) {
    std::string l = lower(name);
    return l.size() > 4 && (l.compare(l.size() - 4, 4, ".nsz") == 0 || l.compare(l.size() - 4, 4, ".xcz") == 0);
}

// ---- SD card space: checked when a game is selected and again before installing.
static const uint64_t kReserve = 1ULL << 30;  // left free for the system (saves, screenshots)
static void runManage(App& a);                 // below

// Bytes a server file takes once installed. NSP / XCI: the file size. NSZ / XCZ grow when unpacked:
// measured from their headers when `measure` (network, once per file, then remembered), else
// estimated at twice the file size.
static uint64_t fileInstallSize(App& a, size_t i, bool measure) {
    const remote::RemoteFile& f = a.files[i];
    if (f.onlyTitle) return f.size;  // one title of a merged package: its installed size is known
    if (!isCompressed(f.displayName)) return f.size;
    auto it = a.instSize.find(f.url);
    if (it != a.instSize.end()) return it->second;
    if (!measure) return f.size * 2;
    uint64_t total = 0;
    std::string err;
    std::string url = f.url;
    container::ReadFn rd = [&](uint64_t o, uint64_t n, std::vector<uint8_t>& out, std::string& e) {
        return http::getToMemory(url, o, n, out, e);
    };
    if (container::installSize(rd, total, err) && total) {
        a.instSize[f.url] = total;
        install::log("install size %s: %llu bytes (file %llu)", baseName(f.displayName).c_str(), (unsigned long long)total,
                     (unsigned long long)f.size);
        return total;
    }
    install::log("install size %s: not measured (%s), estimated", baseName(f.displayName).c_str(), err.c_str());
    return f.size * 2;
}

static uint64_t rowNeed(App& a, const App::Row& r, bool measure) {
    uint64_t n = 0;
    for (size_t i : r.needed) n += fileInstallSize(a, i, measure);
    return n;
}

// Bytes already on the SD card from unfinished downloads of these files (counted in the free space).
static uint64_t keptBytes(App& a, const std::vector<size_t>& files) {
    std::set<std::string> urls;
    for (size_t i : files) urls.insert(a.files[i].url);
    uint64_t k = 0;
    for (const auto& p : install::pendingInstalls())
        if (urls.count(p.url)) k += p.kept;
    return k;
}

struct Space {
    bool known = false;
    uint64_t free = 0;  // SD card free bytes
    uint64_t busy = 0;  // still needed by files installing / waiting
    uint64_t avail() const { return free > busy ? free - busy : 0; }
};

static Space spaceNow(App& a) {
    Space sp;
    sp.free = install::sdFreeBytes();
    sp.known = sp.free > 0;
    const std::map<std::string, size_t>& byUrl = a.urlIndex;
    mutexLock(&g_run.m);
    for (int l = 0; l < kMaxLanes; l++)
        if (g_run.lanes[l].active && g_run.lanes[l].total > g_run.lanes[l].done) sp.busy += g_run.lanes[l].total - g_run.lanes[l].done;
    std::vector<std::string> waiting;
    for (const Job& j : g_run.jobs)
        if (j.state == 0) waiting.push_back(j.url);
    mutexUnlock(&g_run.m);
    for (const auto& u : waiting) {
        auto f = byUrl.find(u);
        if (f != byUrl.end()) sp.busy += fileInstallSize(a, f->second, false);
    }
    return sp;
}

// Selected games other than `except` (file index of a row's base, -1 = none).
static uint64_t selectedNeed(App& a, size_t except) {
    uint64_t n = 0;
    for (const App::Row& r : a.rows)
        if (r.rep != except && a.selected[r.rep]) n += rowNeed(a, r, false);
    return n;
}

static void updateSelInfo(App& a) {
    a.list.selNeed = selectedNeed(a, (size_t)-1);
    Space sp = spaceNow(a);
    if (!sp.known) { a.list.selAvail = -1; return; }
    uint64_t used = a.list.selNeed + kReserve;
    a.list.selAvail = sp.avail() > used ? (int64_t)(sp.avail() - used) : 0;
}

// Selects the game whose base file is `rep` if it fits. `quiet`: no window (batch selection), the
// game's name goes to `skipped` instead. Returns true when it is selected.
static bool trySelect(App& a, size_t rep, bool quiet, std::vector<std::string>* skipped = nullptr) {
    auto ro = a.rowOf.find(rep);
    if (ro == a.rowOf.end()) { a.selected[rep] = true; return true; }
    const App::Row& r = a.rows[ro->second];
    bool measuring = false;
    for (size_t i : r.needed)
        if (isCompressed(a.files[i].displayName) && !a.instSize.count(a.files[i].url)) measuring = true;
    if (measuring && !quiet) showMessage(a, "正在計算安裝後大小…", {a.names[rep]}, "");
    uint64_t need = rowNeed(a, r, true);
    uint64_t kept = keptBytes(a, r.needed);
    need = need > kept ? need - kept : 0;
    Space sp = spaceNow(a);
    uint64_t others = selectedNeed(a, rep);
    if (!sp.known || need == 0 || others + need + kReserve <= sp.avail()) {
        a.selected[rep] = true;
        return true;
    }
    if (skipped) skipped->push_back(a.names[rep] + "（" + screens::humanSize(need) + "）");
    uint64_t shortBy = others + need + kReserve - sp.avail();
    a.list.status = "空間不足：選取「" + a.names[rep] + "」需要 " + screens::humanSize(need) + "，剩餘可用只有 " +
                    screens::humanSize(sp.avail() > others + kReserve ? sp.avail() - others - kReserve : 0) + "。";
    if (quiet) return false;
    size_t selCount = 0;
    for (const App::Row& o : a.rows) selCount += o.rep != rep && a.selected[o.rep];
    std::vector<std::string> lines = {"#" + a.names[rep] + "（約 " + screens::humanSize(need) + "）"};
    std::string used = "SD 卡剩餘 " + screens::humanSize(sp.free);
    if (selCount) used += "，已選取的 " + std::to_string(selCount) + " 款要用 " + screens::humanSize(others);
    if (sp.busy) used += "，安裝中／排隊中還要用 " + screens::humanSize(sp.busy);
    lines.push_back(used + "，");
    lines.push_back("再加上這款就超過了，所以沒有選取。");
    lines.push_back("!還差 " + screens::humanSize(shortBy) + "（另外保留 1 GB 給系統）。");
    lines.push_back(selCount ? "可以先取消一些已選的遊戲，或到 Y 選單「詳細管理遊戲內容」刪掉不玩的遊戲。"
                             : "可以到 Y 選單「詳細管理遊戲內容」刪掉不玩的遊戲騰出空間。");
    int c = askChoice(a, "SD 卡空間不夠", lines, {{"A", "知道了", kGo, 1}, {"Y", "詳細管理遊戲內容", kLater, 2}});
    if (c == 2) runManage(a);
    return false;
}

// Batch selection (一鍵選取 / 全選): in list order until the space runs out; one window at the end.
static void reportSkipped(App& a, const std::vector<std::string>& skipped, size_t picked) {
    if (skipped.empty()) return;
    updateSelInfo(a);
    std::vector<std::string> lines = {"#已選取 " + std::to_string(picked) + " 款，共 " + screens::humanSize(a.list.selNeed) +
                                      (a.list.selAvail >= 0 ? "（剩餘可用 " + screens::humanSize((uint64_t)a.list.selAvail) + "）" : "")};
    lines.push_back("下面這些再選就會超過剩餘空間：");
    for (size_t k = 0; k < skipped.size() && k < 6; k++) lines.push_back("!" + skipped[k]);
    if (skipped.size() > 6) lines.push_back("…還有 " + std::to_string(skipped.size() - 6) + " 款");
    lines.push_back("刪掉不玩的遊戲騰出空間後，再選一次就可以。");
    askChoice(a, "有 " + std::to_string(skipped.size()) + " 款放不下，沒有選取", lines, {{"A", "知道了", kGo, 1}});
}

static void runInstall(App& a) {
    // Each selected game brings what it needs: everything when the game is not installed yet
    // (base + newest update + DLC), otherwise only the missing update / DLC.
    std::vector<size_t> games;
    for (const App::Row& r : a.rows)
        if (a.selected[r.rep]) games.push_back(r.rep);
    if (games.empty() && a.list.cursor < a.view.size()) games.push_back(a.view[a.list.cursor]);
    if (games.empty()) return;
    // Files of these games (and files whose name has no TID) not looked into yet: checked in the
    // background, the games wait in the queue as "確認內容中" and get their final files afterwards.
    std::set<u64> apps;
    for (size_t g : games)
        if (a.rows[a.rowOf[g]].app) apps.insert(a.rows[a.rowOf[g]].app);
    ConfirmReq check;
    if (install::headerKey() && !apps.empty()) {
        mutexLock(&g_tid.m);
        for (const auto& f : a.raw) {
            std::string k = tidCacheKey(f);
            if (g_tid.known.count(k)) continue;
            uint64_t named = covers::titleIdFromName(f.displayName);
            if (named && !apps.count(covers::baseTitleId(named))) continue;
            check.items.push_back({k, f.url, baseName(f.displayName), f.size, named != 0, false});
        }
        mutexUnlock(&g_tid.m);
        check.apps = apps;
    }
    std::vector<size_t> queue;  // indices into a.files
    std::map<size_t, std::set<u64>> skips;
    for (size_t g : games) {
        const App::Row& r = a.rows[a.rowOf[g]];
        queue.insert(queue.end(), r.needed.begin(), r.needed.end());
        for (const auto& kv : r.skip) skips[kv.first].insert(kv.second.begin(), kv.second.end());
    }
    if (queue.empty()) {
        a.list.status = games.size() == 1 ? "這款遊戲的本體、更新、DLC 都已安裝。" : "選取的遊戲都已全部安裝。";
        return;
    }
    orderFiles(a, queue);  // per game base -> update -> DLC

    // Free space, measured exactly (compressed files by their headers) before anything starts.
    {
        uint64_t need = 0;
        bool measuring = false;
        for (size_t i : queue)
            if (isCompressed(a.files[i].displayName) && !a.instSize.count(a.files[i].url)) measuring = true;
        if (measuring) showMessage(a, "正在計算安裝後大小…", {"NSZ / XCZ 解壓縮後會變大，先讀檔頭算出實際需要的空間。"}, "");
        for (size_t i : queue) need += fileInstallSize(a, i, true);
        uint64_t kept = keptBytes(a, queue);
        need = need > kept ? need - kept : 0;
        Space sp = spaceNow(a);
        if (sp.known && need + kReserve > sp.avail()) {
            uint64_t shortBy = need + kReserve - sp.avail();
            std::vector<std::string> lines = {"#選取的遊戲安裝後需要 " + screens::humanSize(need),
                                              "SD 卡剩餘 " + screens::humanSize(sp.free) +
                                                  (sp.busy ? "，安裝中／排隊中的檔案還要用 " + screens::humanSize(sp.busy) : std::string()) + "。",
                                              "!還差 " + screens::humanSize(shortBy) + "（另外保留 1 GB 給系統），沒有開始安裝。",
                                              "可以少選幾款，或到 Y 選單「詳細管理遊戲內容」刪掉不玩的遊戲。"};
            int c = askChoice(a, "SD 卡空間不夠", lines, {{"A", "知道了", kGo, 1}, {"Y", "詳細管理遊戲內容", kLater, 2}});
            if (c == 2) runManage(a);
            updateSelInfo(a);
            return;
        }
    }

    std::vector<Job> jobs;
    for (size_t i : queue) {
        Job j;
        j.url = a.files[i].url;
        j.name = a.files[i].displayName;
        j.group = groupKey(a, i);
        auto sk = skips.find(i);
        if (sk != skips.end()) j.skipTitles = sk->second;
        j.confirm = !check.items.empty() && apps.count(j.group) > 0;
        jobs.push_back(j);
    }
    size_t added = enqueue(a, jobs);
    a.selected.assign(a.files.size(), false);
    rebuildView(a);
    if (added && !check.items.empty()) {
        install::log("check before install: %zu game(s), %zu file(s) to look into", check.apps.size(), check.items.size());
        startConfirm(check);
        a.list.status = "已加入排隊 " + std::to_string(check.apps.size()) + " 款：背景確認檔案內容中，確認完自動開始安裝（按 - 查看進度）。";
        return;
    }
    a.list.status = added ? "已加入安裝：" + std::to_string(added) + " 個檔案在背景安裝，可以繼續選擇其他遊戲（按 - 查看進度）。"
                          : "這些檔案已經在安裝佇列中。";
}

// A game checked in the background: its waiting "確認內容中" files are replaced by the final plan
// (same place in the queue). Nothing left to install / no room: said under 最近完成.
static void replanGame(App& a, u64 app) {
    const App::Row* row = nullptr;
    for (const App::Row& r : a.rows)
        if (r.app == app) { row = &r; break; }
    std::vector<size_t> queue;
    if (row) queue = row->needed;
    if (!queue.empty()) orderFiles(a, queue);
    // room for the final plan (the provisional files are still counted as waiting: leave them out)
    std::set<std::string> provisional;
    mutexLock(&g_run.m);
    for (const Job& j : g_run.jobs)
        if (j.state == 0 && j.confirm && j.group == app) provisional.insert(j.url);
    mutexUnlock(&g_run.m);
    if (provisional.empty()) return;  // taken out of the queue meanwhile
    std::string why;
    bool ok = !queue.empty();
    if (!ok) why = "確認後：主機上已是最新，不需要安裝。";
    else {
        uint64_t need = 0, before = 0;
        for (size_t i : queue) need += fileInstallSize(a, i, false);
        for (const auto& u : provisional) {
            auto f = a.urlIndex.find(u);
            if (f != a.urlIndex.end()) before += fileInstallSize(a, f->second, false);
        }
        uint64_t kept = keptBytes(a, queue);
        need = need > kept ? need - kept : 0;
        Space sp = spaceNow(a);
        uint64_t avail = sp.avail() + before;  // the provisional files are counted in sp.busy
        if (sp.known && need > before && need + kReserve > avail) {
            ok = false;
            why = "確認後需要 " + screens::humanSize(need) + "，SD 卡剩餘可用只有 " +
                  screens::humanSize(avail > kReserve ? avail - kReserve : 0) +
                  "，沒有開始安裝。可以到 Y 選單「詳細管理遊戲內容」騰出空間後再裝。";
        }
    }
    std::set<std::string> want;
    for (size_t i : queue) want.insert(a.files[i].url);
    mutexLock(&g_run.m);
    int64_t seq = INT64_MAX;
    std::string name;
    for (Job& j : g_run.jobs) {
        if (!(j.state == 0 && j.confirm && j.group == app)) continue;
        seq = std::min(seq, j.seq);
        if (name.empty() || titles::parse(j.name).kind == titles::Kind::Base) name = j.name;
        if (ok && want.count(j.url)) {  // same file in the final plan: stays where it is
            j.confirm = false;
            j.skipTitles.clear();
            continue;
        }
        j.state = 2;
        j.removed = true;  // replaced, not a result
    }
    if (!ok) {
        Job n;
        n.url = "check://" + std::to_string(app);
        n.name = name;
        n.group = app;
        n.state = 2;
        bool nothing = queue.empty();
        n.ok = nothing;
        n.skipped = !nothing;
        n.warn = !nothing;
        (nothing ? n.note : n.err) = why;
        g_run.jobs.push_back(n);
    } else {
        std::set<std::string> have;
        for (const Job& j : g_run.jobs)
            if (j.state != 2) have.insert(j.url);
        for (size_t i : queue) {
            const std::string& u = a.files[i].url;
            auto sk = row->skip.find(i);
            for (Job& j : g_run.jobs)  // a file kept from the provisional plan: its final skip list
                if (j.state == 0 && j.url == u && j.group == app && sk != row->skip.end()) j.skipTitles = sk->second;
            if (have.count(u)) continue;
            Job j;
            j.url = u;
            j.name = a.files[i].displayName;
            j.group = app;
            if (sk != row->skip.end()) j.skipTitles = sk->second;
            j.added = (int64_t)time(nullptr);
            j.seq = seq;  // where the game was in the queue
            g_run.jobs.push_back(j);
        }
    }
    mutexUnlock(&g_run.m);
    install::log("check before install %016llX: %s", (unsigned long long)app, ok ? "final files queued" : why.c_str());
    if (ok) ensureWorkers(a);
}

// Background checks that finished: the list learns what was read, the waiting games get their files.
static void pollConfirm(App& a) {
    std::vector<ConfirmReq> done;
    mutexLock(&g_confirm.m);
    done.swap(g_confirm.done);
    mutexUnlock(&g_confirm.m);
    if (done.empty()) return;
    mutexLock(&g_tid.m);
    a.tidGen = g_tid.structGen;
    mutexUnlock(&g_tid.m);
    reloadFiles(a, true);
    buildRows(a);  // the plan with everything read (rebuildView keeps only shown rows' order)
    for (const auto& r : done)
        for (u64 app : r.apps) replanGame(a, app);
    rebuildView(a);
    saveQueueNow();
}

// ------------------------------------------------------------------ menu (Y)
enum MenuPage { kMenuMain = 0, kMenuSort, kMenuServer, kMenuParallel, kMenuServerItem, kMenuSearch, kMenuView, kMenuShortcut };
// main menu rows
enum { kMainSort = 0, kMainView, kMainSearch, kMainPick, kMainAll, kMainManage, kMainRefresh, kMainParallel, kMainServers,
       kMainBoost, kMainCrash, kMainTitleDb, kMainShortcut, kMainCovers, kMainUpdate, kMainAbout };
static const char* kViewNames[] = {"清單", "封面牆", "精簡清單"};

static size_t installableCount(const App& a) {
    size_t n = 0;
    for (const App::Row& r : a.rows) n += r.baseInstalled && !r.card && (r.updateAvailable || r.dlcAvailable);
    return n;
}

static void cleanCrashReports(App& a);          // below
static void createShortcut(App& a, bool fromMenu);  // below
static void removeShortcut(App& a);                 // below
static std::string shortcutText(const App&) { return install::hasApplication(shortcut::kTitleId) ? "已建立" : "未建立"; }

// Menu value for "更新遊戲資料庫": when the data in use was downloaded.
static std::string titleDbText(const App& a) {
    if (!a.db) return "";
    if (a.db->busy()) return "下載中…";
    int64_t t = a.db->downloadedAt();
    if (t <= 0) return a.db->generation() ? "" : "尚未下載";
    time_t tt = (time_t)t;
    struct tm lt;
    if (!localtime_r(&tt, &lt)) return "";
    char b[32];
    snprintf(b, sizeof(b), "%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    return b;
}
static bool runIme(App& a, ime::Mode mode);       // below
static void checkForUpdate(App& a, bool asked);  // below

// ---- servers: test, add, edit, switch, delete. Every change is saved and used right away.
static std::string protoText(const std::string& url) {
    bool tls = url.compare(0, 8, "https://") == 0;
    if (!tls) return "HTTP（未加密）";
    std::string c = http::tlsCipher();
    if (c.find("CHACHA20") != std::string::npos) return "HTTPS（ChaCha20 加密）";
    if (c.find("AES") != std::string::npos) return "HTTPS（AES 加密）";
    return "HTTPS（加密）";
}

// Server test on its own thread: the screen counts the seconds, B gives up at once, 10 s at most.
// A thread given up on finishes by itself (its connection has its own time limits) and is joined
// before the next test.
struct ServerTest {
    std::string url;
    std::vector<remote::RemoteFile> files;
    remote::ServerKind kind = remote::ServerKind::None;
    std::string err;
    bool ok = false;
    std::atomic<bool> done{false};
};
static Thread g_testThread;
static bool g_testUsed = false;
static std::shared_ptr<ServerTest> g_testJob;
static void serverTestThread(void*) {
    std::shared_ptr<ServerTest> t = g_testJob;
    http::setThreadTimeout(8000);
    t->ok = remote::fetchList(t->url, t->files, t->kind, t->err);
    t->done = true;
}
static void joinServerTest() {
    if (!g_testUsed) return;
    threadWaitForExit(&g_testThread);
    threadClose(&g_testThread);
    g_testUsed = false;
}

// Logs in and reads the file list. Shows the result; returns 1 when it worked, 0 when not, 2 when
// the player chose to correct the address.
static int testServerEx(App& a, const servers::Server& s, bool offerEdit) {
    joinServerTest();
    auto job = std::make_shared<ServerTest>();
    job->url = servers::fullUrl(s);
    g_testJob = job;
    u64 t0 = nowMs();
    bool started = R_SUCCEEDED(threadCreate(&g_testThread, serverTestThread, nullptr, nullptr, 0x40000, 0x2D, bgCore(1))) &&
                   R_SUCCEEDED(threadStart(&g_testThread));
    if (started) g_testUsed = true;
    else {  // no thread: test here (still limited by the connection time limits)
        serverTestThread(nullptr);
    }
    bool cancelled = false, timedOut = false;
    int shown = -1;
    while (!job->done) {
        int sec = (int)((nowMs() - t0) / 1000);
        if (sec != shown) {
            shown = sec;
            showMessage(a, "正在測試連線…", {s.name, s.url, "已等 " + std::to_string(sec) + " 秒（最多 10 秒）"}, "B 取消");
        }
        if (buttonsDown(a) & HidNpadButton_B) { cancelled = true; break; }
        if (nowMs() - t0 > 10000) { timedOut = true; break; }
        if (!appletMainLoop()) { cancelled = true; break; }
        svcSleepThread(50000000ULL);
    }
    if (cancelled) {
        install::log("server test %s: cancelled", s.name.c_str());
        return 0;
    }
    char t[48];
    snprintf(t, sizeof(t), "讀取清單花了 %.2f 秒", (nowMs() - t0) / 1000.0);
    bool ok = !timedOut && job->ok;
    std::string err = timedOut ? "10 秒內沒有回應：位址或埠號可能不對，或伺服器沒開。" : job->err;
    install::log("server test %s: %s", s.name.c_str(), ok ? "ok" : err.c_str());
    if (ok) {
        askChoice(a, "測試連線：" + s.name,
                  {"#登入成功", protoText(s.url) + "  ·  " + remote::kindName(job->kind) + "  ·  " + std::to_string(job->files.size()) + " 個檔案", t},
                  {{"A", "確定", kGo, 1}});
        return 1;
    }
    std::vector<std::string> lines = {"!" + err};
    bool account = err.find("帳號") != std::string::npos || err.find("401") != std::string::npos;
    if (account) lines.push_back("請到「帳號」「密碼」重新輸入，改完會立刻再測一次。");
    else if (err.find("http://") == std::string::npos)
        lines.push_back("請確認：位址拼字、埠號（例如 8001）、開頭是 https:// 還是 http://，以及伺服器有沒有開。");
    if (offerEdit && !account) {
        int pick = askChoice(a, "測試連線：" + s.name, lines, {{"A", "修改位址", kGo, 2}, {"B", "關閉", kLater, 0}});
        return pick == 2 ? 2 : 0;
    }
    askChoice(a, "測試連線：" + s.name, lines, {{"A", "確定", kLater, 1}});
    return 0;
}
static bool testServer(App& a, const servers::Server& s) { return testServerEx(a, s, false) == 1; }

// Checks a typed address before anything is tried; false (after saying what is wrong) when it is not usable.
static bool addressOk(App& a, const std::string& typed, const std::string& after) {
    std::string why = servers::checkAddress(typed);
    std::string url, user, pass;
    servers::splitAddress(typed, url, user, pass);
    http::Url u;
    if (why.empty() && (url.empty() || !http::parseUrl(url, u))) why = "請用 http:// 或 https:// 開頭，例如 https://伺服器位址:5006/";
    if (why.empty()) return true;
    askChoice(a, "位址格式不對", {"!" + typed, why, after}, {{"A", "重新輸入", kLater, 1}});
    return false;
}

// Makes server `id` the one in use (asks first while installs run) and reads its list.
static void switchToServer(App& a, const std::string& id) {
    const servers::Server* s = a.store.find(id);
    if (!s || a.store.current == id) return;
    if (installsActive()) {
        size_t running = 0, waiting = 0;
        mutexLock(&g_run.m);
        for (const Job& j : g_run.jobs) { running += j.state == 1; waiting += j.state == 0; }
        mutexUnlock(&g_run.m);
        const servers::Server* cur = a.store.active();
        int pick = askChoice(a, "切換到「" + s->name + "」？",
                             {"正在安裝 " + std::to_string(running) + " 個、排隊 " + std::to_string(waiting) + " 個檔案：會繼續從「" +
                                  (cur ? cur->name : std::string("原本的伺服器")) + "」下載，不受影響。",
                              "清單會換成「" + s->name + "」的檔案，可以繼續選擇安裝。"},
                             {{"A", "切換", kGo, 1}, {"B", "取消", kLater, 0}});
        if (pick != 1) return;
    }
    a.store.current = id;
    saveServers(a);
    applyCurrentServer(a);
    install::log("server: switched to %s", a.list.serverName.c_str());
    refresh(a);
}

static void addServer(App& a) {
    servers::Server s;
    s.id = a.store.newId();
    std::string name = "伺服器 " + std::to_string(a.store.list.size() + 1);
    if (!askText(a, "新增伺服器（1 / 4）：名稱", "自己取一個名字，例如：家裡的伺服器", name, false, name, 40, true)) return;
    s.name = name.empty() ? "伺服器 " + std::to_string(a.store.list.size() + 1) : name;
    std::string addr = "https://";
    bool askedAccount = false;
    for (;;) {
        std::string typed;
        if (!askText(a, "新增伺服器（2 / 4）：位址", "例如 https://伺服器位址:5006/資料夾/ 或 http://192.168.1.10:8000/", addr,
                     false, typed))
            return;
        addr = typed;
        if (!addressOk(a, addr, "")) continue;  // wrong format: said at once, typed again
        std::string user, pass;
        servers::splitAddress(addr, s.url, user, pass);
        if (!user.empty()) { s.user = user; s.pass = pass; }
        if (!askedAccount) {
            askedAccount = true;
            if (s.user.empty() && !askText(a, "新增伺服器（3 / 4）：帳號", "伺服器的登入帳號，沒有就留空", "", false, s.user, 128)) return;
            if (s.pass.empty() && !s.user.empty() &&
                !askText(a, "新增伺服器（4 / 4）：密碼", "伺服器的登入密碼", "", true, s.pass, 128))
                return;
        }
        int t = testServerEx(a, s, true);
        if (t == 2) continue;  // correct the address
        if (t == 1) break;
        int pick = askChoice(a, "仍要儲存「" + s.name + "」嗎？", {"連線測試沒有成功。之後可以在伺服器設定裡修改位址、帳號、密碼。"},
                             {{"A", "仍然儲存", kGo, 1}, {"Y", "修改位址", kLater, 2}, {"B", "不儲存", kLater, 0}});
        if (pick == 2) continue;
        if (pick != 1) return;
        break;
    }
    a.store.list.push_back(s);
    saveServers(a);
    install::log("server: added %s", s.name.c_str());
    if (a.store.current.empty()) {
        a.store.current = s.id;
        saveServers(a);
        applyCurrentServer(a);
        refresh(a);
    } else {
        switchToServer(a, s.id);
    }
}

// field: 0 name, 1 address, 2 account, 3 password
static void editServer(App& a, int idx, int field) {
    if (idx < 0 || idx >= (int)a.store.list.size()) return;
    servers::Server s = a.store.list[(size_t)idx];
    std::string v;
    switch (field) {
        case 0:
            if (!askText(a, "名稱", "這台伺服器在選單和標題上顯示的名字", s.name, false, v, 40, true) || v.empty()) return;
            s.name = v;
            break;
        case 1: {
            std::string start = s.url;
            for (;;) {
                if (!askText(a, "位址", "例如 https://伺服器位址:5006/資料夾/", start, false, v) || v.empty()) return;
                if (addressOk(a, v, "還沒儲存，原本的位址不變。")) break;
                start = v;
            }
            std::string url, user, pass;
            servers::splitAddress(v, url, user, pass);
            s.url = url;
            if (!user.empty()) { s.user = user; s.pass = pass; }  // typed as https://帳號:密碼@…
            break;
        }
        case 2:
            if (!askText(a, "帳號", "伺服器的登入帳號，沒有就留空", s.user, false, v, 128)) return;
            s.user = v;
            break;
        default:
            if (!askText(a, "密碼", "伺服器的登入密碼，沒有就留空", "", true, v, 128)) return;
            s.pass = v;
            break;
    }
    a.store.list[(size_t)idx] = s;  // saved right away
    saveServers(a);
    bool active = a.store.current == s.id;
    if (active) applyCurrentServer(a);
    if (field == 0) return;
    // address / account / password changed: test now, and reload the list when it is the one in use
    if (testServerEx(a, s, true) == 2) { editServer(a, idx, 1); return; }
    if (active) refresh(a);
}

static void deleteServer(App& a, int idx) {
    if (idx < 0 || idx >= (int)a.store.list.size()) return;
    servers::Server s = a.store.list[(size_t)idx];
    int pick = askChoice(a, "刪除伺服器「" + s.name + "」？",
                         {"只會刪除這台的設定（名稱、位址、帳號密碼），不會動到伺服器上的任何檔案。"},
                         {{"A", "刪除", kStop, 1}, {"B", "取消", kLater, 0}});
    if (pick != 1) return;
    bool wasActive = a.store.current == s.id;
    a.store.remove(s.id);
    saveServers(a);
    install::log("server: deleted %s", s.name.c_str());
    if (wasActive) {
        applyCurrentServer(a);
        refresh(a);
    }
}

// ---- 詳細管理遊戲內容: what is installed on the console, and deleting parts of it. Only console
// content is touched (never the server's files); deleting goes through ncm so other games'
// unfinished downloads stay.
struct ManageGame {
    install::InstalledGame g;
    std::string name;
    bool onNas = false;
    std::vector<std::string> labels;  // per piece
};

// Names and display versions read from the console are kept on the SD card, so the screen opens
// at once the next time: "APPID<TAB>version<TAB>display version<TAB>name" (version = highest
// base / update version, so an update or a reinstall reads it again).
static const char* kManageNames = "sdmc:/config/lan-installer/manage-names.txt";
struct ManageCtx {
    std::set<u64> onServer;
    std::map<u64, size_t> fileOf;  // title id -> a server file (names for DLC)
    std::map<u64, std::pair<u32, std::pair<std::string, std::string>>> cache;  // app -> version, (display version, name)
    bool dirty = false;
};

// Games the server has a file of: by the TID in the file names and by the real TIDs read so far.
static std::set<u64> nasBaseIds(App& a) {
    std::set<u64> ids;
    for (size_t i = 0; i < a.info.size(); i++)
        if (a.info[i].baseId) ids.insert(a.info[i].baseId);
    mutexLock(&g_tid.m);
    for (size_t i = 0; i < a.files.size(); i++) {
        if (a.files[i].onlyTitle) { ids.insert(covers::baseTitleId(a.files[i].onlyTitle)); continue; }
        auto it = g_tid.known.find(tidCacheKey(a.files[i]));
        if (it != g_tid.known.end())
            for (const auto& m : it->second.items) ids.insert(covers::baseTitleId(m.titleId));
    }
    mutexUnlock(&g_tid.m);
    return ids;
}

static ManageCtx manageCtx(App& a) {
    ManageCtx c;
    c.onServer = nasBaseIds(a);
    for (size_t i = 0; i < a.info.size(); i++)
        if (a.info[i].titleId && !c.fileOf.count(a.info[i].titleId)) c.fileOf[a.info[i].titleId] = i;
    if (FILE* f = fopen(kManageNames, "r")) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            std::string l(line);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            size_t t1 = l.find('\t'), t2 = t1 == std::string::npos ? t1 : l.find('\t', t1 + 1),
                   t3 = t2 == std::string::npos ? t2 : l.find('\t', t2 + 1);
            if (t3 == std::string::npos) continue;
            u64 id = strtoull(l.substr(0, t1).c_str(), nullptr, 16);
            u32 ver = (u32)strtoul(l.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 10);
            if (id) c.cache[id] = {ver, {l.substr(t2 + 1, t3 - t2 - 1), l.substr(t3 + 1)}};
        }
        fclose(f);
    }
    return c;
}

static void saveManageNames(ManageCtx& c) {
    if (!c.dirty) return;
    if (FILE* f = fopen(kManageNames, "w")) {
        for (const auto& kv : c.cache)
            fprintf(f, "%016llX\t%u\t%s\t%s\n", (unsigned long long)kv.first, kv.second.first, kv.second.second.first.c_str(),
                    kv.second.second.second.c_str());
        fclose(f);
    }
    c.dirty = false;
}

// Name, "NAS 上沒有" and the label of each piece of one game (`m.g` already set).
static void fillManageGame(App& a, ManageCtx& c, ManageGame& m) {
    const install::InstalledGame& g = m.g;
    u32 key = 0, newestPatch = 0;
    for (const auto& p : g.pieces) {
        if (p.type == NcmContentMetaType_Patch) newestPatch = std::max(newestPatch, p.version);
        if (p.type != NcmContentMetaType_AddOnContent) key = std::max(key, p.version);
    }
    std::string disp;
    auto hit = c.cache.find(g.appId);
    if (hit != c.cache.end() && hit->second.first == key && !hit->second.second.second.empty()) {
        disp = hit->second.second.first;
        m.name = hit->second.second.second;
    } else {
        install::installedControl(g.appId, m.name, disp);
        auto clean = [](std::string v) {
            for (char& ch : v) if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
            return v;
        };
        if (!m.name.empty()) {
            c.cache[g.appId] = {key, {clean(disp), clean(m.name)}};
            c.dirty = true;
        }
    }
    auto bf = c.fileOf.find(g.appId);
    if (m.name.empty() && bf != c.fileOf.end() && bf->second < a.names.size()) m.name = a.names[bf->second];
    if (m.name.empty()) m.name = badfile::tidKey(g.appId);
    m.onNas = c.onServer.count(g.appId) > 0;
    m.labels.clear();
    for (const auto& p : g.pieces) {
        std::string l;
        if (p.type == NcmContentMetaType_Application) {
            l = "本體";
        } else if (p.type == NcmContentMetaType_Patch) {
            if (p.version == newestPatch) l = disp.empty() ? "更新 [v" + std::to_string(p.version) + "]" : "更新 v" + disp;
            else l = "舊版更新 [v" + std::to_string(p.version) + "]";
        } else {
            auto f = c.fileOf.find(p.id);
            std::string title, details;
            if (f != c.fileOf.end()) screens::splitName(a.files[f->second].displayName, title, details);
            l = title.empty() ? "DLC " + std::to_string(p.id & 0xFFF) : "DLC " + title;
        }
        if (p.storage == NcmStorageId_BuiltInUser) l += "（主機記憶體）";
        m.labels.push_back(l);
    }
}

static std::vector<ManageGame> loadManageGames(App& a, ManageCtx& c) {
    std::vector<ManageGame> out;
    std::set<u64> present;
    for (auto& g : install::listInstalledGames()) {
        ManageGame m;
        m.g = g;
        fillManageGame(a, c, m);
        out.push_back(m);
        present.insert(g.appId);
    }
    for (auto it = c.cache.begin(); it != c.cache.end();) {  // games deleted since: forget their names
        if (present.count(it->first)) { ++it; continue; }
        it = c.cache.erase(it);
        c.dirty = true;
    }
    saveManageNames(c);
    return out;
}

// NAS check report (see badfile.hpp), once the real TIDs of the whole server are known: games not
// on the NAS, and files whose name carries a wrong TID. One note per opening; each game / file at
// most twice. Sent from a background thread.
static const char* kMissingReported = "sdmc:/config/lan-installer/reported-missing.txt";

static void reportNasCheck(App& a, const std::vector<ManageGame>& games) {
    // only against the author's server, and only with its list loaded (else every game looks missing)
    if (a.files.empty() || !badfile::fromReportServer(a.list.server)) return;
    std::map<std::string, int> done = badfile::reportedCounts(kMissingReported);
    std::vector<std::string> back;
    std::vector<std::pair<uint64_t, std::string>> missing;
    std::vector<std::string> missKeys, nameKeys;
    for (const auto& m : games) {
        if (!badfile::isOfficialTitle(m.g.appId)) continue;  // homebrew / forwarders
        std::string k = badfile::tidKey(m.g.appId);
        auto d = done.find(k);
        int times = d == done.end() ? 0 : d->second;
        if (m.onNas && times) back.push_back(k);  // on the NAS now: counts again from zero
        else if (!m.onNas && times < badfile::kMaxReports) { missing.push_back({m.g.appId, m.name}); missKeys.push_back(k); }
    }
    if (!back.empty()) badfile::clearReported(kMissingReported, back);
    std::vector<std::string> wrong;
    for (size_t i = 0; i < a.files.size(); i++) {
        uint64_t named = covers::titleIdFromName(a.files[i].displayName);
        std::vector<uint64_t> ids = realIds(a, i);
        if (!named || ids.empty() || std::find(ids.begin(), ids.end(), named) != ids.end()) continue;
        std::string file = badfile::fileName(a.files[i].url);
        std::string key = badfile::fileKey(file, badfile::kKindTidName);
        if (badfile::reportedTimes(kReportedPath, key) >= badfile::kMaxReports) continue;
        wrong.push_back(file + "：檔名 TID " + badfile::tidKey(named) + "，實際 " + badfile::tidKey(ids[0]));
        nameKeys.push_back(key);
    }
    static bool askedThisRun = false;  // one question per start (at start-up or in the manage screen)
    if (askedThisRun) { missing.clear(); missKeys.clear(); }
    if (!missing.empty()) {
        askedThisRun = true;
        // the player decides: asked at most kMaxReports times per game. Every question counts (A or
        // B): a report that cannot be sent stays in the outbox and goes out later, not asked again.
        badfile::bumpReported(kMissingReported, missKeys);
        std::vector<std::string> lines;
        lines.push_back("主機上有這些遊戲，但伺服器上找不到：");
        for (size_t k = 0; k < missing.size() && k < 8; k++)
            lines.push_back("· " + missing[k].second + "  [" + badfile::tidKey(missing[k].first) + "]");
        if (missing.size() > 8) lines.push_back("…還有 " + std::to_string(missing.size() - 8) + " 款");
        lines.push_back("要回報給作者嗎？只會傳遊戲名稱和 TID");
        int c = askChoice(a, "伺服器上沒有的遊戲", lines, {{"A", "回報", kGo, 1}, {"B", "不回報", kLater, 2}});
        if (c != 1) {
            install::log("server check: %zu missing game(s), player chose not to report", missing.size());
            missing.clear();
        } else {
            a.list.status = "正在回報給作者…";
        }
    }
    if (missing.empty() && wrong.empty()) return;
    badfile::bumpReported(kReportedPath, nameKeys);  // wrong TIDs in names: sent without asking, counted now
    outboxAdd(badfile::ntfyNasCheckJsons(badfile::kNtfyTopic, missing, wrong),
              "server check report: " + std::to_string(missing.size()) + " game(s), " + std::to_string(wrong.size()) +
                  " file name(s)",
              !missing.empty());
}

// After start: once every file of the server has been looked at, the installed games are compared
// with it (no need to open the manage screen) and, for games the NAS lacks, the player is asked
// whether to report them. Only on the main list with nothing else open; once per start.
static bool g_autoNasDone = false;
static void autoNasCheck(App& a) {
    if (g_autoNasDone) return;
    if (a.raw.empty() || !tidScanDone()) return;
    g_autoNasDone = true;
    if (!badfile::fromReportServer(a.list.server) || !a.installedOk) return;
    u64 tCheck = nowMs();
    std::set<u64> onServer = nasBaseIds(a);
    ManageCtx ctx = manageCtx(a);
    std::vector<ManageGame> games;
    for (const auto& kv : a.installed) {
        u64 app = kv.first;
        if ((app & 0xFFF) != 0 || app == shortcut::kTitleId || !badfile::isOfficialTitle(app)) continue;
        ManageGame m;
        m.g.appId = app;
        m.onNas = onServer.count(app) > 0;
        if (!m.onNas) {
            auto c = ctx.cache.find(app);
            if (c != ctx.cache.end()) m.name = c->second.second.second;
            titledb::Entry e;
            if (m.name.empty() && a.db && a.db->find(app, e)) m.name = e.name;
            std::string dv;
            if (m.name.empty()) install::installedControl(app, m.name, dv);
        }
        games.push_back(m);
    }
    install::log("server check at start: %zu installed game(s) compared (%llu ms)", games.size(),
                 (unsigned long long)(nowMs() - tCheck));
    reportNasCheck(a, games);
}

// Games with an update or DLC on the console but no base: they cannot start (HOME shows an error).
// Asked at every start and when 詳細管理遊戲內容 opens, until the base is there or the rest deleted.
static void checkMissingBase(App& a, bool justRead = false) {
    titles::Installed inst;
    if (justRead && a.installedOk) inst = a.installed;  // at start: just read while connecting, not again
    else if (!install::scanInstalled(inst)) return;
    struct Orphan {
        u64 app = 0;
        bool update = false;
        size_t dlc = 0;
        int file = -1;  // the server's base file, -1 = none
    };
    std::map<u64, Orphan> orphans;
    for (const auto& kv : inst) {
        if ((kv.first & 0xFFF) == 0) continue;
        u64 app = covers::baseTitleId(kv.first);
        if (app == shortcut::kTitleId || inst.count(app)) continue;
        Orphan& o = orphans[app];
        o.app = app;
        if ((kv.first & 0xFFF) == 0x800) o.update = true;
        else o.dlc++;
    }
    {   // the base is installing / waiting already: nothing to ask
        mutexLock(&g_run.m);
        for (const Job& j : g_run.jobs)
            if (j.state != 2) orphans.erase(titles::parse(j.name).baseId);
        mutexUnlock(&g_run.m);
    }
    if (orphans.empty()) return;
    for (size_t i = 0; i < a.files.size(); i++) {
        auto consider = [&](u64 app) {
            auto it = orphans.find(app);
            if (it != orphans.end() && it->second.file < 0) it->second.file = (int)i;
        };
        if (a.info[i].kind == titles::Kind::Base) consider(a.info[i].titleId);
        for (uint64_t t : realIds(a, i))
            if ((t & 0xFFF) == 0) consider(t);
    }
    std::vector<std::string> lines = {"主機上只裝了更新或 DLC，沒有遊戲本體，從 HOME 點這款遊戲會出現錯誤："};
    std::vector<size_t> install;
    bool someMissing = false;
    size_t shown = 0;
    for (const auto& kv : orphans) {
        const Orphan& o = kv.second;
        std::string name, disp;
        install::installedControl(o.app, name, disp);
        if (name.empty() && o.file >= 0) name = a.names[(size_t)o.file];
        if (name.empty()) name = badfile::tidKey(o.app);
        std::string what = o.update ? "更新" : "";
        if (o.dlc) what += (what.empty() ? "" : "、") + std::string("DLC ") + std::to_string(o.dlc) + " 個";
        if (o.file >= 0) install.push_back((size_t)o.file);
        else someMissing = true;
        if (shown++ < 6)
            lines.push_back(std::string(o.file >= 0 ? "#" : "!") + name + "（只有" + what + "）" +
                            (o.file >= 0 ? "" : "：伺服器上沒有本體檔，無法自動安裝"));
    }
    if (orphans.size() > 6) lines.push_back("…還有 " + std::to_string(orphans.size() - 6) + " 款");
    if (someMissing) lines.push_back("伺服器上沒有本體的，可以到 Y 選單「詳細管理遊戲內容」刪掉剩下的更新／DLC。");
    install::log("missing base: %zu game(s), %zu installable from the server", orphans.size(), install.size());
    std::string title = "有 " + std::to_string(orphans.size()) + " 款遊戲沒有本體，無法啟動";
    if (install.empty()) {
        askChoice(a, title, lines, {{"A", "知道了", kGo, 1}});
        return;
    }
    lines.push_back("按「安裝本體」只會裝本體，已經裝好的更新／DLC 不會重裝。");
    int c = askChoice(a, title, lines, {{"A", "安裝本體", kGo, 1}, {"B", "稍後", kLater, 0}});
    if (c != 1) return;
    uint64_t need = 0;
    for (size_t i : install) need += fileInstallSize(a, i, true);
    Space sp = spaceNow(a);
    if (sp.known && need + kReserve > sp.avail()) {
        askChoice(a, "SD 卡空間不夠",
                  {"#本體安裝後需要 " + screens::humanSize(need) + "，SD 卡剩餘可用 " + screens::humanSize(sp.avail()) + "。",
                   "!還差 " + screens::humanSize(need + kReserve - sp.avail()) + "（另外保留 1 GB 給系統），沒有開始安裝。",
                   "可以到 Y 選單「詳細管理遊戲內容」刪掉不玩的遊戲後再試。"},
                  {{"A", "知道了", kGo, 1}});
        return;
    }
    std::vector<Job> jobs;
    for (size_t i : install) {
        Job j;
        j.url = a.files[i].url;
        j.name = a.files[i].displayName;
        j.group = groupKey(a, i);
        jobs.push_back(j);
    }
    size_t added = enqueue(a, jobs);
    a.list.status = "已加入安裝：" + std::to_string(added) + " 款遊戲的本體（背景安裝中，按 - 查看進度）。";
}

// `checking`: the real TIDs of the server are still being read ("比對中…" instead of "NAS 上沒有").
static screens::ManageRow manageRow(const ManageGame& m, bool checking) {
    screens::ManageRow r;
    r.name = m.name;
    r.size = m.g.total;
    r.notOnNas = !m.onNas;
    r.checking = !m.onNas && checking;
    r.titleId = m.g.appId;
    size_t dlc = 0;
    uint64_t dlcBytes = 0;
    std::string upd;
    for (size_t k = 0; k < m.g.pieces.size(); k++) {
        const auto& p = m.g.pieces[k];
        if (p.type == NcmContentMetaType_AddOnContent) { dlc++; dlcBytes += p.size; continue; }
        r.parts.push_back({m.labels[k], p.size});
        if (p.type == NcmContentMetaType_Patch && m.labels[k].compare(0, 6, "更新") == 0) upd = m.labels[k];
    }
    if (dlc) r.parts.push_back({"DLC " + std::to_string(dlc) + " 個", dlcBytes});
    bool base = false;
    for (const auto& p : m.g.pieces) base = base || p.type == NcmContentMetaType_Application;
    r.contents = base ? "本體" : "（沒有本體）";
    r.noBase = !base;
    if (!upd.empty()) r.contents += " · " + upd;
    if (dlc) r.contents += " · DLC " + std::to_string(dlc) + " 個";
    return r;
}

// Is a file of this game installing or waiting right now?
static bool gameBusy(u64 appId) {
    bool busy = false;
    mutexLock(&g_run.m);
    for (const Job& j : g_run.jobs)
        if (j.state != 2 && titles::parse(j.name).baseId == appId) busy = true;
    mutexUnlock(&g_run.m);
    return busy;
}

static void runManage(App& a) {
    if (tidScanPaused()) startTidScan(a);  // the NAS was not answering: try again
    showMessage(a, "正在讀取主機上的遊戲…", {"整理每款遊戲的本體、更新、DLC 和佔用空間。"}, "");
    // faster CPU while reading / deleting (back to normal afterwards unless installs need it)
    auto boostOn = [&]() { bool was = g_boostOn; if (!was) boostStart(a); return !was; };
    auto boostOff = [&](bool mine) { if (mine && !installsActive()) boostStop(); };
    bool boosted = boostOn();
    ManageCtx ctx = manageCtx(a);
    std::vector<ManageGame> games = loadManageGames(a, ctx);
    boostOff(boosted);
    bool nasReported = false;
    if (tidScanDone()) { reportNasCheck(a, games); nasReported = true; }
    uint32_t tidSeen = 0;
    mutexLock(&g_tid.m);
    tidSeen = g_tid.gen;
    mutexUnlock(&g_tid.m);
    screens::ManageState ms;
    ms.version = APP_VERSION;
    bool byName = false, changedAny = false;
    std::vector<size_t> order;  // rows -> games
    std::vector<bool> ticked;   // menu: per piece of the open game
    size_t openGame = 0;
    auto rebuild = [&](u64 keepId) {
        order.clear();
        for (size_t k = 0; k < games.size(); k++) order.push_back(k);
        std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
            if (byName) return tsconv::searchKey(games[x].name) < tsconv::searchKey(games[y].name);
            return games[x].g.total > games[y].g.total;
        });
        ms.rows.clear();
        ms.total = 0;
        bool checking = !tidScanDone();
        for (size_t k : order) {
            ms.rows.push_back(manageRow(games[k], checking));
            ms.total += games[k].g.total;
            if (games[k].g.appId == keepId) ms.cursor = ms.rows.size() - 1;
        }
        ms.sortLabel = byName ? "名稱" : "佔用空間";
        ms.sdFree = install::sdFreeBytes();
    };
    rebuild(0);
    ms.status = "按 A 選擇要刪除的內容（本體、更新、DLC 都可以單獨刪除）。伺服器上的檔案不會動。";
    checkMissingBase(a);  // games with an update / DLC but no base: offer to install the base

    std::shared_ptr<const covers::Image> coverImg;
    auto updateManageCover = [&]() {
        screens::CoverView v;
        coverImg.reset();
        if (!ms.rows.empty() && a.covers) {
            v.titleId = ms.rows[ms.cursor].titleId;
            a.covers->request(v.titleId);
            switch (a.covers->get(v.titleId, coverImg)) {
                case covers::State::Ready: v.state = screens::CoverState::Ready; break;
                case covers::State::Missing: v.state = screens::CoverState::Missing; break;
                default: v.state = screens::CoverState::Loading; break;
            }
            if (coverImg) { v.w = coverImg->w; v.h = coverImg->h; v.px = coverImg->px.data(); }
        }
        bool changed = v.state != ms.cover.state || v.titleId != ms.cover.titleId || v.px != ms.cover.px;
        ms.cover = v;
        return changed;
    };

    auto openPieces = [&]() {
        if (ms.rows.empty()) return;
        openGame = order[ms.cursor];
        const ManageGame& m = games[openGame];
        if (gameBusy(m.g.appId)) {
            askChoice(a, "這款遊戲正在安裝", {"#" + m.name, "正在安裝或排隊中的遊戲不能刪除，請等安裝完成（或取消）後再來。"},
                      {{"A", "確定", kGo, 1}});
            return;
        }
        ticked.assign(m.g.pieces.size(), false);
        ms.menu.open = true;
        ms.menu.child = true;
        ms.menu.title = "管理：" + m.name;
        ms.menu.cursor = 0;
        ms.menu.scroll = 0;
    };
    auto fillPieces = [&]() {  // menu rows from `ticked`
        const ManageGame& m = games[openGame];
        size_t cur = ms.menu.cursor;
        ms.menu.items.clear();
        uint64_t freed = 0;
        bool base = false;
        for (size_t k = 0; k < m.g.pieces.size(); k++)
            if (ticked[k] && m.g.pieces[k].type == NcmContentMetaType_Application) base = true;
        for (size_t k = 0; k < m.g.pieces.size(); k++) {
            screens::MenuItem it{m.labels[k], screens::humanSize(m.g.pieces[k].size)};
            it.checked = ticked[k] || base;  // the base takes everything with it
            if (it.checked) freed += m.g.pieces[k].size;
            ms.menu.items.push_back(it);
        }
        screens::MenuItem del{"刪除勾選的項目", freed ? "可釋放 " + screens::humanSize(freed) : "請先勾選"};
        del.enabled = freed > 0;
        ms.menu.items.push_back(del);
        ms.menu.cursor = std::min(cur, ms.menu.items.size() - 1);
    };
    auto deleteTicked = [&]() {
        const ManageGame& m = games[openGame];
        bool base = false;
        for (size_t k = 0; k < m.g.pieces.size(); k++)
            if (ticked[k] && m.g.pieces[k].type == NcmContentMetaType_Application) base = true;
        std::vector<install::ContentPiece> pick;
        std::vector<std::string> what;
        uint64_t bytes = 0;
        for (size_t k = 0; k < m.g.pieces.size(); k++) {
            if (!ticked[k] && !base) continue;
            pick.push_back(m.g.pieces[k]);
            what.push_back(m.labels[k] + "（" + screens::humanSize(m.g.pieces[k].size) + "）");
            bytes += m.g.pieces[k].size;
        }
        if (pick.empty()) return;
        bool dropSave = false;
        if (base) {
            std::string list = m.name + "：";
            for (size_t k = 0; k < what.size() && k < 4; k++) list += (k ? "、" : "") + m.labels[k];
            if (what.size() > 4) list += "…等 " + std::to_string(what.size()) + " 項";
            int c = askChoice(a, "刪除整款遊戲？",
                              {"#" + list, "可釋放 " + screens::humanSize(bytes) + "。伺服器上的檔案不會受影響，之後可以再裝回來。",
                               "!要一起刪除這款遊戲的存檔嗎？刪除後無法復原。"},
                              {{"A", "刪除，保留存檔", kGo, 1}, {"X", "連存檔一起刪除", kStop, 2}, {"B", "取消", kLater, 0}});
            if (c != 1 && c != 2) return;
            dropSave = c == 2;
        } else {
            std::vector<std::string> lines = {"#" + m.name};
            for (size_t k = 0; k < what.size() && k < 6; k++) lines.push_back(what[k]);
            if (what.size() > 6) lines.push_back("…還有 " + std::to_string(what.size() - 6) + " 項");
            lines.push_back("可釋放 " + screens::humanSize(bytes) + "。存檔會保留，伺服器上的檔案不會受影響，之後可以再裝回來。");
            int c = askChoice(a, "刪除這些內容？", lines, {{"A", "刪除", kStop, 1}, {"B", "取消", kLater, 0}});
            if (c != 1) return;
        }
        const std::string gname = m.name;  // `games` changes below
        showMessage(a, "正在刪除…", {gname}, "");
        bool boostedNow = boostOn();
        uint64_t freed = 0;
        std::string err;
        u64 app = m.g.appId;
        bool ok = install::deletePieces(app, pick, freed, err);
        int saves = dropSave ? install::deleteSaveData(app) : 0;
        // only this game is read again
        {
            install::InstalledGame now;
            bool left = install::listInstalledGame(app, now);
            for (size_t k = 0; k < games.size(); k++) {
                if (games[k].g.appId != app) continue;
                if (left) {
                    games[k].g = now;
                    fillManageGame(a, ctx, games[k]);
                    saveManageNames(ctx);
                } else {
                    games.erase(games.begin() + (long)k);
                }
                break;
            }
        }
        boostOff(boostedNow);
        install::log("manage: %s %016llX, %zu item(s), %llu bytes, saves %d%s", base ? "deleted game" : "deleted parts",
                     (unsigned long long)app, pick.size(), (unsigned long long)freed, saves, ok ? "" : (" — " + err).c_str());
        changedAny = true;
        ms.menu.open = false;
        rebuild(app);
        if (ok)
            ms.status = "已刪除" + std::string(base ? "「" + gname + "」" : "所選內容") + "，釋放了 " + screens::humanSize(freed) +
                        (base ? (dropSave ? "，存檔也已刪除。" : "，存檔已保留。") : "。");
        else
            ms.status = "刪除時發生問題：" + err;
        updateSdFree(a);
    };

    auto draw = [&]() {
        updateManageCover();
        ms.marqueeMs = nowMs() - a.lastMove;
        present(a, [&](gfx::Canvas& c) { screens::drawManage(c, a.font, ms); });
    };
    a.lastMove = nowMs();
    draw();
    u64 holdKey = 0, holdStart = 0, lastRepeat = 0;
    bool touching = false;
    int tx = 0, ty = 0, sx = 0, sy = 0;
    while (appletMainLoop()) {
        pollInstalls(a);
        padUpdate(&a.pad);
        u64 down = padGetButtonsDown(&a.pad), held = padGetButtons(&a.pad);
        u64 now = nowMs();
        bool redraw = false;
        {   // real TIDs arriving from the background scan: "比對中…" turns into the result
            mutexLock(&g_tid.m);
            uint32_t gen = g_tid.gen;
            mutexUnlock(&g_tid.m);
            std::string prog = tidProgress();
            if (prog != ms.checkText) { ms.checkText = prog; redraw = true; }
            static u64 lastSeen = 0;
            if (gen != tidSeen && (now - lastSeen >= 1000 || tidScanDone())) {  // at most once a second
                tidSeen = gen;
                lastSeen = now;
                ctx.onServer = nasBaseIds(a);
                for (auto& m : games) m.onNas = ctx.onServer.count(m.g.appId) > 0;
                rebuild(ms.rows.empty() ? 0 : games[order[ms.cursor]].g.appId);
                redraw = true;
                if (!nasReported && tidScanDone()) { reportNasCheck(a, games); nasReported = true; }
            }
        }
        // direction repeat
        const u64 dirs = HidNpadButton_AnyUp | HidNpadButton_AnyDown | HidNpadButton_L | HidNpadButton_R;
        u64 step = down & dirs;
        if (step) { holdKey = step; holdStart = lastRepeat = now; }
        else if (holdKey && (held & holdKey) && now - holdStart > 350 && now - lastRepeat > 70) { step = holdKey; lastRepeat = now; }
        else if (!(held & holdKey)) holdKey = 0;
        int tapId = -1;
        HidTouchScreenState ts = {0};
        if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
            tx = (int)ts.touches[0].x;
            ty = (int)ts.touches[0].y;
            if (!touching) { touching = true; sx = tx; sy = ty; }
        } else if (touching) {
            touching = false;
            if (std::abs(tx - sx) < 30 && std::abs(ty - sy) < 30)
                for (const auto& h : ms.hits)
                    if (tx >= h.x && tx < h.x + h.w && ty >= h.y && ty < h.y + h.h) tapId = h.id;
        }

        if (ms.menu.open) {
            size_t n = ms.menu.items.size();
            bool activate = (down & HidNpadButton_A) != 0;
            if (tapId >= 400 && tapId < 400 + (int)n) { ms.menu.cursor = (size_t)(tapId - 400); activate = true; }
            if ((down & (HidNpadButton_B | HidNpadButton_Y)) || tapId == 300) { ms.menu.open = false; redraw = true; }
            else if (step & HidNpadButton_AnyUp) { ms.menu.cursor = ms.menu.cursor ? ms.menu.cursor - 1 : n - 1; redraw = true; }
            else if (step & HidNpadButton_AnyDown) { ms.menu.cursor = (ms.menu.cursor + 1) % n; redraw = true; }
            else if (activate) {
                if (ms.menu.cursor + 1 < n) {
                    ticked[ms.menu.cursor] = !ticked[ms.menu.cursor];
                    // unticking the base: the others go back to what they were
                    fillPieces();
                } else if (ms.menu.items.back().enabled) {
                    deleteTicked();
                    if (ms.menu.open) fillPieces();
                }
                redraw = true;
            }
        } else {
            size_t n = ms.rows.size();
            if ((down & HidNpadButton_B) || (down & HidNpadButton_Plus)) break;
            if (n && (step & HidNpadButton_AnyUp)) { ms.cursor = ms.cursor ? ms.cursor - 1 : n - 1; a.lastMove = now; redraw = true; }
            if (n && (step & HidNpadButton_AnyDown)) { ms.cursor = (ms.cursor + 1) % n; a.lastMove = now; redraw = true; }
            if (n && (step & HidNpadButton_L)) { ms.cursor = ms.cursor >= screens::kListRows ? ms.cursor - screens::kListRows : 0; a.lastMove = now; redraw = true; }
            if (n && (step & HidNpadButton_R)) { ms.cursor = std::min(n - 1, ms.cursor + screens::kListRows); a.lastMove = now; redraw = true; }
            if ((down & HidNpadButton_Y) || tapId == 200) {
                byName = !byName;
                rebuild(n ? games[order[ms.cursor]].g.appId : 0);
                redraw = true;
            }
            if (tapId >= 1000 && tapId < 1000 + (int)n) {
                if ((size_t)(tapId - 1000) == ms.cursor) down |= HidNpadButton_A;  // second tap opens it
                else { ms.cursor = (size_t)(tapId - 1000); a.lastMove = now; redraw = true; }
            }
            if ((down & HidNpadButton_A) && n) {
                openPieces();
                if (ms.menu.open) fillPieces();
                redraw = true;
            }
        }
        if (updateManageCover()) redraw = true;
        if (redraw || ms.animating) draw();
        svcSleepThread(16000000);
    }
    if (changedAny) {  // the list's 已安裝 / 可更新 badges follow
        classifyAll(a);
        rebuildView(a);
    }
}

static void openMenu(App& a, int page, size_t cursor = 0) {
    screens::MenuState& m = a.list.menu;
    m.open = true;
    m.items.clear();
    m.scroll = 0;
    a.menuPage = page;
    switch (page) {
        case kMenuShortcut: {
            m.title = "HOME 捷徑";
            m.child = true;
            bool have = install::hasApplication(shortcut::kTitleId);
            m.items.push_back({have ? "重新建立捷徑" : "建立捷徑", have ? "捷徑被刪掉、壞掉時用" : "在 HOME 選單加上遊戲書架"});
            screens::MenuItem rm{"移除捷徑", "從 HOME 選單拿掉"};
            rm.enabled = have;
            m.items.push_back(rm);
            break;
        }
        case kMenuView: {
            m.title = "檢視方式";
            m.child = true;
            const char* labels[] = {"清單（含封面）", "封面牆", "精簡清單"};
            const char* notes[] = {"一頁 14 款，右側大封面", "一頁 32 款封面", "一頁 14 款，表格顯示發售日、版本"};
            for (int k = 0; k < 3; k++) {
                screens::MenuItem it{labels[k], notes[k]};
                it.checked = a.list.view == k;
                m.items.push_back(it);
            }
            break;
        }
        case kMenuSort:
            m.title = "排序方式";
            m.child = true;
            for (int k = 0; k < kSortCount; k++) {
                screens::MenuItem it{kSortMenu[k]};
                it.checked = a.sort == k;
                m.items.push_back(it);
            }
            break;
        case kMenuServer:
            m.title = "伺服器設定";
            m.child = true;
            for (const auto& sv : a.store.list) {
                screens::MenuItem it{sv.name, servers::hostOf(sv.url), true};
                it.checked = sv.id == a.store.current;
                m.items.push_back(it);
            }
            m.items.push_back({"＋ 新增伺服器"});
            break;
        case kMenuServerItem: {
            if (a.menuServer < 0 || a.menuServer >= (int)a.store.list.size()) { openMenu(a, kMenuServer); return; }
            const servers::Server& sv = a.store.list[(size_t)a.menuServer];
            bool cur = sv.id == a.store.current;
            m.title = sv.name;
            m.child = true;
            screens::MenuItem sw{"切換到這台", cur ? "使用中" : ""};
            sw.enabled = !cur;
            m.items.push_back(sw);
            m.items.push_back({"名稱", sv.name});
            m.items.push_back({"位址", sv.url});
            m.items.push_back({"帳號", sv.user.empty() ? "（沒有）" : sv.user});
            m.items.push_back({"密碼", sv.pass.empty() ? "（沒有）" : "●●●●●●●●"});
            m.items.push_back({"測試連線"});
            m.items.push_back({"刪除這台伺服器"});
            break;
        }
        case kMenuSearch: {
            m.title = "搜尋";
            m.child = true;
            m.items.push_back({"注音輸入", "內建，候選字只列檔名裡有的字"});
            m.items.push_back({"倉頡輸入", "內建"});
            m.items.push_back({"英數輸入", "英文名稱、遊戲 ID"});
            m.items.push_back({"系統鍵盤", "主機內建的各種輸入法"});
            screens::MenuItem clr{"清除搜尋", a.search};
            clr.enabled = !a.search.empty();
            m.items.push_back(clr);
            break;
        }
        case kMenuParallel:
            m.title = "同時安裝數量";
            m.child = true;
            for (int k = 0; k <= 3; k++) {
                screens::MenuItem it{k == 0 ? std::string("自動（建議）") :
                                              std::to_string(k) + " 個" + (k == 1 ? "（一次一個）" : "")};
                it.checked = a.parallel == k;
                m.items.push_back(it);
            }
            break;
        default: {
            m.title = "選單";
            m.child = false;
            size_t n = installableCount(a);
            m.items.push_back({"排序方式", kSorts[a.sort], true});
            m.items.push_back({"檢視方式", kViewNames[a.list.view], true});
            m.items.push_back({"搜尋", a.search});
            screens::MenuItem pick{"一鍵選取所有更新與 DLC", std::to_string(n) + " 款遊戲"};
            pick.enabled = n > 0;
            m.items.push_back(pick);
            m.items.push_back({"全選／取消全選（目前分頁）"});
            {
                size_t games = 0;
                for (const auto& kv : a.installed)
                    games += (kv.first & 0xFFF) == 0 && kv.first != shortcut::kTitleId;
                m.items.push_back({"詳細管理遊戲內容", "已安裝 " + std::to_string(games) + " 款", true});
            }
            m.items.push_back({"重新整理"});
            m.items.push_back({"同時安裝數量",
                               a.parallel ? std::to_string(a.parallel) + " 個" : "自動（目前 " + std::to_string(g_auto.lanes) + " 個）",
                               true});
            m.items.push_back({"伺服器設定", a.list.serverName.empty() ? "尚未設定" : a.list.serverName, true});
            m.items.push_back({"安裝時 CPU 加速", a.boost ? "開" : "關"});
            m.items.push_back({"清理當機報告"});
            m.items.push_back({"更新遊戲資料庫", titleDbText(a)});
            m.items.push_back({"HOME 捷徑", shortcutText(a), true});
            {
                int miss = a.covers ? a.covers->missingCount() : 0;
                m.items.push_back({"重新抓取封面", miss ? std::to_string(miss) + " 款沒有封面" : ""});
            }
            m.items.push_back({"檢查更新", APP_VERSION});
            m.items.push_back({"關於"});
            break;
        }
    }
    m.cursor = cursor < m.items.size() ? cursor : 0;
}

static void closeMenu(App& a) { a.list.menu.open = false; }

// Back one level: submenu -> main menu -> closed.
static void menuBack(App& a) {
    if (a.menuPage == kMenuMain) { closeMenu(a); return; }
    if (a.menuPage == kMenuServerItem) { openMenu(a, kMenuServer, (size_t)std::max(0, a.menuServer)); return; }
    size_t from = a.menuPage == kMenuSort       ? kMainSort
                  : a.menuPage == kMenuView     ? kMainView
                  : a.menuPage == kMenuShortcut ? kMainShortcut
                  : a.menuPage == kMenuParallel ? kMainParallel
                  : a.menuPage == kMenuSearch   ? kMainSearch
                                                : kMainServers;
    openMenu(a, kMenuMain, from);
}

// Shown once before a batch selection that has to read NSZ / XCZ headers.
static void batchMeasureNotice(App& a, const std::vector<size_t>& reps) {
    for (size_t rep : reps) {
        auto ro = a.rowOf.find(rep);
        if (ro == a.rowOf.end()) continue;
        for (size_t i : a.rows[ro->second].needed)
            if (isCompressed(a.files[i].displayName) && !a.instSize.count(a.files[i].url)) {
                showMessage(a, "正在計算安裝後大小…", {"NSZ / XCZ 解壓縮後會變大，先讀檔頭算出實際需要的空間。"}, "");
                return;
            }
    }
}

static void selectAllInView(App& a) {
    bool all = !a.view.empty();
    for (size_t i : a.view) all = all && a.selected[i];
    std::vector<std::string> skipped;
    size_t picked = 0;
    if (all) {
        for (size_t i : a.view) a.selected[i] = false;
    } else {
        batchMeasureNotice(a, a.view);
        for (size_t i : a.view) {
            if (!a.selected[i]) trySelect(a, i, true, &skipped);
            picked += a.selected[i];
        }
    }
    for (size_t v = 0; v < a.view.size(); v++) a.list.items[v].selected = a.selected[a.view[v]];
    updateSelInfo(a);
    reportSkipped(a, skipped, picked);
}

static void menuActivate(App& a) {
    screens::MenuState& m = a.list.menu;
    if (m.cursor >= m.items.size() || !m.items[m.cursor].enabled) return;
    size_t c = m.cursor;
    switch (a.menuPage) {
        case kMenuView:
            a.list.view = (int)c;
            remote::saveSetting(kConfigPath, "view", std::to_string(a.list.view));  // saved and used right away
            closeMenu(a);
            a.list.status = std::string("檢視方式：") + kViewNames[a.list.view];
            return;
        case kMenuShortcut:
            closeMenu(a);
            if (c == 0) createShortcut(a, true);
            else removeShortcut(a);
            return;
        case kMenuSort:
            a.sort = (int)c;
            remote::saveSetting(kConfigPath, "sort", std::to_string(a.sort));  // saved and used right away
            closeMenu(a);
            rebuildView(a, false, false);  // same games, another order: no recomputing
            a.list.status = std::string("排序：") + kSorts[a.sort];
            return;
        case kMenuParallel:
            a.parallel = (int)c;  // 0 = auto
            remote::saveSetting(kConfigPath, "parallel", std::to_string(a.parallel));
            if (a.parallel == 0 && installsActive()) autoReset();
            openMenu(a, kMenuMain, kMainParallel);
            a.list.status = a.parallel ? "同時安裝 " + std::to_string(a.parallel) + " 個檔案。"
                                       : std::string("同時安裝數量：自動（先裝 1 個，總速度真的變快才多開）。");
            return;
        case kMenuServer: {
            size_t n = a.store.list.size();
            if (c < n) {
                a.menuServer = (int)c;
                openMenu(a, kMenuServerItem);
            } else if (c == n) {
                closeMenu(a);
                addServer(a);
            }
            return;
        }
        case kMenuSearch:
            closeMenu(a);
            if (c <= 2) {
                remote::saveSetting(kConfigPath, "ime", std::to_string(c));  // opens there next time
                if (runIme(a, (ime::Mode)c)) rebuildView(a, false, false);
            } else if (c == 3) {
                if (askSearch(a)) rebuildView(a, false, false);
            } else {
                a.search.clear();
                rebuildView(a, false, false);
                a.list.status = "已清除搜尋。";
            }
            return;
        case kMenuServerItem: {
            int idx = a.menuServer;
            if (idx < 0 || idx >= (int)a.store.list.size()) { openMenu(a, kMenuServer); return; }
            std::string id = a.store.list[(size_t)idx].id;
            switch (c) {
                case 0:
                    closeMenu(a);
                    switchToServer(a, id);
                    return;
                case 1: case 2: case 3: case 4:
                    editServer(a, idx, (int)c - 1);
                    if (a.store.find(id)) openMenu(a, kMenuServerItem, c);  // show the new value
                    return;
                case 5:
                    testServer(a, a.store.list[(size_t)idx]);
                    openMenu(a, kMenuServerItem, c);
                    return;
                default:
                    deleteServer(a, idx);
                    openMenu(a, kMenuServer);
                    return;
            }
        }
        default:
            break;
    }
    switch (c) {
        case kMainSort: openMenu(a, kMenuSort, (size_t)a.sort); break;
        case kMainView: openMenu(a, kMenuView, (size_t)a.list.view); break;
        case kMainShortcut: openMenu(a, kMenuShortcut, 0); break;
        case kMainSearch:
            openMenu(a, kMenuSearch, (size_t)atoi(remote::loadSetting(kConfigPath, "ime", "0").c_str()));
            break;
        case kMainPick: {
            closeMenu(a);
            std::vector<size_t> want;
            for (const App::Row& r : a.rows)
                if (r.baseInstalled && !r.card && (r.updateAvailable || r.dlcAvailable)) want.push_back(r.rep);
            batchMeasureNotice(a, want);
            size_t n = 0;
            std::vector<std::string> skipped;
            for (size_t rep : want)  // only what fits on the SD card
                if (a.selected[rep] || trySelect(a, rep, true, &skipped)) n++;
            rebuildView(a);
            a.list.status = "已選取 " + std::to_string(n) + " 款有更新或 DLC 可裝的遊戲，按 X 開始安裝。";
            if (!skipped.empty()) a.list.status += std::to_string(skipped.size()) + " 款因 SD 卡空間不夠沒有選取。";
            reportSkipped(a, skipped, n);
            break;
        }
        case kMainAll:
            selectAllInView(a);
            closeMenu(a);
            break;
        case kMainManage:
            closeMenu(a);
            runManage(a);
            showList(a);
            break;
        case kMainRefresh:
            closeMenu(a);
            refresh(a);
            break;
        case kMainParallel: openMenu(a, kMenuParallel, (size_t)a.parallel); break;
        case kMainServers: openMenu(a, kMenuServer, (size_t)std::max(0, a.store.indexOf(a.store.current))); break;
        case kMainBoost:
            a.boost = !a.boost;
            remote::saveSetting(kConfigPath, "boost", a.boost ? "1" : "0");
            if (!a.boost) boostStop();
            else if (installsActive()) boostStart(a);
            openMenu(a, kMenuMain, kMainBoost);
            a.list.status = a.boost ? "安裝時 CPU 加速：開（安裝較快，較耗電、機身較熱，裝完自動恢復）。"
                                    : "安裝時 CPU 加速：關。";
            break;
        case kMainCrash:
            closeMenu(a);
            cleanCrashReports(a);
            break;
        case kMainTitleDb:
            closeMenu(a);
            if (!a.db) break;
            if (a.db->refresh()) {
                a.dbShown.clear();
                a.list.status = "開始更新遊戲資料庫（約 3 MB），在背景下載，可以繼續使用。";
            } else {
                a.list.status = "遊戲資料庫正在下載中，請稍候。";
            }
            break;
        case kMainCovers:
            closeMenu(a);
            if (!a.covers) break;
            {
                int miss = a.covers->missingCount();
                a.covers->retryMissing();
                a.shownCover.reset();
                a.list.status = miss ? "重新抓取封面：" + std::to_string(miss) + " 款沒有封面的遊戲，捲到那款遊戲時會再下載一次。"
                                     : "目前沒有找不到封面的遊戲。";
            }
            break;
        case kMainUpdate:
            closeMenu(a);
            checkForUpdate(a, true);
            break;
        default:
            closeMenu(a);
            a.list.status = std::string("遊戲書架 ") + APP_VERSION + "：從自己的電腦或伺服器安裝自己的備份。作者 Disappear。";
            break;
    }
}

// ---- HOME 捷徑: a small package on the HOME menu that starts this app as an application
#ifdef HAVE_SHORTCUT
// Builds the shortcut package (keys from the user's prod.keys) and installs it in place of the old
// one. No screen output: used by the menu and by the background version update.
static bool buildShortcutNow(shortcut::Inputs& in, const uint8_t* hk, std::string& err) {
    static const char* kMem = "gameshelf-shortcut.nsp";
    memcpy(in.headerKey, hk, 0x20);
    in.mainNso.assign(forwarder_main_bin, forwarder_main_bin + forwarder_main_bin_size);
    in.npdm.assign(forwarder_npdm_bin, forwarder_npdm_bin + forwarder_npdm_bin_size);
    in.iconJpg.assign(shortcut_icon_bin, shortcut_icon_bin + shortcut_icon_bin_size);
    // HOME shows this app's version ("Ver. 3.0.12"); the content version goes up on every rebuild so
    // the HOME menu reads the new name / icon / version text
    std::string ver = APP_VERSION;
    if (!ver.empty() && (ver[0] == 'v' || ver[0] == 'V')) ver.erase(0, 1);
    in.displayVersion = ver;
    int seq = atoi(remote::loadSetting(kConfigPath, "shortcut_seq", "0").c_str()) + 1;
    if (seq < 1 || seq > 0xFFFF) seq = 1;
    in.version = (uint32_t)seq << 16;
    auto nsp = std::make_shared<std::vector<uint8_t>>();
    std::string note;
    bool ok = shortcut::buildNsp(in, *nsp, err);
    if (ok) {
        if (install::hasApplication(shortcut::kTitleId)) install::deleteApplication(shortcut::kTitleId);  // rebuild
        http::setMemoryFile(kMem, nsp);
        install::Options opt;
        opt.keepProgress = false;
        opt.displayName = "HOME 捷徑";
        ok = install::installPackage(std::string("mem://") + kMem, opt, [](const install::Progress&) { return true; }, err, note);
        http::setMemoryFile(kMem, nullptr);
        if (ok) {
            remote::saveSetting(kConfigPath, "shortcut_seq", std::to_string(seq));
            remote::saveSetting(kConfigPath, "shortcut_rebuilding", "");
            install::refreshAppInfo(shortcut::kTitleId);
        }
    }
    install::log("shortcut %s: %s", ver.c_str(), ok ? "created" : err.c_str());
    return ok;
}

// Background rebuild after an update of the app: only the version text changes.
static Thread g_scThread;
static bool g_scUsed = false;
static std::atomic<int> g_scState{0};  // 0 idle, 1 building, 2 done ok, 3 failed
static void shortcutThread(void*) {
    shortcut::Inputs in;
    const uint8_t* hk = install::headerKey();
    std::string err;
    bool ok = hk && install::prodKey("key_area_key_application_00", in.keyAreaKey, 0x10) && buildShortcutNow(in, hk, err);
    g_scState = ok ? 2 : 3;
}
#endif

static void createShortcut(App& a, bool fromMenu) {
#ifndef HAVE_SHORTCUT
    install::log("shortcut: this build has no loader (switch/forwarder was not built)");
    if (fromMenu)
        askChoice(a, "無法建立 HOME 捷徑", {"這個版本沒有包含捷徑程式。", "請用 GitHub 發佈的版本（自行編譯時要先編譯 switch/forwarder）。"},
                  {{"A", "確定", kGo, 1}});
    return;
#else
    if (installsActive()) {
        a.list.status = "正在安裝遊戲：裝完後再按 Y →「HOME 捷徑」→「重新建立捷徑」。";
        return;
    }
    shortcut::Inputs in;
    const uint8_t* hk = install::headerKey();
    if (!hk || !install::prodKey("key_area_key_application_00", in.keyAreaKey, 0x10)) {
        install::log("shortcut: prod.keys missing (header_key / key_area_key_application_00)");
        askChoice(a, "無法建立 HOME 捷徑",
                  {"!SD 卡上找不到 prod.keys（或裡面缺少需要的金鑰）。",
                   "請把 Lockpick_RCM 匯出的 prod.keys 放在 SD 卡的 switch 資料夾，",
                   "再按 Y →「HOME 捷徑」→「重新建立捷徑」。"},
                  {{"A", "確定", kGo, 1}});
        return;
    }
    showMessage(a, "正在建立 HOME 捷徑…", {"建立後，從 HOME 選單點「遊戲書架」就是完整記憶體模式。"}, "");
    std::string err;
    bool ok = buildShortcutNow(in, hk, err);
    if (ok) {
        a.list.status = "已在 HOME 選單建立「遊戲書架」捷徑：之後從 HOME 選單點它，就是完整記憶體模式。";
    } else {
        askChoice(a, "建立 HOME 捷徑失敗",
                  {"!" + err, "最常見的原因：主機沒有安裝簽章補丁（sigpatches）。", "遊戲書架本身不受影響，可以繼續使用。"},
                  {{"A", "確定", kGo, 1}});
    }
#endif
}

static void removeShortcut(App& a) {
    if (!install::hasApplication(shortcut::kTitleId)) {
        a.list.status = "HOME 選單上沒有遊戲書架的捷徑。";
        return;
    }
    int pick = askChoice(a, "移除 HOME 捷徑？",
                         {"HOME 選單上的「遊戲書架」圖示會被拿掉。", "遊戲書架本身、設定和下載的遊戲都不受影響，",
                          "之後隨時可以在這裡重新建立。"},
                         {{"A", "移除", kStop, 1}, {"B", "取消", kLater, 0}});
    if (pick != 1) return;
    remote::saveSetting(kConfigPath, "shortcut_rebuilding", "");  // removed on purpose: not made again
    Result rc = install::deleteApplication(shortcut::kTitleId);
    install::refreshAppInfo(shortcut::kTitleId);
    char code[16];
    snprintf(code, sizeof code, "0x%X", (unsigned)rc);
    a.list.status = R_SUCCEEDED(rc) ? std::string("已移除 HOME 選單上的「遊戲書架」捷徑。")
                                    : std::string("移除 HOME 捷徑失敗（錯誤碼 ") + code + "）。";
}

// The shortcut is created once, on the first start of a version that has the feature (later only
// from the menu). When a newer build of the shortcut exists (kShortcutBuild), an installed one is
// rebuilt once; one the user removed stays removed.
// 2: starts as an application; 3: version text "隨主程式"; 4: finds the app wherever it is;
// 5: shows this app's version — rebuilt (in the background) whenever the app's version changes;
// 6: same, and a rebuild cut short (the app closed half-way) is finished at the next start.
static const char* kShortcutBuild = "6";
static void autoShortcut(App& a) {
    std::string want = std::string(kShortcutBuild) + "/" + APP_VERSION;
    std::string saved = remote::loadSetting(kConfigPath, "shortcut_build", "");
    bool have = install::hasApplication(shortcut::kTitleId);
    // the old one was deleted but the new one never installed (v3.0.12's first build closed while
    // rebuilding, or the console was switched off): not the player's choice, so it is made again
    bool cutShort = !have && (remote::loadSetting(kConfigPath, "shortcut_rebuilding", "") == "1" || saved.compare(0, 2, "5/") == 0);
    if (saved == want && !cutShort) return;
    if (installsActive()) return;  // try again next time
    bool first = remote::loadSetting(kConfigPath, "shortcut_auto", "") != "1";
    remote::saveSetting(kConfigPath, "shortcut_auto", "1");
    remote::saveSetting(kConfigPath, "shortcut_build", want);
    if (cutShort) {
        install::log("shortcut: an earlier rebuild did not finish, making it again");
        remote::saveSetting(kConfigPath, "shortcut_rebuilding", "");
        createShortcut(a, false);
        return;
    }
    if (!first && !have) return;  // removed by the player: stays removed
    if (first && !have) { createShortcut(a, false); return; }
#ifdef HAVE_SHORTCUT
    // an existing shortcut: only its version text changes, built while the list is in use
    remote::saveSetting(kConfigPath, "shortcut_rebuilding", "1");  // cleared once it is back
    g_scState = 1;
    if (R_SUCCEEDED(threadCreate(&g_scThread, shortcutThread, nullptr, nullptr, 0x40000, 0x2E, bgCore(0))) &&
        R_SUCCEEDED(threadStart(&g_scThread))) {
        g_scUsed = true;
        return;
    }
    g_scState = 0;
    createShortcut(a, false);
#endif
}

// The background rebuild finished: say so under the list (once).
static void pollShortcut(App& a) {
#ifdef HAVE_SHORTCUT
    int st = g_scState;
    if (st < 2) return;
    if (g_scUsed) {
        threadWaitForExit(&g_scThread);
        threadClose(&g_scThread);
        g_scUsed = false;
    }
    g_scState = 0;
    if (st == 2 || install::hasApplication(shortcut::kTitleId)) remote::saveSetting(kConfigPath, "shortcut_rebuilding", "");
    a.list.status = st == 2 ? std::string("HOME 捷徑已更新為 ") + APP_VERSION + "。"
                            : std::string("HOME 捷徑版本更新失敗（捷徑照常可用），詳情見 install.log。");
#else
    (void)a;
#endif
}

// ---- 清理當機報告: Atmosphère's report folders (written when any program crashes)
static void cleanCrashReports(App& a) {
    const std::string root = "sdmc:/atmosphere";
    std::vector<reports::Usage> u = reports::scan(root);
    size_t files = 0;
    uint64_t bytes = 0;
    std::vector<std::string> lines;
    for (const auto& x : u) {
        files += x.files;
        bytes += x.bytes;
        lines.push_back(x.folder + "：" + std::to_string(x.files) + " 個檔案，" + screens::humanSize(x.bytes));
    }
    if (!files) {
        askChoice(a, "清理當機報告", {"#目前沒有當機報告", "crash_reports、erpt_reports、fatal_reports、fatal_errors 都是空的。"},
                  {{"A", "確定", kGo, 1}});
        return;
    }
    lines.insert(lines.begin(), "#共 " + std::to_string(files) + " 個檔案，" + screens::humanSize(bytes));
    lines.push_back("這些是 Atmosphère 在程式當機時留下的紀錄，刪掉不影響主機和遊戲；資料夾會保留。");
    int pick = askChoice(a, "清理當機報告？", lines, {{"A", "清除", kStop, 1}, {"B", "取消", kLater, 0}});
    if (pick != 1) return;
    uint64_t freed = 0;
    size_t n = reports::clean(root, freed);
    install::log("crash reports: deleted %zu file(s), %llu bytes", n, (unsigned long long)freed);
    a.list.status = "已清除 " + std::to_string(n) + " 個當機報告，釋放 " + screens::humanSize(freed) + "。";
    updateSdFree(a);
}

// ---- online update (GitHub Releases of Antaite/GameShelf-releases, signed by the author)
static const char* kNroName = "LanInstaller.nro";  // the release asset (and the file on the SD card)
static std::string g_selfPath;      // this .nro on the SD card (from argv[0])
static bool g_restartNow = false;   // leave the main loop and start the new version

// Background check at start: the answer waits here until the main loop can ask.
static Mutex g_updLock;
static int g_updState = 0;          // 0 idle, 1 checking, 2 done
static bool g_updOk = false;
static update::Release g_updRelease;
static std::string g_updErr;
static Thread g_updThread;

static bool fetchLatest(update::Release& r, std::string& err) {
    std::vector<uint8_t> body;
    if (!http::getToMemory(update::latestApiUrl(), 0, 0, body, err)) {
        if (err.find("404") != std::string::npos) err = "發佈網站上還沒有任何版本";
        return false;
    }
    return update::parseLatest(std::string(body.begin(), body.end()), kNroName, r, err);
}

static void updateThread(void*) {
    update::Release r;
    std::string err;
    bool ok = fetchLatest(r, err);
    mutexLock(&g_updLock);
    g_updOk = ok;
    g_updRelease = r;
    g_updErr = err;
    g_updState = 2;
    mutexUnlock(&g_updLock);
}

static void startUpdateCheck() {
    if (!update::builtInKey()[0]) return;  // a build without the signing key cannot update itself
    g_updState = 1;
    if (R_FAILED(threadCreate(&g_updThread, updateThread, nullptr, nullptr, 0x20000, 0x2F, bgCore(1))) ||
        R_FAILED(threadStart(&g_updThread))) {
        g_updState = 0;
    }
}

static std::string selfPath() {
    if (g_selfPath.compare(0, 6, "sdmc:/") == 0) return g_selfPath;
    return std::string("sdmc:/switch/") + kNroName;
}

static bool writeFile(const std::string& path, const std::vector<uint8_t>& d) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(d.data(), 1, d.size(), f) == d.size();
    ok = fclose(f) == 0 && ok;
    return ok;
}

// Downloads, checks the signature, replaces this .nro. The old file stays until the new one is in place.
static void installUpdate(App& a, const update::Release& r) {
    showMessage(a, "正在下載 " + r.tag + "…", {"下載完成後會先檢查數位簽章，確認是作者發佈的版本才會更新。"}, "");
    std::vector<uint8_t> nro, sig;
    std::string err;
    bool ok = http::getToMemory(r.nroUrl, 0, 0, nro, err) && http::getToMemory(r.sigUrl, 0, 0, sig, err);
    if (ok && r.nroSize && nro.size() != r.nroSize) { ok = false; err = "下載的大小不對"; }
    if (ok && (nro.size() < 0x80 || memcmp(nro.data() + 0x10, "NRO0", 4) != 0)) { ok = false; err = "下載的檔案不是 .nro"; }
    if (ok && !update::verify(nro, sig, update::builtInKey())) { ok = false; err = "數位簽章不符，可能不是作者發佈的檔案，已拒絕更新"; }
    std::string path = selfPath(), tmp = path + ".new", old = path + ".old";
    if (ok && !writeFile(tmp, nro)) { ok = false; err = "無法寫入 SD 卡"; }
    if (ok) {
        remove(old.c_str());
        bool hadOld = rename(path.c_str(), old.c_str()) == 0;
        if (rename(tmp.c_str(), path.c_str()) != 0) {
            if (hadOld) rename(old.c_str(), path.c_str());  // put the old version back
            ok = false;
            err = "無法替換程式檔";
        } else {
            remove(old.c_str());
        }
    }
    remove(tmp.c_str());
    install::log("update %s: %s", r.tag.c_str(), ok ? ("installed to " + path).c_str() : err.c_str());
    if (!ok) {
        askChoice(a, "更新失敗", {"!" + err, "目前的版本沒有任何改變，可以繼續使用。"}, {{"A", "確定", kLater, 1}});
        return;
    }
    if (installsActive()) {
        askChoice(a, "更新完成：" + r.tag, {"新版本已經放好。", "目前還在安裝遊戲：裝完後關閉再開啟就是新版本。"}, {{"A", "確定", kGo, 1}});
        return;
    }
    int pick = askChoice(a, "更新完成：" + r.tag, {"新版本已經放好，重新開啟就能使用。"},
                         {{"A", "立即重新開啟", kGo, 1}, {"B", "稍後", kLater, 0}});
    if (pick == 1) {
        if (envHasNextLoad() && R_SUCCEEDED(envSetNextLoad(path.c_str(), path.c_str()))) {
            g_restartNow = true;
        } else {
            askChoice(a, "請手動重新開啟", {"請按 HOME 關閉本程式，再重新開啟。"}, {{"A", "確定", kGo, 1}});
        }
    }
}

static void offerUpdate(App& a, const update::Release& r, bool asked) {
    if (update::compareVersions(r.tag, APP_VERSION) <= 0) {
        if (asked) askChoice(a, "已經是最新版本", {std::string("目前版本 ") + APP_VERSION + "，發佈網站上最新是 " + r.tag + "。"}, {{"A", "確定", kGo, 1}});
        return;
    }
    if (!asked && remote::loadSetting(kConfigPath, "skip_update", "") == r.tag) return;  // the user skipped this one
    std::vector<std::string> lines = {std::string("#有新版本 ") + r.tag + "（目前 " + APP_VERSION + "）"};
    size_t p = 0;
    int shown = 0;
    while (p < r.notes.size() && shown < 8) {
        size_t q = r.notes.find('\n', p);
        std::string l = r.notes.substr(p, q == std::string::npos ? std::string::npos : q - p);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (!l.empty()) { lines.push_back(l); shown++; }
        if (q == std::string::npos) break;
        p = q + 1;
    }
    int pick = askChoice(a, "有新版本可以更新", lines,
                         {{"A", "立即更新", kGo, 1}, {"B", "稍後再說", kLater, 0}, {"X", "略過這個版本", kStop, 2}});
    if (pick == 2) {
        remote::saveSetting(kConfigPath, "skip_update", r.tag);
        a.list.status = "已略過 " + r.tag + "，之後可以在選單「檢查更新」再更新。";
    } else if (pick == 1) {
        installUpdate(a, r);
    }
}

static void checkForUpdate(App& a, bool asked) {
    if (!update::builtInKey()[0]) {
        askChoice(a, "無法線上更新", {"這個版本是自行編譯的，沒有更新用的金鑰。", "從發佈網站下載的版本才能線上更新。"},
                  {{"A", "確定", kLater, 1}});
        return;
    }
    showMessage(a, "正在檢查更新…", {std::string("發佈網站：github.com/") + update::kRepo}, "");
    update::Release r;
    std::string err;
    if (!fetchLatest(r, err)) {
        if (asked) askChoice(a, "無法檢查更新", {"!" + err}, {{"A", "確定", kLater, 1}});
        return;
    }
    offerUpdate(a, r, asked);
}

// Main loop: the background check finished — ask now (only from the plain list).
static void pollUpdateCheck(App& a) {
    if (g_updState != 2) return;
    mutexLock(&g_updLock);
    bool ok = g_updOk;
    update::Release r = g_updRelease;
    std::string err = g_updErr;
    g_updState = 0;
    mutexUnlock(&g_updLock);
    threadWaitForExit(&g_updThread);
    threadClose(&g_updThread);
    if (!ok) { install::log("update check: %s", err.c_str()); return; }
    install::log("update check: latest %s, running %s", r.tag.c_str(), APP_VERSION);
    offerUpdate(a, r, false);
}

// ---- built-in 注音 / 倉頡 / 英數 search keyboard. Returns true when the search was confirmed.
static size_t countMatches(App& a, const std::string& text) {
    std::string needle = tsconv::searchKey(text);
    size_t n = 0;
    for (const App::Row& r : a.rows) n += needle.empty() || rowMatches(a, r, needle);
    return n;
}

static bool runIme(App& a, ime::Mode mode) {
    screens::ImeState st;
    st.mode = mode;
    st.text = a.search;
    st.total = a.rows.size();
    st.zhuyinGlyphs = a.font.hasGlyph(0x3105);  // ㄅ
    // the characters that appear in this server's file names (how often): the only candidates offered
    std::map<uint32_t, int> weights;
    for (const auto& f : a.files)
        for (uint32_t cp : ime::codepoints(baseName(f.displayName)))
            if (cp >= 0x3400) {
                weights[cp]++;
                // a simplified file name also offers the traditional characters ("宝" -> "寶")
                for (uint32_t t : tsconv::traditionalOf(tsconv::fold(cp))) weights[t]++;
            }
    auto refreshCands = [&]() {
        st.cands = ime::candidates(st.mode, st.composing, weights);
        st.candSel = 0;
        st.matches = countMatches(a, st.text);
    };
    auto clampCursor = [&]() {
        const auto& rows = ime::layout(st.mode);
        st.row = std::max(0, std::min(st.row, (int)rows.size() - 1));
        st.col = std::max(0, std::min(st.col, (int)rows[(size_t)st.row].size() - 1));
    };
    auto pick = [&](size_t i) {
        if (i >= st.cands.size()) return;
        st.text += st.cands[i];
        st.composing.clear();
        refreshCands();
    };
    std::string before = a.search;
    refreshCands();
    bool touching = false;
    int tx = 0, ty = 0, sx = 0, sy = 0;
    u64 holdKey = 0, holdStart = 0, lastRepeat = 0;
    while (appletMainLoop()) {
        padUpdate(&a.pad);
        u64 down = padGetButtonsDown(&a.pad), held = padGetButtons(&a.pad);
        pollInstalls(a);  // installs keep going in the background
        int tap = -1;
        {
            HidTouchScreenState ts = {0};
            if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
                tx = (int)ts.touches[0].x;
                ty = (int)ts.touches[0].y;
                if (!touching) { touching = true; sx = tx; sy = ty; }
            } else if (touching) {
                touching = false;
                if (std::abs(tx - sx) < 30 && std::abs(ty - sy) < 30)
                    for (const auto& h : st.hits)
                        if (tx >= h.x && tx < h.x + h.w && ty >= h.y && ty < h.y + h.h) tap = h.id;
            }
        }
        // direction keys move the key cursor (repeat while held)
        const u64 dirs = HidNpadButton_AnyUp | HidNpadButton_AnyDown | HidNpadButton_AnyLeft | HidNpadButton_AnyRight;
        u64 act = down & dirs;
        u64 now = nowMs();
        if (act) { holdKey = act; holdStart = lastRepeat = now; }
        else if (holdKey && (held & holdKey) && now - holdStart > 350 && now - lastRepeat > 80) { act = holdKey; lastRepeat = now; }
        else if (!(held & holdKey)) holdKey = 0;
        if (act & HidNpadButton_AnyUp) st.row--;
        if (act & HidNpadButton_AnyDown) st.row++;
        if (act & HidNpadButton_AnyLeft) st.col--;
        if (act & HidNpadButton_AnyRight) st.col++;
        if (act) clampCursor();

        int pressRow = -1, pressCol = -1;
        if (tap >= 700 && tap < 900) { pressRow = (tap - 700) / 20; pressCol = (tap - 700) % 20; st.row = pressRow; st.col = pressCol; }
        else if (down & HidNpadButton_A) { pressRow = st.row; pressCol = st.col; }
        if (tap >= 900) pick((size_t)(tap - 900));

        if (down & HidNpadButton_L) { if (st.candSel > 0) st.candSel--; }
        if (down & HidNpadButton_R) { if (st.candSel + 1 < st.cands.size()) st.candSel++; }
        if (down & HidNpadButton_Y) pick(st.candSel);
        bool done = (down & HidNpadButton_Plus) != 0, cancel = (down & HidNpadButton_Minus) != 0;
        bool back = (down & HidNpadButton_B) != 0, toggle = (down & HidNpadButton_X) != 0;

        if (pressRow >= 0) {
            const auto& rows = ime::layout(st.mode);
            if (pressRow < (int)rows.size() && pressCol < (int)rows[(size_t)pressRow].size()) {
                const ime::Key& k = rows[(size_t)pressRow][(size_t)pressCol];
                switch (k.action) {
                    case ime::kType:
                        if (st.mode == ime::Mode::Latin) {
                            st.text += k.value;
                        } else if (st.mode == ime::Mode::Zhuyin) {
                            // a tone ends the syllable: another tone replaces it; a new symbol after a
                            // tone takes the first candidate and starts the next syllable
                            std::string c = st.composing;
                            bool isTone = k.value == "ˊ" || k.value == "ˇ" || k.value == "ˋ" || k.value == "˙";
                            bool hadTone = false;
                            for (const char* t : {"ˊ", "ˇ", "ˋ", "˙", "ˉ"}) {
                                size_t n = strlen(t);
                                if (c.size() >= n && c.compare(c.size() - n, n, t) == 0) { c.erase(c.size() - n); hadTone = true; break; }
                            }
                            if (isTone) {
                                if (!c.empty()) st.composing = c + k.value;
                            } else if (hadTone) {
                                if (!st.cands.empty()) pick(0);
                                st.composing = k.value;
                            } else {
                                st.composing += k.value;
                            }
                        } else if (st.composing.size() < 5) {
                            st.composing += k.value;  // 倉頡: at most 5 radicals
                        }
                        refreshCands();
                        break;
                    case ime::kTone1:
                        if (!st.composing.empty()) {
                            std::string c = st.composing;
                            for (const char* t : {"ˊ", "ˇ", "ˋ", "˙", "ˉ"}) {
                                size_t n = strlen(t);
                                if (c.size() >= n && c.compare(c.size() - n, n, t) == 0) { c.erase(c.size() - n); break; }
                            }
                            st.composing = c + "ˉ";
                            refreshCands();
                        }
                        break;
                    case ime::kSpace:
                        if (!st.cands.empty()) pick(st.candSel);  // like a desktop IME: space takes the first
                        else if (st.composing.empty()) { st.text += " "; refreshCands(); }
                        break;
                    case ime::kBack: back = true; break;
                    case ime::kClear:
                        st.text.clear();
                        st.composing.clear();
                        refreshCands();
                        break;
                    case ime::kSwitch: toggle = true; break;
                    case ime::kDone: done = true; break;
                }
            }
        }
        if (back) {
            if (!st.composing.empty()) ime::popChar(st.composing);
            else ime::popChar(st.text);
            refreshCands();
        }
        if (toggle) {
            st.mode = (ime::Mode)(((int)st.mode + 1) % 3);
            st.composing.clear();
            remote::saveSetting(kConfigPath, "ime", std::to_string((int)st.mode));
            clampCursor();
            refreshCands();
        }
        if (cancel) { a.search = before; return false; }
        if (done) {
            a.search = st.text;
            a.list.status = a.search.empty() ? "已清除搜尋。" : "搜尋「" + a.search + "」：" + std::to_string(st.matches) + " 款遊戲。";
            return true;
        }
        present(a, [&](gfx::Canvas& c) { screens::drawIme(c, a.font, st); });
        svcSleepThread(16000000);
    }
    return false;
}

static void waitInstallsStopped(App& a) {
    showMessage(a, "正在停止安裝…", {"已下載的部分會保留，下次可以從中斷的地方繼續。"}, "");
    for (;;) {
        pollInstalls(a);
        mutexLock(&g_run.m);
        int alive = g_run.alive;
        mutexUnlock(&g_run.m);
        if (alive == 0) break;
        svcSleepThread(50000000);
    }
    pollInstalls(a);  // join the last workers
}

// "+" while installing: ask first. Returns true to quit.
static bool confirmExit(App& a) {
    if (!installsActive()) return true;
    size_t open = 0;
    mutexLock(&g_run.m);
    for (const Job& j : g_run.jobs) open += j.state != 2;
    mutexUnlock(&g_run.m);
    int c = askChoice(a, "還有 " + std::to_string(open) + " 個檔案正在安裝或排隊",
                      {"離開會停止安裝，已下載的部分會保留：", "下次開啟軟體時可以選擇從中斷的地方繼續。"},
                      {{"A", "離開", kStop, 1}, {"B", "繼續安裝", kGo, 0}});
    if (c != 1) return false;
    saveQueueNow();    // waiting + installing files are offered again next time
    g_exiting = true;
    cancelAllInstalls();
    waitInstallsStopped(a);
    return true;
}

// ------------------------------------------------------------------ unfinished installs
// Offered at start: installs that stopped part-way (cancelled, quit, connection lost).
static std::string ageText(int64_t since, bool& old) {
    old = false;
    if (since <= 0) return "";
    int64_t now = (int64_t)time(nullptr);
    if (now < since) return "";
    int days = (int)((now - since) / 86400);
    old = days > 7;
    return days == 0 ? "今天" : "已放置 " + std::to_string(days) + " 天";
}

static void askResume(App& a) {
    std::vector<install::PendingInstall> pend = install::pendingInstalls();
    std::vector<resume::QueueEntry> queue = resume::loadQueue(kQueuePath);
    std::set<std::string> pendUrls;
    for (const auto& p : pend) pendUrls.insert(p.url);
    std::vector<resume::QueueEntry> waiting;  // queued but never started
    for (const auto& q : queue)
        if (!pendUrls.count(q.url)) waiting.push_back(q);
    if (pend.empty() && waiting.empty()) return;

    std::vector<std::string> lines;
    uint64_t kept = 0;
    bool anyOld = false;
    if (!pend.empty()) {
        lines.push_back("#進行到一半（" + std::to_string(pend.size()) + " 個）");
        for (size_t k = 0; k < pend.size(); k++) {
            kept += pend[k].kept;
            bool old = false;
            std::string age = ageText(pend[k].started, old);
            anyOld = anyOld || old;
            if (k >= 3) continue;
            std::string n = baseName(pend[k].name);  // the NAS file name as it is
            if (pend[k].wiped)
                lines.push_back("!" + n + "　" + screens::humanSize(pend[k].total) + "，未完成的下載資料已被系統清除，需要從頭下載");
            else
                lines.push_back(std::string(old ? "!" : "") + n + "　" + screens::humanSize(pend[k].total) + "，已完成 " +
                                std::to_string(pend[k].percent) + "%" + (age.empty() ? "" : "，" + age));
        }
        bool anyWiped = false;
        for (const auto& p : pend) anyWiped = anyWiped || p.wiped;
        if (anyWiped)
            lines.push_back("在 HOME 選單或系統設定刪除軟體時，系統會一併清掉所有未完成的下載資料。");
        if (pend.size() > 3) lines.push_back("…還有 " + std::to_string(pend.size() - 3) + " 個");
    }
    if (!waiting.empty()) {
        lines.push_back("#排隊中尚未開始（" + std::to_string(waiting.size()) + " 個）");
        for (size_t k = 0; k < waiting.size() && k < 3; k++) lines.push_back(baseName(waiting[k].name));
        if (waiting.size() > 3) lines.push_back("…還有 " + std::to_string(waiting.size() - 3) + " 個");
    }
    if (anyOld) lines.push_back("!有未完成的安裝佔用 " + screens::humanSize(kept) + "，若不需要可以放棄以釋放空間。");

    size_t total = pend.size() + waiting.size();
    int c = askChoice(a, "有 " + std::to_string(total) + " 個未完成的安裝", lines,
                      {{"A", "繼續安裝", kGo, 1}, {"X", "放棄並清除", kStop, 2}, {"B", "稍後再說", kLater, 0}});
    if (c == 1) {
        // half-done files that are not in the saved queue first, then the queue in its order
        std::vector<Job> jobs;
        std::set<std::string> inQueue;
        for (const auto& q : queue) inQueue.insert(q.url);
        for (const auto& p : pend) {
            if (inQueue.count(p.url)) continue;
            Job j;
            j.url = p.url;
            j.name = p.name;
            titles::FileInfo fi = titles::parse(p.name);
            j.group = fi.baseId ? fi.baseId : (0x8000000000000000ULL | jobs.size());
            j.added = p.started;
            jobs.push_back(j);
        }
        for (const auto& q : queue) {
            Job j;
            j.url = q.url;
            j.name = q.name;
            j.group = q.group;
            j.added = q.added;
            jobs.push_back(j);
        }
        size_t n = enqueue(a, jobs);
        a.list.status = "繼續安裝 " + std::to_string(n) + " 個檔案（背景進行中，按 - 查看進度）。";
    } else if (c == 2) {
        showMessage(a, "正在清除…", {"刪除未完成安裝已下載的部分。"}, "");
        uint64_t freed = 0;
        for (const auto& p : pend) freed += install::discardPending(p.path);
        resume::saveQueue(kQueuePath, {});
        g_deferred.clear();
        updateSdFree(a);
        a.list.status = "已清除 " + std::to_string(total) + " 個未完成的安裝，釋放了 " + screens::humanSize(freed) + "。";
    } else {
        g_deferred = waiting;  // keep them in the saved queue even when other games are installed now
        a.list.status = "有 " + std::to_string(total) + " 個未完成的安裝：下次開啟時會再詢問，或在清單中再按一次安裝。";
    }
}

// ------------------------------------------------------------------ fonts
static bool loadFonts(App& a) {
    if (R_FAILED(plInitialize(PlServiceType_User))) return false;
    // Traditional Chinese first so shared Han characters use Taiwanese glyph shapes.
    const PlSharedFontType order[] = {PlSharedFontType_ChineseTraditional, PlSharedFontType_Standard,
                                      PlSharedFontType_ChineseSimplified, PlSharedFontType_ExtChineseSimplified,
                                      PlSharedFontType_KO, PlSharedFontType_NintendoExt};
    for (PlSharedFontType t : order) {
        PlFontData fd;
        if (R_SUCCEEDED(plGetSharedFontByType(&fd, t))) a.font.addFace((const uint8_t*)fd.address, fd.size, 0);
    }
    return !a.font.empty();
}

// The HOME shortcut starts the app from where it last ran (written here on every start). When the
// shortcut had to search for it and found more than one copy, it lists the others: offer to delete
// them, so an online update never leaves an old copy behind.
static void rememberAppPath(App& a) {
    static const char* kPathFile = "sdmc:/config/lan-installer/app_path.txt";
    static const char* kDupesFile = "sdmc:/config/lan-installer/app_dupes.txt";
    std::string self = g_selfPath.compare(0, 6, "sdmc:/") == 0 ? g_selfPath : std::string();  // from argv[0]
    if (!self.empty()) {
        std::string cur;
        if (FILE* f = fopen(kPathFile, "r")) {
            char line[1024] = {0};
            if (fgets(line, sizeof line, f)) cur = line;
            fclose(f);
            while (!cur.empty() && (cur.back() == '\n' || cur.back() == '\r')) cur.pop_back();
        }
        if (cur != self) {
            if (FILE* f = fopen(kPathFile, "w")) { fputs(self.c_str(), f); fclose(f); }
            install::log("app path: %s", self.c_str());
        }
    }
    std::vector<std::string> others;
    if (FILE* f = fopen(kDupesFile, "r")) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            std::string l(line);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            if (!l.empty() && l != self && l.compare(0, 6, "sdmc:/") == 0) others.push_back(l);
        }
        fclose(f);
    }
    remove(kDupesFile);
    if (others.empty()) return;
    std::vector<std::string> lines = {"HOME 捷徑找遊戲書架時，SD 卡上找到好幾份，現在用的是版本最新的：", "#" + (self.empty() ? std::string("（目前這份）") : self.substr(5)),
                                      "另外這些是舊的複本（線上更新只會更新現在這一份）："};
    for (size_t k = 0; k < others.size() && k < 5; k++) lines.push_back("!" + others[k].substr(5));
    if (others.size() > 5) lines.push_back("…還有 " + std::to_string(others.size() - 5) + " 份");
    int pick = askChoice(a, "發現 " + std::to_string(others.size()) + " 份舊的遊戲書架", lines,
                         {{"A", "刪除舊的複本", kStop, 1}, {"B", "保留", kLater, 0}});
    if (pick != 1) return;
    int n = 0;
    for (const auto& o : others)
        if (remove(o.c_str()) == 0) n++;
    install::log("app copies: deleted %d of %zu", n, others.size());
    a.list.status = "已刪除 " + std::to_string(n) + " 份舊的遊戲書架。";
}

int main(int argc, char** argv) {
    if (argc > 0 && argv[0]) g_selfPath = argv[0];
    App a;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&a.pad);
    hidInitializeTouchScreen();

    if (R_FAILED(framebufferCreate(&a.fb, nwindowGetDefault(), screens::kWidth, screens::kHeight, PIXEL_FORMAT_RGBA_8888, 2)))
        return 0;
    framebufferMakeLinear(&a.fb);

    if (!loadFonts(a)) {
        // Without a font nothing can be shown; just wait for a button and quit.
        present(a, [](gfx::Canvas& c) { gfx::clear(c, gfx::rgb(120, 20, 20)); });
        waitForConfirm(a);
        framebufferClose(&a.fb);
        plExit();
        return 0;
    }

    // Bigger TCP buffers = noticeably faster transfers.
    SocketInitConfig sockCfg = *socketGetDefaultInitConfig();
    sockCfg.tcp_tx_buf_size = 0x10000;
    sockCfg.tcp_rx_buf_size = 0x80000;
    sockCfg.tcp_rx_buf_max_size = 0x400000;  // larger receive window: keeps several parallel downloads fed
    sockCfg.sb_efficiency = 4;
    Result sockRc = socketInitialize(&sockCfg);
    http::tlsInit();  // HTTPS is optional; plain HTTP keeps working if this fails
    nifmInitialize(NifmServiceType_User);
    http::setNetCheck([] { return netLinkUp(); });  // no address at all: connections fail at once instead of waiting
    Result svcRc = install::initServices();

    if (R_FAILED(sockRc) || R_FAILED(svcRc)) {
        char detail[96];
        snprintf(detail, sizeof(detail), "網路或系統服務初始化失敗（socket=0x%X，services=0x%X）。", sockRc, svcRc);
        showMessage(a, "無法啟動", {detail, "請確認你使用的是 Atmosphère，並且從 hbmenu 啟動。"}, "離開");
        waitForConfirm(a);
    } else {
        a.list.version = APP_VERSION;
        outboxStart();  // notes to the author left from an earlier start go out in the background
        {
            AppletType at = appletGetAppletType();
            g_appletMode = !(at == AppletType_Application || at == AppletType_SystemApplication);
            u64 total = 0, used = 0;
            svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
            svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
            install::log("%s %s: memory %llu MB (used %llu MB)%s", "LanInstaller", APP_VERSION,
                         (unsigned long long)(total >> 20), (unsigned long long)(used >> 20),
                         g_appletMode ? ", started from the album (applet mode)" : ", started through a game");
            if (g_appletMode) a.list.notice = "相簿模式：記憶體較少";
            // with the full memory, each install may run up to 64 MB ahead of the SD card
            install::setDownloadBuffer(g_appletMode ? 8u << 20 : 64u << 20);
            a.boost = remote::loadSetting(kConfigPath, "boost", "1") != "0";
        }
        a.parallel = atoi(remote::loadSetting(kConfigPath, "parallel", "0").c_str());  // 0 = auto
        if (a.parallel < 0 || a.parallel > 3) a.parallel = 0;
        // Cover cache: past 80 MB on the SD card, the covers viewed longest ago go until 65 MB is left
        // (covers are kept scaled to the screen size, about 30 KB each: roughly 2000+ fit).
        int trimmed = covers::trimCache(kCoverDir, 80ULL * 1024 * 1024, 65ULL * 1024 * 1024);
        if (trimmed) install::log("cover cache: removed %d old covers", trimmed);
        std::string coverUrl = remote::loadSetting(kConfigPath, "cover", kDefaultCoverUrl);
        a.covers.reset(new covers::Cache(kCoverDir, coverUrl, kCoverBox));
        a.covers->setLogger([](const std::string& line) { install::log("%s", line.c_str()); });
        a.covers->setMaxImages(g_appletMode ? 100 : 300);  // decoded pictures kept in memory

        // Game names / release dates (downloads once in the background, then cached on the SD card).
        {
            std::vector<std::string> urls;
            std::string list = remote::loadSetting(kConfigPath, "titledb", kDefaultTitleDb);
            if (list == kOldTitleDb) list = kDefaultTitleDb;
            size_t p = 0;
            while (p <= list.size()) {
                size_t q = list.find(',', p);
                if (q == std::string::npos) q = list.size();
                if (q > p) urls.push_back(list.substr(p, q - p));
                p = q + 1;
            }
            a.db.reset(new titledb::Db(urls, kTitleDbCache));
            titledb::Db* db = a.db.get();
            std::string proxy = remote::loadSetting(kConfigPath, "icon_proxy", kDefaultIconProxy);
            a.covers->setUrlLookup([db, proxy](uint64_t id) {
                titledb::Entry e;
                return db->find(id, e) ? covers::proxiedUrl(proxy, e.iconUrl) : std::string();
            });
        }

        // Servers: the list from servers.ini; on the first start of v3 the single old address
        // (account:password inside) becomes server 1 and is removed from config.ini.
        servers::load(kServersPath, a.store);
        std::string old = remote::loadServer(kConfigPath);
        if (servers::migrateOld(a.store, old, "伺服器 1")) {
            saveServers(a);
            install::log("servers: old address moved to servers.ini");
        }
        if (!old.empty() && !a.store.list.empty()) remote::saveServer(kConfigPath, "");
        a.sort = atoi(remote::loadSetting(kConfigPath, "sort", "0").c_str());
        a.list.view = atoi(remote::loadSetting(kConfigPath, "view", "0").c_str());
        if (a.list.view < 0 || a.list.view > 2) a.list.view = 0;
        if (a.sort < 0 || a.sort >= kSortCount) a.sort = 0;
        applyCurrentServer(a);

        // Repair HOME-menu records damaged by earlier versions (update / DLC replacing the base game).
        // With a server this is the first step of the connection bar.
        if (a.list.server.empty())
            showMessage(a, "正在檢查遊戲紀錄…", {"確認主機上每款遊戲的本體、更新、DLC 都有正確登記。"}, "");
        else
            showConnect(a, 0, 0.03, "確認主機上每款遊戲的本體、更新、DLC 都有正確登記");
        int repaired = install::repairRecords();

        if (a.list.server.empty()) {
            refresh(a);  // shows how to add a server (Y → 伺服器設定 → 新增伺服器)
        } else {
            refresh(a);
            // the server's greeting (Tinfoil-style index "success" text): only now, at start, once connected
            if (!a.greeting.empty() && !a.raw.empty()) {
                std::vector<std::string> lines;
                std::string g = a.greeting;
                for (size_t p = 0; p <= g.size() && lines.size() < 12;) {  // its own line breaks are kept
                    size_t q = g.find('\n', p);
                    if (q == std::string::npos) q = g.size();
                    std::string line = g.substr(p, q - p);
                    while (!line.empty() && line.back() == '\r') line.pop_back();
                    // a long line is cut into pieces that fit the window (at character boundaries)
                    while (a.font.measure(line, 22) > 1100) {
                        size_t cut = 0, k = 0;
                        while (k < line.size()) {
                            size_t len = 1;
                            unsigned char ch = (unsigned char)line[k];
                            if (ch >= 0xF0) len = 4; else if (ch >= 0xE0) len = 3; else if (ch >= 0xC0) len = 2;
                            if (a.font.measure(line.substr(0, k + len), 22) > 1100) break;
                            k += len;
                            cut = k;
                        }
                        if (!cut) break;
                        lines.push_back(line.substr(0, cut));
                        line = line.substr(cut);
                    }
                    lines.push_back(line.empty() ? " " : line);
                    p = q + 1;
                }
                install::log("server greeting shown (%zu line(s))", lines.size());
                askChoice(a, a.list.serverName.empty() ? std::string("伺服器") : a.list.serverName, lines, {{"A", "知道了", kGo, 1}});
            }
        }
        if (repaired) a.list.status = "已自動修復 " + std::to_string(repaired) + " 款遊戲的紀錄。" + a.list.status;
        u64 tStart = nowMs();
        autoShortcut(a);  // HOME 捷徑: created once, on the first start of this version
        u64 tSc = nowMs();
        rememberAppPath(a);
        askResume(a);
        u64 tRes = nowMs();
        checkMissingBase(a, true);  // update / DLC without its base game: offer to install the base
        install::log("start: shortcut %llu ms, resume %llu ms, missing base %llu ms", (unsigned long long)(tSc - tStart),
                     (unsigned long long)(tRes - tSc), (unsigned long long)(nowMs() - tRes));
        showList(a);
        startUpdateCheck();  // in the background; asks when a new version exists

        // Direction keys repeat while held: after 0.35 s every 70 ms, after 1.5 s faster and by
        // several rows at a time.
        const u64 kRepeatMask = HidNpadButton_AnyUp | HidNpadButton_AnyDown | HidNpadButton_AnyLeft |
                                HidNpadButton_AnyRight | HidNpadButton_L | HidNpadButton_R;
        u64 holdKey = 0, holdStart = 0, lastRepeat = 0;

        u64 lastTick = 0;
        while (appletMainLoop()) {
            padUpdate(&a.pad);
            u64 down = padGetButtonsDown(&a.pad), held = padGetButtons(&a.pad);
            auto& L = a.list;
            bool redraw = false;
            u64 now = nowMs();

            if (a.openServerMenu) {  // connecting failed, 伺服器設定 chosen there
                a.openServerMenu = false;
                openMenu(a, kMenuServer, (size_t)std::max(0, a.store.indexOf(a.store.current)));
                redraw = true;
            }

            // background installs
            pollInstalls(a);
            pollConfirm(a);
            pollShortcut(a);
            {
                std::string before = L.status;
                pollOutbox(a);
                if (L.status != before) redraw = true;
            }
            if (!g_autoNasDone && !a.showProgress && !L.menu.open && !a.screenOff) {
                autoNasCheck(a);
                if (g_autoNasDone) redraw = true;
            }
            if (g_restartNow) break;  // updated: leave, the loader starts the new version
            {   // background scan results that change the list (merged packages split, files without a TID
                // in their name join their game): applied at most every 2 s, the list is not rebuilt per file
                mutexLock(&g_tid.m);
                uint32_t gen = g_tid.structGen;
                bool scanDone = g_tid.done;
                mutexUnlock(&g_tid.m);
                static u64 lastApply = nowMs();
                // the whole list is rebuilt for it: not while reading (that made the list stutter),
                // only when the scan ends, or at most once a minute during a long first scan
                if (gen != a.tidGen && (scanDone || now - lastApply >= 60000) && !L.menu.open) {
                    a.tidGen = gen;
                    lastApply = now;
                    u64 tr = nowMs();
                    reloadFiles(a, true);
                    install::log("list rebuilt for new scan results: %llu ms", (unsigned long long)(nowMs() - tr));
                    redraw = true;
                }
                tidSetInstalling(installsActive());
            }
            if (!a.showProgress && !L.menu.open && !a.screenOff && g_updState == 2) {
                pollUpdateCheck(a);
                if (g_restartNow) break;
                present(a, [&](gfx::Canvas& c) { screens::drawList(c, a.font, a.list); });
                continue;
            }
            if (a.screenOff) {
                // screen off: installs go on; any button or touch turns it back on (and does nothing else)
                HidTouchScreenState ts = {0};
                bool touched = hidGetTouchScreenStates(&ts, 1) && ts.count > 0;
                if (down || touched) {
                    screenOn(a);
                    if (touched) { a.touching = true; a.touchStartX = a.touchStartY = -10000; }  // not a tap
                    a.yHeld = 0;
                    holdKey = 0;
                } else {
                    svcSleepThread(50000000);
                    continue;
                }
                svcSleepThread(16000000);
                continue;
            }
            if (!a.retryLater.empty() && !installsActive()) {
                // files lost to the network after all retries: ask once the batch is over
                std::vector<resume::QueueEntry> again;
                again.swap(a.retryLater);
                std::vector<std::string> lines = {"這些檔案重試幾次後仍連線失敗，已下載的部分都有保留："};
                for (size_t k = 0; k < again.size() && k < 6; k++) lines.push_back("!" + shortName(again[k].name));
                if (again.size() > 6) lines.push_back("…還有 " + std::to_string(again.size() - 6) + " 個");
                lines.push_back("要現在再試一次嗎？會從中斷的地方接著下載。");
                int pick = askChoice(a, "有 " + std::to_string(again.size()) + " 個檔案因連線問題沒裝完", lines,
                                     {{"A", "再試一次", kGo, 1}, {"B", "稍後再說", kLater, 0}});
                if (pick < 0) break;
                if (pick == 1) {
                    std::vector<Job> jobs;
                    for (const auto& e : again) {
                        Job j;
                        j.url = e.url;
                        j.name = e.name;
                        j.group = e.group;
                        j.added = e.added;
                        jobs.push_back(j);
                    }
                    size_t n = enqueue(a, jobs);
                    L.status = "重新安裝 " + std::to_string(n) + " 個連線失敗的檔案（從中斷處接續）。";
                } else {
                    L.status = "沒裝完的檔案已保留進度：下次開啟時會再詢問，或在清單中再按一次安裝。";
                }
                present(a, [&](gfx::Canvas& c) { screens::drawList(c, a.font, a.list); });
                continue;
            }
            bool active = !L.installSummary.empty();
            bool tick = active && now - lastTick >= 250;
            if (tick) lastTick = now;
            if (a.showProgress) {
                if (held & HidNpadButton_Y) {
                    if (!a.yHeld) a.yHeld = now;
                    else if (now - a.yHeld > 800) { cancelAllInstalls(true); a.yHeld = 0; }
                } else {
                    a.yHeld = 0;
                }
                // touch on the progress screen
                int ptap = -1;
                {
                    HidTouchScreenState ts = {0};
                    if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
                        a.touchX = (int)ts.touches[0].x;
                        a.touchY = (int)ts.touches[0].y;
                        if (!a.touching) { a.touching = true; a.touchStartX = a.touchX; a.touchStartY = a.touchY; }
                    } else if (a.touching) {
                        a.touching = false;
                        if (std::abs(a.touchX - a.touchStartX) < 30 && std::abs(a.touchY - a.touchStartY) < 30)
                            for (const auto& h : a.progressHits)
                                if (a.touchX >= h.x && a.touchX < h.x + h.w && a.touchY >= h.y && a.touchY < h.y + h.h) ptap = h.id;
                    }
                }
                std::vector<u64> groups;
                mutexLock(&g_run.m);
                groups = queueGroups();
                mutexUnlock(&g_run.m);
                auto selIndex = [&]() -> int {
                    auto it = std::find(groups.begin(), groups.end(), a.qSel);
                    return it == groups.end() ? -1 : (int)(it - groups.begin());
                };
                if (a.qSel && selIndex() < 0) a.qSel = 0;  // that game started or finished
                int si = selIndex();
                if (ptap >= 600 && ptap - 600 < (int)groups.size()) {  // tapped a waiting game
                    a.qSel = groups[(size_t)(ptap - 600)];
                    si = selIndex();
                }
                bool up = down & HidNpadButton_AnyUp, dn = down & HidNpadButton_AnyDown;
                if ((up || dn) && !groups.empty()) {
                    if (si < 0) si = up ? (int)groups.size() - 1 : 0;
                    else si = up ? std::max(0, si - 1) : std::min((int)groups.size() - 1, si + 1);
                    a.qSel = groups[(size_t)si];
                }
                if (a.qSel) {
                    if ((down & HidNpadButton_ZL) || ptap == 521) moveQueuedGame(a.qSel, -1);
                    else if ((down & HidNpadButton_ZR) || ptap == 522) moveQueuedGame(a.qSel, +1);
                    else if (ptap == 520) moveQueuedGame(a.qSel, INT_MIN);
                    else if ((down & HidNpadButton_X) || ptap == 523) {
                        screens::ProgressState cur = progressState(a);
                        std::string name = cur.groupCursor >= 0 ? cur.groups[(size_t)cur.groupCursor].label : "";
                        bool started = cur.groupCursor >= 0 && cur.groups[(size_t)cur.groupCursor].started;
                        std::vector<std::string> lines = {name, "排隊中的檔案會移除，不會安裝。"};
                        if (started) lines.push_back("!正在安裝的那個檔案會繼續裝完。");
                        int pick = askChoice(a, "從排隊移除這款遊戲？", lines,
                                             {{"A", "移除", kStop, 1}, {"B", "取消", kLater, 0}});
                        if (pick == 1) {
                            int at = selIndex();
                            size_t n = removeQueuedGame(a.qSel);
                            install::log("queue: removed %zu waiting file(s) of %s", n, name.c_str());
                            groups.erase(std::remove(groups.begin(), groups.end(), a.qSel), groups.end());
                            a.qSel = groups.empty() ? 0 : groups[(size_t)std::min(std::max(at, 0), (int)groups.size() - 1)];
                        }
                        if (pick < 0) break;  // the app is closing
                    }
                }
                if ((down & HidNpadButton_R) || ptap == 530) {
                    screenOff(a);
                    continue;
                }
                if (down & HidNpadButton_Plus) {
                    if (confirmExit(a)) break;
                } else if (down & (HidNpadButton_B | HidNpadButton_Minus)) {
                    a.showProgress = false;
                    a.qSel = 0;
                    present(a, [&](gfx::Canvas& c) { screens::drawList(c, a.font, a.list); });
                    svcSleepThread(16000000);
                    continue;
                }
                updateSysStatus();
                screens::ProgressState ps = progressState(a);
                present(a, [&](gfx::Canvas& c) { screens::drawProgress(c, a.font, ps); });
                a.progressHits = ps.hits;
                a.qScroll = ps.groupScroll;
                svcSleepThread(33000000);
                continue;
            }
            size_t n = L.items.size();

            u64 act = 0;
            size_t step = 1;
            bool repeat = false;
            if (down & kRepeatMask) {
                act = down & kRepeatMask;
                holdKey = act;
                holdStart = lastRepeat = now;
            } else if (holdKey && (held & holdKey)) {
                u64 heldFor = now - holdStart;
                u64 interval = heldFor > 1500 ? 30 : 70;
                if (heldFor > 350 && now - lastRepeat >= interval) {
                    act = holdKey;
                    lastRepeat = now;
                    repeat = true;
                    if (heldFor > 1500) step = 3;
                }
            } else {
                holdKey = 0;
            }

            const size_t page = screens::pageSize(L.view);
            const bool grid = L.view == screens::kViewGrid;
            if (L.menu.open) {  // menu: up/down move inside it, everything else below
                size_t mn = L.menu.items.size();
                if (act & HidNpadButton_AnyDown) { L.menu.cursor = (L.menu.cursor + 1) % mn; redraw = true; }
                else if (act & HidNpadButton_AnyUp) { L.menu.cursor = (L.menu.cursor + mn - 1) % mn; redraw = true; }
                act = 0;
            }
            if (act && n && grid) {  // cover wall: left/right one tile, up/down one row, L/R a page
                size_t before = L.cursor;
                const size_t cols = screens::kGridCols;
                if (act & HidNpadButton_AnyDown) {
                    if (L.cursor + cols < n) L.cursor += cols;
                    else if (L.cursor / cols < (n - 1) / cols) L.cursor = n - 1;  // last row is shorter
                    else if (!repeat) L.cursor %= cols;                         // bottom: back to the top
                } else if (act & HidNpadButton_AnyUp) {
                    if (L.cursor >= cols) L.cursor -= cols;
                    else if (!repeat) {  // top: to the same column of the last row
                        size_t lastRow = (n - 1) / cols * cols;
                        L.cursor = std::min(n - 1, lastRow + L.cursor);
                    }
                } else if (act & HidNpadButton_AnyRight) {
                    if (L.cursor + 1 < n) L.cursor++;
                    else if (!repeat) L.cursor = 0;
                } else if (act & HidNpadButton_AnyLeft) {
                    if (L.cursor > 0) L.cursor--;
                    else if (!repeat) L.cursor = n - 1;
                } else if (act & HidNpadButton_R) {
                    size_t next = (L.cursor / page + 1) * page;
                    if (next < n) L.cursor = next;
                    else if (!repeat) L.cursor = n - 1;
                } else if (act & HidNpadButton_L) {
                    size_t cur = (L.cursor / page) * page;
                    L.cursor = cur >= page ? cur - page : 0;
                }
                if (L.cursor != before) { redraw = true; a.lastMove = now; }
            } else if (act && n) {
                size_t before = L.cursor;
                if (act & HidNpadButton_AnyDown) {
                    if (L.cursor + step < n) L.cursor += step;
                    else L.cursor = repeat ? n - 1 : (L.cursor == n - 1 ? 0 : n - 1);
                } else if (act & HidNpadButton_AnyUp) {
                    if (L.cursor >= step) L.cursor -= step;
                    else L.cursor = repeat ? 0 : (L.cursor == 0 ? n - 1 : 0);
                } else if (act & (HidNpadButton_AnyRight | HidNpadButton_R)) {
                    size_t next = (L.cursor / page + 1) * page;
                    if (next < n) L.cursor = next;
                    else if (!repeat) L.cursor = n - 1;
                } else if (act & (HidNpadButton_AnyLeft | HidNpadButton_L)) {
                    size_t cur = (L.cursor / page) * page;
                    L.cursor = cur >= page ? cur - page : 0;
                }
                if (L.cursor != before) { redraw = true; a.lastMove = now; }
            }

            // ---- touch: a tap is a short touch that ends close to where it started
            int tapId = -1;
            {
                HidTouchScreenState ts = {0};
                bool down1 = hidGetTouchScreenStates(&ts, 1) && ts.count > 0;
                if (down1) {
                    a.touchX = (int)ts.touches[0].x;
                    a.touchY = (int)ts.touches[0].y;
                    if (!a.touching) { a.touching = true; a.touchStartX = a.touchX; a.touchStartY = a.touchY; }
                } else if (a.touching) {
                    a.touching = false;
                    if (std::abs(a.touchX - a.touchStartX) < 30 && std::abs(a.touchY - a.touchStartY) < 30) {
                        tapId = -2;  // tapped, but on nothing
                        for (const auto& h : L.hits)
                            if (a.touchX >= h.x && a.touchX < h.x + h.w && a.touchY >= h.y && a.touchY < h.y + h.h) tapId = h.id;
                    }
                }
            }

            if (L.menu.open) {
                if (tapId >= 400 && tapId < 400 + (int)L.menu.items.size()) {
                    L.menu.cursor = (size_t)(tapId - 400);
                    menuActivate(a);
                } else if (tapId == 300) {
                    menuBack(a);
                } else if (tapId != -1 && a.touchX < screens::kWidth - 470) {
                    closeMenu(a);  // tapped the darkened list
                } else if (down & HidNpadButton_A) {
                    menuActivate(a);
                } else if (down & HidNpadButton_B) {
                    menuBack(a);
                } else if (down & (HidNpadButton_Y | HidNpadButton_Plus)) {
                    closeMenu(a);
                }
                if (tapId != -1 || (down & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_Y | HidNpadButton_Plus)))
                    redraw = true;
            } else if (down & HidNpadButton_Plus) {
                if (confirmExit(a)) break;
                redraw = true;
            } else if (tapId == 212) {  // server name in the title: switch / manage servers
                openMenu(a, kMenuServer, (size_t)std::max(0, a.store.indexOf(a.store.current)));
                redraw = true;
            } else if (tapId == 211) {
                showMessage(a, "從相簿開啟（記憶體較少）",
                            {"從相簿開啟時，主機只給本程式一小部分記憶體。",
                             "大型 NSZ／XCZ 解壓縮需要較多記憶體，可能會裝不起來，或要等其他檔案裝完才單獨安裝。",
                             "建議：在 HOME 選單按住 R 鍵開啟任一遊戲，進入 hbmenu 後再開啟本程式，可用記憶體會多好幾倍。"},
                            "知道了");
                waitForConfirm(a);
                redraw = true;
            } else if (tapId == 210 || ((down & HidNpadButton_Minus) && (active || !a.recent.empty()))) {
                a.showProgress = true;  // installs keep going; B comes back here
                a.progressSince = now;
                a.qSel = 0;
                continue;
            } else if (tapId >= 1000 && tapId - 1000 < (int)n) {  // tap a row: select it (and move there)
                size_t v = (size_t)(tapId - 1000);
                size_t i = a.view[v];
                if (a.selected[i]) a.selected[i] = false;
                else trySelect(a, i, false);  // only when it fits on the SD card
                L.items[v].selected = a.selected[i];
                updateSelInfo(a);
                if (L.cursor != v) { L.cursor = v; a.lastMove = now; }
                redraw = true;
            } else if (tapId >= 100 && tapId < 104) {  // tab
                if (a.category != tapId - 100) { a.category = tapId - 100; rebuildView(a, false, false); }
                redraw = true;
            } else if (tapId == 200) {
                openMenu(a, kMenuSort, (size_t)a.sort);
                redraw = true;
            } else if (tapId == 201) {
                a.search.clear();
                rebuildView(a, false, false);
                redraw = true;
            } else if ((down & HidNpadButton_A) && n) {
                size_t i = a.view[L.cursor];
                if (a.selected[i]) a.selected[i] = false;
                else trySelect(a, i, false);  // only when it fits on the SD card
                L.items[L.cursor].selected = a.selected[i];
                updateSelInfo(a);
                redraw = true;
            } else if (down & (HidNpadButton_ZL | HidNpadButton_ZR)) {
                a.category = (a.category + ((down & HidNpadButton_ZR) ? 1 : 3)) % 4;
                rebuildView(a, false, false);  // same games, another tab: no recomputing
                redraw = true;
            } else if (down & HidNpadButton_Y) {
                openMenu(a, kMenuMain);
                redraw = true;
            } else if (down & HidNpadButton_B) {
                // only back to the screen shown at start; reading the server again is Y → 重新整理
                if (!isInitialView(a)) {
                    a.search.clear();
                    a.category = 0;
                    rebuildView(a, false, false);
                    L.status = "已回到全部檔案。";
                    redraw = true;
                }
            } else if (down & HidNpadButton_X) {
                runInstall(a);
                redraw = true;
            }

            if (a.db) {
                // new data (cache loaded, or a background download finished): dates and covers again
                uint32_t gen = a.db->generation();
                if (gen != a.dbGen) {
                    a.dbGen = gen;
                    bool split = false;
                    for (const auto& f : a.files) split = split || f.onlyTitle;
                    u64 tr = nowMs();
                    if (split) reloadFiles(a, true);  // their names come from the titledb
                    else {
                        applyTitleDb(a);
                        rebuildView(a);
                    }
                    install::log("list rebuilt for new game database data: %llu ms", (unsigned long long)(nowMs() - tr));
                    if (a.covers) a.covers->retryMissing();
                    redraw = true;
                }
                std::string m = a.db->message();
                if (!m.empty() && m != a.dbShown) { a.dbShown = m; L.status = m; redraw = true; }
            }
            L.marqueeMs = now - a.lastMove;
            if (updateCover(a)) redraw = true;  // cover finished loading
            if (!L.items.empty() && L.cursor < L.items.size() && L.items[L.cursor].versionLine.empty()) {
                updateVersionLine(a);
                if (!L.items[L.cursor].versionLine.empty()) redraw = true;
            }
            if (tick) redraw = true;  // the install pill counts down
            if (updateSysStatus()) redraw = true;  // clock / network in the header
            if (redraw || L.animating) present(a, [&](gfx::Canvas& c) { screens::drawList(c, a.font, a.list); });
            svcSleepThread(16000000);
        }
        if (installsActive()) {  // the app is closing (HOME menu / power): stop, progress stays saved
            saveQueueNow();
            g_exiting = true;
            cancelAllInstalls();
            waitInstallsStopped(a);
        }
        boostStop();   // clocks back to normal
        screenOn(a);   // never leave the backlight off
        a.list.cover = screens::CoverView();
        a.shownCover.reset();
        a.covers.reset();  // stop the cover thread before networking shuts down
        a.db.reset();
    }

    if (g_updState != 0) {  // the update check may still be talking to the network
        threadWaitForExit(&g_updThread);
        threadClose(&g_updThread);
    }
    outboxStop();
    joinServerTest();
#ifdef HAVE_SHORTCUT
    if (g_scUsed) {  // the shortcut is being rebuilt: let it finish (a few seconds at most)
        threadWaitForExit(&g_scThread);
        threadClose(&g_scThread);
        g_scUsed = false;
    }
#endif
    stopConfirm();
    stopTidScan();
    install::exitServices();
    http::tlsExit();
    nifmExit();
    socketExit();
    framebufferClose(&a.fb);
    plExit();

    // Running as an application (HOME-menu forwarder, or a game launched while holding R): let libnx
    // close it through the system (ISelfController::Exit) and go back to the HOME menu. Returning to
    // the homebrew loader fails on some forwarders (2347-0003), and ending the process directly with
    // svcExitProcess is treated by the system as a crash ("the software was closed because an error
    // occurred"). Applet mode (album) keeps returning to hbmenu as usual.
    AppletType at = appletGetAppletType();
    // started as an application (HOME shortcut, a game): + goes back to the HOME menu. Not after an update:
    // then the loader must get control back to start the new version.
    if ((at == AppletType_Application || at == AppletType_SystemApplication) && !g_restartNow) __nx_applet_exit_mode = 1;
    return 0;
}
