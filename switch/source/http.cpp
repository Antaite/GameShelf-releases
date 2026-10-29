#include "http.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <vector>
#include <chrono>
#include <condition_variable>
#include <new>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#ifdef LANINST_MBEDTLS
// In-process TLS (like Tinfoil / Sphaira via curl): much faster than sending every TLS record
// through the system ssl service. The system service stays as the fallback.
#include <mbedtls/aes.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#if defined(MBEDTLS_PSA_CRYPTO_C)
#include <psa/crypto.h>
#endif
#ifndef __SWITCH__
#include <sys/random.h>
#endif
#endif

#ifdef __SWITCH__
#include <switch.h>
#else
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace http {

static const int kTimeoutMs = 15000;   // waiting for data (downloads: a slow NAS is still fine)
static const int kConnectMs = 5000;    // name lookup + TCP connect: a wrong address answers fast or never

static thread_local int t_timeoutMs = 0;  // 0 = kTimeoutMs
static int readTimeoutMs() { return t_timeoutMs > 0 ? t_timeoutMs : kTimeoutMs; }
void setThreadTimeout(int ms) { t_timeoutMs = ms; }
int threadTimeout() { return readTimeoutMs(); }

static std::mutex g_netCheckLock;
static std::function<bool()> g_netCheck;
void setNetCheck(std::function<bool()> fn) {
    std::lock_guard<std::mutex> lk(g_netCheckLock);
    g_netCheck = std::move(fn);
}
static bool netOk() {
    std::function<bool()> fn;
    {
        std::lock_guard<std::mutex> lk(g_netCheckLock);
        fn = g_netCheck;
    }
    return !fn || fn();
}

static thread_local uint64_t t_sockWaitNs = 0;
static thread_local uint64_t t_tlsReadNs = 0;  // inside TLS reads (network + decryption)
static thread_local uint64_t t_sockCalls = 0;  // socket reads (each one is a system call)
uint64_t threadSocketWaitNs() { return t_sockWaitNs; }
uint64_t threadTlsReadNs() { return t_tlsReadNs; }
uint64_t threadSocketCalls() { return t_sockCalls; }
void resetThreadSocketWait() { t_sockWaitNs = 0; t_tlsReadNs = 0; t_sockCalls = 0; }

// Measures the time of one blocking socket / ssl-service read.
struct SockTimer {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    explicit SockTimer(bool isRead = true) { if (isRead) t_sockCalls++; }
    ~SockTimer() {
        t_sockWaitNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    }
};
struct TlsTimer {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~TlsTimer() {
        t_tlsReadNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    }
};

// ------------------------------------------------------------------ URL helpers
static std::string base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i + 2 < in.size()) {
        uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        uint32_t v = (uint8_t)in[i] << 16;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8);
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '=';
    }
    return out;
}

bool parseUrl(const std::string& url, Url& out) {
    out = Url();
    std::string rest = url;
    while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\r' || rest.back() == '\n')) rest.pop_back();
    while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
    if (strncasecmp(rest.c_str(), "http://", 7) == 0) rest = rest.substr(7);
    else if (strncasecmp(rest.c_str(), "https://", 8) == 0) { rest = rest.substr(8); out.tls = true; }
    else if (rest.find("://") != std::string::npos) return false;

    size_t slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);

    size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        out.userinfo = authority.substr(0, at);
        authority = authority.substr(at + 1);
        out.authHeader = "Basic " + base64(percentDecode(out.userinfo));
    }
    size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        out.host = authority.substr(0, colon);
        out.port = atoi(authority.c_str() + colon + 1);
    } else {
        out.host = authority;
        out.port = out.tls ? 443 : 80;
    }
    return !out.host.empty() && out.port > 0 && out.port < 65536;
}

std::string Url::origin() const {
    return std::string(tls ? "https://" : "http://") + (userinfo.empty() ? std::string() : userinfo + "@") + host + ":" +
           std::to_string(port);
}

std::string urlJoin(const std::string& base, const std::string& rel) {
    if (base.empty()) return rel;
    if (base.back() == '/' && !rel.empty() && rel.front() == '/') return base + rel.substr(1);
    if (base.back() != '/' && (rel.empty() || rel.front() != '/')) return base + "/" + rel;
    return base + rel;
}

std::string percentDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            o += s[i];
        }
    }
    return o;
}

