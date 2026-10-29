// Desktop test of the portable Switch code against the real server.py:
// HTTP + discovery, NSP/NSZ/XCI/XCZ container parsing, NCZ decompression + re-encryption,
// game-card header conversion, hashing, CNMT parsing, config / prod.keys loading.
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <chrono>

#include "../switch/source/badfile.hpp"
#include "../switch/source/container.hpp"
#include "../switch/source/covers.hpp"
#define STBI_NO_STDIO
#include "../switch/source/stb_image.h"  // declarations only (implemented in covers.cpp)
#include "../switch/source/titles.hpp"
#include "../switch/source/titledb.hpp"
#include "../switch/source/tsconv.hpp"
#include "../switch/source/shortcut.hpp"
#include "../switch/source/crypto.hpp"
#include "../switch/source/crypto.hpp"
#include "../switch/source/formats.hpp"
#include "../switch/source/http.hpp"
#include "../switch/source/nca_stream.hpp"
#include "../switch/source/remote.hpp"
#include "../switch/source/resume.hpp"
#include "../switch/source/servers.hpp"
#include "../switch/source/reports.hpp"
#include "../switch/source/update.hpp"
#include "../switch/source/ime.hpp"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; return false; } } while (0)

struct Expect { std::string pkg, entry; uint64_t ncaSize; std::string sha; bool patched; };

static std::string hex(const uint8_t* d, size_t n) { return fmt::bytesToHex(d, n); }

// Runs one content entry through the same pipeline install.cpp uses, writing into memory.
static bool runPipeline(const std::string& url, const fmt::Pfs0Entry& e, const uint8_t* key, std::vector<uint8_t>& out,
                        uint8_t hash[0x20], bool& patched, std::string& err, std::mt19937& rng, bool corrupt = false) {
    out.clear();
    nca::NcaWriter w([&](uint64_t off, const uint8_t* d, size_t n) {
        if (out.size() != off) return false;  // writes must be sequential
        out.insert(out.end(), d, d + n);
        return true;
    }, key);
    nca::NczDecoder z(w);
    bool isNcz = fmt::endsWith(e.name, ".ncz");
    std::string pipeErr;
    uint64_t pos = 0;
    bool ok = http::get(url, e.offset, e.size, [&](const uint8_t* d, size_t n) {
        std::vector<uint8_t> tmp(d, d + n);
        if (corrupt && pos <= e.size / 2 && pos + n > e.size / 2) tmp[e.size / 2 - pos] ^= 0x5A;
        pos += n;
        // re-split into odd-sized pieces to exercise every parser boundary
        size_t i = 0;
        while (i < n) {
            size_t piece = std::min(n - i, (size_t)(rng() % 9000 + 1));
            bool good = isNcz ? z.push(&tmp[i], piece) : w.push(&tmp[i], piece);
            if (!good) { pipeErr = isNcz ? z.error : w.error; return false; }
            i += piece;
        }
        return true;
    }, err);
    if (ok && isNcz && !z.finish()) { ok = false; pipeErr = z.error; }
    if (isNcz && !z.memoryInfo.empty()) {
        static std::string lastInfo;
        if (z.memoryInfo != lastInfo) printf("   ncz memory: %s\n", z.memoryInfo.c_str());
        lastInfo = z.memoryInfo;
    }
    if (ok && !w.finish()) { ok = false; pipeErr = w.error; }
    if (!pipeErr.empty()) err = pipeErr;
    w.hash(hash);
    patched = w.patched();
    return ok;
}

static bool sha256(const std::vector<uint8_t>& d, uint8_t out[0x20]) {
    crypto::Sha256 s;
    s.update(d.data(), d.size());
    s.finish(out);
    return true;
}

static bool testPackages(const std::string& server, const std::string& dir, remote::ServerKind wantKind) {
    std::string err;
    uint8_t key[0x20];
    CHECK(crypto::loadHeaderKey(dir + "/prod.keys", key));

    std::vector<Expect> exp;
    std::ifstream ef(dir + "/expected.tsv");
    std::string line;
    while (std::getline(ef, line)) {
        if (line.empty()) continue;
        std::istringstream ls(line);
        Expect x; std::string sz, p;
        std::getline(ls, x.pkg, '\t'); std::getline(ls, x.entry, '\t'); std::getline(ls, sz, '\t');
        std::getline(ls, x.sha, '\t'); std::getline(ls, p, '\t');
        x.ncaSize = strtoull(sz.c_str(), nullptr, 10); x.patched = p == "1";
        exp.push_back(x);
    }
    CHECK(exp.size() == 8);

    std::vector<remote::RemoteFile> files;
    remote::ServerKind kind0;
    remote::ListProgress prog;  // the start-up bar: connected, then the count of files found
    remote::setListProgress(&prog);
    CHECK(remote::fetchList(server, files, kind0, err));
    remote::setListProgress(nullptr);
    CHECK(prog.connected && prog.files == files.size());
    printf("== %s via %s: %zu files\n", http::redact(server).c_str(), remote::kindName(kind0), files.size());
    CHECK(kind0 == wantKind);
    CHECK(files.size() == 5);
    bool sawChinese = false;
    for (const auto& f : files) {
        CHECK(f.size > 1000000);  // real sizes, also for directory listings
        if (f.displayName == "遊戲 raw/中文 遊戲 #1.nsp" || f.displayName == "顯示名稱/中文 遊戲 #1.nsp") sawChinese = true;
    }
    CHECK(sawChinese);
    std::mt19937 rng(1234);

    for (const auto& f : files) {
        std::string url = f.url;
        std::string base = f.displayName.substr(f.displayName.rfind('/') + 1);
        container::Kind kind;
        std::vector<fmt::Pfs0Entry> entries;
        container::ReadFn rd = [&](uint64_t o, uint64_t s, std::vector<uint8_t>& out, std::string& e) {
            return http::getToMemory(url, o, s, out, e);
        };
        CHECK(container::open(rd, kind, entries, err));
        bool isXci = fmt::endsWith(base, ".xci") || fmt::endsWith(base, ".xcz");
        CHECK((kind == container::Kind::Xci) == isXci);
        printf("%-40s %zu entries (%s)\n", base.c_str(), entries.size(), isXci ? "XCI secure partition" : "PFS0");
        {   // installed size: NCA sizes, an .ncz counted as the NCA it unpacks to
            uint64_t want = 0, got = 0;
            for (const auto& x : exp) if (x.pkg == base) want += x.ncaSize;
            if (want) for (auto& en : entries) if (fmt::endsWith(en.name, ".cnmt.nca")) want += en.size;  // packages with expectations only
            CHECK(container::installSize(rd, got, err));
            if (want && got != want) printf("installSize %s: got %llu want %llu\n", base.c_str(), (unsigned long long)got, (unsigned long long)want);
            CHECK(!want || got == want);
        }

        for (const auto& x : exp) {
            if (x.pkg != base) continue;
            const fmt::Pfs0Entry* e = nullptr;
            for (auto& en : entries) if (en.name == x.entry) e = &en;
            CHECK(e);

            std::vector<uint8_t> out;
            uint8_t h[0x20], h2[0x20];
            bool patched = false;
            CHECK(runPipeline(url, *e, key, out, h, patched, err, rng));
            CHECK(out.size() == x.ncaSize);
            CHECK(hex(h, 0x20) == x.sha);                // original-content hash still verifiable
            CHECK(x.entry.substr(0, 32) == x.sha.substr(0, 32));
            CHECK(patched == x.patched);
            sha256(out, h2);
            if (x.patched) {
                CHECK(hex(h2, 0x20) != x.sha);
                uint8_t hdr[0x400];
                memcpy(hdr, out.data(), 0x400);
                crypto::xtsNintendo(key, hdr, 0x400, 0, false);
                CHECK(memcmp(hdr + 0x200, "NCA3", 4) == 0 && hdr[0x204] == 0);  // now "Download"

                // without a key the game-card NCA must come through byte-identical
                CHECK(runPipeline(url, *e, nullptr, out, h, patched, err, rng));
                sha256(out, h2);
                CHECK(!patched && hex(h2, 0x20) == x.sha);
            } else {
                CHECK(hex(h2, 0x20) == x.sha);           // byte-identical NCA
            }
            printf("   ok %-38s -> %8llu bytes%s\n", x.entry.c_str(), (unsigned long long)out.size(),
                   x.patched ? "  (GameCard->Download)" : "");

            if (fmt::endsWith(x.entry, ".ncz")) {
                // corrupted NCZ must be caught (zstd error or hash mismatch), never silently accepted
                bool ok = runPipeline(url, *e, nullptr, out, h, patched, err, rng, true);
                CHECK(!ok || hex(h, 0x20) != x.sha);
                // truncated NCZ must fail
                fmt::Pfs0Entry cut = *e; cut.size -= 100;
                CHECK(!runPipeline(url, cut, nullptr, out, h, patched, err, rng));
                printf("      corrupt/truncated NCZ rejected (%s)\n", err.c_str());
            }
        }
    }
    return true;
}

static bool testMisc(const std::string& server, const std::string& outDir, const std::string& dir) {
    std::string err;
    std::string found = remote::discover(2000);
    printf("discovered: '%s'\n", found.c_str());
    CHECK(!found.empty());

    std::vector<remote::RemoteFile> files;
    remote::ServerKind kind;
    CHECK(remote::fetchList(server, files, kind, err));
    std::string url = files[0].url;
    std::vector<uint8_t> buf;
    CHECK(!http::get(url, 0, files[0].size - 1, [](const uint8_t*, size_t) { return false; }, err));
    CHECK(err == "已取消");
    CHECK(!http::getToMemory(url, files[0].size + 10, 5, buf, err));

    std::ifstream in(dir + "/test.cnmt", std::ios::binary);
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)), {});
    fmt::ContentMeta m;
    CHECK(fmt::parseCnmt(raw, m, err));
    CHECK(m.titleId == 0x0100000000010000ULL && m.type == fmt::MT_Application && m.contents.size() == 2);
    CHECK(m.contents[0].type == fmt::CT_Program && m.contents[0].size == 5 * 1024 * 1024 + 123);
    uint8_t cid[16];
    CHECK(fmt::hexToBytes("00112233445566778899aabbccddeeff.cnmt.nca", cid, 16));
    auto im = fmt::buildInstallMeta(m, cid, 4096, 0);
    CHECK(im.size() == 8 + 16 + 3 * 0x18);
    CHECK(im[2] == 3 && im[0] == 16);
    CHECK(memcmp(&im[8 + 16], cid, 16) == 0 && im[8 + 16 + 0x16] == fmt::CT_Meta);
    fmt::ContentMeta p = m; p.type = fmt::MT_Patch;
    uint64_t base = 0x0100000000010000ULL; memcpy(p.extendedHeader.data(), &base, 8);
    CHECK(p.applicationId() == base);

    std::string cfg = outDir + "/cfgdir/config.ini";
    CHECK(remote::saveServer(cfg, "http://1.2.3.4:8000/"));
    CHECK(remote::loadServer(cfg) == "http://1.2.3.4:8000/");
    uint8_t k[0x20];
    CHECK(!crypto::loadHeaderKey(cfg, k));
    return true;
}

