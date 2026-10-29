#pragma once
// Minimal HTTP/1.1 client over BSD sockets (works on libnx and on desktop for testing).
// Supports: Basic auth from "http://user:pass@host/", Range, chunked responses, redirects,
// and arbitrary methods (WebDAV PROPFIND).
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace http {

struct Url {
    std::string host;
    int port = 80;
    bool tls = false;         // https://
    std::string path = "/";   // as given (may contain raw or %-encoded characters)
    std::string userinfo;     // "user:pass" exactly as typed (may be %-encoded), empty if none
    std::string authHeader;   // "Basic xxx" or empty
    std::string origin() const;  // "http(s)://[userinfo@]host:port"
};

bool parseUrl(const std::string& url, Url& out);

// Called with each received body chunk. Return false to abort the transfer.
using Sink = std::function<bool(const uint8_t* data, size_t size)>;

struct Response;

struct Request {
    std::string method = "GET";
    std::vector<std::string> headers;  // extra "Name: value" lines
    std::string body;
    uint64_t rangeStart = 0, rangeLen = 0;  // rangeLen 0 = no Range header
    // The URL is already encoded (has a "?query" with %-escapes): sent as it is, not escaped again.
    bool encodedUrl = false;
    // Called once the final (post-redirect) headers are in; return false to stop before the body.
    std::function<bool(const Response&)> onHeaders;
};

struct Response {
    int status = 0;
    std::map<std::string, std::string> headers;  // lower-case names
};

// Low level: performs one request (following up to 3 redirects), streams the body into `sink`.
// Returns false on network/protocol errors or if the sink aborted; `res` is filled once headers arrive.
bool request(const std::string& url, const Request& req, const Sink& sink, Response& res, std::string& err);
bool requestToMemory(const std::string& url, const Request& req, std::vector<uint8_t>& out, Response& res,
                     std::string& err);

// GET url. If rangeLen > 0 the server must answer 206 with exactly rangeLen bytes, else 200.
bool get(const std::string& url, uint64_t rangeStart, uint64_t rangeLen, const Sink& sink, std::string& err);
bool getToMemory(const std::string& url, uint64_t rangeStart, uint64_t rangeLen,
                 std::vector<uint8_t>& out, std::string& err);

// Several small range reads of the same server over one kept-alive connection (no new TCP + TLS
// handshake per read). Reopens the connection when the server closed it; anything unusual
// (redirect, chunked answer) falls back to getToMemory. One Session per thread.
class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    // Same rules as getToMemory with rangeLen > 0: the server must answer 206 with exactly `len` bytes.
    bool getRange(const std::string& url, uint64_t off, uint64_t len, std::vector<uint8_t>& out, std::string& err);
    // A whole small file (at most `limit` bytes). `status` is the HTTP status (0 = no answer).
    bool get(const std::string& url, size_t limit, std::vector<uint8_t>& out, int& status, std::string& err,
             bool encodedUrl = false);
    void close();
    // From another thread: makes a read in progress return at once and refuses new ones (until resume()).
    void interrupt();
    void resume();

private:
    struct Impl;
    Impl* d_;
};

// Time limit for waiting on data, for requests made by the calling thread (0 = default 15 s).
// Connecting (name lookup + TCP) is always limited to 5 s.
void setThreadTimeout(int ms);
int threadTimeout();
// Asked before every new connection: false = no network (fails at once with "Switch 沒有連上網路").
void setNetCheck(std::function<bool()> fn);

// Size of a remote file (via "Range: bytes=0-0"), without downloading it.
bool probeSize(const std::string& url, uint64_t& size, std::string& err);

// "mem://<name>" addresses served from memory (e.g. the HOME shortcut package built on the console,
// installed through the normal installer). nullptr removes the name.
void setMemoryFile(const std::string& name, std::shared_ptr<const std::vector<uint8_t>> data);

std::string urlJoin(const std::string& base, const std::string& rel);
std::string percentDecode(const std::string& s);
// %-encodes characters that may not appear raw in a request path; existing %XX are kept.
std::string encodePath(const std::string& path);
// Hides the password part of a URL for display.
std::string redact(const std::string& url);
std::string statusMessage(int status);

// HTTPS support (Switch: system ssl service, desktop: OpenSSL). Certificates are NOT verified,
// because NAS boxes almost always use self-signed certificates. Call once at start / exit.
bool tlsInit();
// Which TLS code HTTPS uses right now: "mbedTLS" or "system" (for install.log).
const char* tlsBackend();
// TLS suite of the latest in-process (mbedTLS) connection, "" if none / system TLS.
std::string tlsCipher();
// Time the calling thread spent waiting inside socket reads (network), for the speed analysis.
uint64_t threadSocketWaitNs();
// Time the calling thread spent inside TLS reads (socket + decryption) and how many socket reads it made.
uint64_t threadTlsReadNs();
uint64_t threadSocketCalls();
void resetThreadSocketWait();
void tlsExit();

}  // namespace http