std::string encodePath(const std::string& p) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (size_t i = 0; i < p.size(); i++) {
        unsigned char c = (unsigned char)p[i];
        bool keep = isalnum(c) || strchr("-._~!$&'()*+,;=:@/", c) != nullptr;
        if (c == '%' && i + 2 < p.size() && isxdigit((unsigned char)p[i + 1]) && isxdigit((unsigned char)p[i + 2])) keep = true;
        if (c == 0) keep = false;
        if (keep) o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

// A redirect Location is already a valid, encoded URL (GitHub release downloads point to a signed
// address with a "?query" that must reach the server byte for byte): only characters that may never
// appear in a request line are escaped.
static std::string encodeLocation(const std::string& p) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : p) {
        bool keep = c > 0x20 && c < 0x7F && !strchr("\"<>\\^`{|}", c);
        if (keep) o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

std::string redact(const std::string& url) {
    Url u;
    if (!parseUrl(url, u) || u.userinfo.empty()) return url;
    size_t colon = u.userinfo.find(':');
    std::string user = colon == std::string::npos ? u.userinfo : u.userinfo.substr(0, colon);
    return std::string(u.tls ? "https://" : "http://") + user + ":****@" + u.host + ":" + std::to_string(u.port) + u.path;
}

std::string statusMessage(int status) {
    std::string s;
    switch (status) {
        case 400: s = "伺服器不接受這個請求"; break;
        case 401: s = "帳號或密碼錯誤，或伺服器需要登入"; break;
        case 403: s = "沒有權限存取"; break;
        case 404: s = "找不到這個路徑"; break;
        case 405: s = "伺服器不支援這個操作"; break;
        case 416: s = "分段下載範圍錯誤"; break;
        default: s = "伺服器回應錯誤"; break;
    }
    return s + "（HTTP " + std::to_string(status) + "）";
}

// ------------------------------------------------------------------ sockets
namespace {

// Set after the first successful handshake: later failures are the server refusing, not a wrong port.
std::atomic<bool> g_tlsEverOk{false};
#ifdef __SWITCH__
SslContext g_sslCtx;
bool g_sslReady = false;
#else
SSL_CTX* g_sslCtx = nullptr;
#endif
#ifdef LANINST_MBEDTLS
mbedtls_ssl_config g_mconf;
bool g_mbedReady = false;
std::atomic<bool> g_mbedOff{false};  // this server did not work with mbedTLS: use the system service
// ChaCha20-Poly1305 runs several times faster than AES-GCM in software on the Switch CPU, but many
// servers pick AES when the client offers both. So the first try offers ChaCha only; if the server
// cannot do it, every later connection uses the full list.
mbedtls_ssl_config g_mconfFast;
bool g_fastReady = false;
// per server ("host:port"): 0 = not tried yet, 1 = it does ChaCha, -1 = it does not
std::mutex g_fastLock;
std::map<std::string, int> g_fastState;
int fastState(const std::string& key) {
    std::lock_guard<std::mutex> lk(g_fastLock);
    auto it = g_fastState.find(key);
    return it == g_fastState.end() ? 0 : it->second;
}
void setFastState(const std::string& key, int v) {
    std::lock_guard<std::mutex> lk(g_fastLock);
    int& cur = g_fastState[key];
    if (v == 1 || cur == 0) cur = v;
}
std::vector<int> g_suitesFast, g_suitesAll;
std::atomic<const char*> g_cipher{nullptr};  // suite of the latest mbedTLS connection

int mbedRng(void*, unsigned char* out, size_t len) {
#ifdef __SWITCH__
    randomGet(out, len);  // libnx: thread safe, seeded by the kernel
#else
    while (len) {
        ssize_t n = getrandom(out, len, 0);
        if (n <= 0) return -1;
        out += n;
        len -= (size_t)n;
    }
#endif
    return 0;
}
#endif

// One TCP connection, optionally wrapped in TLS.
struct Conn {
    int fd = -1;
    bool tls = false;
#ifdef __SWITCH__
    SslConnection ssl;
    bool sslOpen = false;
    int sslFd = -1;
#else
    SSL* ssl = nullptr;
#endif
#ifdef LANINST_MBEDTLS
    bool mbed = false;  // this connection uses mbedTLS
    int mbedErr = 0;    // last handshake error (decides whether another try makes sense)
    mbedtls_ssl_context mssl;
    // mbedTLS asks for each record's 5-byte header and then its body: two socket calls (= two system
    // calls on the Switch) per 16 KB. One big socket read here serves many records instead.
    std::vector<uint8_t> rbuf;
    size_t rpos = 0, rlen = 0;
#endif

#ifdef LANINST_MBEDTLS
    static int mbedSend(void* ctx, const unsigned char* buf, size_t len) {
        Conn* c = (Conn*)ctx;
        ssize_t n = ::send(c->fd, buf, len, 0);
        if (n >= 0) return (int)n;
        if (errno == EINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_TIMEOUT;
        return -0x004E;  // MBEDTLS_ERR_NET_SEND_FAILED
    }
    static int mbedRecv(void* ctx, unsigned char* buf, size_t len) {
        Conn* c = (Conn*)ctx;
        if (c->rpos < c->rlen) {
            size_t k = std::min(len, c->rlen - c->rpos);
            memcpy(buf, c->rbuf.data() + c->rpos, k);
            c->rpos += k;
            return (int)k;
        }
        if (c->rbuf.empty()) c->rbuf.resize(256 * 1024);
        ssize_t n;
        {
            SockTimer t;
            n = ::recv(c->fd, c->rbuf.data(), c->rbuf.size(), 0);
        }
        if (n > 0) {
            c->rpos = 0;
            c->rlen = (size_t)n;
            size_t k = std::min(len, c->rlen);
            memcpy(buf, c->rbuf.data(), k);
            c->rpos = k;
            return (int)k;
        }
        if (n >= 0) return (int)n;
        if (errno == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_TIMEOUT;
        return -0x004C;  // MBEDTLS_ERR_NET_RECV_FAILED
    }
    // The answer was not TLS at all (plain HTTP on that port, …): another TLS try cannot help.
    static bool notTls(int r) {
        return r == MBEDTLS_ERR_SSL_INVALID_RECORD
#ifdef MBEDTLS_ERR_SSL_BAD_PROTOCOL_VERSION
               || r == MBEDTLS_ERR_SSL_BAD_PROTOCOL_VERSION
#endif
#ifdef MBEDTLS_ERR_SSL_DECODE_ERROR
               || r == MBEDTLS_ERR_SSL_DECODE_ERROR
#endif
            ;
    }
    // The server answered with TLS but refused these settings (cipher list / version): worth one
    // more try with the full list or the system TLS.
    static bool refused(int r) {
        return r == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE
#ifdef MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE
               || r == MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE
#endif
#ifdef MBEDTLS_ERR_SSL_NO_CIPHER_CHOSEN
               || r == MBEDTLS_ERR_SSL_NO_CIPHER_CHOSEN
#endif
            ;
    }
    bool startMbed(const std::string& host, std::string& err, const mbedtls_ssl_config* conf) {
        mbedtls_ssl_init(&mssl);
        mbed = true;
        if (mbedtls_ssl_setup(&mssl, conf) != 0) { err = "HTTPS 連線建立失敗"; return false; }
        mbedtls_ssl_set_hostname(&mssl, host.c_str());
        mbedtls_ssl_set_bio(&mssl, this, mbedSend, mbedRecv, nullptr);
        int r;
        while ((r = mbedtls_ssl_handshake(&mssl)) != 0) {
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            mbedErr = r;
            char b[160];
            if (r == MBEDTLS_ERR_SSL_TIMEOUT)
                snprintf(b, sizeof(b), "伺服器沒有回應 HTTPS（逾時）");
            else if (notTls(r))
                snprintf(b, sizeof(b), "這個埠回應的不是 HTTPS：如果伺服器沒有開加密，請把開頭改成 http://（-0x%04X）", (unsigned)-r);
            else
                snprintf(b, sizeof(b), "HTTPS 交握失敗（mbedTLS -0x%04X）", (unsigned)-r);
            err = b;
            return false;
        }
        g_cipher = mbedtls_ssl_get_ciphersuite(&mssl);
        return true;
    }
#endif

    ~Conn() {
#ifdef LANINST_MBEDTLS
        if (mbed) mbedtls_ssl_free(&mssl);
#endif
#ifdef __SWITCH__
        if (sslFd >= 0) close(sslFd);
        if (sslOpen) sslConnectionClose(&ssl);
#else
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
#endif
        if (fd >= 0) close(fd);
    }

    // useMbed: try the in-process TLS (connectTo falls back to the system service if it fails).
    // fast: offer only the ChaCha20 suites (see g_mconfFast).
    bool startTls(const std::string& host, std::string& err, bool useMbed = true, bool fast = false) {
        tls = true;
        {
            // TLS reads block (inside the ssl service or mbedTLS); the socket timeout keeps a dead
            // server from hanging the install forever.
            struct timeval tv;
            tv.tv_sec = readTimeoutMs() / 1000;
            tv.tv_usec = (readTimeoutMs() % 1000) * 1000;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }
#ifdef LANINST_MBEDTLS
        if (useMbed && g_mbedReady && !g_mbedOff) {
            if (!startMbed(host, err, fast && g_fastReady ? &g_mconfFast : &g_mconf)) return false;
            g_tlsEverOk = true;
            return true;
        }
#else
        (void)useMbed;
        (void)fast;
#endif
#ifdef __SWITCH__
        if (!g_sslReady) { err = "HTTPS 無法使用（ssl 服務未啟動）"; return false; }
        Result rc = sslContextCreateConnection(&g_sslCtx, &ssl);
        if (R_FAILED(rc)) { err = "HTTPS 連線建立失敗"; return false; }
        sslOpen = true;
        int r = socketSslConnectionSetSocketDescriptor(&ssl, fd);
        if (r == -1 && errno != ENOENT) { err = "HTTPS 連線建立失敗"; return false; }
        if (r != -1) sslFd = r;
        sslConnectionSetHostName(&ssl, host.c_str(), (u32)host.size() + 1);
        sslConnectionSetOption(&ssl, SslOptionType_SkipDefaultVerify, true);
        sslConnectionSetVerifyOption(&ssl, 0);  // self-signed NAS certificates
        sslConnectionSetIoMode(&ssl, SslIoMode_Blocking);
        rc = sslConnectionDoHandshake(&ssl, nullptr, nullptr, nullptr, 0);
        if (R_FAILED(rc)) {
            char b[96];
            if (g_tlsEverOk) snprintf(b, sizeof(b), "HTTPS 交握失敗（0x%X），伺服器暫時拒絕連線", rc);
            else snprintf(b, sizeof(b), "HTTPS 交握失敗（0x%X），這個埠可能不是 HTTPS", rc);
            err = b;
            return false;
        }
        g_tlsEverOk = true;
        return true;
#else
        ssl = SSL_new(g_sslCtx);
        SSL_set_fd(ssl, fd);
        SSL_set_tlsext_host_name(ssl, host.c_str());
        if (SSL_connect(ssl) != 1) { err = "HTTPS 交握失敗，這個埠可能不是 HTTPS"; return false; }
        return true;
#endif
    }

    bool wait() {
#ifdef LANINST_MBEDTLS
        // data already here (decrypted, or raw bytes in our read buffer): no need to wait
        if (mbed && (mbedtls_ssl_get_bytes_avail(&mssl) > 0 || rpos < rlen)) return true;
        if (mbed) goto plainPoll;  // in-process TLS: polling the socket is cheap
#endif
        if (tls) {
#ifdef __SWITCH__
            // No separate wait: the blocking read below waits by itself (with the socket timeout).
            // Asking the ssl service "anything pending?" + "poll" before every read cost two extra
            // IPC round trips per 16 KB TLS record and capped HTTPS downloads at a few MB/s.
            return true;
#else
            if (SSL_pending(ssl) > 0) return true;
#endif
        }
#ifdef LANINST_MBEDTLS
    plainPoll:
#endif
        struct pollfd p;
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        int r;
        {
            SockTimer t(false);  // waiting here is waiting for the network
            r = poll(&p, 1, readTimeoutMs());
        }
        return r > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR));
    }

    // >0 bytes read, 0 = closed, <0 = error
    long recvSome(uint8_t* buf, size_t n) {
        if (!tls) {
            SockTimer t;
            return (long)::recv(fd, buf, n, 0);
        }
#ifdef LANINST_MBEDTLS
        if (mbed) {
            TlsTimer tt;
            for (;;) {
                int r = mbedtls_ssl_read(&mssl, buf, n);
                if (r > 0) return r;
                if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF) return 0;
                if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
                if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
#endif
                return -1;
            }
        }
#endif
#ifdef __SWITCH__
        u32 got = 0;
        Result rc;
        {
            SockTimer t;  // includes the decryption inside the system ssl service
            rc = sslConnectionRead(&ssl, buf, (u32)n, &got);
        }
        if (R_FAILED(rc)) return got ? (long)got : 0;  // treat TLS close/errors at EOF as end of stream
        return (long)got;
#else
        int r = SSL_read(ssl, buf, (int)n);
        if (r > 0) return r;
        int e = SSL_get_error(ssl, r);
        return (e == SSL_ERROR_ZERO_RETURN || e == SSL_ERROR_SYSCALL) ? 0 : -1;
#endif
    }

    bool sendAll(const std::string& data) {
        size_t off = 0;
        while (off < data.size()) {
            long n;
            if (!tls) {
                n = (long)::send(fd, data.data() + off, data.size() - off, 0);
            } else {
#ifdef LANINST_MBEDTLS
                if (mbed) {
                    int r = mbedtls_ssl_write(&mssl, (const unsigned char*)data.data() + off, data.size() - off);
                    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
                    n = r;
                    if (n <= 0) return false;
                    off += (size_t)n;
                    continue;
                }
#endif
#ifdef __SWITCH__
                u32 w = 0;
                n = R_SUCCEEDED(sslConnectionWrite(&ssl, data.data() + off, (u32)(data.size() - off), &w)) ? (long)w : -1;
#else
                n = SSL_write(ssl, data.data() + off, (int)(data.size() - off));
#endif
            }
            if (n <= 0) return false;
            off += (size_t)n;
        }
        return true;
    }
};

// Name lookups: answered from a short cache, else asked on a helper thread so a lookup that never
// answers (network down, DNS server gone) gives up after kConnectMs instead of blocking the caller.
struct DnsJob {
    std::mutex m;
    std::condition_variable cv;
    bool done = false, ok = false;
    struct sockaddr_in addr;
};
static std::mutex g_dnsLock;
static std::map<std::string, std::pair<struct sockaddr_in, std::chrono::steady_clock::time_point>> g_dnsCache;

static void dnsLookup(const std::string& host, DnsJob& job) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    bool ok = getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res &&
              res->ai_addrlen >= (socklen_t)sizeof(struct sockaddr_in);
    {
        std::lock_guard<std::mutex> lk(job.m);
        if (ok) memcpy(&job.addr, res->ai_addr, sizeof(job.addr));
        job.ok = ok;
        job.done = true;
        job.cv.notify_all();
    }
    if (res) freeaddrinfo(res);
}