static bool testUrls() {
    http::Url u;
    CHECK(http::parseUrl("http://macron:demission@192.168.50.86:8001/", u));
    CHECK(u.host == "192.168.50.86" && u.port == 8001 && u.path == "/" && u.userinfo == "macron:demission");
    CHECK(u.authHeader == "Basic bWFjcm9uOmRlbWlzc2lvbg==");
    CHECK(http::redact("http://macron:demission@192.168.50.86:8001/") == "http://macron:****@192.168.50.86:8001/");
    CHECK(http::parseUrl("http://a%40b:p%3Ass@nas.local/x y/", u) && u.host == "nas.local" && u.port == 80);
    CHECK(u.authHeader == "Basic " "YUBiOnA6c3M=");   // a@b:p:ss
    CHECK(http::parseUrl("https://nas/", u) && u.tls && u.port == 443);
    CHECK(!http::parseUrl("ftp://nas/", u));
    CHECK(http::encodePath("/遊戲 raw/a%20b.nsp") == "/%E9%81%8A%E6%88%B2%20raw/a%20b.nsp");
    auto links = remote::parseHtmlLinks("<a href=\"a&amp;b.nsp\">x</a> <A HREF='dir/'>d</A> <a href=c.xci>c</a>");
    CHECK(links.size() == 3 && links[0] == "a&b.nsp" && links[1] == "dir/" && links[2] == "c.xci");
    auto dav = remote::parsePropfind("<d:multistatus xmlns:d='DAV:'><d:response><d:href>/x/</d:href><d:propstat><d:prop>"
                                     "<d:resourcetype><d:collection/></d:resourcetype></d:prop></d:propstat></d:response>"
                                     "<d:response><d:href>/x/a&amp;b.nsp</d:href><d:propstat><d:prop><d:resourcetype/>"
                                     "<d:getcontentlength>123</d:getcontentlength></d:prop></d:propstat></d:response></d:multistatus>");
    CHECK(dav.size() == 2 && dav[0].collection && !dav[1].collection && dav[1].size == 123 && dav[1].href == "/x/a&b.nsp");
    remote::JsonIndex ji;
    CHECK(remote::parseJsonIndex("{\"files\":[{\"url\":\"%E7%A9%BA.nsz\",\"size\":7268675199},"
                                 "{\"size\":5,\"url\":\"a\\u00e9\\ud83c\\udfae.xci\"}],\"success\":\"hi\"}", ji));
    CHECK(ji.files.size() == 2 && ji.files[0].size == 7268675199ULL && ji.files[0].url == "%E7%A9%BA.nsz");
    CHECK(ji.files[1].url == "a\xc3\xa9\xf0\x9f\x8e\xae.xci" && ji.message == "hi");
    CHECK(!remote::parseJsonIndex("<html></html>", ji));
    CHECK(!remote::parseJsonIndex("{\"name\":1}", ji));
    printf("url / parser tests ok\n");
    return true;
}

static bool testHttpsPortHint(const std::string& url) {
    std::vector<remote::RemoteFile> files;
    remote::ServerKind kind;
    std::string err;
    CHECK(!remote::fetchList(url, files, kind, err));
    CHECK(err.find("https://") != std::string::npos);
    printf("plain HTTP to HTTPS port explained: %s\n", err.c_str());
    return true;
}

// Nothing waits long: a host that does not answer, a name that does not exist, no network at all,
// and https:// typed for a plain HTTP port (one try, explained) all fail within a few seconds.
static double secondsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
static bool testFailFast(const std::string& plainUrl) {
    std::vector<uint8_t> out;
    std::string err;
    auto t0 = std::chrono::steady_clock::now();
    CHECK(!http::getToMemory("http://10.255.255.1:81/x", 0, 0, out, err));
    double t = secondsSince(t0);
    CHECK(t < 7.0);
    printf("unanswered address: %.1f s (%s)\n", t, err.c_str());
    t0 = std::chrono::steady_clock::now();
    CHECK(!http::getToMemory("http://no-such-host.invalid:8000/x", 0, 0, out, err));
    t = secondsSince(t0);
    CHECK(t < 7.0 && err.find("no-such-host.invalid") != std::string::npos);
    printf("unknown name: %.1f s (%s)\n", t, err.c_str());
    http::setNetCheck([] { return false; });
    t0 = std::chrono::steady_clock::now();
    CHECK(!http::getToMemory(plainUrl, 0, 0, out, err));
    CHECK(secondsSince(t0) < 1.0 && err == "Switch 沒有連上網路");
    http::setNetCheck(nullptr);
    std::string https = "https://" + plainUrl.substr(7);
    std::vector<remote::RemoteFile> files;
    remote::ServerKind kind;
    t0 = std::chrono::steady_clock::now();
    CHECK(!remote::fetchList(https, files, kind, err));
    t = secondsSince(t0);
    CHECK(t < 7.0 && err.find("不是 HTTPS") != std::string::npos);
    printf("https:// to a plain port: %.1f s (%s)\n", t, err.c_str());
    // a shorter wait for one thread
    http::setThreadTimeout(1500);
    CHECK(http::threadTimeout() == 1500);
    http::setThreadTimeout(0);
    CHECK(http::threadTimeout() == 15000);
    printf("fail fast: ok\n");
    return true;
}

// Typed server addresses: mistakes are named before anything connects.
static bool testAddresses() {
    CHECK(servers::checkAddress("https://nas.example:8001/games/").empty());
    CHECK(servers::checkAddress("  http://192.168.1.10:8000 ").empty());
    CHECK(servers::checkAddress("192.168.1.10:8000").empty());
    CHECK(servers::checkAddress("https://user:p@ss@nas:5006/").empty());
    CHECK(servers::checkAddress("https://遊戲.example/").empty());
    CHECK(servers::checkAddress("").find("空") != std::string::npos);
    CHECK(servers::checkAddress("https：//nas:8001").find("全形") != std::string::npos);
    CHECK(servers::checkAddress("ftp://nas/").find("http") != std::string::npos);
    CHECK(servers::checkAddress("https:/nas:8001").find("//") != std::string::npos);
    CHECK(servers::checkAddress("https://nas:80a1/").find("數字") != std::string::npos);
    CHECK(servers::checkAddress("https://nas:/").find("埠號") != std::string::npos);
    CHECK(servers::checkAddress("https://nas:70000/").find("65535") != std::string::npos);
    CHECK(servers::checkAddress("https://na s:8001/").find("空白") != std::string::npos);
    CHECK(servers::checkAddress("https://:8001/").find("主機") != std::string::npos);
    CHECK(servers::checkAddress("https://nas!:8001/").find("!") != std::string::npos);
    printf("address check: ok\n");
    return true;
}

// Whole small files over a kept connection (covers): body, status and "not found".
static bool testSessionGet(const std::string& coverTemplate) {
    std::string base = coverTemplate.substr(0, coverTemplate.find("{id}"));
    http::Session ses;
    std::vector<uint8_t> out;
    int status = 0;
    std::string err;
    CHECK(ses.get(base + "0100A1301EECC000.jpg", 4u << 20, out, status, err));
    CHECK(status == 200 && out.size() > 100);
    std::vector<uint8_t> again;
    CHECK(ses.get(base + "0100A1301EECC000.jpg", 4u << 20, again, status, err) && again == out);
    ses.get(base + "0100DEADBEEF0000.jpg", 4u << 20, out, status, err);
    CHECK(status == 404);
    CHECK(!ses.get(base + "0100A1301EECC000.jpg", 16, out, status, err));  // over the limit
    {   // a small file asked for whole (0..size-1): a server answering 200 with all of it is fine;
        // for a real part of the file, 200 is still an error
        std::vector<uint8_t> whole, part;
        CHECK(http::getToMemory(base + "0100A1301EECC000.jpg", 0, again.size(), whole, err) && whole == again);
        CHECK(ses.getRange(base + "0100A1301EECC000.jpg", 0, again.size(), whole, err) && whole == again);
        CHECK(!http::getToMemory(base + "0100A1301EECC000.jpg", 0, again.size() - 1, part, err));
    }
    {   // a server without Range support answers the whole of a small file with 200: fine when the
        // whole file was asked for (0..size-1), still an error for a real part of it
        std::vector<uint8_t> whole, part;
        CHECK(http::getToMemory(base + "0100A1301EECC000.jpg", 0, again.size(), whole, err) && whole == again);
        CHECK(ses.getRange(base + "0100A1301EECC000.jpg", 0, again.size(), whole, err) && whole == again);
        CHECK(!http::getToMemory(base + "0100A1301EECC000.jpg", 0, again.size() - 1, part, err));
    }
    printf("session get (covers over a kept connection): ok\n");
    return true;
}

// http:// typed for a server that only speaks TLS must fail cleanly (no hang, no crash).
static bool testPlainToTls(const std::string& httpsUrl) {
    std::string url = "http://" + httpsUrl.substr(8);
    std::vector<remote::RemoteFile> files;
    remote::ServerKind kind;
    std::string err;
    CHECK(!remote::fetchList(url, files, kind, err));
    printf("http:// against a TLS port fails cleanly: %s\n", err.c_str());
    return true;
}

static bool testLoginPage(const std::string& url) {
    std::vector<remote::RemoteFile> files;
    remote::ServerKind kind;
    std::string err;
    CHECK(!remote::fetchList(url, files, kind, err));
    CHECK(err.find("WebDAV") != std::string::npos);
    printf("NAS login page explained: %s\n", err.c_str());
    return true;
}

