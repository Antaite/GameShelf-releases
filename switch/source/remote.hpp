#pragma once
// Finding installable files on the server. Three kinds of server are understood, tried in order:
//   1. server.py from this project            (GET /api/list.txt)
//   1b. a Tinfoil-style JSON index             ({"files":[{"url":...,"size":...}], "directories":[...]})
//   2. WebDAV (Synology, QNAP, ASUSTOR, ...)   (PROPFIND, recursive)
//   3. a plain web directory listing           (nginx / Apache / lighttpd autoindex, python -m http.server)
// Credentials may be given in the URL: http://user:password@192.168.1.20:5005/games/
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace remote {

enum class ServerKind { None, LanServer, JsonIndex, WebDav, DirectoryListing };

struct RemoteFile {
    std::string url;          // full download URL (credentials included when the server URL had them)
    std::string displayName;  // decoded path relative to the server URL
    uint64_t size = 0;
    // A title inside a merged package (base + update + DLC in one file), listed as a file of its own:
    // url = "<package url>#gs-title=<TID>", size = its bytes once installed. Empty pkgUrl: a real file.
    std::string pkgUrl, pkgName;
    uint64_t pkgSize = 0;
    uint64_t onlyTitle = 0;
};

bool fetchList(const std::string& serverUrl, std::vector<RemoteFile>& out, ServerKind& kind, std::string& err);
const char* kindName(ServerKind k);
// The greeting the server put in its file list ("success" of a Tinfoil-style JSON index) during the
// last fetchList on this thread; "" when there was none.
std::string lastListMessage();

// What a fetchList running on another thread has done so far (for a progress bar).
struct ListProgress {
    std::atomic<bool> connected{false};  // the server answered the first request
    std::atomic<size_t> files{0};        // installable files found so far
};
// fetchList calls on this thread report to `p` (nullptr = none).
void setListProgress(ListProgress* p);

// Broadcasts on the LAN and returns the first server.py URL that answers ("" if none).
std::string discover(int timeoutMs = 2000);

std::string loadServer(const std::string& configPath);
bool saveServer(const std::string& configPath, const std::string& url);
// Generic "key=value" settings in the same file (other lines are preserved when saving).
std::string loadSetting(const std::string& configPath, const std::string& key, const std::string& fallback);
bool saveSetting(const std::string& configPath, const std::string& key, const std::string& value);

// Exposed for tests.
bool isInstallable(const std::string& name);
struct DavEntry { std::string href; uint64_t size; bool collection; };
std::vector<DavEntry> parsePropfind(const std::string& xml);
std::vector<std::string> parseHtmlLinks(const std::string& html);
struct JsonIndex {
    struct File { std::string url; uint64_t size; };
    std::vector<File> files;
    std::vector<std::string> directories;
    std::string message;  // "success" / "error" text, if any
    bool isError = false;
};
// Returns false if `text` is not a JSON object with a "files" or "directories" array.
bool parseJsonIndex(const std::string& text, JsonIndex& out);

}  // namespace remote