#ifdef __SWITCH__
struct DnsThread {
    Thread th;
    std::shared_ptr<DnsJob> job;
    std::string host;
    std::atomic<bool> finished{false};
};
static std::mutex g_dnsThreadsLock;
static std::vector<DnsThread*> g_dnsThreads;
// Closes lookup threads that have ended (they cannot close themselves).
static void reapDns(bool all) {
    std::vector<DnsThread*> done;
    {
        std::lock_guard<std::mutex> lk(g_dnsThreadsLock);
        for (auto it = g_dnsThreads.begin(); it != g_dnsThreads.end();) {
            if (all || (*it)->finished) { done.push_back(*it); it = g_dnsThreads.erase(it); }
            else ++it;
        }
    }
    for (DnsThread* t : done) {
        threadWaitForExit(&t->th);  // returning right now (or, at exit, within the lookup's own limit)
        threadClose(&t->th);
        delete t;
    }
}
static bool startDns(const std::shared_ptr<DnsJob>& job, const std::string& host) {
    reapDns(false);
    DnsThread* t = new DnsThread;
    t->job = job;
    t->host = host;
    auto fn = [](void* p) {
        DnsThread* d = (DnsThread*)p;
        dnsLookup(d->host, *d->job);
        d->finished = true;
    };
    if (R_FAILED(threadCreate(&t->th, fn, t, nullptr, 0x20000, 0x2C, -2))) { delete t; return false; }
    if (R_FAILED(threadStart(&t->th))) { threadClose(&t->th); delete t; return false; }
    std::lock_guard<std::mutex> lk(g_dnsThreadsLock);
    g_dnsThreads.push_back(t);
    return true;
}
#else
static void reapDns(bool) {}
static bool startDns(const std::shared_ptr<DnsJob>& job, const std::string& host) {
    std::thread([job, host]() { dnsLookup(host, *job); }).detach();
    return true;
}
#endif

