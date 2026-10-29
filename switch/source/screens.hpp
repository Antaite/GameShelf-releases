#pragma once
// All screens of the app, drawn into a 1280x720 canvas. Portable (no libnx) so they can be
// rendered and inspected on a desktop.
#include <cstdint>
#include <string>
#include <vector>

#include "gfx.hpp"
#include "ime.hpp"

namespace screens {

constexpr int kWidth = 1280, kHeight = 720;
constexpr int kListRows = 14;
// How the file list is shown (menu 檢視方式).
enum View { kViewList = 0, kViewGrid = 1, kViewCompact = 2 };
constexpr int kGridCols = 8, kGridRows = 4;
// Games per page in a view: 14 in the lists, 32 on the cover wall.
inline size_t pageSize(int view) { return view == kViewGrid ? (size_t)(kGridCols * kGridRows) : (size_t)kListRows; }

enum class CoverState { None, Loading, Ready, Missing };

struct CoverView {
    CoverState state = CoverState::None;
    int w = 0, h = 0;
    const uint32_t* px = nullptr;  // RGBA8888, valid while drawing
    uint64_t titleId = 0;          // 0 = no title id in the file name
};

enum class Badge { None, Installed, Update, Dlc, Reinstall, Queued, Installing };

struct ListItem {
    std::string name;   // what the list shows (Chinese title when known, else the file name)
    uint64_t size;
    bool selected;
    Badge badge = Badge::None;
    std::string file;   // server path, used for the details line ("" = same as name)
    uint32_t releaseDate = 0;  // YYYYMMDD, 0 = unknown
    std::string versionLine;   // e.g. "主機 v1.0.0  →  伺服器 v1.0.3" ("" = hide)
    std::string contentsLine;  // e.g. "包含：本體、更新 v1.7.0、DLC 12 個" ("" = hide)
    std::string version;       // newest version on the server, for the compact list ("v1.2.1"), "" = unknown
    uint64_t titleId = 0;      // base title id (cover wall), 0 = none in the file name
};

// Pop-up menu on the right (Y). Touch and buttons both drive it.
struct MenuItem {
    std::string label;
    std::string value;     // shown on the right, dim ("" = none)
    bool submenu = false;  // draws "›"
    bool checked = false;  // draws a check mark (sort choices)
    bool enabled = true;
};

struct MenuState {
    bool open = false;
    std::string title;     // "選單", "排序方式"
    bool child = false;    // a submenu: shows "‹ 返回"
    std::vector<MenuItem> items;
    size_t cursor = 0;
    size_t scroll = 0;     // first visible item (kept around the cursor by drawList)
};

// Touch targets reported back by drawList, so input can hit-test what was drawn.
struct HitBox { int x, y, w, h, id; };

struct ListState {
    std::string version;
    std::string server;  // empty = not set
    std::string serverName;  // name of the server in use, shown in the title (tap: hit id 212)
    std::vector<ListItem> items;
    size_t cursor = 0;
    size_t scroll = 0;   // adjusted by drawList: always the first row of the cursor's page
    std::string status;
    CoverView cover;     // cover of the file under the cursor
    int view = kViewList;            // kViewList / kViewGrid / kViewCompact
    std::vector<CoverView> covers;   // cover wall: one per game on the cursor's page (index - scroll)
    size_t updates = 0, dlcs = 0;  // installable updates / DLC found by the comparison
    int category = 0;              // tab: 0 全部, 1 可更新, 2 可裝 DLC, 3 已安裝
    size_t tabCounts[4] = {0, 0, 0, 0};
    uint64_t sdFree = 0;           // free bytes on the SD card, 0 = unknown
    MenuState menu;
    std::string installSummary;    // e.g. "安裝中 2 · 排隊 3 · 9.8 MB/s" ("" = nothing installing)
    std::string notice;            // yellow header pill, e.g. "相簿模式" (hit id 211), "" = none
    std::vector<HitBox> hits;      // filled by drawList
    std::string sortLabel;         // e.g. "名稱", "" = hide
    std::string search;            // active search text, "" = none
    uint64_t selNeed = 0;          // space the selected games take once installed (0 = use the item sizes)
    int64_t selAvail = -1;         // SD space left after them (1 GB kept for the system), -1 = unknown
    uint64_t marqueeMs = 0;  // time since the cursor last moved, drives scrolling long names
    bool animating = false;  // set by drawList: true while a marquee is scrolling
};

// One install running in parallel with others.
struct ProgressLane {
    bool active = false;
    std::string file;
    std::string stage;
    uint64_t done = 0, total = 0;
    double speedMBs = 0;
};

struct ProgressState {
    size_t finished = 0, count = 0;  // files done (ok or failed) / files queued
    std::vector<ProgressLane> lanes;  // one per worker
    bool cancelling = false;
    bool discarding = false;          // the cancel deletes what was downloaded
    bool background = false;          // shown on top of the list: B goes back, installs go on
    struct WaitGroup {
        std::string label;  // game name
        size_t files = 0;   // its files still waiting
        bool started = false;  // one of its files is installing right now
    };
    std::vector<WaitGroup> groups;    // waiting games, in install order
    int groupCursor = -1;             // selected waiting game (-1 = none)
    size_t groupScroll = 0;           // first visible row, kept in view by drawProgress
    std::vector<std::string> recent;  // last finished files ("○ name" / "× name")
    std::vector<bool> recentOk;
    std::vector<std::string> recentDetail;  // reason (failed) or note, shown in full under the name
    std::vector<bool> recentWarn;     // installed, with a problem: detail in yellow
    uint64_t marqueeMs = 0;           // drives scrolling of long names
    // filled by drawProgress
    std::vector<HitBox> hits;  // 530 關閉螢幕, 600+i waiting game i, 520 置頂, 521 上移, 522 下移, 523 移除
    bool animating = false;
};

// A question with big buttons (A / X / B …), also tappable.
struct ChoiceOption {
    std::string key;    // "A", "X", "B"
    std::string label;  // "繼續安裝"
    uint32_t color;     // button colour
    int id;             // reported in hits
};

struct ResultState {
    int ok = 0;
    std::vector<std::string> failures;
    std::vector<std::string> notes;
};

// Clock + network shown at the top right of every screen.
struct SysStatus {
    std::string clock;  // "23:57", "" = hide
    int net = -2;       // -2 unknown, -1 no network, 0..3 Wi-Fi strength, 4 wired
    bool connecting = false;
};
void setSysStatus(const SysStatus& st);
const int kStatusW = 150;  // width the status takes at the right of the header

// Built-in 注音 / 倉頡 / 英數 keyboard for the search.
struct ImeState {
    ime::Mode mode = ime::Mode::Zhuyin;
    std::string text;       // search text so far
    std::string composing;  // 注音 symbols / 倉頡 letters not yet turned into a character
    std::vector<std::string> cands;
    size_t candSel = 0;
    int row = 0, col = 0;   // key under the cursor
    size_t matches = 0, total = 0;  // games matching `text` / all games
    bool zhuyinGlyphs = true;       // the font can draw 注音 symbols (else letters are shown)
    std::vector<HitBox> hits;       // 700 + row * 20 + col: key, 900 + i: candidate
};
void drawIme(gfx::Canvas& c, gfx::Font& f, ImeState& s);

// 詳細管理遊戲內容: what is installed on the console, biggest first.
struct ManageRow {
    std::string name;      // the console's name for the game
    std::string contents;  // e.g. "本體 · 更新 v1.2.1 · DLC 3 個"
    uint64_t size = 0;
    bool notOnNas = false; // no file of this game on the server in use
    bool checking = false; // not decided yet: the server's real TIDs are still being read
    bool noBase = false;   // only updates / DLC installed: the game cannot start
    uint64_t titleId = 0;
    std::vector<std::pair<std::string, uint64_t>> parts;  // side panel: "本體" 6.6 GB, "更新 v3.0.1" 1.6 GB…
};
struct ManageState {
    std::string version;
    std::vector<ManageRow> rows;
    size_t cursor = 0, scroll = 0;
    uint64_t total = 0, sdFree = 0;
    std::string sortLabel;   // "佔用空間" / "名稱"
    std::string checkText;   // "123 / 850" while the server's real TIDs are being read, "" when done
    std::string status;
    CoverView cover;         // of the game under the cursor
    MenuState menu;          // choosing what to delete (base / updates / DLC)
    std::vector<HitBox> hits;
    uint64_t marqueeMs = 0;
    bool animating = false;
};
void drawManage(gfx::Canvas& c, gfx::Font& f, ManageState& s);

void drawList(gfx::Canvas& c, gfx::Font& f, ListState& s);
void drawProgress(gfx::Canvas& c, gfx::Font& f, ProgressState& s);
void drawResult(gfx::Canvas& c, gfx::Font& f, const ResultState& s);
void drawChoice(gfx::Canvas& c, gfx::Font& f, const std::string& title, const std::vector<std::string>& lines,
                const std::vector<ChoiceOption>& options, std::vector<HitBox>& hits);
// Connecting to the server (at start and on 重新整理): a bar that is full once the server has
// answered and its list is read and compared.
struct ConnectState {
    std::string server;      // the server's name (never its address)
    std::string version;     // "v3.0.12"
    int stage = 0;           // 0 records, 1 connect, 2 list, 3 compare, 4 done
    double percent = 0;      // 0..1
    std::string detail;      // e.g. "已讀到 2,310 個檔案"
    bool failed = false;     // the current stage failed: A retry / Y server settings / B go on without
};
void drawConnect(gfx::Canvas& c, gfx::Font& f, const ConnectState& s);
// Generic centred message (startup errors, "searching…").
void drawMessage(gfx::Canvas& c, gfx::Font& f, const std::string& title, const std::vector<std::string>& lines,
                 const std::string& hint);

std::string humanSize(uint64_t bytes);
// Splits "folder/Name [0100…][v0].nsp" into a readable title and a details line.
void splitName(const std::string& file, std::string& title, std::string& details);

}  // namespace screens