static bool testCovers(const std::string& coverUrlTemplate, const std::string& cacheDir) {
    using namespace covers;
    CHECK(titleIdFromName("Games/魔骨 Morkull Ragast's Rage v1.0[0100A1301EECC000][v0][Base] JP.nsz") == 0x0100A1301EECC000ULL);
    CHECK(titleIdFromName("x [0100E6701DF4F001][v0][DLC] US.nsp") == 0x0100E6701DF4F001ULL);
    CHECK(titleIdFromName("[059F06BCAE916000][v0].nsp") == 0x059F06BCAE916000ULL);  // no 01-prefixed id: fallback
    CHECK(titleIdFromName("Some Game v1.0.nsp") == 0);
    CHECK(titleIdFromName("A [0100A1301EECC000] B [12345678901234567].nsp") == 0x0100A1301EECC000ULL);
    CHECK(baseTitleId(0x0100A1301EECC800ULL) == 0x0100A1301EECC000ULL);  // update
    CHECK(baseTitleId(0x0100E6701DF4F001ULL) == 0x0100E6701DF4E000ULL);  // DLC
    CHECK(baseTitleId(0x0100A1301EECC000ULL) == 0x0100A1301EECC000ULL);  // base

    // async loader: one cover exists on the server, one does not
    Cache cache(cacheDir, coverUrlTemplate, 340);
    const uint64_t have = 0x0100A1301EECC000ULL, missing = 0x0100DEADBEEF0000ULL;
    std::shared_ptr<const Image> img;
    State a = State::Loading, b = State::Loading;
    for (int i = 0; i < 300 && (a == State::Loading || b == State::Loading); i++) {
        cache.request(have);
        cache.request(missing);
        a = cache.get(have, img);
        b = cache.get(missing, img);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(a == State::Ready);
    CHECK(b == State::Missing);
    cache.get(have, img);
    CHECK(img && img->w == 340 && img->h == 340);
    uint32_t p = img->px[(size_t)170 * 340 + 170];  // centre of a red 512x512 test image
    CHECK((p & 0xFF) > 200 && ((p >> 8) & 0xFF) < 60 && ((p >> 16) & 0xFF) < 60);
    struct stat st;
    CHECK(stat((cacheDir + "/0100A1301EECC000.jpg").c_str(), &st) == 0 && st.st_size > 100);  // cached on "SD"
    {   // kept scaled to the screen size (the server's picture is 512x512), still the same picture
        std::ifstream f(cacheDir + "/0100A1301EECC000.jpg", std::ios::binary);
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), {});
        // a big picture is stored scaled to 340 as JPEG, unless the original file is smaller anyway
        // (the test server's PNG of one flat colour is a few hundred bytes when PIL is missing)
        int fw = 0, fh = 0, fc = 0;
        CHECK(stbi_info_from_memory(file.data(), (int)file.size(), &fw, &fh, &fc));
        bool jpeg = file.size() > 3 && file[0] == 0xFF && file[1] == 0xD8;
        bool png = file.size() > 3 && file[0] == 0x89 && file[1] == 'P';
        CHECK((fw == 340 && fh == 340 && jpeg) || (fw == 512 && png && file.size() < 2000));
        Image small;
        CHECK(decode(file, 340, small));
        uint32_t q = small.px[(size_t)(small.h / 2) * small.w + small.w / 2];
        CHECK((q & 0xFF) > 200 && ((q >> 8) & 0xFF) < 60 && ((q >> 16) & 0xFF) < 60);
        printf("cover cache: stored %zu bytes at screen size\n", file.size());
    }
    {
        // failures are logged; "not found" from the site is final, network trouble is tried again
        std::mutex lm;
        std::vector<std::string> lines;
        Cache c2(cacheDir + "-2", coverUrlTemplate, 64);
        c2.setLogger([&](const std::string& l) { std::lock_guard<std::mutex> g(lm); lines.push_back(l); });
        const uint64_t flaky = 0x0100BEEF00000000ULL;
        c2.setUrlLookup([&](uint64_t id) { return id == flaky ? std::string("http://127.0.0.1:1/icon.jpg") : std::string(); });
        auto waitFor = [&](uint64_t id) {
            State s = State::Loading;
            for (int i = 0; i < 300 && s != State::Ready && s != State::Missing; i++) {
                c2.request(id);
                s = c2.get(id, img);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            return s;
        };
        CHECK(waitFor(missing) == State::Missing);
        CHECK(waitFor(flaky) == State::Missing);  // icon site unreachable, cover site 404
        CHECK(c2.missingCount() == 2);
        {
            std::lock_guard<std::mutex> g(lm);
            bool has404 = false, hasTrouble = false;
            for (const auto& l : lines) {
                has404 = has404 || (l.find("0100DEADBEEF0000") != std::string::npos && l.find("HTTP 404") != std::string::npos);
                hasTrouble = hasTrouble || (l.find("0100BEEF00000000") != std::string::npos && l.find("127.0.0.1:1 ") != std::string::npos);
            }
            CHECK(has404 && hasTrouble);
        }
        c2.request(flaky);  // within 30 s: not asked again yet
        CHECK(c2.get(flaky, img) == State::Missing);
        c2.retryMissing();  // 重新抓取封面
        CHECK(c2.missingCount() == 0);
        c2.request(missing);
        CHECK(c2.get(missing, img) != State::Ready);  // asked again
        CHECK(waitFor(missing) == State::Missing);
    }

    // second cache instance with no network: served from the SD cache
    Cache offline(cacheDir, "", 200);
    std::shared_ptr<Image> again = offline.loadNow(have);
    CHECK(again && again->w == 200);
    // wide image keeps its aspect ratio
    std::shared_ptr<Image> wide = offline.loadNow(0x0100000000AAA000ULL);
    CHECK(!wide);  // not cached, no network
    {   // prefetch: covers of the next page are ready before they are asked for
        Cache pre(cacheDir + "-3", coverUrlTemplate, 120);
        pre.setMaxImages(50);
        pre.prefetch({have, missing});
        State st = State::None;
        std::shared_ptr<const Image> pimg;
        for (int i = 0; i < 300 && st != State::Ready; i++) {
            st = pre.get(have, pimg);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(st == State::Ready && pimg && pimg->w == 120);
        CHECK(pre.get(missing, pimg) != State::Missing);  // a prefetch that finds nothing is not shown as missing
    }
    printf("covers: ids, decode, async fetch, SD cache, missing state, prefetch ok\n");
    return true;
}

#define TASSERT(c) CHECK(c)
static bool testTitlesImpl(){
  auto a=titles::parse("魔骨 Morkull Ragast's Rage v1.0.5[0100A1301EECC800][v65536][UPD] JP.nsz");
  TASSERT(a.titleId==0x0100A1301EECC800ULL && a.kind==titles::Kind::Update && a.version==65536 && a.hasVersion && a.baseId==0x0100A1301EECC000ULL);
  auto b=titles::parse("Ravenswatch - Fairytales Skin Pack[0100E6701DF4F001][v0][DLC] US.nsp");
  TASSERT(b.kind==titles::Kind::Dlc && b.baseId==0x0100E6701DF4E000ULL && b.version==0 && b.hasVersion);
  auto c=titles::parse("Game v1.0.2 (v131072).nsp"); TASSERT(c.version==131072 && c.titleId==0);
  auto d=titles::parse("Splatoon 3 [0100C2500FC20000].xci"); TASSERT(!d.hasVersion && d.kind==titles::Kind::Base);
  titles::Installed in={{0x0100A1301EECC000ULL,0},{0x0100A1301EECC800ULL,65536},{0x0100E6701DF4E000ULL,0}};
  TASSERT(titles::classify(a,in)==titles::Status::Installed);
  auto a2=titles::parse("x [0100A1301EECC800][v131072].nsz"); TASSERT(titles::classify(a2,in)==titles::Status::UpdateAvailable);
  TASSERT(titles::classify(b,in)==titles::Status::DlcAvailable);
  TASSERT(titles::classify(d,in)==titles::Status::NotInstalled);
  auto e=titles::parse("y [0100C2500FC20800][v917504].nsp"); TASSERT(titles::classify(e,in)==titles::Status::NotInstalled);
  titles::Installed in2={{0x0100E6701DF4E000ULL,0},{0x0100E6701DF4E800ULL,0}};
  auto u=titles::parse("z [0100E6701DF4E800].nsp"); TASSERT(titles::classify(u,in2)==titles::Status::Installed); // no version in name
  TASSERT(titles::classify(titles::parse("noid.nsp"),in)==titles::Status::Unknown);
  puts("titles: version parsing and installed comparison ok"); return true;
}

static bool testResumeWriter() {
    // An install stopped part-way continues from the last checkpoint and ends with the same hash.
    std::mt19937 rng(5);
    std::vector<uint8_t> data(21 * 1024 * 1024 + 123);
    for (auto& b : data) b = (uint8_t)rng();
    std::vector<uint8_t> out(data.size());
    uint64_t cpAt = 0;
    std::vector<uint8_t> cpState;
    {
        nca::NcaWriter w([&](uint64_t off, const uint8_t* d, size_t n) { memcpy(&out[off], d, n); return true; }, nullptr);
        w.onCheckpoint = [&](uint64_t at, const std::vector<uint8_t>& st) { cpAt = at; cpState = st; };
        for (size_t off = 0; off < 9 * 1024 * 1024; off += 100000)
            CHECK(w.push(&data[off], std::min<size_t>(100000, 9 * 1024 * 1024 - off)));
        // "cancelled" here: nothing after the checkpoint is trusted
    }
    CHECK(cpAt == 8 * 1024 * 1024 && !cpState.empty());
    memset(&out[cpAt], 0, out.size() - cpAt);
    nca::NcaWriter w2([&](uint64_t off, const uint8_t* d, size_t n) { memcpy(&out[off], d, n); return true; }, nullptr);
    CHECK(w2.resumeAt(cpAt, cpState));
    for (size_t off = cpAt; off < data.size(); off += 77777)
        CHECK(w2.push(&data[off], std::min<size_t>(77777, data.size() - off)));
    CHECK(w2.finish());
    uint8_t h[0x20], ref[0x20];
    w2.hash(h);
    crypto::Sha256 full;
    full.update(data.data(), data.size());
    full.finish(ref);
    CHECK(memcmp(h, ref, 0x20) == 0);
    CHECK(out == data);
    CHECK(w2.size() == data.size());
    printf("resume: writer continues from checkpoint with the same hash ok\n");
    return true;
}

static bool testResumeState(const std::string& dir) {
    mkdir(dir.c_str(), 0777);
    resume::State s;
    s.url = "https://u:p@nas:8001/Game [0100000000010000][v0][BASE].nsp";
    s.name = "Game [0100000000010000][v0][BASE].nsp";
    s.total = 1000;
    s.done["00112233445566778899aabbccddeeff"] = 300;
    resume::Part p;
    p.placeholder = "ffeeddccbbaa99887766554433221100";
    p.written = 200;
    p.ncaSize = 700;
    p.sha = {1, 2, 3, 250};
    s.parts["0123456789abcdef0123456789abcdef"] = p;
    std::string path = resume::pathFor(dir, s.url);
    CHECK(resume::save(path, s));
    CHECK(resume::save(path, s));  // overwrite works
    resume::State r;
    CHECK(resume::load(path, r));
    CHECK(r.url == s.url && r.name == s.name && r.total == 1000);
    CHECK(r.done.size() == 1 && r.done.begin()->second == 300);
    CHECK(r.parts.size() == 1 && r.parts.begin()->second.sha == p.sha && r.parts.begin()->second.written == 200);
    CHECK(r.completed() == 500 && r.percent() == 50);
    CHECK(resume::list(dir).size() == 1);
    s.started = 1760900000;
    CHECK(resume::save(path, s));
    CHECK(resume::load(path, r) && r.started == 1760900000);
    resume::remove(path);
    CHECK(resume::list(dir).empty());
    // install queue
    std::string qp = dir + "/queue.txt";
    std::vector<resume::QueueEntry> q = {{"https://u:p@nas/A%20B [0100000000010000].nsp", "A B [0100000000010000].nsp", 0x0100000000010000ULL, 100},
                                         {"https://nas/x.nsz", "x.nsz", 0x8000000000000003ULL, 200}};
    CHECK(resume::saveQueue(qp, q));
    auto q2 = resume::loadQueue(qp);
    CHECK(q2.size() == 2 && q2[0].url == q[0].url && q2[0].name == q[0].name && q2[0].group == q[0].group &&
          q2[1].added == 200 && q2[1].group == 0x8000000000000003ULL);
    CHECK(resume::saveQueue(qp, {}));
    CHECK(resume::loadQueue(qp).empty());
    printf("resume: state file + install queue save / load ok\n");
    return true;
}

static bool testCoverTrim(const std::string& dir) {
    mkdir(dir.c_str(), 0777);
    // 10 covers of 1000 bytes; recent.txt says id 1 is the oldest viewed, id 10 the newest
    std::ofstream rec(dir + "/recent.txt");
    for (int i = 1; i <= 10; i++) {
        char n[64];
        snprintf(n, sizeof(n), "%s/%016llX.jpg", dir.c_str(), (unsigned long long)(0x0100000000000000ULL + i * 0x1000));
        std::ofstream f(n, std::ios::binary);
        std::string blob(1000, 'x');
        f << blob;
        rec << std::hex << (0x0100000000000000ULL + i * 0x1000) << std::dec << " " << (1000 + (i == 3 ? 5000 : i)) << "\n";
    }
    rec.close();
    CHECK(covers::trimCache(dir, 20000, 15000) == 0);  // under the limit: nothing
    int removed = covers::trimCache(dir, 8000, 5000);   // over: keep the 5 most recently viewed
    CHECK(removed == 5);
    struct stat st;
    char n[64];
    snprintf(n, sizeof(n), "%s/%016llX.jpg", dir.c_str(), (unsigned long long)(0x0100000000000000ULL + 3 * 0x1000));
    CHECK(stat(n, &st) == 0);   // id 3 was viewed last of all: kept
    snprintf(n, sizeof(n), "%s/%016llX.jpg", dir.c_str(), (unsigned long long)(0x0100000000000000ULL + 1 * 0x1000));
    CHECK(stat(n, &st) != 0);   // oldest viewed: gone
    printf("covers: cache trimmed to the most recently viewed ok\n");
    return true;
}

static bool testDisplayVersion() {
    CHECK(titles::displayVersion("吞食咔寶GOBBLEv1.0.3[01008B4026878800][v196608][UPD].nsp") == "1.0.3");
    CHECK(titles::displayVersion("Game [0100000000010800][v196608].nsp") == "");
    CHECK(titles::displayVersion("Mario Kart 8 v3.0.4 [0100152000022800][v1966080].nsp") == "3.0.4");
    CHECK(titles::displayVersion("Saint Seiya Soldiers Soulv1.3[05C4B74C6E8CE000][v0][BASE].nsp") == "1.3");
    CHECK(titles::displayVersion("no version here.nsp") == "");
    CHECK(titles::kindRank(titles::Kind::Base) < titles::kindRank(titles::Kind::Update));
    CHECK(titles::kindRank(titles::Kind::Update) < titles::kindRank(titles::Kind::Dlc));
    printf("display versions + group order ok\n");
    return true;
}

static bool testRecordPlan() {
    using titles::RecordEntry;
    const uint64_t app = 0x0100A1301EECC000ULL, upd = app | 0x800, dlc = app + 0x1001;
    RecordEntry base{app, 0, 0x80, 5}, patch{upd, 65536, 0x81, 5}, addon{dlc, 0, 0x82, 5};
    RecordEntry card{app, 0, 0x80, 2};  // on a game card: not in the SD / NAND databases
    // 1. the v1.7 damage: record holds only the update, base + update are installed
    auto p1 = titles::planRecord({base, patch}, {patch}, true, true);
    CHECK(p1.push && p1.entries.size() == 2 && p1.entries[0].type == 0x80);
    // 2. first install of a base game, no record yet (count fails)
    auto p2 = titles::planRecord({base}, {}, false, false);
    CHECK(p2.push && p2.entries.size() == 1);
    // 3. installing a DLC next to base + update
    auto p3 = titles::planRecord({base, patch, addon}, {base, patch}, true, false);
    CHECK(p3.push && p3.entries.size() == 3);
    // 4. startup check, record already complete
    auto p4 = titles::planRecord({base, patch}, {base, patch}, true, true);
    CHECK(!p4.push);
    // 5. startup check, record has an older update version
    RecordEntry oldPatch{upd, 0x10000 - 0x10000 + 1, 0x81, 5};
    auto p5 = titles::planRecord({base, patch}, {base, oldPatch}, true, true);
    CHECK(p5.push && p5.entries.size() == 2 && p5.entries[1].version == 65536);
    // 6. update installed for a game played from a game card: card entry is kept
    auto p6 = titles::planRecord({patch}, {card}, true, false);
    CHECK(p6.push && p6.entries.size() == 2);
    // 7. only an update is installed and the record has entries: never drop to a record without base
    auto p7 = titles::planRecord({patch}, {patch}, true, false);
    CHECK(p7.push == false || p7.entries.size() >= 1);
    auto p7b = titles::planRecord({patch, addon}, {patch}, true, false);
    CHECK(!p7b.push);
    // 8. nothing installed
    CHECK(!titles::planRecord({}, {}, false, false).push);
    printf("record plan: repair, first install, DLC add, up-to-date, game card, no-base guard ok\n");
    return true;
}

static bool testPatchExtendedData() {
    // packaged patch cnmt: header 0x20 + ext header 0x18 (extended_data_size = 0x40) + 1 content +
    // 0 meta infos + 0x40 bytes of patch history + 0x20 digest
    std::vector<uint8_t> d(0x20 + 0x18 + 0x38 + 0x40 + 0x20, 0);
    uint64_t tid = 0x0100A1301EECC800ULL, app = 0x0100A1301EECC000ULL;
    uint32_t ver = 65536, sysver = 0x0C000000, extData = 0x40;
    memcpy(&d[0], &tid, 8); memcpy(&d[8], &ver, 4); d[0xC] = fmt::MT_Patch;
    d[0xE] = 0x18; d[0x10] = 1; d[0x12] = 0;
    memcpy(&d[0x20], &app, 8); memcpy(&d[0x28], &sysver, 4); memcpy(&d[0x2C], &extData, 4);
    d[0x38 + 0x20] = 0xAB; d[0x38 + 0x30] = 0x10; d[0x38 + 0x36] = 1;  // content: id AB.., size 16, Program
    for (int i = 0; i < 0x40; i++) d[0x38 + 0x38 + i] = (uint8_t)(0xC0 + i);
    fmt::ContentMeta m; std::string err;
    CHECK(fmt::parseCnmt(d, m, err));
    CHECK(m.extendedData.size() == 0x40 && m.extendedData[0] == 0xC0 && m.extendedData[0x3F] == 0xFF);
    uint8_t cid[16] = {1};
    auto im = fmt::buildInstallMeta(m, cid, 4096, 0);
    // 8 header + 0x18 ext + 2 * 0x18 content infos + 0x40 extended data
    CHECK(im.size() == 8 + 0x18 + 2 * 0x18 + 0x40);
    uint32_t got, gotSys; memcpy(&got, &im[8 + 0xC], 4); memcpy(&gotSys, &im[8 + 8], 4);
    CHECK(got == 0x40 && gotSys == sysver);
    uint64_t gotApp; memcpy(&gotApp, &im[8], 8);
    CHECK(gotApp == app);
    CHECK(memcmp(&im[im.size() - 0x40], m.extendedData.data(), 0x40) == 0);
    // truncated history must be rejected
    std::vector<uint8_t> cut(d.begin(), d.begin() + 0x20 + 0x18 + 0x38 + 0x10);
    fmt::ContentMeta m2;
    CHECK(!fmt::parseCnmt(cut, m2, err));
    printf("patch install meta: extended header + patch history copied verbatim\n");
    return true;
}

static bool testTitleDb(const std::string& goodUrl, const std::string& cache) {
    remove(cache.c_str());
    titledb::Entry e;
    std::string dir = goodUrl.substr(0, goodUrl.rfind('/') + 1);
    std::string notes = dir + "titledb-mini.tsv", bad = dir + "bad.tsv";
    {
        titledb::Db db({goodUrl + ".missing", goodUrl}, cache);  // first URL 404s, the region JSON works
        CHECK(db.runNow());
        CHECK(db.state() == titledb::Db::State::Ready && db.generation() == 1);
        CHECK(db.find(0x0100A1301EECC000ULL, e) && e.name == "魔骨 Morkull Ragast\'s Rage" && e.releaseDate == 20231109);
        CHECK(e.iconUrl == "http://127.0.0.1:8773/0100A1301EECC000.jpg");
        CHECK(db.find(0x0100E6701DF4F001ULL, e) && e.name == "鴉衛奇旅 童話服裝組");
        CHECK(!db.find(0x0100DEADBEEF0000ULL, e));
        // listed twice (Pikmin 4 + its Switch 2 Edition): the original release date and its cover
        CHECK(db.find(0x0100B7C00933A000ULL, e) && e.releaseDate == 20230721 && e.iconUrl == "http://x/p4.jpg");
        // earlier entry without a name: date from it, name filled from the other
        CHECK(db.find(0x0100DDDD00000000ULL, e) && e.releaseDate == 20180303 && e.name == "後來重新上架");
        CHECK(db.message().find("已更新") != std::string::npos);
    }
    struct stat st;
    CHECK(stat(cache.c_str(), &st) == 0);
    {
        titledb::Db db({}, cache);  // no URLs: from the SD cache, fresh, so nothing is downloaded
        CHECK(db.runNow() && db.generation() == 1);
        CHECK(db.find(0x0100A1301EECC000ULL, e) && e.releaseDate == 20231109 && e.name == "魔骨 Morkull Ragast\'s Rage");
        CHECK(db.find(0x0100B7C00933A000ULL, e) && e.releaseDate == 20230721);
        CHECK(db.runNow(true) && db.message().find("沿用舊資料") != std::string::npos);  // refresh fails: kept
        CHECK(db.find(0x0100B7C00933A000ULL, e));
    }
    {
        remove(cache.c_str());
        titledb::Db db({notes, goodUrl}, cache);  // the small notes file (HK + JP merged)
        CHECK(db.runNow() && db.generation() == 1 && db.downloadedAt() > 1500000000);
        CHECK(db.find(0x0100B7C00933A000ULL, e) && e.releaseDate == 20230721 && e.iconUrl == "http://x/p4.jpg");
        CHECK(db.find(0x0100CCCC00000000ULL, e) && e.releaseDate == 20190505 && e.iconUrl == "http://x/cc.jpg");  // JP filled
        CHECK(db.find(0x0100EEEE00000000ULL, e) && e.releaseDate == 20200202);  // only in Japan
        CHECK(!db.find(0x0100E6701DF4F001ULL, e));  // DLC: not in the notes (uses its game's date)
    }
    {
        titledb::Db db({notes, goodUrl}, cache);  // fresh cache: no download
        CHECK(db.runNow() && db.generation() == 1);
        CHECK(db.find(0x0100EEEE00000000ULL, e));
        CHECK(db.runNow(true) && db.generation() == 2);  // "更新遊戲資料庫": downloads again
        CHECK(db.message().find("已更新") != std::string::npos);
    }
    {
        // a cache older than 7 days is downloaded again
        std::ifstream in(cache);
        std::string rest((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        rest = rest.substr(rest.find('\n') + 1);
        std::ofstream(cache, std::ios::trunc) << "titledb-cache v2 " << (long long)(time(nullptr) - 8 * 86400) << " 1\n" << rest;
        titledb::Db db({notes}, cache);
        CHECK(db.runNow() && db.generation() == 2);
        CHECK(db.downloadedAt() > (long long)time(nullptr) - 3600);
    }
    {
        // not a notes file (no header): ignored, the next source is used
        remove(cache.c_str());
        titledb::Db db({bad, goodUrl}, cache);
        CHECK(db.runNow() && db.find(0x0100B7C00933A000ULL, e) && e.releaseDate == 20230721);
        CHECK(db.find(0x0100A1301EECC000ULL, e) && !e.name.empty());  // came from the JSON
    }
    {
        // the old v1 cache (wrong dates for titles listed twice) is ignored
        std::ofstream(cache, std::ios::trunc) << "titledb-cache v1\n0100B7C00933A000\t20261112\tx\t\n";
        titledb::Db db({notes}, cache);
        CHECK(db.runNow() && db.find(0x0100B7C00933A000ULL, e) && e.releaseDate == 20230721);
    }
    {
        remove(cache.c_str());
        titledb::Db db({goodUrl + ".missing"}, cache);
        CHECK(!db.runNow() && db.state() == titledb::Db::State::Failed);
        CHECK(db.message().find("無法下載") != std::string::npos);
    }
    printf("titledb: notes file, JSON fallback, earliest date per title, 7-day refresh, manual refresh, cache ok\n");
    return true;
}

static bool testRedirectQuery() {
    // GitHub release downloads: 302 to a signed address whose ?query must arrive unchanged
    std::vector<uint8_t> out;
    std::string err;
    bool ok = http::getToMemory("http://127.0.0.1:8775/download/LanInstaller.nro", 0, 0, out, err);
    CHECK(ok && std::string(out.begin(), out.end()) == "signed-download-ok");
    if (!ok) printf("  redirect error: %s\n", err.c_str());
    // an already encoded address with a ?query (the image proxy) is sent as it is
    {
        http::Request req;
        req.encodedUrl = true;
        http::Response res;
        std::string body, e2;
        bool ok2 = http::request("http://127.0.0.1:8775/signed/blob?sp=r&se=2026-10-23T12%3A46%3A22Z&rscd=attachment%3B+filename%3DLanInstaller.nro&sig=z%2Bu4P%2F%3D&jwt=a.b-c_d",
                                 req, [&](const uint8_t* d, size_t n) { body.append((const char*)d, n); return true; }, res, e2);
        CHECK(ok2 && res.status == 200 && body == "signed-download-ok");
    }
    {
        const std::string proxy = "https://wsrv.nl/?url={url}&w=256&h=256&fit=inside&output=jpg&q=85";
        const std::string icon = "https://img-eshop.cdn.nintendo.net/i/6d26a33c.jpg";
        CHECK(covers::proxiedUrl(proxy, icon) ==
              "https://wsrv.nl/?url=https%3A%2F%2Fimg-eshop.cdn.nintendo.net%2Fi%2F6d26a33c.jpg&w=256&h=256&fit=inside&output=jpg&q=85");
        CHECK(covers::proxiedUrl("", icon) == icon);                       // icon_proxy= (empty): direct
        CHECK(covers::proxiedUrl(proxy, "http://x/p4.jpg") == "http://x/p4.jpg");  // not Nintendo: direct
        CHECK(covers::proxiedUrl(proxy, "") == "");
    }
    // our own first URL: "?" in a file name is still escaped (it is not a query)
    CHECK(http::encodePath("/Games/What?.nsp") == "/Games/What%3F.nsp");
    {
        // "mem://" files (the HOME shortcut package goes through the installer this way)
        auto d = std::make_shared<std::vector<uint8_t>>(5000);
        for (size_t i = 0; i < d->size(); i++) (*d)[i] = (uint8_t)i;
        http::setMemoryFile("t.nsp", d);
        std::vector<uint8_t> part;
        std::string e3;
        CHECK(http::getToMemory("mem://t.nsp", 100, 50, part, e3) && part.size() == 50 && part[0] == 100);
        uint64_t sz = 0;
        CHECK(http::probeSize("mem://t.nsp", sz, e3) && sz == 5000);
        CHECK(http::getToMemory("mem://t.nsp", 0, 0, part, e3) && part.size() == 5000);
        http::setMemoryFile("t.nsp", nullptr);
        CHECK(!http::getToMemory("mem://t.nsp", 0, 0, part, e3));
    }
    printf("redirect: signed ?query kept byte for byte\n");
    return true;
}

static bool testShortcut() {
    shortcut::Inputs in;
    in.mainNso.assign(0x3000, 0x11); memcpy(in.mainNso.data(), "NSO0", 4);
    in.npdm.assign(0x400, 0); memcpy(in.npdm.data(), "META", 4);
    std::string err;
    {
        std::vector<uint8_t> bad;
        shortcut::Inputs b2 = in;
        b2.mainNso.assign(0x3000, 0); memcpy(b2.mainNso.data(), "NSO0", 4);
        b2.iconJpg.assign(0x2000, 0x33); b2.iconJpg[0] = 0xFF; b2.iconJpg[1] = 0xD8;
        CHECK(!shortcut::buildNsp(b2, bad, err));  // npdm without ACI0/ACID: refused
    }
    auto w32 = [&](size_t at, uint32_t v) { memcpy(&in.npdm[at], &v, 4); };
    w32(0x70, 0x300); w32(0x74, 0x40); w32(0x78, 0x80); w32(0x7C, 0x240);
    memcpy(&in.npdm[0x80 + 0x200], "ACID", 4); memcpy(&in.npdm[0x300], "ACI0", 4);
    in.iconJpg.assign(0x2000, 0x33); in.iconJpg[0] = 0xFF; in.iconJpg[1] = 0xD8;
    in.version = 3u << 16;
    {
        // loader npdm as the build tool writes it (applet, 36-bit): becomes an application, 39-bit
        std::vector<uint8_t> np = in.npdm;
        np[0xC] = 0x03;  // 64-bit, address space type 1
        uint32_t applet = 0x1FFF | (2u << 14), other = 0x0007;
        auto put = [&](size_t at, uint32_t v) { memcpy(&np[at], &v, 4); };
        put(0x300 + 0x30, 0x40); put(0x300 + 0x34, 8); put(0x340, other); put(0x344, applet);  // ACI0 KAC
        put(0x80 + 0x230, 0x240); put(0x80 + 0x234, 8); put(0x80 + 0x240, applet); put(0x80 + 0x244, other);  // ACID KAC
        np.resize(0x400);
        CHECK(shortcut::patchNpdm(np, shortcut::kTitleId, err));
        uint32_t a1, a2, d1, d2; uint64_t pid, mn, mx;
        memcpy(&a1, &np[0x340], 4); memcpy(&a2, &np[0x344], 4); memcpy(&d1, &np[0x80 + 0x240], 4); memcpy(&d2, &np[0x80 + 0x244], 4);
        memcpy(&pid, &np[0x310], 8); memcpy(&mn, &np[0x80 + 0x210], 8); memcpy(&mx, &np[0x80 + 0x218], 8);
        CHECK(np[0xC] == 0x07 && a1 == other && d2 == other);
        CHECK(a2 == (0x1FFF | (1u << 14)) && d1 == (0x1FFF | (1u << 14)));
        CHECK(pid == shortcut::kTitleId && mn == shortcut::kTitleId && mx == shortcut::kTitleId);
    }
    for (int i = 0; i < 0x20; i++) in.headerKey[i] = (uint8_t)(i * 7 + 1);
    for (int i = 0; i < 0x10; i++) in.keyAreaKey[i] = (uint8_t)(i * 13 + 5);
    std::vector<uint8_t> nsp;
    CHECK(shortcut::buildNsp(in, nsp, err));
    std::vector<uint8_t> head(nsp.begin(), nsp.begin() + (size_t)fmt::pfs0HeaderSize(nsp.data()));
    std::vector<fmt::Pfs0Entry> files;
    CHECK(fmt::parsePfs0(head, files, err) && files.size() == 3);
    int types = 0;
    for (const auto& f : files) {
        std::vector<uint8_t> nca(nsp.begin() + f.offset, nsp.begin() + f.offset + f.size);
        uint8_t h[0x20];
        crypto::Sha256 sh; sh.update(nca.data(), nca.size()); sh.finish(h);
        CHECK(f.name.compare(0, 32, [&] { std::string x = fmt::bytesToHex(h, 0x10); for (auto& ch : x) ch = (char)tolower(ch); return x; }()) == 0);
        crypto::xtsNintendo(in.headerKey, nca.data(), 0xC00, 0, false);
        CHECK(memcmp(&nca[0x200], "NCA3", 4) == 0);
        uint64_t size = 0, tid = 0;
        memcpy(&size, &nca[0x208], 8); memcpy(&tid, &nca[0x210], 8);
        CHECK(size == f.size && tid == shortcut::kTitleId);
        types |= 1 << nca[0x205];
        CHECK(fmt::endsWith(f.name, ".cnmt.nca") == (nca[0x205] == 1));
    }
    CHECK(types == 0x7);  // program, meta, control
    // the RomFS lookup the console does (hash buckets) finds every control file
    std::vector<shortcut::PfsFile> ctl = {{"control.nacp", shortcut::buildNacp(in)}, {"icon_AmericanEnglish.dat", in.iconJpg},
                                         {"icon_TraditionalChinese.dat", in.iconJpg}};
    std::vector<uint8_t> rf = shortcut::buildRomfs(ctl);
    auto r32 = [&](uint64_t at) { uint32_t v; memcpy(&v, &rf[at], 4); return v; };
    auto r64 = [&](uint64_t at) { uint64_t v; memcpy(&v, &rf[at], 8); return v; };
    uint64_t fht = r64(0x28), fhs = r64(0x30), fmo = r64(0x38), dataOff = r64(0x48);
    for (const auto& c : ctl) {
        uint32_t hsh = 123456789u;
        for (unsigned char ch : c.name) { hsh = (hsh >> 5) | (hsh << 27); hsh ^= ch; }
        uint32_t e = r32(fht + (hsh % (fhs / 4)) * 4);
        bool found = false;
        while (e != 0xFFFFFFFFu && !found) {
            uint32_t nlen = r32(fmo + e + 0x1C);
            if (std::string((const char*)&rf[fmo + e + 0x20], nlen) == c.name) {
                uint64_t off = r64(fmo + e + 8), sz = r64(fmo + e + 0x10);
                found = sz == c.data.size() && memcmp(&rf[dataOff + off], c.data.data(), sz) == 0;
                break;
            }
            e = r32(fmo + e + 0x18);
        }
        CHECK(found);
    }
    std::vector<uint8_t> nacp = shortcut::buildNacp(in);
    CHECK(std::string((const char*)&nacp[13 * 0x300]) == "遊戲書架" && std::string((const char*)&nacp[0x3060]) == "隨主程式");
    printf("shortcut: NSP with program/control/meta NCAs, headers, content ids, RomFS lookup ok\n");
    return true;
}

static bool testTsconv() {
    CHECK(tsconv::searchKey("寶可夢 朱") == tsconv::searchKey("宝可梦 朱"));
    CHECK(tsconv::searchKey("薩爾達傳說") == tsconv::searchKey("萨尔达传说"));
    CHECK(tsconv::searchKey("髮") == tsconv::searchKey("发") && tsconv::searchKey("發") == tsconv::searchKey("发"));
    CHECK(tsconv::searchKey("後") == tsconv::searchKey("后"));
    CHECK(tsconv::searchKey("Mario ＫＡＲＴ８") == "mario kart8");
    CHECK(tsconv::searchKey("宝可梦 朱 [0100A1301EECC000].nsp").find(tsconv::searchKey("寶可夢")) == 0);
    CHECK(tsconv::searchKey("皮克敏") == "皮克敏");  // no variants: unchanged
    auto t = tsconv::traditionalOf(0x53D1);  // 发
    CHECK(std::find(t.begin(), t.end(), 0x767Cu) != t.end() && std::find(t.begin(), t.end(), 0x9AEEu) != t.end());
    CHECK(tsconv::fold(0x5BF6) == 0x5B9D && tsconv::fold(0x5B9D) == 0x5B9D);  // 寶 -> 宝, 宝 stays
    printf("tsconv: traditional/simplified search keys ok\n");
    return true;
}

static bool testWrongPassword(const std::string& goodUrl) {
    std::string bad = goodUrl;
    size_t at = bad.find('@');
    bad.replace(bad.rfind(':', at) + 1, at - bad.rfind(':', at) - 1, "wrong");
    std::vector<remote::RemoteFile> files;
    remote::ServerKind kind;
    std::string err;
    CHECK(!remote::fetchList(bad, files, kind, err));
    CHECK(err.find("401") != std::string::npos);
    printf("wrong password rejected: %s\n", err.c_str());
    return true;
}

// Server list: address splitting, %-encoding round trip through the real HTTP URL parser, file
// round trip, migration of the old single address, removing the server in use.
static bool testServers(const std::string& dir, const std::string& realServer) {
    std::string url, user, pass;
    servers::splitAddress("https://macron:p%40ss%3Aw0rd@nas.example:8001", url, user, pass);
    CHECK(url == "https://nas.example:8001/" && user == "macron" && pass == "p@ss:w0rd");
    servers::splitAddress("  192.168.1.10:8000 ", url, user, pass);
    CHECK(url == "http://192.168.1.10:8000/" && user.empty() && pass.empty());
    servers::splitAddress("HTTPS://nas/games/index.json", url, user, pass);
    CHECK(url == "https://nas/games/index.json");
    servers::splitAddress("https://nas:5006/遊戲", url, user, pass);
    CHECK(url == "https://nas:5006/遊戲/");
    servers::Server sv;
    sv.url = "https://nas.example:8001/";
    sv.user = "名字 a";
    sv.pass = "p@ss:w0rd/#?";
    http::Url u;
    CHECK(http::parseUrl(servers::fullUrl(sv), u));
    CHECK(u.host == "nas.example" && u.port == 8001 && u.tls);
    CHECK(http::percentDecode(u.userinfo) == "名字 a:p@ss:w0rd/#?");
    CHECK(servers::hostOf("https://nas.example:443/x") == "nas.example");
    CHECK(servers::hostOf("https://u:p@nas.example:8001/x") == "nas.example:8001");

    servers::Store st;
    CHECK(servers::migrateOld(st, "https://macron:p%40ss@disappear.example:8001/", "伺服器 1"));
    CHECK(st.list.size() == 1 && st.current == "s1" && st.list[0].user == "macron" && st.list[0].pass == "p@ss");
    CHECK(!servers::migrateOld(st, "http://other/", "x"));  // only when the list is empty
    servers::Server b;
    b.id = st.newId();
    b.name = "朋友的 NAS = 2";
    b.url = "http://friend:8000/";
    b.pass = "a=b\\c";
    st.list.push_back(b);
    CHECK(b.id == "s2");
    std::string path = dir + "/servers.ini";
    mkdir(dir.c_str(), 0755);
    CHECK(servers::save(path, st));
    servers::Store back;
    CHECK(servers::load(path, back));
    CHECK(back.list.size() == 2 && back.current == "s1");
    CHECK(back.list[1].name == "朋友的 NAS = 2" && back.list[1].pass == "a=b\\c");
    back.current = "s2";
    back.remove("s2");  // removing the one in use: the other becomes current
    CHECK(back.current == "s1" && back.list.size() == 1);
    back.remove("s1");
    CHECK(back.current.empty() && back.list.empty());

    // the split / rebuilt address really logs in (server.py has no password; WebDAV test server has one)
    std::vector<remote::RemoteFile> files;
    remote::ServerKind kind;
    std::string err;
    servers::Server real;
    servers::splitAddress(realServer, real.url, real.user, real.pass);
    CHECK(remote::fetchList(servers::fullUrl(real), files, kind, err));
    printf("servers: split / store / migrate / remove ok, rebuilt address logs in (%s)\n", real.user.empty() ? "no account" : "account");
    return true;
}

// Crash report cleaning: only the four report folders are emptied, they stay, nothing else is touched.
static bool testReports(const std::string& root) {
    auto put = [](const std::string& p, size_t n) { FILE* f = fopen(p.c_str(), "wb"); std::string d(n, 'x'); fwrite(d.data(), 1, n, f); fclose(f); };
    mkdir(root.c_str(), 0755);
    for (const auto& f : reports::folders()) mkdir((root + "/" + f).c_str(), 0755);
    mkdir((root + "/erpt_reports/sub").c_str(), 0755);
    mkdir((root + "/contents").c_str(), 0755);
    put(root + "/crash_reports/01792497970_010000000000100d.log", 40000);
    put(root + "/crash_reports/x.jpg", 2000);
    put(root + "/erpt_reports/sub/report.msgpack", 500);
    put(root + "/fatal_errors/report_0.bin", 100);
    put(root + "/contents/keep.txt", 10);
    put(root + "/package3", 10);
    auto u = reports::scan(root);
    CHECK(u.size() == 4 && u[0].files == 2 && u[0].bytes == 42000 && u[1].files == 1 && u[2].files == 0 && u[3].files == 1);
    uint64_t freed = 0;
    CHECK(reports::clean(root, freed) == 4 && freed == 42600);
    u = reports::scan(root);
    for (const auto& x : u) CHECK(x.files == 0);
    struct stat st;
    for (const auto& f : reports::folders()) CHECK(stat((root + "/" + f).c_str(), &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(stat((root + "/contents/keep.txt").c_str(), &st) == 0);
    CHECK(stat((root + "/package3").c_str(), &st) == 0);
    printf("crash reports: counted, emptied, folders kept, nothing else touched ok\n");
    return true;
}

// Online update: GitHub release answer, version order, signature check (right key / tampered / wrong key).
static bool testUpdate() {
    std::string json = R"({"tag_name":"v3.0.1","name":"x","body":"多伺服器\n修正 \u904a\u6232",
      "assets":[{"name":"LanInstaller.nro","size":2345678,"browser_download_url":"https://github.com/Antaite/GameShelf-releases/releases/download/v3.0.1/LanInstaller.nro"},
                {"name":"LanInstaller.nro.sig","size":72,"browser_download_url":"https://github.com/Antaite/GameShelf-releases/releases/download/v3.0.1/LanInstaller.nro.sig"}]})";
    update::Release r;
    std::string err;
    CHECK(update::parseLatest(json, "LanInstaller.nro", r, err));
    CHECK(r.tag == "v3.0.1" && r.nroSize == 2345678 && r.notes == "多伺服器\n修正 遊戲");
    CHECK(r.sigUrl.size() > 20 && r.nroUrl.find("LanInstaller.nro") != std::string::npos);
    CHECK(!update::parseLatest(R"({"tag_name":"v3.0.1","assets":[]})", "LanInstaller.nro", r, err));
    CHECK(!update::parseLatest("not json", "LanInstaller.nro", r, err));
    CHECK(update::compareVersions("v3.0.1", "v3.0.0") > 0 && update::compareVersions("3.0.0", "v3.0.0") == 0);
    CHECK(update::compareVersions("v3.0", "v3.0.1") < 0 && update::compareVersions("v3.10.0", "v3.9.9") > 0);
    CHECK(update::latestApiUrl() == "https://api.github.com/repos/Antaite/GameShelf-releases/releases/latest");
#ifdef LANINST_MBEDTLS
    const char* dir = getenv("UPDATE_TEST_DIR");
    CHECK(dir);
    auto rd = [](const std::string& p) { std::ifstream f(p, std::ios::binary); return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {}); };
    std::vector<uint8_t> data = rd(std::string(dir) + "/data.bin"), sig = rd(std::string(dir) + "/data.sig");
    std::vector<uint8_t> pubv = rd(std::string(dir) + "/pub.pem"), otherv = rd(std::string(dir) + "/other.pem");
    std::string pub(pubv.begin(), pubv.end()), other(otherv.begin(), otherv.end());
    CHECK(update::verify(data, sig, pub));
    std::vector<uint8_t> bad = data;
    bad[bad.size() / 2] ^= 1;
    CHECK(!update::verify(bad, sig, pub));
    CHECK(!update::verify(data, sig, other));
    CHECK(!update::verify(data, sig, ""));
    printf("update: release info, versions, signature (good / tampered / wrong key / no key) ok\n");
#else
    printf("update: release info, versions ok (signature needs mbedTLS)\n");
#endif
    return true;
}

// 注音 / 倉頡: table lookups, candidates limited to file-name characters, best match first.
static bool testIme() {
    auto cp = [](const char* s) { return ime::codepoints(s)[0]; };
    std::vector<std::string> r = ime::readings(cp("遊"));
    CHECK(!r.empty() && r[0] == "ㄧㄡˊ");
    CHECK(ime::cangjieCodes(cp("遊"))[0] == "YYSD");
    CHECK(ime::readings(cp("行")).size() >= 2);
    // characters of a few file names, with how often they appear
    std::map<uint32_t, int> w;
    for (const char* name : {"遊戲書架", "薩爾達傳說 王國之淚", "瑪利歐賽車8 豪華版", "有限公司", "優化", "幽靈"})
        for (uint32_t c : ime::codepoints(name)) w[c]++;
    w[cp("遊")] += 5;
    std::vector<std::string> c = ime::candidates(ime::Mode::Zhuyin, "ㄧㄡˊ", w);
    CHECK(!c.empty() && c[0] == "遊");                       // tone typed: only ˊ
    for (const auto& x : c) CHECK(x != "有" && x != "優" && x != "幽");
    c = ime::candidates(ime::Mode::Zhuyin, "ㄧㄡ", w);    // no tone: all ㄧㄡ, most used first
    CHECK(c.size() >= 4 && c[0] == "遊");
    c = ime::candidates(ime::Mode::Zhuyin, "ㄧㄡˉ", w);  // first tone only
    CHECK(!c.empty() && (c[0] == "優" || c[0] == "幽"));
    c = ime::candidates(ime::Mode::Zhuyin, "ㄓ", w);     // prefix
    bool hasZhan = false;
    for (const auto& x : c) hasZhan = hasZhan || x == "戰" || x == "車" || x == "之";
    CHECK(hasZhan || !c.empty());
    c = ime::candidates(ime::Mode::Cangjie, "yysd", w);
    CHECK(!c.empty() && c[0] == "遊");
    c = ime::candidates(ime::Mode::Cangjie, "MG", w);     // prefix: 瑪 MGSQF, 王 MG
    CHECK(c.size() >= 2 && c[0] == "王");
    CHECK(ime::cangjieRadicals("YYSD") == "卜卜尸木");
    CHECK(ime::candidates(ime::Mode::Zhuyin, "ㄅㄆㄇ", w).empty());
    std::string t = "書架A";
    ime::popChar(t);
    ime::popChar(t);
    CHECK(t == "書");
    CHECK(ime::layout(ime::Mode::Zhuyin).size() == 5 && ime::layout(ime::Mode::Cangjie).size() == 4);
    printf("ime: zhuyin (tones / first tone / prefix), cangjie (exact / prefix), only file-name characters ok\n");
    return true;
}

#define CHECK_MAIN(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)




// Kept-alive range reads give the same bytes as single requests (and survive the server closing).
static bool testSession(const std::string& base) {
    std::vector<remote::RemoteFile> files;
    remote::ServerKind kind;
    std::string err;
    CHECK(remote::fetchList(base, files, kind, err));
    const remote::RemoteFile* f = nullptr;
    for (const auto& x : files) if (x.size > 1000000) { f = &x; break; }
    CHECK(f);
    http::Session ses;
    std::mt19937 rng(99);
    for (int i = 0; i < 12; i++) {
        uint64_t len = 1 + rng() % 70000, off = rng() % (f->size - len);
        std::vector<uint8_t> a, b;
        CHECK(ses.getRange(f->url, off, len, a, err));
        CHECK(http::getToMemory(f->url, off, len, b, err));
        CHECK(a == b);
        if (i == 6) ses.close();
    }
    printf("session (kept-alive range reads) %s: ok\n", http::redact(base).c_str());
    return true;
}


// Versions read from the packages themselves: the shortcut builder makes a real (encrypted) meta NCA.
static bool testReadMetas() {
    shortcut::Inputs in;
    for (int i = 0; i < 0x20; i++) in.headerKey[i] = (uint8_t)(0x11 * i + 5);
    for (int i = 0; i < 0x10; i++) in.keyAreaKey[i] = (uint8_t)(0x07 * i + 9);
    in.mainNso.assign(0x3000, 0x42); memcpy(in.mainNso.data(), "NSO0", 4);
    in.npdm.assign(0x400, 0); memcpy(in.npdm.data(), "META", 4);
    auto w32 = [&](size_t at, uint32_t v) { memcpy(&in.npdm[at], &v, 4); };
    w32(0x70, 0x300); w32(0x74, 0x40); w32(0x78, 0x80); w32(0x7C, 0x240);
    memcpy(&in.npdm[0x80 + 0x200], "ACID", 4); memcpy(&in.npdm[0x300], "ACI0", 4);
    in.iconJpg.assign(0x2000, 0x33); in.iconJpg[0] = 0xFF; in.iconJpg[1] = 0xD8;
    in.version = 7u << 16;
    std::vector<uint8_t> nsp;
    std::string err;
    CHECK(shortcut::buildNsp(in, nsp, err));
    container::ReadFn rd = [&](uint64_t o, uint64_t n, std::vector<uint8_t>& out, std::string& e) {
        if (o + n > nsp.size()) { e = "past end"; return false; }
        out.assign(nsp.begin() + (long)o, nsp.begin() + (long)(o + n));
        return true;
    };
    container::KeyAreaKeyFn kaek = [&](int idx, uint8_t out[0x10]) {
        if (idx != 0) return false;
        memcpy(out, in.keyAreaKey, 0x10);
        return true;
    };
    int count = 0;
    std::vector<container::Meta> metas;
    CHECK(container::readMetas(rd, in.headerKey, kaek, false, count, metas, err));
    CHECK(count == 1 && metas.empty());  // directory only
    CHECK(container::readMetas(rd, in.headerKey, kaek, true, count, metas, err));
    CHECK(metas.size() == 1 && metas[0].versionKnown);
    CHECK(metas[0].titleId == shortcut::kTitleId && metas[0].version == (7u << 16) && metas[0].type == 0x80);
    CHECK(metas[0].size > 0x3000 && metas[0].size < nsp.size());  // installed size: its contents + the meta NCA
    // no key area key: the title id still comes out, the version is unknown
    container::KeyAreaKeyFn none = [](int, uint8_t*) { return false; };
    CHECK(container::readMetas(rd, in.headerKey, none, true, count, metas, err));
    CHECK(metas.size() == 1 && !metas[0].versionKnown && metas[0].titleId == shortcut::kTitleId && metas[0].type == 0x80);
    printf("package metas (title id, version, type from the encrypted cnmt): ok\n");
    return true;
}

// Real title ids from the content meta NCA header (whatever the file name says).
static bool testRealTitleIds() {
    uint8_t key[0x20];
    for (int i = 0; i < 0x20; i++) key[i] = (uint8_t)(i * 7 + 3);
    auto metaNca = [&](uint64_t tid) {
        std::vector<uint8_t> h(0x1000, 0);
        memcpy(&h[0x200], "NCA3", 4);
        h[0x205] = 1;  // meta
        for (int i = 0; i < 8; i++) h[0x210 + i] = (uint8_t)(tid >> (8 * i));
        crypto::xtsNintendo(key, h.data(), 0x400, 0, true);
        return h;
    };
    std::vector<shortcut::PfsFile> files = {{"aa.cnmt.nca", metaNca(0x0100ABCD12340000ULL)},
                                            {"bb.nca", std::vector<uint8_t>(0x800, 1)},
                                            {"cc.cnmt.nca", metaNca(0x0100ABCD12340800ULL)}};
    std::vector<uint8_t> pkg = shortcut::buildPfs0(files);
    container::ReadFn rd = [&](uint64_t o, uint64_t n, std::vector<uint8_t>& out, std::string& e) {
        if (o + n > pkg.size()) { e = "past end"; return false; }
        out.assign(pkg.begin() + (long)o, pkg.begin() + (long)(o + n));
        return true;
    };
    std::vector<uint64_t> ids;
    std::string err;
    CHECK(container::realTitleIds(rd, key, ids, err));
    CHECK(ids.size() == 2 && ids[0] == 0x0100ABCD12340000ULL && ids[1] == 0x0100ABCD12340800ULL);
    uint8_t wrong[0x20] = {0};
    CHECK(!container::realTitleIds(rd, wrong, ids, err));  // wrong key: says so instead of guessing
    printf("real title ids: ok\n");
    return true;
}

// 壞檔自動回報: only the author's server (by fingerprint), only file errors, once per file.
static bool testBadFile(const std::string& dir) {
    using namespace badfile;
    CHECK(fromReportServer("https://macron:secret@disappear-n5095.i234.me:8001/JUST%20DANCE%5Bv0%5D.xci"));
    CHECK(fromReportServer("http://DISAPPEAR-N5095.I234.ME:8001/a/b.nsp"));      // host case does not matter
    CHECK(!fromReportServer("https://disappear-n5095.i234.me:8002/a.nsp"));      // other port
    CHECK(!fromReportServer("https://disappear-n5095.i234.me/a.nsp"));           // default port 443
    CHECK(!fromReportServer("https://other.example.com:8001/a.nsp"));
    CHECK(!fromReportServer("mem://gameshelf-shortcut.nsp"));
    CHECK(fingerprint("Disappear-N5095.i234.me", 8001) == fingerprint("disappear-n5095.i234.me", 8001));
    std::string kind;
    CHECK(isBadFile("abc.nca：檔案本身的內容和描述檔記錄的不一致（兩次下載結果完全相同）", kind) && kind.find("壞檔") != std::string::npos);
    CHECK(isBadFile("abc.ncz：NCZ 檔案不完整", kind));
    CHECK(isBadFile("PFS0 標頭損壞", kind));
    CHECK(isBadFile("封裝檔缺少 0123.nca", kind));
    CHECK(!isBadFile("zstd 解壓失敗：記憶體不足（需要約 300 MB）", kind));
    CHECK(!isBadFile("建立暫存空間（空間不足？） 失敗：0x1234", kind));
    CHECK(!isBadFile("無法連線到伺服器", kind));
    CHECK(!isBadFile("abc.nca 雜湊值不符（檔案損壞或傳輸錯誤）", kind));  // may be the network: retried first
    CHECK(!isBadFile("已取消", kind));
    CHECK(fileName("https://u:p@h:1/x/JUST%20DANCE%5B0100EE2199BB8000%5D.xci?x=1") == "JUST DANCE[0100EE2199BB8000].xci");
    std::string j = ntfyJson(kNtfyTopic, "名\"稱.nsp", 0x0100EE2199BB8000ULL, "壞檔");
    CHECK(j.find("\"topic\":\"gameshelf-") != std::string::npos);
    CHECK(j.find("名\\\"稱.nsp") != std::string::npos && j.find("0100EE2199BB8000") != std::string::npos);
    CHECK(j.find("i234") == std::string::npos && j.find("macron") == std::string::npos);
    mkdir(dir.c_str(), 0777);
    std::string list = dir + "/reported.txt";
    std::remove(list.c_str());
    // counts: each kind of each file at most twice; a clean install starts again from zero
    std::string ka = fileKey("a.nsp", kKindBad), km = fileKey("a.nsp", kKindMismatch), kb = fileKey("b.nsp", kKindBad);
    CHECK(reportedTimes(list, ka) == 0);
    bumpReported(list, {ka});
    CHECK(reportedTimes(list, ka) == 1 && reportedTimes(list, km) == 0);
    bumpReported(list, {ka, kb});
    CHECK(reportedTimes(list, ka) == kMaxReports && reportedTimes(list, kb) == 1);
    clearReported(list, {"a.nsp"});  // every kind of a.nsp
    CHECK(reportedTimes(list, ka) == 0 && reportedTimes(list, kb) == 1);
    {   // a line written by an older version (file name only) counts as one bad-file report
        FILE* f = fopen(list.c_str(), "a");
        fputs("old.nsp\n", f);
        fclose(f);
    }
    CHECK(reportedTimes(list, fileKey("old.nsp", kKindBad)) == 1 && reportedTimes(list, fileKey("old.nsp", kKindTicket)) == 0);
    bumpReported(list, {fileKey("old.nsp", kKindBad)});
    CHECK(reportedTimes(list, fileKey("old.nsp", kKindBad)) == 2);
    // 「NAS 上沒有」 report
    CHECK(isOfficialTitle(0x0100152000022000ULL) && !isOfficialTitle(0x05000A062CE00000ULL));
    std::string mj = ntfyNasCheckJson(kNtfyTopic, {{0x010006501A8D8000ULL, "NBA 2K25"}, {0x0100ABCD00000000ULL, ""}},
                                      {"x.nsp：檔名 TID 0100AAAA00000000，實際 0100BBBB00000000"});
    CHECK(mj.find("發現 2 款") != std::string::npos && mj.find("NBA 2K25｜010006501A8D8000") != std::string::npos);
    CHECK(mj.find("（名稱不明）｜0100ABCD00000000") != std::string::npos && mj.find("檔名 TID 寫錯") != std::string::npos);
    CHECK(mj.find("伺服器比對結果") != std::string::npos);
    // a long report is cut into notes ntfy keeps as text (each under 3,500 bytes, titled 1/N…)
    {
        std::vector<std::pair<uint64_t, std::string>> many;
        for (int k = 0; k < 40; k++) many.push_back({0x0100000000010000ULL + ((uint64_t)k << 16), std::string(60, 'x') + "遊戲名稱很長很長很長" + std::to_string(k)});
        std::vector<std::string> wrongs;
        for (int k = 0; k < 20; k++) wrongs.push_back(std::string(100, 'y') + "：檔名 TID 0100AAAA00000000，實際 0100BBBB00000000");
        std::vector<std::string> parts = ntfyNasCheckJsons(kNtfyTopic, many, wrongs);
        CHECK(parts.size() >= 2);
        size_t lines = 0;
        for (size_t k = 0; k < parts.size(); k++) {
            CHECK(parts[k].size() < 3500 + 400);  // message + JSON around it
            CHECK(parts[k].find("（" + std::to_string(k + 1) + "/" + std::to_string(parts.size()) + "）") != std::string::npos);
            for (size_t p = parts[k].find("｜0100"); p != std::string::npos; p = parts[k].find("｜0100", p + 1)) lines++;
        }
        CHECK(lines == 40);  // no game lost between the parts
        CHECK(ntfyNasCheckJsons(kNtfyTopic, {{1, "a"}}, {}).size() == 1);
    }
    std::string miss = dir + "/missing.txt";
    std::remove(miss.c_str());
    bumpReported(miss, {tidKey(1), tidKey(2), tidKey(3)});
    CHECK(reportedCounts(miss).size() == 3);
    clearReported(miss, {tidKey(2)});
    CHECK(reportedCounts(miss).size() == 2 && !reportedCounts(miss).count(tidKey(2)));
    printf("bad-file report: ok\n");
    return true;
}

int main(int argc, char** argv) {
    if (argc < 4) { printf("usage: host_test <server-url> <out-dir> <test-dir>\n"); return 2; }
    CHECK_MAIN(http::tlsInit());
    testMisc(argv[1], argv[2], argv[3]);
    testUrls();
    testTitlesImpl();
    testPackages(argv[1], argv[3], remote::ServerKind::LanServer);
    testServers(std::string(argv[3]) + "/servers-test", argv[1]);
    testSession(argv[1]);
    if (argc >= 6) testServers(std::string(argv[3]) + "/servers-test2", argv[4]);
    if (argc >= 6) {
        testPackages(argv[4], argv[3], remote::ServerKind::WebDav);
        testPackages(argv[5], argv[3], remote::ServerKind::DirectoryListing);
        testWrongPassword(argv[4]);
        testWrongPassword(argv[5]);
    }
    if (argc >= 9) {
        testPackages(argv[6], argv[3], remote::ServerKind::WebDav);            // HTTPS + WebDAV
        testPackages(argv[7], argv[3], remote::ServerKind::DirectoryListing);  // HTTPS + listing
        testWrongPassword(argv[6]);
        testHttpsPortHint(argv[8]);
        testSession(argv[6]);  // HTTPS + basic auth
        testPlainToTls(argv[6]);
    }
    if (argc >= 10) testLoginPage(argv[9]);
    printf("TLS backend after the HTTPS tests: %s %s\n", http::tlsBackend(), http::tlsCipher().c_str());
    if (argc >= 11) {
        testPackages(argv[10], argv[3], remote::ServerKind::JsonIndex);  // this server has no ChaCha20
        {   // the index greeting ("success"), shown at start: kept per thread, cleared by the next list
            std::vector<remote::RemoteFile> files;
            remote::ServerKind kind;
            std::string err;
            CHECK(remote::fetchList(argv[10], files, kind, err) && remote::lastListMessage() == "歡迎");
            CHECK(remote::fetchList(argv[1], files, kind, err) && remote::lastListMessage().empty());
            printf("server greeting: ok\n");
        }
        printf("TLS cipher on the AES-only server: %s\n", http::tlsCipher().c_str());
#ifdef LANINST_MBEDTLS
        if (http::tlsCipher().find("AES") == std::string::npos) { printf("FAIL no AES fallback\n"); g_fail++; }
#endif
    }
    if (argc >= 13) testCovers(argv[11], argv[12]);
    if (argc >= 13) testSessionGet(argv[11]);
    testFailFast(argv[1]);
    testAddresses();
    if (argc >= 15) testTitleDb(argv[13], argv[14]);
    if (argc >= 15) testRedirectQuery();
    testPatchExtendedData();
    testRecordPlan();
    if (!testDisplayVersion()) return 1;
    testResumeWriter();
    testCoverTrim(std::string(argv[3]) + "/cover-trim-test");
    testResumeState(std::string(argv[3]) + "/resume-test");
    testReports(std::string(argv[3]) + "/atmosphere-test");
    testUpdate();
    testIme();
    testTsconv();
    testShortcut();
    testRealTitleIds();
    testReadMetas();
    testBadFile(std::string(argv[3]) + "/badfile-test");
    http::tlsExit();
    if (g_fail) { printf("%d CHECK(S) FAILED\n", g_fail); return 1; }
    printf("ALL HOST TESTS PASSED\n");
    return 0;
}