static bool resolve(const std::string& host, struct sockaddr_in& out, std::string& err) {
    memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    if (inet_pton(AF_INET, host.c_str(), &out.sin_addr) == 1) return true;
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(g_dnsLock);
        auto it = g_dnsCache.find(host);
        if (it != g_dnsCache.end() && now - it->second.second < std::chrono::minutes(10)) {
            out = it->second.first;
            return true;
        }
    }
    // The lookup runs on its own thread: if it answers after the time limit nobody waits for it.
    // (No pthreads on the Switch: libnx cannot detach them. A finished lookup thread is closed by
    // the next lookup, see reapDns.)
    std::shared_ptr<DnsJob> job = std::make_shared<DnsJob>();
    if (!startDns(job, host)) {  // no thread: look it up here (no time limit, but it works)
        dnsLookup(host, *job);
    }
    std::unique_lock<std::mutex> lk(job->m);
    if (!job->cv.wait_for(lk, std::chrono::milliseconds(kConnectMs), [&] { return job->done; })) {
        err = "找不到主機 " + host + "（查詢網址逾時：網路沒通，或網址打錯）";
        return false;
    }
    if (!job->ok) {
        err = "找不到主機 " + host + "（網址打錯了嗎？）";
        return false;
    }
    out = job->addr;
    std::lock_guard<std::mutex> g(g_dnsLock);
    g_dnsCache[host] = {out, now};
    return true;
}

