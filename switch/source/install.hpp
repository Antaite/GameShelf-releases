#pragma once
#include <switch.h>

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "titles.hpp"

namespace install {

struct Progress {
    std::string stage;      // e.g. "Installing 0123...abc.nca"
    uint64_t done = 0;      // bytes downloaded so far
    uint64_t total = 0;     // bytes to download for this file
    double speedMBs = 0.0;
    uint64_t received = 0;  // bytes really received in this attempt (for the total speed)
};

// Called frequently during install; return false to cancel.
using ProgressFn = std::function<bool(const Progress&)>;

// What an install noticed that does not stop it (shown in yellow, reported to the author).
struct Findings {
    std::vector<std::string> mismatched;     // NCAs installed although their hash differs from the content meta
    std::vector<std::string> missingTicket;  // rights ids the game needs, in neither the package nor the console
};

struct Options {
    NcmStorageId storage = NcmStorageId_SdCard;
    bool verifyHashes = true;
    // NCA header key (from prod.keys). When set and convertGamecard is true, NCAs whose header says
    // "GameCard" (XCI/XCZ dumps) are rewritten to "Download" so they run without the cartridge.
    const uint8_t* headerKey = nullptr;
    bool convertGamecard = true;
    // Other files of the same game are installing at the same time or still queued: skip the
    // "install the update first" style warnings, the final state is not known yet.
    bool siblingsBusy = false;
    // Keep progress on the SD card so an interrupted install can continue later.
    bool keepProgress = true;
    std::string displayName;  // for the "continue?" prompt
    Findings* findings = nullptr;  // filled when set
    // Titles inside the package not to install (a merged package whose update is older than the one
    // installed from another file, …). Besides, an update / DLC older than the one on the console is
    // never installed over it.
    std::set<uint64_t> skipTitles;
    // Bytes of the one title installed from a merged package ("…#gs-title=TID"), for the progress.
    uint64_t onlyTotal = 0;
};

// "<package url>#gs-title=<TID>" (one title of a merged package): the package URL, `onlyTitle` set.
// Any other URL comes back as it is, `onlyTitle` 0.
std::string splitTitleUrl(const std::string& url, uint64_t& onlyTitle);

// Installs that stopped part-way (cancelled, quit, connection lost) and can continue.
struct PendingInstall {
    std::string path;  // state file
    std::string url, name;
    int percent = 0;
    uint64_t total = 0;
    uint64_t kept = 0;     // bytes already on the SD card
    int64_t started = 0;   // unix time of the first attempt, 0 = unknown
    bool wiped = false;    // its data on the SD card is gone (the system cleared it): starts over
};
std::vector<PendingInstall> pendingInstalls();
// Forget one: frees its half-written data on the SD card. Returns the bytes freed.
uint64_t discardPending(const std::string& path);
// Same, by the package URL (after a cancel). 0 when there was nothing.
uint64_t discardPendingUrl(const std::string& url);

Result initServices();

// Reads every installed base game, update and DLC (SD card + system memory) from ncm.
// Header key read from the user's own prod.keys on the SD card (null when not found). Loaded once.
const uint8_t* headerKey();
// Any other key from the same prod.keys (e.g. "key_area_key_application_00").
bool prodKey(const char* name, uint8_t* out, size_t len);
// An application (base title) with this id is installed (SD card or system memory).
bool hasApplication(u64 appId);
// Removes an application completely (contents + HOME-menu record), save data kept. Done through
// ncm, never ns: the system's own delete also wipes every unfinished download on the SD card.
Result deleteApplication(u64 appId);

// One installed piece of a game: the base, one update version or one DLC.
struct ContentPiece {
    u64 id = 0;
    u32 version = 0;
    u8 type = 0;      // NcmContentMetaType: 0x80 base, 0x81 update, 0x82 DLC
    u8 storage = 0;   // NcmStorageId
    uint64_t size = 0;
};
struct InstalledGame {
    u64 appId = 0;
    std::vector<ContentPiece> pieces;  // base, updates (old to new), DLC
    uint64_t total = 0;
};
// Every installed game (SD card + system memory) with its pieces and sizes. Not the HOME shortcut.
std::vector<InstalledGame> listInstalledGames();
// One game only (after deleting part of it). False when nothing of it is installed any more.
bool listInstalledGame(u64 appId, InstalledGame& out);
// Name (繁體中文 first) and display version ("1.0.3") the console has for an installed game, in one read.
bool installedControl(u64 appId, std::string& name, std::string& displayVersion);
// Deletes the given pieces of one game (the base takes the whole game with it), rewrites its HOME
// record and launch version. `freed` = bytes deleted. Unfinished downloads are not touched.
bool deletePieces(u64 appId, const std::vector<ContentPiece>& pieces, uint64_t& freed, std::string& err);
// Deletes the game's save data (all users). Returns how many were deleted.
int deleteSaveData(u64 appId);
// Makes the HOME menu read a title's name / icon / version again (after it was reinstalled).
void refreshAppInfo(u64 appId);

// Base games installed from a cartridge dump without conversion (their program NCA still says
// "GameCard"). Updates and DLC do not start on top of those. Needs the header key.
bool isGamecardInstall(u64 appId);
void scanGamecardInstalls(const titles::Installed& installed, std::set<u64>& out);

bool scanInstalled(titles::Installed& out);

// Checks every installed game and rebuilds its HOME-menu record (base + update + DLC) when entries
// are missing, e.g. after an older version of this app replaced it with only the update.
// Returns how many games were repaired.
int repairRecords();

// Loads caches used by installs (header key, system version). Call before starting worker threads.
void prepare();
uint64_t sdFreeBytes();                      // 0 = unknown
std::string installedDisplayVersion(u64 appId);  // e.g. "1.0.3" from the installed control data, "" = unknown
// Lines logged from the calling thread get a "[tag]" prefix (0 = none). For parallel installs.
void setLogTag(int tag);
// CPU core for the download thread of installs started from this thread (-2 = default core).
void setReaderCore(int core);
// Downloaded bytes each install may hold in memory ahead of the SD card (default 8 MB).
void setDownloadBuffer(size_t bytes);

// Appends a line to sdmc:/config/lan-installer/install.log.
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void exitServices();

// Installs an .nsp / .nsz / .xci / .xcz served at `url`.
// On failure `err` explains why; `note` may carry a non-fatal remark on success.
bool installPackage(const std::string& url, const Options& opt, const ProgressFn& progress, std::string& err,
                    std::string& note);

}  // namespace install
