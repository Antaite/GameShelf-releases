#pragma once
// 壞檔自動回報: when a file from the author's NAS turns out to be broken (a problem only replacing
// the file fixes), a short note goes to the author's phone through ntfy. Portable (no libnx).
//
// Sent only when BOTH hold: the install ran in 遊戲書架, and the broken file's own address is the
// author's server. That address is not in the program: only its SHA-256 ("fingerprint") is, so
// the public .nro does not show it. A report has the file name, the game id and what was wrong:
// never a server address, account, password or anything about the console.
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace badfile {

// SHA-256 (hex) of "host:port", host in lower case: what is compared with the fingerprint.
std::string fingerprint(const std::string& host, int port);
// The file at `url` comes from the author's server (host case-insensitive, same port).
bool fromReportServer(const std::string& url);
// The error means the file itself is broken (not the network, the SD card or memory).
// `kind` gets a short description for the report.
bool isBadFile(const std::string& err, std::string& kind);
// The file name in `url` (last path part, %-decoded, without "?query").
std::string fileName(const std::string& url);
// ntfy JSON body (topic, title, message).
std::string ntfyJson(const std::string& topic, const std::string& file, uint64_t titleId, const std::string& kind);

// 「NAS 上沒有」: installed games the author's server has no file for (name + TID only).
// Only real games count: their id starts with 01 (homebrew / forwarders do not).
inline bool isOfficialTitle(uint64_t appId) { return (appId >> 56) == 0x01; }
std::string ntfyMissingJson(const std::string& topic, const std::vector<std::pair<uint64_t, std::string>>& games);
// The NAS check after comparing real TIDs: games not on the NAS and/or files whose name has a wrong
// TID ("<file>：檔名 TID X，實際 Y" lines). Either list may be empty.
std::string ntfyNasCheckJson(const std::string& topic, const std::vector<std::pair<uint64_t, std::string>>& missing,
                             const std::vector<std::string>& wrongNames);
// The same, cut into several notes when it is too long for one (ntfy: 4,096 bytes of text).
std::vector<std::string> ntfyNasCheckJsons(const std::string& topic, const std::vector<std::pair<uint64_t, std::string>>& missing,
                                           const std::vector<std::string>& wrongNames);
// One ntfy note per part of `msg` (cut at line breaks, at most `maxBytes` each; "（1/2）" in the title).
std::vector<std::string> ntfyBodies(const std::string& topic, const std::string& title, const std::string& msg,
                                    const std::string& tag, size_t maxBytes = 3500);
std::string tidKey(uint64_t appId);  // "0100ABCD12345000"
// How often each problem was reported, per console: `listPath` holds "key<TAB>times" lines.
// Keys: "<file name>|<kind>" for file problems (kinds below), the TID for 「NAS 上沒有」.
// Each key is reported at most kMaxReports times; a file that installs cleanly later (or a game
// found on the NAS later) starts again from zero. Lines of older versions (no count) count once.
constexpr int kMaxReports = 2;
extern const char* const kKindBad;       // "bad": broken file, the install failed
extern const char* const kKindMismatch;  // "mismatch": hash differs, installed anyway
extern const char* const kKindTicket;    // "ticket": needs a ticket nobody has
extern const char* const kKindTidName;   // "tidname": the TID in the file name is not the real one
std::string fileKey(const std::string& file, const char* kind);
int reportedTimes(const std::string& listPath, const std::string& key);
std::map<std::string, int> reportedCounts(const std::string& listPath);
void bumpReported(const std::string& listPath, const std::vector<std::string>& keys);
// Removes the keys, and for a file name every "<file>|kind" key of it.
void clearReported(const std::string& listPath, const std::vector<std::string>& keys);

extern const char* const kNtfyUrl;    // "https://ntfy.sh/"
extern const char* const kNtfyTopic;  // the author's topic

}  // namespace badfile