static bool tcpOpen(const std::string& host, int portNum, Conn& s, std::string& err) {
    if (!netOk()) {
        err = "Switch 沒有連上網路";
        return false;
    }
    struct sockaddr_in addr;
    if (!resolve(host, addr, err)) return false;
    addr.sin_port = htons((uint16_t)portNum);
    s.fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s.fd < 0) {
        err = "無法建立網路連線";
        return false;
    }
    int one = 1;
    setsockopt(s.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int rcvbuf = 1024 * 1024;
    setsockopt(s.fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    // connect with a time limit: a wrong address / closed port behind a firewall never answers
    int flags = fcntl(s.fd, F_GETFL, 0);
    bool nb = flags >= 0 && fcntl(s.fd, F_SETFL, flags | O_NONBLOCK) == 0;
    int rc = connect(s.fd, (struct sockaddr*)&addr, sizeof(addr));
    bool timedOut = false;
    if (rc != 0 && nb && (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN)) {
        struct pollfd p;
        p.fd = s.fd;
        p.events = POLLOUT;
        p.revents = 0;
        int pr = poll(&p, 1, kConnectMs);
        if (pr > 0) {
            int soErr = 0;
            socklen_t len = sizeof(soErr);
            rc = (getsockopt(s.fd, SOL_SOCKET, SO_ERROR, &soErr, &len) == 0 && soErr == 0) ? 0 : -1;
        } else {
            rc = -1;
            timedOut = pr == 0;
        }
    }
    if (nb) fcntl(s.fd, F_SETFL, flags);
    if (rc != 0) {
        err = "無法連線到 " + host + ":" + std::to_string(portNum) +
              (timedOut ? "（5 秒沒有回應：位址或埠號可能不對，或伺服器沒開）" : "（伺服器有開嗎？埠號對嗎？防火牆？）");
        return false;
    }
    return true;
}

bool tcpConnect(const Url& u, Conn& s, std::string& err) { return tcpOpen(u.host, u.port, s, err); }

bool connectTo(const Url& u, Conn& s, std::string& err) {
    if (!tcpConnect(u, s, err)) return false;
    if (!u.tls) return true;
#if defined(LANINST_MBEDTLS)
    const std::string fastKey = u.host + ":" + std::to_string(u.port);
    if (g_mbedReady && !g_mbedOff && g_fastReady && fastState(fastKey) >= 0) {
        if (s.startTls(u.host, err, true, true)) { setFastState(fastKey, 1); return true; }
        if (!Conn::refused(s.mbedErr)) return false;  // not TLS / no answer: another try cannot help
        setFastState(fastKey, -1);  // this server has no ChaCha20: the full list for it from now on
        s.~Conn();
        new (&s) Conn();
        if (!tcpConnect(u, s, err)) return false;
    }
    if (g_mbedReady && !g_mbedOff) {
        if (s.startTls(u.host, err, true)) return true;
        if (!Conn::refused(s.mbedErr)) return false;
        // mbedTLS could not talk to this server: try the system ssl service on a new connection,
        // and keep using it if that works.
        std::string mbedErr = err;
        s.~Conn();
        new (&s) Conn();
        if (!tcpConnect(u, s, err)) return false;
        if (!s.startTls(u.host, err, false)) { err += "（mbedTLS：" + mbedErr + "）"; return false; }
        g_mbedOff = true;
        return true;
    }
#endif
    return s.startTls(u.host, err);
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// Incremental decoder for "Transfer-Encoding: chunked".
struct Chunked {
    enum { Size, Data, DataEnd, Trailer, Done } state = Size;
    std::string line;
    uint64_t left = 0;

    // Returns false on malformed input; `deliver` gets decoded bytes.
    template <class F>
    bool feed(const uint8_t* d, size_t n, F&& deliver) {
        size_t i = 0;
        while (i < n && state != Done) {
            if (state == Size || state == DataEnd || state == Trailer) {
                char c = (char)d[i++];
                if (c != '\n') { if (c != '\r') line += c; if (line.size() > 1024) return false; continue; }
                if (state == DataEnd) {  // the CRLF after a chunk
                    if (!line.empty()) return false;
                    state = Size;
                } else if (state == Size) {
                    if (line.empty()) return false;
                    left = strtoull(line.c_str(), nullptr, 16);  // ignores ";ext"
                    state = left ? Data : Trailer;
                } else if (line.empty()) {
                    state = Done;  // blank line ends the trailer
                }
                line.clear();
            } else {  // Data
                size_t take = (size_t)(left < n - i ? left : n - i);
                if (!deliver(d + i, take)) return false;
                i += take;
                left -= take;
                if (!left) state = DataEnd;
            }
        }
        return true;
    }
};

// Resolves a redirect Location against the current URL, keeping credentials for the same host.
std::string resolveLocation(const Url& cur, const std::string& loc) {
    if (loc.find("://") != std::string::npos) {
        Url n;
        if (parseUrl(loc, n) && n.userinfo.empty() && n.host == cur.host && !cur.userinfo.empty()) {
            size_t p = loc.find("://") + 3;
            return loc.substr(0, p) + cur.userinfo + "@" + loc.substr(p);
        }
        return loc;
    }
    if (!loc.empty() && loc[0] == '/') return cur.origin() + loc;
    std::string dir = cur.path.substr(0, cur.path.rfind('/') + 1);
    return cur.origin() + dir + loc;
}

}  // namespace

// ------------------------------------------------------------------ memory files
static std::mutex g_memLock;
static std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> g_memFiles;

void setMemoryFile(const std::string& name, std::shared_ptr<const std::vector<uint8_t>> data) {
    std::lock_guard<std::mutex> g(g_memLock);
    if (data) g_memFiles[name] = std::move(data);
    else g_memFiles.erase(name);
}

// Answers a request for "mem://name" like a web server with Range support would.
static bool memRequest(const std::string& url, const Request& req, const Sink& sink, Response& res, std::string& err) {
    res = Response();
    std::shared_ptr<const std::vector<uint8_t>> d;
    {
        std::lock_guard<std::mutex> g(g_memLock);
        auto it = g_memFiles.find(url.substr(6));
        if (it != g_memFiles.end()) d = it->second;
    }
    uint64_t total = d ? d->size() : 0, start = 0, len = total;
    if (!d) res.status = 404;
    else if (req.rangeLen > 0) {
        if (req.rangeStart >= total) res.status = 416;
        else {
            start = req.rangeStart;
            len = std::min<uint64_t>(req.rangeLen, total - start);
            res.status = 206;
            res.headers["content-range"] = "bytes " + std::to_string(start) + "-" + std::to_string(start + len - 1) + "/" + std::to_string(total);
        }
    } else res.status = 200;
    if (res.status == 200 || res.status == 206) res.headers["content-length"] = std::to_string(len);
    if (req.onHeaders && !req.onHeaders(res)) { err = "已取消"; return false; }
    if (req.method == "HEAD" || (res.status != 200 && res.status != 206)) return true;
    for (uint64_t off = 0; off < len;) {
        size_t n = (size_t)std::min<uint64_t>(256 * 1024, len - off);
        if (!sink(d->data() + start + off, n)) { err = "已取消"; return false; }
        off += n;
    }
    return true;
}

// ------------------------------------------------------------------ requests
bool request(const std::string& urlIn, const Request& req, const Sink& sink, Response& res, std::string& err) {
    if (urlIn.compare(0, 6, "mem://") == 0) return memRequest(urlIn, req, sink, res, err);
    std::string url = urlIn;
    std::string method = req.method;
    std::string body = req.body;

    for (int hop = 0; hop < 4; hop++) {
        res = Response();
        Url u;
        if (!parseUrl(url, u)) {
            err = "網址格式錯誤（請用 http:// 或 https:// 開頭）：" + redact(url);
            return false;
        }
        Conn s;
        if (!connectTo(u, s, err)) return false;

        // the first URL is ours (file names may contain any character); after a redirect it is the server's
        std::string target = (hop == 0 && !req.encodedUrl) ? encodePath(u.path) : encodeLocation(u.path);
        // Host without the default port ("example.com", not "example.com:443"): some CDNs refuse the latter
        bool defaultPort = u.port == (u.tls ? 443 : 80);
        std::string head = method + " " + target + " HTTP/1.1\r\nHost: " + u.host +
                           (defaultPort ? std::string() : ":" + std::to_string(u.port)) +
                           "\r\nUser-Agent: GameShelf\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
        if (!u.authHeader.empty()) head += "Authorization: " + u.authHeader + "\r\n";
        if (req.rangeLen > 0) {
            char r[96];
            snprintf(r, sizeof(r), "Range: bytes=%llu-%llu\r\n", (unsigned long long)req.rangeStart,
                     (unsigned long long)(req.rangeStart + req.rangeLen - 1));
            head += r;
        }
        for (const auto& h : req.headers) head += h + "\r\n";
        if (!body.empty() || (method != "GET" && method != "HEAD")) head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        head += "\r\n";
        if (!s.sendAll(head) || (!body.empty() && !s.sendAll(body))) {
            err = "傳送資料失敗";
            return false;
        }

        // Read until the end of the headers.
        static const size_t kBuf = 256 * 1024;
        std::vector<uint8_t> buf(kBuf);
        std::string hdr;
        size_t bodyStart = 0, bodyBytes = 0;
        for (;;) {
            if (!s.wait()) { err = "等待伺服器回應逾時"; return false; }
            long n = s.recvSome(buf.data(), kBuf);
            if (n <= 0) { err = "伺服器提前中斷連線"; return false; }
            size_t prev = hdr.size();
            hdr.append((const char*)buf.data(), (size_t)n);
            size_t end = hdr.find("\r\n\r\n");
            if (end != std::string::npos) {
                bodyStart = end + 4 - prev;
                bodyBytes = (size_t)n - bodyStart;
                hdr.resize(end);
                break;
            }
            if (hdr.size() > 64 * 1024) { err = "伺服器回應標頭過大"; return false; }
        }
        if (sscanf(hdr.c_str(), "HTTP/%*d.%*d %d", &res.status) != 1) { err = "伺服器回應格式錯誤"; return false; }
        size_t pos = hdr.find("\r\n");
        while (pos != std::string::npos) {
            size_t next = hdr.find("\r\n", pos + 2);
            std::string line = hdr.substr(pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string v = line.substr(colon + 1);
                while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
                while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
                res.headers[lower(line.substr(0, colon))] = v;
            }
            pos = next;
        }

        // Redirects
        if ((res.status == 301 || res.status == 302 || res.status == 303 || res.status == 307 || res.status == 308) &&
            res.headers.count("location") && hop < 3) {
            url = resolveLocation(u, res.headers["location"]);
            if (res.status == 303) { method = "GET"; body.clear(); }
            continue;
        }

        if (req.onHeaders && !req.onHeaders(res)) { err = "已取消"; return false; }
        if (method == "HEAD" || res.status == 204 || res.status == 304) return true;

        bool chunked = lower(res.headers["transfer-encoding"]).find("chunked") != std::string::npos;
        long long contentLength = -1;
        if (!chunked && res.headers.count("content-length")) contentLength = atoll(res.headers["content-length"].c_str());

        bool aborted = false;
        Chunked ch;
        uint64_t received = 0;
        auto deliver = [&](const uint8_t* d, size_t n) -> bool {
            if (!n) return true;
            if (!sink(d, n)) { aborted = true; return false; }
            return true;
        };
        auto consume = [&](const uint8_t* d, size_t n) -> bool {
            if (chunked) {
                if (!ch.feed(d, n, deliver)) { if (!aborted) err = "伺服器回應格式錯誤（chunked）"; return false; }
                return true;
            }
            return deliver(d, n);
        };

        if (bodyBytes) {
            if (!consume(buf.data() + bodyStart, bodyBytes)) { if (aborted) err = "已取消"; return false; }
            received += bodyBytes;
        }
        for (;;) {
            if (chunked && ch.state == Chunked::Done) break;
            if (contentLength >= 0 && received >= (uint64_t)contentLength) break;
            if (!s.wait()) { err = "傳輸逾時"; return false; }
            size_t want = kBuf;
            if (contentLength >= 0 && (uint64_t)contentLength - received < want) want = (size_t)((uint64_t)contentLength - received);
            long n = s.recvSome(buf.data(), want);
            if (n == 0) break;
            if (n < 0) { err = "接收資料失敗"; return false; }
            if (!consume(buf.data(), (size_t)n)) { if (aborted) err = "已取消"; return false; }
            received += (uint64_t)n;
        }
        if (contentLength >= 0 && received != (uint64_t)contentLength) { err = "連線提前中斷"; return false; }
        if (chunked && ch.state != Chunked::Done) { err = "連線提前中斷"; return false; }
        return true;
    }
    err = "重新導向次數過多";
    return false;
}

// ------------------------------------------------------------------ kept-alive range reads
struct Session::Impl {
    std::unique_ptr<Conn> conn;
    std::string key;  // origin of the open connection
    std::atomic<int> fd{-1};          // socket of `conn`, for interrupt() from another thread
    std::atomic<bool> stopped{false};
    bool open(const Url& u, const std::string& k, std::string& err) {
        if (stopped) { err = "已停止"; return false; }
        conn.reset(new Conn);
        if (!connectTo(u, *conn, err)) { conn.reset(); key.clear(); return false; }
        key = k;
        fd = conn->fd;
        if (stopped) { ::shutdown(conn->fd, SHUT_RDWR); }
        return true;
    }
};

Session::Session() : d_(new Impl) {}
Session::~Session() { delete d_; }
void Session::close() { d_->fd = -1; d_->conn.reset(); d_->key.clear(); }
void Session::interrupt() {
    d_->stopped = true;
    int fd = d_->fd.load();
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);  // a blocked read returns at once
}
void Session::resume() { d_->stopped = false; }

// One whole GET over the kept connection (small files such as covers). Anything unusual (redirect,
// chunked answer, no length) goes through request() on its own connection.
bool Session::get(const std::string& url, size_t limit, std::vector<uint8_t>& out, int& status, std::string& err,
                  bool encodedUrl) {
    out.clear();
    status = 0;
    Url u;
    if (!parseUrl(url, u)) { err = "網址格式錯誤：" + redact(url); return false; }
    std::string key = u.origin();
    for (int attempt = 0; attempt < 2; attempt++) {
        bool fresh = false;
        if (!d_->conn || d_->key != key) {
            if (!d_->open(u, key, err)) return false;
            fresh = true;
        }
        Conn& s = *d_->conn;
        bool defaultPort = u.port == (u.tls ? 443 : 80);
        std::string head = "GET " + (encodedUrl ? encodeLocation(u.path) : encodePath(u.path)) + " HTTP/1.1\r\nHost: " + u.host +
                           (defaultPort ? std::string() : ":" + std::to_string(u.port)) +
                           "\r\nUser-Agent: GameShelf\r\nAccept-Encoding: identity\r\nConnection: keep-alive\r\n";
        if (!u.authHeader.empty()) head += "Authorization: " + u.authHeader + "\r\n";
        head += "\r\n";
        if (!s.sendAll(head)) {
            close();
            if (!fresh) continue;
            err = "傳送資料失敗";
            return false;
        }
        std::vector<uint8_t> buf(64 * 1024);
        std::string hdr;
        size_t bodyStart = 0, bodyBytes = 0;
        bool headOk = false;
        for (;;) {
            if (!s.wait()) break;
            long n = s.recvSome(buf.data(), buf.size());
            if (n <= 0) break;
            size_t prev = hdr.size();
            hdr.append((const char*)buf.data(), (size_t)n);
            size_t end = hdr.find("\r\n\r\n");
            if (end != std::string::npos) {
                bodyStart = end + 4 - prev;
                bodyBytes = (size_t)n - bodyStart;
                hdr.resize(end);
                headOk = true;
                break;
            }
            if (hdr.size() > 64 * 1024) break;
        }
        if (!headOk) {
            close();
            if (!fresh && hdr.empty()) continue;
            err = "等待伺服器回應逾時";
            return false;
        }
        sscanf(hdr.c_str(), "HTTP/%*d.%*d %d", &status);
        std::map<std::string, std::string> h;
        for (size_t pos = hdr.find("\r\n"); pos != std::string::npos;) {
            size_t next = hdr.find("\r\n", pos + 2);
            std::string line = hdr.substr(pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string v = line.substr(colon + 1);
                while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
                h[lower(line.substr(0, colon))] = v;
            }
            pos = next;
        }
        bool chunked = lower(h["transfer-encoding"]).find("chunked") != std::string::npos;
        if ((status >= 300 && status < 400) || chunked || !h.count("content-length")) {
            close();
            Request req;
            req.encodedUrl = encodedUrl;
            Response res;
            bool tooBig = false;
            bool ok = request(url, req, [&](const uint8_t* d, size_t n) {
                if (out.size() + n > limit) { tooBig = true; return false; }
                out.insert(out.end(), d, d + n);
                return true;
            }, res, err);
            status = res.status;
            if (tooBig) { err = "檔案太大"; return false; }
            return ok;
        }
        uint64_t len = strtoull(h["content-length"].c_str(), nullptr, 10);
        if (len > limit) { close(); err = "檔案太大"; return false; }
        out.reserve((size_t)len);
        out.insert(out.end(), buf.data() + bodyStart, buf.data() + bodyStart + std::min<size_t>(bodyBytes, (size_t)len));
        while (out.size() < len) {
            if (!s.wait()) { close(); err = "傳輸逾時"; return false; }
            long n = s.recvSome(buf.data(), std::min<size_t>(buf.size(), (size_t)(len - out.size())));
            if (n <= 0) { close(); err = "接收資料失敗"; return false; }
            out.insert(out.end(), buf.data(), buf.data() + n);
        }
        if (lower(h["connection"]).find("close") != std::string::npos) close();
        return true;
    }
    err = "連線中斷";
    return false;
}

bool Session::getRange(const std::string& url, uint64_t off, uint64_t len, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    if (url.compare(0, 6, "mem://") == 0 || len == 0) return getToMemory(url, off, len, out, err);
    Url u;
    if (!parseUrl(url, u)) { err = "網址格式錯誤：" + redact(url); return false; }
    std::string key = u.origin();
    for (int attempt = 0; attempt < 2; attempt++) {
        bool fresh = false;
        if (!d_->conn || d_->key != key) {
            if (!d_->open(u, key, err)) return false;
            fresh = true;
        }
        Conn& s = *d_->conn;
        bool defaultPort = u.port == (u.tls ? 443 : 80);
        std::string head = "GET " + encodePath(u.path) + " HTTP/1.1\r\nHost: " + u.host +
                           (defaultPort ? std::string() : ":" + std::to_string(u.port)) +
                           "\r\nUser-Agent: GameShelf\r\nAccept-Encoding: identity\r\nConnection: keep-alive\r\n";
        if (!u.authHeader.empty()) head += "Authorization: " + u.authHeader + "\r\n";
        char r[96];
        snprintf(r, sizeof(r), "Range: bytes=%llu-%llu\r\n\r\n", (unsigned long long)off, (unsigned long long)(off + len - 1));
        head += r;
        if (!s.sendAll(head)) {
            close();
            if (!fresh) continue;  // the server had closed the kept connection: once more on a new one
            err = "傳送資料失敗";
            return false;
        }
        std::vector<uint8_t> buf(64 * 1024);
        std::string hdr;
        size_t bodyStart = 0, bodyBytes = 0;
        bool headOk = false;
        for (;;) {
            if (!s.wait()) break;
            long n = s.recvSome(buf.data(), buf.size());
            if (n <= 0) break;
            size_t prev = hdr.size();
            hdr.append((const char*)buf.data(), (size_t)n);
            size_t end = hdr.find("\r\n\r\n");
            if (end != std::string::npos) {
                bodyStart = end + 4 - prev;
                bodyBytes = (size_t)n - bodyStart;
                hdr.resize(end);
                headOk = true;
                break;
            }
            if (hdr.size() > 64 * 1024) break;
        }
        if (!headOk) {
            close();
            if (!fresh && hdr.empty()) continue;
            err = "等待伺服器回應逾時";
            return false;
        }
        int status = 0;
        sscanf(hdr.c_str(), "HTTP/%*d.%*d %d", &status);
        std::map<std::string, std::string> h;
        for (size_t pos = hdr.find("\r\n"); pos != std::string::npos;) {
            size_t next = hdr.find("\r\n", pos + 2);
            std::string line = hdr.substr(pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string v = line.substr(colon + 1);
                while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
                h[lower(line.substr(0, colon))] = v;
            }
            pos = next;
        }
        bool chunked = lower(h["transfer-encoding"]).find("chunked") != std::string::npos;
        // the whole of a small file asked for (0..size-1): many servers answer 200 with all of it
        bool whole = status == 200 && off == 0 && h.count("content-length") &&
                     strtoull(h["content-length"].c_str(), nullptr, 10) == len;
        if ((status != 206 && !whole) || chunked || !h.count("content-length")) {
            close();  // redirect, error page, no length: the full code path handles (and words) it
            return getToMemory(url, off, len, out, err);
        }
        if (strtoull(h["content-length"].c_str(), nullptr, 10) != len) { close(); err = "分段下載長度不符"; return false; }
        out.reserve((size_t)len);
        out.insert(out.end(), buf.data() + bodyStart, buf.data() + bodyStart + std::min<size_t>(bodyBytes, (size_t)len));
        while (out.size() < len) {
            if (!s.wait()) { close(); err = "傳輸逾時"; return false; }
            long n = s.recvSome(buf.data(), std::min<size_t>(buf.size(), (size_t)(len - out.size())));
            if (n <= 0) { close(); err = "接收資料失敗"; return false; }
            out.insert(out.end(), buf.data(), buf.data() + n);
        }
        if (lower(h["connection"]).find("close") != std::string::npos) close();
        return true;
    }
    err = "連線中斷";
    return false;
}

bool requestToMemory(const std::string& url, const Request& req, std::vector<uint8_t>& out, Response& res,
                     std::string& err) {
    out.clear();
    return request(url, req, [&](const uint8_t* d, size_t n) { out.insert(out.end(), d, d + n); return true; }, res, err);
}

bool get(const std::string& url, uint64_t rangeStart, uint64_t rangeLen, const Sink& sink, std::string& err) {
    Request req;
    req.rangeStart = rangeStart;
    req.rangeLen = rangeLen;
    std::string statusErr;
    req.onHeaders = [&](const Response& r) {
        // asking for the whole file (a small one: 0..size-1) many servers answer 200 with all of it
        auto len = r.headers.find("content-length");
        bool whole = r.status == 200 && rangeStart == 0 && len != r.headers.end() &&
                     strtoull(len->second.c_str(), nullptr, 10) == rangeLen;
        if (rangeLen > 0 && r.status != 206 && !whole) {
            statusErr = r.status == 200 ? std::string("伺服器不支援分段下載（HTTP 200）") : statusMessage(r.status);
            return false;
        }
        if (rangeLen == 0 && r.status != 200) { statusErr = statusMessage(r.status); return false; }
        auto it = r.headers.find("content-length");
        if (rangeLen > 0 && it != r.headers.end() && strtoull(it->second.c_str(), nullptr, 10) != rangeLen) {
            statusErr = "分段下載長度不符";
            return false;
        }
        return true;
    };
    Response res;
    bool ok = request(url, req, sink, res, err);
    if (!statusErr.empty()) err = statusErr;
    return ok;
}

bool getToMemory(const std::string& url, uint64_t rangeStart, uint64_t rangeLen, std::vector<uint8_t>& out,
                 std::string& err) {
    out.clear();
    if (rangeLen) out.reserve((size_t)rangeLen);
    return get(url, rangeStart, rangeLen, [&](const uint8_t* d, size_t n) { out.insert(out.end(), d, d + n); return true; }, err);
}

bool probeSize(const std::string& url, uint64_t& size, std::string& err) {
    Request req;
    req.rangeStart = 0;
    req.rangeLen = 1;
    bool got = false;
    req.onHeaders = [&](const Response& r) {
        if (r.status == 206) {
            auto it = r.headers.find("content-range");
            if (it != r.headers.end()) {
                size_t slash = it->second.rfind('/');
                if (slash != std::string::npos && it->second[slash + 1] != '*') {
                    size = strtoull(it->second.c_str() + slash + 1, nullptr, 10);
                    got = true;
                }
            }
        } else if (r.status == 200) {
            auto it = r.headers.find("content-length");
            if (it != r.headers.end()) { size = strtoull(it->second.c_str(), nullptr, 10); got = true; }
        } else {
            err = statusMessage(r.status);
        }
        return false;  // never download the body
    };
    Response res;
    std::string e;
    request(url, req, [](const uint8_t*, size_t) { return false; }, res, e);
    if (!got && err.empty()) err = e;
    return got;
}

#ifdef __SWITCH__
static bool g_mbedReady_or_false() {
#ifdef LANINST_MBEDTLS
    return g_mbedReady;
#else
    return false;
#endif
}
#endif

bool tlsInit() {
#ifdef LANINST_MBEDTLS
    if (!g_mbedReady) {
#if defined(MBEDTLS_PSA_CRYPTO_C)
        psa_crypto_init();  // mbedTLS 3.x uses PSA internally; harmless if it was not needed
#endif
        {   // build mbedTLS's lazily generated AES tables now, before several threads use them
            mbedtls_aes_context aes;
            unsigned char key[16] = {0};
            mbedtls_aes_init(&aes);
            mbedtls_aes_setkey_enc(&aes, key, 128);
            mbedtls_aes_free(&aes);
        }
        mbedtls_ssl_config_init(&g_mconf);
        if (mbedtls_ssl_config_defaults(&g_mconf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) == 0) {
            mbedtls_ssl_conf_authmode(&g_mconf, MBEDTLS_SSL_VERIFY_NONE);  // self-signed NAS certificates
            mbedtls_ssl_conf_rng(&g_mconf, mbedRng, nullptr);
            // TLS 1.2: every NAS speaks it, and it needs nothing beyond the classic crypto code.
#if MBEDTLS_VERSION_NUMBER >= 0x03020000
            mbedtls_ssl_conf_max_tls_version(&g_mconf, MBEDTLS_SSL_VERSION_TLS1_2);
#else
            mbedtls_ssl_conf_max_version(&g_mconf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
#endif
            // Cipher order: ChaCha20-Poly1305, then AES-128-GCM (fewer rounds than AES-256), then the rest.
            const int* all = mbedtls_ssl_list_ciphersuites();
            auto name = [](int id) {
                const char* n = mbedtls_ssl_get_ciphersuite_name(id);
                return std::string(n ? n : "");
            };
            for (int pass = 0; pass < 3; pass++)
                for (const int* p = all; *p; p++) {
                    std::string n = name(*p);
                    bool chacha = n.find("CHACHA20") != std::string::npos && n.find("ECDHE") != std::string::npos;
                    bool aes128 = n.find("AES-128-GCM") != std::string::npos && n.find("ECDHE") != std::string::npos;
                    int rank = chacha ? 0 : aes128 ? 1 : 2;
                    if (rank != pass) continue;
                    g_suitesAll.push_back(*p);
                    if (chacha) g_suitesFast.push_back(*p);
                }
            g_suitesAll.push_back(0);
            mbedtls_ssl_conf_ciphersuites(&g_mconf, g_suitesAll.data());
            g_mbedReady = true;
            if (!g_suitesFast.empty()) {
                g_suitesFast.push_back(0);
                mbedtls_ssl_config_init(&g_mconfFast);
                if (mbedtls_ssl_config_defaults(&g_mconfFast, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                                MBEDTLS_SSL_PRESET_DEFAULT) == 0) {
                    mbedtls_ssl_conf_authmode(&g_mconfFast, MBEDTLS_SSL_VERIFY_NONE);
                    mbedtls_ssl_conf_rng(&g_mconfFast, mbedRng, nullptr);
#if MBEDTLS_VERSION_NUMBER >= 0x03020000
                    mbedtls_ssl_conf_max_tls_version(&g_mconfFast, MBEDTLS_SSL_VERSION_TLS1_2);
#else
                    mbedtls_ssl_conf_max_version(&g_mconfFast, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
#endif
                    mbedtls_ssl_conf_ciphersuites(&g_mconfFast, g_suitesFast.data());
                    g_fastReady = true;
                }
            }
        }
    }
#endif
#ifdef __SWITCH__
    if (g_sslReady) return true;
    if (R_FAILED(sslInitialize(3))) return g_mbedReady_or_false();
    if (R_FAILED(sslCreateContext(&g_sslCtx, SslVersion_Auto))) { sslExit(); return g_mbedReady_or_false(); }
    g_sslReady = true;
    return true;
#else
    if (g_sslCtx) return true;
    g_sslCtx = SSL_CTX_new(TLS_client_method());
    if (!g_sslCtx) return false;
    SSL_CTX_set_verify(g_sslCtx, SSL_VERIFY_NONE, nullptr);
    return true;
#endif
}

const char* tlsBackend() {
#ifdef LANINST_MBEDTLS
    if (g_mbedReady && !g_mbedOff) return "mbedTLS";
#endif
    return "system";
}

std::string tlsCipher() {
#ifdef LANINST_MBEDTLS
    const char* c = g_cipher;
    if (c && g_mbedReady && !g_mbedOff) return c;
#endif
    return "";
}

void tlsExit() {
    reapDns(true);  // name lookups still running end within their own limit
#ifdef LANINST_MBEDTLS
    if (g_mbedReady) { mbedtls_ssl_config_free(&g_mconf); g_mbedReady = false; }
    if (g_fastReady) { mbedtls_ssl_config_free(&g_mconfFast); g_fastReady = false; }
#endif
#ifdef __SWITCH__
    if (!g_sslReady) return;
    sslContextClose(&g_sslCtx);
    sslExit();
    g_sslReady = false;
#else
    if (g_sslCtx) SSL_CTX_free(g_sslCtx);
    g_sslCtx = nullptr;
#endif
}

}  // namespace http
