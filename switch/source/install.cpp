// Package installation via ncm (content storage + meta database), es (tickets) and ns (application records).
// Flow mirrors the well-known open-source installers (Tinfoil 0.x by Adubbz / Awoo Installer):
//   tickets -> cnmt NCA -> read cnmt -> content NCAs (plain or NCZ) -> ContentMetaDatabase::Set -> ns record
#include "install.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <set>
#include <vector>

#include "container.hpp"
#include "crypto.hpp"
#include "resume.hpp"
#include "formats.hpp"
#include "http.hpp"
#include "nca_stream.hpp"
#include "shortcut.hpp"
#include "stb_image.h"  // zlib inflate only (implementation in covers.cpp)
#include "titles.hpp"

namespace install {

namespace {
// ---------------------------------------------------------------- es (tickets)
Service g_es;
const char* kResumeDir = "sdmc:/config/lan-installer/resume";

// Several installs can run at once (one per worker thread).
Mutex g_logLock;   // install.log lines
Mutex g_regLock;   // content meta database writes, HOME records, launch versions
thread_local int t_logTag = 0;
Mutex g_badHashLock;
std::map<std::string, std::string> g_badHash;  // url|content id -> hash it had when the check failed
thread_local int t_readerCore = -2;  // core for download threads started by this thread
int readerCore() { return t_readerCore; }

struct Lock {
    Mutex& m;
    explicit Lock(Mutex& mm) : m(mm) { mutexLock(&m); }
    ~Lock() { mutexUnlock(&m); }
};

Result esImportTicket(const void* tik, size_t tikSize, const void* cert, size_t certSize) {
    return serviceDispatch(&g_es, 1,
                           .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
                                            SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
                           .buffers = {{tik, tikSize}, {cert, certSize}});
}

// Rights ids of every ticket on the console (common + personalized), read once per install.
std::set<std::string> esTicketRights() {
    std::set<std::string> out;
    for (u32 cmd : {11u, 12u}) {  // ListCommonTicketRightsIds, ListPersonalizedTicketRightsIds
        u32 count = 0;
        if (R_FAILED(serviceDispatchOut(&g_es, cmd == 11 ? 9 : 10, count)) || count == 0) continue;
        std::vector<uint8_t> buf((size_t)count * 0x10 + 0x10 * 16);
        u32 written = 0;
        if (R_FAILED(serviceDispatchOut(&g_es, cmd, written, .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                                        .buffers = {{buf.data(), buf.size()}})))
            continue;
        for (u32 i = 0; i < written && (size_t)(i + 1) * 0x10 <= buf.size(); i++) out.insert(fmt::bytesToHex(&buf[i * 0x10], 0x10));
    }
    return out;
}

// ---------------------------------------------------------------- ns (application records)
struct ContentStorageRecord {
    NcmContentMetaKey key;
    u64 storageId;
};
static_assert(sizeof(ContentStorageRecord) == 0x18, "ContentStorageRecord size");

// IApplicationManagerInterface: on [3.0.0+] it must be requested from the ns getter interface
// (nsGetServiceSession_ApplicationManagerInterface() is only valid before 3.0.0; using it on newer
// firmware fails with 0xE401 "invalid handle").
Service g_nsAm;
bool g_nsAmOwned = false;
Service* nsAm() { return g_nsAmOwned ? &g_nsAm : nsGetServiceSession_ApplicationManagerInterface(); }

Result nsPushApplicationRecord(u64 appId, const ContentStorageRecord* recs, size_t count) {
    struct {
        u8 last_modified_event;
        u8 pad[7];
        u64 application_id;
    } in = {3 /* Installed */, {0}, appId};
    return serviceDispatchIn(nsAm(), 16, in,
                             .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
                             .buffers = {{recs, count * sizeof(ContentStorageRecord)}});
}

Result nsListApplicationRecordContentMeta(u64 offset, u64 appId, ContentStorageRecord* out, size_t max, u32* count) {
    struct {
        u64 offset;
        u64 application_id;
    } in = {offset, appId};
    return serviceDispatchInOut(nsAm(), 17, in, *count,
                                .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                                .buffers = {{out, max * sizeof(ContentStorageRecord)}});
}

Result nsDeleteApplicationRecord(u64 appId) { return serviceDispatchIn(nsAm(), 27, appId); }
// The HOME menu keeps each title's name / icon / version in a cache: forget one so it is read again.
Result nsInvalidateApplicationControlCache(u64 appId) { return serviceDispatchIn(nsAm(), 404, appId); }

std::string rcStr(const char* what, Result rc) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s 失敗：0x%08X（%04d-%04d）", what, rc, 2000 + R_MODULE(rc), R_DESCRIPTION(rc));
    return buf;
}

// ---------------------------------------------------------------- context / progress
struct Ctx {
    std::string url;
    Options opt;
    ProgressFn progress;
    Progress p;
    u64 lastReportTick = 0;
    u64 speedTick = 0;
    u64 speedBytes = 0;
    NcmContentStorage cs;
    NcmContentMetaDatabase db;
    int patchedNcas = 0;
    resume::State rs;    // progress kept on the SD card for continuing later
    std::string rsPath;  // "" = do not keep progress
    uint64_t rxBytes = 0;  // bytes really received (the speed; skipped / resumed parts do not count)
    double smoothSpeed = -1;  // shown speed: averaged over about 8 s so it does not jump around
    bool wiped = false;  // the earlier attempt's data was cleared by the system: starting over
};

bool report(Ctx& c, bool force = false) {
    u64 now = armGetSystemTick();
    if (!force && armTicksToNs(now - c.lastReportTick) < 200000000ULL) return true;
    c.lastReportTick = now;
    u64 ns = armTicksToNs(now - c.speedTick);
    if (ns > 1000000000ULL) {
        double dt = (double)ns / 1e9;
        double inst = (double)(c.rxBytes - c.speedBytes) / 1048576.0 / dt;
        if (c.smoothSpeed < 0) c.smoothSpeed = inst;
        else {
            double k = 1.0 - exp(-dt / 8.0);  // exponential average, ~8 s
            c.smoothSpeed += (inst - c.smoothSpeed) * k;
        }
        c.p.speedMBs = c.smoothSpeed;
        c.speedTick = now;
        c.speedBytes = c.rxBytes;
    }
    c.p.received = c.rxBytes;
    if (c.wiped && c.progress) {  // say why it starts from zero (shown under the file name)
        Progress shown = c.p;
        shown.stage += "（之前下載的部分已被系統清除，從頭下載）";
        return c.progress(shown);
    }
    return c.progress ? c.progress(c.p) : true;
}

// Drops from `st` what is no longer on the SD card (placeholders / contents). The system clears
// them when software is deleted from the HOME menu or System Settings. True when something was lost.
bool dropMissing(NcmContentStorage& cs, resume::State& st) {
    bool lost = false;
    for (auto it = st.parts.begin(); it != st.parts.end();) {
        NcmPlaceHolderId ph;
        bool exists = false;
        if (fmt::hexToBytes(it->second.placeholder, ph.uuid.uuid, 0x10))
            exists = R_SUCCEEDED(ncmContentStorageHasPlaceHolder(&cs, &exists, &ph)) && exists;
        if (exists) { ++it; continue; }
        lost = true;
        it = st.parts.erase(it);
    }
    for (auto it = st.done.begin(); it != st.done.end();) {
        NcmContentId id;
        bool has = false;
        if (fmt::hexToBytes(it->first, id.c, 0x10)) has = R_SUCCEEDED(ncmContentStorageHas(&cs, &has, &id)) && has;
        if (has) { ++it; continue; }
        lost = true;
        it = st.done.erase(it);
    }
    return lost;
}

bool download(Ctx& c, const fmt::Pfs0Entry& e, std::vector<uint8_t>& out, std::string& err) {
    if (e.size > 64 * 1024 * 1024) { err = "封裝檔中有異常巨大的檔案：" + e.name; return false; }
    if (e.size == 0) { out.clear(); return true; }
    return http::getToMemory(c.url, e.offset, e.size, out, err);
}

// ---- download pipeline
// The network is read on its own thread into a bounded queue while this thread decompresses,
// hashes and writes to the SD card, so neither waits for the other (a single thread spends
// most of its time on one or the other).
u64 nowNs() { return armTicksToNs(armGetSystemTick()); }

// Where the time of one content went (for the speed lines in install.log).
struct Timing {
    u64 total = 0, waitNet = 0, readerBlocked = 0, write = 0;
    u64 sockWait = 0;  // download thread: time inside socket reads
    u64 tlsRead = 0;   // download thread: time inside TLS reads (socket + decryption)
    u64 sockCalls = 0; // download thread: socket reads made
    uint64_t moved = 0;  // bytes actually transferred
};

struct Pipe {
    Mutex m = 0;
    CondVar canPush = 0, canPop = 0;
    std::deque<std::vector<uint8_t>> q;
    size_t bytes = 0;
    bool eof = false, ok = false, abort = false;
    std::string err;
    Timing* timing = nullptr;
};
// Downloaded data waiting for the SD card / decompression, per install. Bigger when the app runs
// with the full memory (started through a game): writing hiccups then never hold up the download.
std::atomic<size_t> g_pipeCap{8 * 1024 * 1024};
// A transfer that delivers almost nothing for this long is treated as stuck and reconnected.
const u64 kStallNs = 30ULL * 1000000000ULL;
const size_t kStallBytes = 64 * 1024;

struct ReaderArg {
    Pipe* pipe;
    const std::string* url;
    uint64_t off, size;
};

void readerThread(void* a) {
    ReaderArg* r = (ReaderArg*)a;
    Pipe& p = *r->pipe;
    std::string err;
    http::resetThreadSocketWait();
    const size_t cap = g_pipeCap.load();
    u64 windowStart = armTicksToNs(armGetSystemTick());
    size_t windowBytes = 0;
    bool stalled = false;
    bool ok = http::get(*r->url, r->off, r->size,
                        [&](const uint8_t* d, size_t n) {
                            // stuck connection: data trickles in (so the socket timeout never fires)
                            u64 now = armTicksToNs(armGetSystemTick());
                            windowBytes += n;
                            if (now - windowStart >= kStallNs) {
                                if (windowBytes < kStallBytes) { stalled = true; return false; }
                                windowStart = now;
                                windowBytes = 0;
                            }
                            mutexLock(&p.m);
                            if (!p.abort && p.bytes >= cap) {
                                u64 w0 = armTicksToNs(armGetSystemTick());
                                while (!p.abort && p.bytes >= cap) condvarWait(&p.canPush, &p.m);
                                u64 w1 = armTicksToNs(armGetSystemTick());
                                if (p.timing) p.timing->readerBlocked += w1 - w0;
                                windowStart = w1;  // waiting for the SD card is not the network's fault
                                windowBytes = 0;
                            }
                            if (p.abort) { mutexUnlock(&p.m); return false; }
                            p.q.emplace_back(d, d + n);
                            p.bytes += n;
                            condvarWakeOne(&p.canPop);
                            mutexUnlock(&p.m);
                            return true;
                        },
                        err);
    if (stalled) err = "連線卡住（30 秒內幾乎沒有收到資料），重新連線";
    mutexLock(&p.m);
    if (p.timing) {
        p.timing->sockWait += http::threadSocketWaitNs();
        p.timing->tlsRead += http::threadTlsReadNs();
        p.timing->sockCalls += http::threadSocketCalls();
    }
    p.eof = true;
    p.ok = ok;
    p.err = err;
    condvarWakeAll(&p.canPop);
    mutexUnlock(&p.m);
}

// Like http::get, with the download on a helper thread. `consume` runs on the calling thread.
bool streamDownload(const std::string& url, uint64_t off, uint64_t size,
                    const std::function<bool(const uint8_t*, size_t)>& consume, std::string& err, Timing* tm = nullptr) {
    Pipe p;
    p.timing = tm;
    u64 t0 = nowNs();
    ReaderArg ra{&p, &url, off, size};
    Thread th;
    if (R_FAILED(threadCreate(&th, readerThread, &ra, nullptr, 0x40000, 0x2C, readerCore()))) return http::get(url, off, size, consume, err);
    if (R_FAILED(threadStart(&th))) { threadClose(&th); return http::get(url, off, size, consume, err); }
    bool consumerOk = true;
    for (;;) {
        mutexLock(&p.m);
        if (p.q.empty() && !p.eof) {
            u64 w0 = nowNs();
            while (p.q.empty() && !p.eof) condvarWait(&p.canPop, &p.m);
            if (tm) tm->waitNet += nowNs() - w0;
        }
        if (p.q.empty()) { mutexUnlock(&p.m); break; }  // eof and drained
        std::vector<uint8_t> chunk = std::move(p.q.front());
        p.q.pop_front();
        p.bytes -= chunk.size();
        condvarWakeOne(&p.canPush);
        mutexUnlock(&p.m);
        if (!consume(chunk.data(), chunk.size())) { consumerOk = false; break; }
    }
    mutexLock(&p.m);
    p.abort = true;  // stop the reader if we gave up early
    condvarWakeAll(&p.canPush);
    mutexUnlock(&p.m);
    threadWaitForExit(&th);
    threadClose(&th);
    if (tm) tm->total += nowNs() - t0;
    if (!consumerOk) { if (err.empty()) err = "已取消"; return false; }
    if (!p.ok) { err = p.err; return false; }
    return true;
}

// Streams one NCA (or NCZ, decompressed on the fly) from the server into a placeholder and registers it.
//   ncaSize      final NCA size (for NCZ this comes from the cnmt)
//   expectedHash full SHA-256 from the cnmt, or null to check against the content id
//   allowPatch   whether the gamecard->download header patch may be applied
std::string phHex(const NcmPlaceHolderId& ph) { return fmt::bytesToHex(ph.uuid.uuid, 0x10); }

void saveResume(Ctx& c) {
    if (!c.rsPath.empty()) resume::save(c.rsPath, c.rs);
}

bool installContent(Ctx& c, const fmt::Pfs0Entry& e, const NcmContentId& id, u64 ncaSize,
                    const uint8_t* expectedHash, bool allowPatch, std::string& err) {
    Result rc;
    const std::string cid = fmt::bytesToHex(id.c, 0x10);
    const bool isNcz = fmt::endsWith(e.name, ".ncz");
    bool has = false;
    ncmContentStorageHas(&c.cs, &has, &id);

    // Finished in an earlier, interrupted attempt: keep it.
    if (has && c.rs.done.count(cid)) {
        log("  resume: %s already installed earlier, skipped", e.name.c_str());
        c.p.done += e.size;
        report(c, true);
        return true;
    }

    // Half written earlier: continue from the last checkpoint (plain NCA only; a zstd stream
    // cannot be restarted in the middle).
    NcmPlaceHolderId ph;
    uint64_t startAt = 0;
    std::vector<uint8_t> shaState;
    auto part = c.rs.parts.find(cid);
    if (part != c.rs.parts.end()) {
        bool usable = false;
        if (!isNcz && part->second.ncaSize == ncaSize && part->second.written < ncaSize &&
            fmt::hexToBytes(part->second.placeholder, ph.uuid.uuid, 0x10)) {
            bool phExists = false;
            usable = R_SUCCEEDED(ncmContentStorageHasPlaceHolder(&c.cs, &phExists, &ph)) && phExists;
        }
        if (usable) {
            startAt = part->second.written;
            shaState = part->second.sha;
            log("  resume: %s continues at %llu of %llu bytes", e.name.c_str(), (unsigned long long)startAt,
                (unsigned long long)ncaSize);
        } else {
            NcmPlaceHolderId old;
            if (fmt::hexToBytes(part->second.placeholder, old.uuid.uuid, 0x10)) ncmContentStorageDeletePlaceHolder(&c.cs, &old);
            c.rs.parts.erase(part);
            saveResume(c);
        }
    }
    if (!startAt) {
        if (has) ncmContentStorageDelete(&c.cs, &id);
        if (R_FAILED(rc = ncmContentStorageGeneratePlaceHolderId(&c.cs, &ph))) { err = rcStr("產生暫存 ID", rc); return false; }
        ncmContentStorageDeletePlaceHolder(&c.cs, &ph);
        if (R_FAILED(rc = ncmContentStorageCreatePlaceHolder(&c.cs, &id, &ph, (s64)ncaSize))) {
            err = rcStr("建立暫存空間（空間不足？）", rc);
            return false;
        }
    }

    c.p.stage = std::string(isNcz ? "正在解壓縮 " : "正在安裝 ") + e.name;
    if (startAt) c.p.stage += "（接續）";
    report(c, true);

    Timing tm;
    Result writeRc = 0;
    nca::NcaWriter writer(
        [&](uint64_t off, const uint8_t* d, size_t n) {
            if (off + n > ncaSize) return false;
            u64 w0 = nowNs();
            writeRc = ncmContentStorageWritePlaceHolder(&c.cs, &ph, off, d, n);
            tm.write += nowNs() - w0;
            return R_SUCCEEDED(writeRc);
        },
        allowPatch && c.opt.convertGamecard ? c.opt.headerKey : nullptr);
    if (startAt && !writer.resumeAt(startAt, shaState)) {
        startAt = 0;  // state from another build: start over in a fresh placeholder
        ncmContentStorageDeletePlaceHolder(&c.cs, &ph);
        c.rs.parts.erase(cid);
        saveResume(c);
        return installContent(c, e, id, ncaSize, expectedHash, allowPatch, err);
    }
    // Plain NCAs save a checkpoint every 64 MB so an interruption costs at most that much.
    bool resumable = !isNcz && !c.rsPath.empty();
    int flushes = 0;
    if (resumable)
        writer.onCheckpoint = [&](uint64_t written, const std::vector<uint8_t>& st) {
            if (++flushes % 16 != 0) return;
            resume::Part& pt = c.rs.parts[cid];
            pt.placeholder = phHex(ph);
            pt.written = written;
            pt.ncaSize = ncaSize;
            pt.sha = st;
            saveResume(c);
        };
    nca::NczDecoder ncz(writer, ncaSize);
    std::string pipeErr;

    c.p.done += startAt;
    auto consume = [&](const uint8_t* d, size_t n) {
        c.p.done += n;
        c.rxBytes += n;
        tm.moved += n;
        bool good = isNcz ? ncz.push(d, n) : writer.push(d, n);
        if (!good) { pipeErr = isNcz ? ncz.error : writer.error; return false; }
        return report(c);
    };
    bool ok = streamDownload(c.url, e.offset + startAt, e.size - startAt, consume, err, &tm);
    if (ok && isNcz && !ncz.finish()) { ok = false; pipeErr = ncz.error; }
    if (isNcz && !ncz.memoryInfo.empty()) log("  %s: %s", e.name.c_str(), ncz.memoryInfo.c_str());
    if (ok && !writer.finish()) { ok = false; pipeErr = writer.error; }
    if (R_FAILED(writeRc)) err = rcStr("寫入暫存空間", writeRc);
    else if (!pipeErr.empty()) err = e.name + "：" + pipeErr;
    if (ok && writer.size() != ncaSize) {
        ok = false;
        err = e.name + (isNcz ? "：解壓縮後大小不符" : "：下載後大小不符");
    }

    // speed line: where the time went
    if (tm.total && tm.moved >= 32ULL * 1024 * 1024) {
        double sec = tm.total / 1e9, mb = tm.moved / 1048576.0;
        auto pct = [&](u64 v) { return tm.total ? (int)(std::min(v, tm.total) * 100 / tm.total) : 0; };
        // install thread: waiting for downloaded data / writing to SD / own work;
        // download thread: socket reads (network) / decryption (inside TLS reads, minus the socket part)
        // / HTTP + handing data on (the rest) / blocked because the install side was full
        u64 tlsCpu = tm.tlsRead > tm.sockWait ? tm.tlsRead - tm.sockWait : 0;
        int pNet = pct(tm.sockWait), pTls = pct(tlsCpu), pSd = pct(tm.readerBlocked);
        log("  speed %s: %.0f MB in %.0f s = %.1f MB/s%s | install thread: waiting for data %d%%, SD write %d%%, "
            "decompress/decrypt/hash %d%% | download thread: network %d%%, TLS decrypt %d%%, HTTP/other %d%%, "
            "waited for SD %d%% | %.0f socket reads/MB",
            e.name.c_str(), mb, sec, mb / sec, ok ? "" : " (stopped)", pct(tm.waitNet), pct(tm.write),
            std::max(0, 100 - pct(tm.waitNet) - pct(tm.write)), pNet, pTls, std::max(0, 100 - pNet - pTls - pSd), pSd,
            mb > 0 ? (double)tm.sockCalls / mb : 0.0);
        static Mutex cipherLock;
        static std::string loggedCipher;
        std::string cipher = http::tlsCipher();
        mutexLock(&cipherLock);
        bool fresh = !cipher.empty() && cipher != loggedCipher;
        if (fresh) loggedCipher = cipher;
        mutexUnlock(&cipherLock);
        if (fresh) log("  TLS cipher: %s", cipher.c_str());
    }

    if (!ok) {
        // keep the placeholder when a checkpoint exists: the next attempt continues from there
        if (!c.rs.parts.count(cid)) ncmContentStorageDeletePlaceHolder(&c.cs, &ph);
        return false;
    }

    if (c.opt.verifyHashes) {
        u8 hash[0x20];
        writer.hash(hash);  // hash of the original NCA bytes, so it holds even after patching
        bool match = expectedHash ? memcmp(hash, expectedHash, 0x20) == 0 : memcmp(hash, id.c, 0x10) == 0;
        if (!match && !expectedHash) {
            // The meta NCA is checked against its file name only. Some repacked NSPs keep the old
            // name after rebuilding it; Tinfoil and DBI accept those. Its contents are still read
            // and every other NCA is checked against the hashes listed inside it.
            log("  note: %s does not match its name (repacked package?), accepted", e.name.c_str());
            match = true;
        }
        if (!match) {
            // The file does not match the hash its own content meta lists. Merged / repacked
            // packages often carry edited NCAs whose hash was never updated; Tinfoil and DBI install
            // those (they do not check). Accepted like they do, with a note:
            //  - over HTTPS at once (TLS rules out damage on the way: the server has these bytes);
            //  - over plain HTTP when a second download gives exactly the same result.
            std::string got = fmt::bytesToHex(hash, 0x20);
            bool again;
            {
                Lock lk(g_badHashLock);
                again = g_badHash[c.url + "|" + cid] == got;
                g_badHash[c.url + "|" + cid] = got;
            }
            bool tls = c.url.compare(0, 8, "https://") == 0;
            if (again || tls) {
                log("  hash mismatch %s: got %s%s -> installed anyway (edited / merged package?)", e.name.c_str(),
                    got.c_str(), again ? " (same as last time)" : " (HTTPS)");
                if (c.opt.findings) c.opt.findings->mismatched.push_back(e.name);
            } else {
                ncmContentStorageDeletePlaceHolder(&c.cs, &ph);
                c.rs.parts.erase(cid);  // never continue from data that failed the check
                saveResume(c);
                log("  hash mismatch %s: got %s (download again to compare)", e.name.c_str(), got.c_str());
                err = e.name + " 雜湊值不符（檔案損壞或傳輸錯誤）";
                return false;
            }
        }
    }
    if (writer.patched()) c.patchedNcas++;

    if (R_FAILED(rc = ncmContentStorageRegister(&c.cs, &id, &ph))) {
        ncmContentStorageDeletePlaceHolder(&c.cs, &ph);
        c.rs.parts.erase(cid);
        saveResume(c);
        err = rcStr("註冊內容", rc);
        return false;
    }
    c.rs.parts.erase(cid);
    if (!c.rsPath.empty()) {
        c.rs.done[cid] = e.size;
        saveResume(c);
    }
    return true;
}

// Mounts an installed meta NCA and reads the *.cnmt file inside it.
bool readInstalledCnmt(Ctx& c, const NcmContentId& id, std::vector<uint8_t>& out, std::string& err) {
    char path[FS_MAX_PATH] = {0};
    Result rc = ncmContentStorageGetPath(&c.cs, path, sizeof(path), &id);
    if (R_FAILED(rc)) { err = rcStr("取得內容路徑", rc); return false; }

    FsFileSystem fs;
    rc = fsOpenFileSystemWithId(&fs, 0, FsFileSystemType_ContentMeta, path, FsContentAttributes_All);
    if (R_FAILED(rc)) { err = rcStr("開啟內容描述 NCA（缺少 ticket 或 sigpatches？）", rc); return false; }

    bool ok = false;
    FsDir dir;
    if (R_SUCCEEDED(rc = fsFsOpenDirectory(&fs, "/", FsDirOpenMode_ReadFiles, &dir))) {
        FsDirectoryEntry entries[16];
        s64 total = 0;
        if (R_SUCCEEDED(rc = fsDirRead(&dir, &total, 16, entries))) {
            for (s64 i = 0; i < total && !ok; i++) {
                std::string name = entries[i].name;
                if (!fmt::endsWith(name, ".cnmt")) continue;
                FsFile f;
                std::string fpath = "/" + name;
                if (R_FAILED(rc = fsFsOpenFile(&fs, fpath.c_str(), FsOpenMode_Read, &f))) break;
                s64 size = 0;
                fsFileGetSize(&f, &size);
                out.resize((size_t)size);
                u64 read = 0;
                rc = fsFileRead(&f, 0, out.data(), (u64)size, FsReadOption_None, &read);
                fsFileClose(&f);
                ok = R_SUCCEEDED(rc) && read == (u64)size;
            }
        }
        fsDirClose(&dir);
    }
    fsFsClose(&fs);
    if (!ok) err = R_FAILED(rc) ? rcStr("讀取 cnmt", rc) : "內容描述 NCA 中找不到 .cnmt";
    return ok;
}

// Every installed content meta (base, update, DLC) of one application, latest version per title,
// read from the SD card and system-memory databases.
std::vector<ContentStorageRecord> recordsFromNcm(u64 appId) {
    std::map<u64, ContentStorageRecord> best;
    const NcmStorageId storages[] = {NcmStorageId_SdCard, NcmStorageId_BuiltInUser};
    for (NcmStorageId st : storages) {
        NcmContentMetaDatabase db;
        if (R_FAILED(ncmOpenContentMetaDatabase(&db, st))) continue;
        NcmContentMetaKey keys[256];
        s32 total = 0, written = 0;
        // an application's base, update and DLC ids all lie in [appId, appId + 0x1FFF]
        if (R_SUCCEEDED(ncmContentMetaDatabaseList(&db, &total, &written, keys, 256, NcmContentMetaType_Unknown, 0,
                                                   appId, appId + 0x1FFF, NcmContentInstallType_Full))) {
            for (s32 i = 0; i < written; i++) {
                const NcmContentMetaKey& k = keys[i];
                if (k.type != NcmContentMetaType_Application && k.type != NcmContentMetaType_Patch &&
                    k.type != NcmContentMetaType_AddOnContent)
                    continue;
                auto it = best.find(k.id);
                if (it != best.end() && it->second.key.version >= k.version) continue;
                ContentStorageRecord r;
                memset(&r, 0, sizeof(r));
                r.key = k;
                r.storageId = st;
                best[k.id] = r;
            }
        }
        ncmContentMetaDatabaseClose(&db);
    }
    std::vector<ContentStorageRecord> out;
    for (auto& kv : best) out.push_back(kv.second);
    // base first, then update, then DLC (the order official installs use)
    std::sort(out.begin(), out.end(), [](const ContentStorageRecord& a, const ContentStorageRecord& b) {
        return a.key.type != b.key.type ? a.key.type < b.key.type : a.key.id < b.key.id;
    });
    return out;
}

// Current HOME-menu record of an application. Returns false if it could not be read.
bool readRecord(u64 appId, std::vector<ContentStorageRecord>& out) {
    out.clear();
    s32 count = 0;
    Result rc = nsCountApplicationContentMeta(appId, &count);
    if (R_FAILED(rc)) return false;  // usually: no record yet
    if (count <= 0) return true;
    out.resize((size_t)count + 8);
    u32 got = 0;
    rc = nsListApplicationRecordContentMeta(0, appId, out.data(), out.size(), &got);
    if (R_FAILED(rc)) { out.clear(); return false; }
    out.resize(std::min<size_t>(got, out.size()));
    return true;
}

// Rebuilds the record of `appId` from what is really installed. Entries ncm does not know about
// (for example a game card) are kept. Never replaces a record with one that has no base game.
static titles::RecordEntry toEntry(const ContentStorageRecord& r) {
    return {r.key.id, r.key.version, r.key.type, (uint8_t)r.storageId};
}

static ContentStorageRecord fromEntry(const titles::RecordEntry& e) {
    ContentStorageRecord r;
    memset(&r, 0, sizeof(r));
    r.key.id = e.id;
    r.key.version = e.version;
    r.key.type = e.type;
    r.key.install_type = NcmContentInstallType_Full;
    r.storageId = e.storage;
    return r;
}

bool rebuildRecord(u64 appId, std::string& err, int* changed = nullptr) {
    std::vector<ContentStorageRecord> rebuilt = recordsFromNcm(appId), existing;
    bool readOk = readRecord(appId, existing);
    std::vector<titles::RecordEntry> inst, have;
    for (const auto& r : rebuilt) inst.push_back(toEntry(r));
    for (const auto& r : existing) have.push_back(toEntry(r));
    titles::RecordPlan plan = titles::planRecord(inst, have, readOk, changed != nullptr);
    log("record %016llX: installed=%zu existing=%zu (read %s) -> %s", (unsigned long long)appId, inst.size(),
        have.size(), readOk ? "ok" : "failed", plan.reason.c_str());
    if (!plan.push) return true;
    // Keep the exact original entry (all key fields) where we have one.
    std::vector<ContentStorageRecord> recs;
    for (const auto& e : plan.entries) {
        const ContentStorageRecord* src = nullptr;
        for (const auto& r : rebuilt) if (r.key.id == e.id) src = &r;
        if (!src) for (const auto& r : existing) if (r.key.id == e.id) src = &r;
        recs.push_back(src ? *src : fromEntry(e));
    }
    nsDeleteApplicationRecord(appId);  // fails harmlessly when there is none
    Result rc = nsPushApplicationRecord(appId, recs.data(), recs.size());
    log("  push %zu entries -> 0x%X", recs.size(), rc);
    if (R_FAILED(rc)) { err = rcStr("註冊遊戲紀錄", rc); return false; }
    if (changed) (*changed)++;
    return true;
}

// ---- launch version (avm, 6.0.0+) ----
// The system keeps, per game, the version it expects to launch. Installers must push the installed
// version after adding an update (Tinfoil, Sphaira, DBI do); otherwise HOME refuses to start the game
// with "無法啟動軟體". Own session, so this does not depend on the libnx version used to build.
Service g_avm;
bool g_avmOpen = false;

bool avmOpen() {
    if (g_avmOpen) return true;
    if (hosversionBefore(6, 0, 0)) return false;
    if (R_FAILED(smGetService(&g_avm, "avm"))) return false;
    if (R_FAILED(serviceConvertToDomain(&g_avm))) { serviceClose(&g_avm); return false; }
    g_avmOpen = true;
    return true;
}

Result avmLaunchRequired(u64 appId, u32* out) { return serviceDispatchInOut(&g_avm, 200, appId, *out); }

Result avmPushLaunch(u64 appId, u32 version) {
    const struct {
        u32 version;
        u32 pad;
        u64 application_id;
    } in = {version, 0, appId};
    return serviceDispatchIn(&g_avm, 1000, in);
}

const char* kLaunchPath = "sdmc:/config/lan-installer/launch-versions.txt";

// "APPID version" lines of launch versions already pushed (see syncLaunchVersion).
std::set<std::string>& launchDone() {
    static std::set<std::string> done;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        if (FILE* f = fopen(kLaunchPath, "r")) {
            char line[64];
            while (fgets(line, sizeof(line), f)) {
                std::string l(line);
                while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
                if (!l.empty()) done.insert(l);
            }
            fclose(f);
        }
    }
    return done;
}

// Keeps launch-versions.txt as long as the number of installed games: drops games that were
// deleted and older versions of the others.
void pruneLaunchVersions(const titles::Installed& inst) {
    std::set<std::string>& done = launchDone();
    std::set<std::string> keep;
    for (const std::string& t : done) {
        unsigned long long app = 0;
        unsigned ver = 0;
        if (sscanf(t.c_str(), "%llx %u", &app, &ver) != 2) continue;
        auto b = inst.find((u64)app);
        if (b == inst.end()) continue;  // game no longer installed
        u32 latest = b->second;
        auto pt = inst.find((u64)app | 0x800);
        if (pt != inst.end() && pt->second > latest) latest = pt->second;
        if (ver == latest) keep.insert(t);
    }
    if (keep.size() == done.size()) return;
    log("launch versions: kept %zu of %zu entries", keep.size(), done.size());
    done = keep;
    if (FILE* f = fopen(kLaunchPath, "w")) {
        for (const std::string& t : done) fprintf(f, "%s\n", t.c_str());
        fclose(f);
    }
}

// Pushes the highest installed game version (base or update, not DLC) as the launch version.
// force=false: only when the system's value differs from it.
bool syncLaunchVersion(u64 appId, bool force, int* changed = nullptr) {
    if (!avmOpen()) return false;
    u32 latest = 0;
    bool any = false;
    for (const auto& r : recordsFromNcm(appId)) {
        if (r.key.id != appId && r.key.id != (appId | 0x800)) continue;
        any = true;
        latest = std::max(latest, r.key.version);
    }
    if (!any) return false;
    // remember what was pushed, so the startup check does each game/version only once
    std::set<std::string>& done = launchDone();
    const char* path = kLaunchPath;
    char tag[40];
    snprintf(tag, sizeof(tag), "%016llX %u", (unsigned long long)appId, latest);
    u32 required = 0;
    Result rcGet = avmLaunchRequired(appId, &required);
    if (!force && (done.count(tag) || (R_SUCCEEDED(rcGet) && required == latest))) return true;
    Result rc = avmPushLaunch(appId, latest);
    log("launch version %016llX: system %u%s -> push v%u -> 0x%X", (unsigned long long)appId, required,
        R_SUCCEEDED(rcGet) ? "" : " (unread)", latest, rc);
    if (R_SUCCEEDED(rc)) {
        if (!done.count(tag)) {
            done.insert(tag);
            if (FILE* f = fopen(path, "a")) { fprintf(f, "%s\n", tag); fclose(f); }
        }
        if (changed) (*changed)++;
    }
    return R_SUCCEEDED(rc);
}

bool pushRecord(Ctx& c, const fmt::ContentMeta& meta, const NcmContentMetaKey& key, std::string& err) {
    (void)c;
    log("installed %016llX v%u type 0x%02X", (unsigned long long)key.id, key.version, key.type);
    if (!rebuildRecord(meta.applicationId(), err)) return false;
    syncLaunchVersion(meta.applicationId(), true);
    return true;
}

const fmt::Pfs0Entry* findByName(const std::vector<fmt::Pfs0Entry>& v, const std::string& name) {
    for (const auto& e : v)
        if (e.name == name) return &e;
    return nullptr;
}

bool isContentFile(const std::string& n) { return fmt::endsWith(n, ".nca") || fmt::endsWith(n, ".ncz"); }

}  // namespace

// ---------------------------------------------------------------- public API
Result initServices() {
    Result rc;
    if (R_FAILED(rc = ncmInitialize())) return rc;
    if (R_FAILED(rc = nsInitialize())) return rc;
    if (hosversionAtLeast(3, 0, 0)) {
        if (R_FAILED(rc = nsGetApplicationManagerInterface(&g_nsAm))) return rc;
        g_nsAmOwned = true;
    }
    if (R_FAILED(rc = smGetService(&g_es, "es"))) return rc;
    return 0;
}

void setLogTag(int tag) { t_logTag = tag; }
void setReaderCore(int core) { t_readerCore = core; }
void setDownloadBuffer(size_t bytes) { g_pipeCap = bytes; }

void log(const char* fmt, ...) {
    Lock lk(g_logLock);
    static bool first = true;
    static int lines = 0;
    const char* path = "sdmc:/config/lan-installer/install.log";
    const char* old = "sdmc:/config/lan-installer/install.old.log";
    if (first) {
        first = false;
        mkdir("sdmc:/config", 0777);
        mkdir("sdmc:/config/lan-installer", 0777);
    }
    if (lines++ % 200 == 0) {  // keep it small: past 512 KB it becomes install.old.log (the two latest stay)
        struct stat st;
        if (stat(path, &st) == 0 && st.st_size > 512 * 1024) {
            remove(old);
            rename(path, old);
        }
    }
    FILE* f = fopen(path, "a");
    if (!f) return;
    time_t t = time(nullptr);
    struct tm* tm = localtime(&t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
    if (t_logTag) fprintf(f, "%s  [%d] ", ts, t_logTag);
    else fprintf(f, "%s  ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

int repairRecords() {
    Lock reg(g_regLock);
    titles::Installed inst;
    if (!scanInstalled(inst)) return 0;
    pruneLaunchVersions(inst);
    int fixed = 0;
    for (const auto& kv : inst) {
        if ((kv.first & 0xFFF) != 0) continue;  // base games only
        std::string err;
        int changed = 0;
        rebuildRecord(kv.first, err, &changed);
        syncLaunchVersion(kv.first, false, &changed);
        if (changed) fixed++;
    }
    if (fixed) log("repaired %d record(s)", fixed);
    return fixed;
}

// ---------------------------------------------------------------- cartridge dumps
const uint8_t* headerKey() {
    static bool tried = false;
    static uint8_t key[0x20];
    static bool have = false;
    static const char* where = nullptr;
    if (!tried) {
        tried = true;
        const char* paths[] = {"sdmc:/switch/prod.keys", "sdmc:/prod.keys", "sdmc:/switch/keys.txt",
                               "sdmc:/config/lan-installer/prod.keys"};
        for (const char* p : paths)
            if (crypto::loadHeaderKey(p, key)) { have = true; where = p; break; }
        log("header key: %s", have ? where : "not found (cartridge dumps are installed unconverted)");
    }
    return have ? key : nullptr;
}

bool prodKey(const char* name, uint8_t* out, size_t len) {
    const char* paths[] = {"sdmc:/switch/prod.keys", "sdmc:/prod.keys", "sdmc:/switch/keys.txt",
                           "sdmc:/config/lan-installer/prod.keys"};
    for (const char* p : paths)
        if (crypto::loadKey(p, name, out, len)) return true;
    return false;
}

// Highest installed version of exactly this title id (update or DLC), 0 when none.
static u32 installedTitleVersion(u64 titleId, bool* found) {
    *found = false;
    u32 best = 0;
    const NcmStorageId storages[] = {NcmStorageId_SdCard, NcmStorageId_BuiltInUser};
    for (NcmStorageId st : storages) {
        NcmContentMetaDatabase db;
        if (R_FAILED(ncmOpenContentMetaDatabase(&db, st))) continue;
        NcmContentMetaKey key;
        if (R_SUCCEEDED(ncmContentMetaDatabaseGetLatestContentMetaKey(&db, &key, titleId)) && key.id == titleId) {
            if (!*found || key.version > best) best = key.version;
            *found = true;
        }
        ncmContentMetaDatabaseClose(&db);
    }
    return best;
}

std::string splitTitleUrl(const std::string& url, uint64_t& onlyTitle) {
    onlyTitle = 0;
    size_t h = url.rfind("#gs-title=");
    if (h == std::string::npos) return url;
    onlyTitle = strtoull(url.c_str() + h + 10, nullptr, 16);
    return url.substr(0, h);
}

bool hasApplication(u64 appId) {
    const NcmStorageId storages[] = {NcmStorageId_SdCard, NcmStorageId_BuiltInUser};
    for (NcmStorageId st : storages) {
        NcmContentMetaDatabase db;
        if (R_FAILED(ncmOpenContentMetaDatabase(&db, st))) continue;
        NcmContentMetaKey key;
        Result rc = ncmContentMetaDatabaseGetLatestContentMetaKey(&db, &key, appId);
        ncmContentMetaDatabaseClose(&db);
        if (R_SUCCEEDED(rc) && key.type == NcmContentMetaType_Application) return true;
    }
    // also count a HOME-menu record that lists content (the content meta lookup above can miss it)
    ContentStorageRecord recs[4];
    u32 count = 0;
    if (R_SUCCEEDED(nsListApplicationRecordContentMeta(0, appId, recs, 4, &count)) && count > 0) return true;
    return false;
}

void refreshAppInfo(u64 appId) {
    Result rc = nsInvalidateApplicationControlCache(appId);
    log("control cache %016llX -> 0x%X", (unsigned long long)appId, (unsigned)rc);
}

// ---------------------------------------------------------------- deleting installed content
// Never through ns (nsDeleteApplicationCompletely / the HOME menu's 刪除軟體): after deleting, the
// system also clears every placeholder on the SD card, which throws away the half-done downloads
// of all other games. Here each content meta is removed from ncm directly, then the HOME record is
// rewritten; placeholders are left alone.
namespace {

const NcmStorageId kStorages[] = {NcmStorageId_SdCard, NcmStorageId_BuiltInUser};

bool isGameMeta(u8 t) {
    return t == NcmContentMetaType_Application || t == NcmContentMetaType_Patch || t == NcmContentMetaType_AddOnContent;
}

std::vector<NcmContentMetaKey> appKeys(NcmContentMetaDatabase& db, u64 appId) {
    std::vector<NcmContentMetaKey> keys(512);
    s32 total = 0, written = 0;
    if (R_FAILED(ncmContentMetaDatabaseList(&db, &total, &written, keys.data(), (s32)keys.size(), NcmContentMetaType_Unknown,
                                            0, appId, appId + 0x1FFF, NcmContentInstallType_Full)))
        return {};
    keys.resize((size_t)std::max<s32>(0, std::min<s32>(written, (s32)keys.size())));
    keys.erase(std::remove_if(keys.begin(), keys.end(), [](const NcmContentMetaKey& k) { return !isGameMeta(k.type); }),
               keys.end());
    return keys;
}

std::vector<NcmContentInfo> keyContents(NcmContentMetaDatabase& db, const NcmContentMetaKey& k) {
    std::vector<NcmContentInfo> out;
    NcmContentInfo buf[32];
    for (s32 start = 0;;) {
        s32 n = 0;
        if (R_FAILED(ncmContentMetaDatabaseListContentInfo(&db, &n, buf, 32, &k, start)) || n <= 0) break;
        out.insert(out.end(), buf, buf + n);
        if (n < 32) break;
        start += n;
    }
    return out;
}

bool sameKey(const NcmContentMetaKey& a, const ContentPiece& p) {
    return a.id == p.id && a.version == p.version && a.type == p.type;
}

}  // namespace

namespace {
// Adds every game piece (base / update / DLC) with an id in [lo, hi] to `games`, by application.
void collectPieces(std::map<u64, InstalledGame>& games, u64 lo, u64 hi) {
    for (NcmStorageId st : kStorages) {
        NcmContentMetaDatabase db;
        if (R_FAILED(ncmOpenContentMetaDatabase(&db, st))) continue;
        std::vector<NcmContentMetaKey> keys(4096);
        s32 total = 0, written = 0;
        Result rc = ncmContentMetaDatabaseList(&db, &total, &written, keys.data(), (s32)keys.size(),
                                               NcmContentMetaType_Unknown, 0, lo, hi, NcmContentInstallType_Full);
        if (R_SUCCEEDED(rc)) {
            keys.resize((size_t)std::max<s32>(0, std::min<s32>(written, (s32)keys.size())));
            for (const auto& k : keys) {
                if (!isGameMeta(k.type)) continue;
                u64 app = k.type == NcmContentMetaType_Application ? k.id
                          : k.type == NcmContentMetaType_Patch     ? (k.id & ~0xFFFULL)
                                                                   : ((k.id - 0x1000) & ~0xFFFULL);
                ContentPiece p;
                p.id = k.id;
                p.version = k.version;
                p.type = k.type;
                p.storage = (u8)st;
                for (const auto& ci : keyContents(db, k)) {
                    u64 sz = 0;
                    ncmContentInfoSizeToU64(&ci, &sz);
                    p.size += sz;
                }
                InstalledGame& g = games[app];
                g.appId = app;
                g.pieces.push_back(p);
                g.total += p.size;
            }
        }
        ncmContentMetaDatabaseClose(&db);
    }
    for (auto& kv : games)  // base, then updates (old to new), then DLC
        std::sort(kv.second.pieces.begin(), kv.second.pieces.end(), [](const ContentPiece& a, const ContentPiece& b) {
            if (a.type != b.type) return a.type < b.type;
            if (a.id != b.id) return a.id < b.id;
            return a.version < b.version;
        });
}
}  // namespace

std::vector<InstalledGame> listInstalledGames() {
    std::map<u64, InstalledGame> games;
    collectPieces(games, 0, UINT64_MAX);
    std::vector<InstalledGame> out;
    for (auto& kv : games)
        if (kv.first != shortcut::kTitleId) out.push_back(kv.second);  // our own HOME shortcut is not a game
    return out;
}

bool listInstalledGame(u64 appId, InstalledGame& out) {
    std::map<u64, InstalledGame> games;
    collectPieces(games, appId, appId + 0x1FFF);
    auto it = games.find(appId);
    if (it == games.end()) { out = InstalledGame(); out.appId = appId; return false; }
    out = it->second;
    return true;
}

bool installedControl(u64 appId, std::string& name, std::string& displayVersion) {
    name.clear();
    displayVersion.clear();
    NsApplicationControlData* d = (NsApplicationControlData*)malloc(sizeof(NsApplicationControlData));
    if (!d) return false;
    u64 size = 0;
    // "Storage" = the system's cache first (what the HOME menu shows), the game files only when missing
    bool ok = R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, appId, d, sizeof(*d), &size)) &&
              size >= sizeof(d->nacp);
    if (ok) {
        const int order[] = {13, 14, 2, 0, 1, 12, 3, 4, 5, 6, 7, 8, 9, 10, 11, 15};
        // [21.0.0+] titles may be stored DEFLATE-compressed (32 entries once unpacked). Read by
        // offset: older libnx versions do not name these fields.
        const uint8_t* raw = (const uint8_t*)&d->nacp;
        std::vector<NacpLanguageEntry> langs(32);
        size_t nLangs = 16;
        if (raw[0x3215] == 1) {  // TitlesDataFormat
            int len = std::min<int>(raw[0] | (raw[1] << 8), 0x2FFE);
            int got = stbi_zlib_decode_noheader_buffer((char*)langs.data(), (int)(langs.size() * sizeof(NacpLanguageEntry)),
                                                       (const char*)raw + 2, len);
            nLangs = got > 0 ? (size_t)got / sizeof(NacpLanguageEntry) : 0;
        } else {
            memcpy(langs.data(), raw, 16 * sizeof(NacpLanguageEntry));
        }
        for (int k : order) {
            if ((size_t)k >= nLangs) continue;
            const NacpLanguageEntry& e = langs[(size_t)k];
            if (e.name[0]) {
                name.assign(e.name, strnlen(e.name, sizeof(e.name)));
                break;
            }
        }
        char v[0x11] = {0};
        memcpy(v, d->nacp.display_version, 0x10);
        displayVersion = v;
    }
    free(d);
    return ok;
}

int deleteSaveData(u64 appId) {
    int n = 0;
    const FsSaveDataSpaceId spaces[] = {FsSaveDataSpaceId_User, FsSaveDataSpaceId_SdUser};
    for (FsSaveDataSpaceId sp : spaces) {
        std::vector<u64> ids;
        FsSaveDataInfoReader rd;
        if (R_FAILED(fsOpenSaveDataInfoReader(&rd, sp))) continue;
        FsSaveDataInfo info[32];
        for (;;) {
            s64 got = 0;
            if (R_FAILED(fsSaveDataInfoReaderRead(&rd, info, 32, &got)) || got <= 0) break;
            for (s64 i = 0; i < got; i++)
                if (info[i].application_id == appId && info[i].save_data_type != FsSaveDataType_System &&
                    info[i].save_data_type != FsSaveDataType_SystemBcat)
                    ids.push_back(info[i].save_data_id);
        }
        fsSaveDataInfoReaderClose(&rd);
        for (u64 id : ids) {
            Result rc = fsDeleteSaveDataFileSystemBySaveDataSpaceId(sp, id);
            log("  save data %016llX (space %d) -> 0x%X", (unsigned long long)id, (int)sp, (unsigned)rc);
            if (R_SUCCEEDED(rc)) n++;
        }
    }
    return n;
}

bool deletePieces(u64 appId, const std::vector<ContentPiece>& asked, uint64_t& freed, std::string& err) {
    Lock reg(g_regLock);
    freed = 0;
    // Without its base game nothing of a game can be used: deleting the base takes everything.
    std::vector<ContentPiece> pieces = asked;
    bool withBase = false;
    for (const auto& p : asked) withBase = withBase || p.type == NcmContentMetaType_Application;
    if (withBase) {
        pieces.clear();
        for (NcmStorageId st : kStorages) {
            NcmContentMetaDatabase db;
            if (R_FAILED(ncmOpenContentMetaDatabase(&db, st))) continue;
            for (const auto& k : appKeys(db, appId)) {
                ContentPiece p;
                p.id = k.id;
                p.version = k.version;
                p.type = k.type;
                p.storage = (u8)st;
                pieces.push_back(p);
            }
            ncmContentMetaDatabaseClose(&db);
        }
    }
    int removed = 0;
    bool baseGone = false;
    for (NcmStorageId st : kStorages) {
        NcmContentMetaDatabase db;
        if (R_FAILED(ncmOpenContentMetaDatabase(&db, st))) continue;
        NcmContentStorage cs;
        if (R_FAILED(ncmOpenContentStorage(&cs, st))) { ncmContentMetaDatabaseClose(&db); continue; }
        std::vector<NcmContentMetaKey> keys = appKeys(db, appId);
        std::vector<NcmContentMetaKey> drop, keep;
        for (const auto& k : keys) {
            bool d = false;
            for (const auto& p : pieces) d = d || (sameKey(k, p) && p.storage == (u8)st);
            (d ? drop : keep).push_back(k);
        }
        // contents still used by something that stays (rare: shared between versions) are kept
        std::set<std::string> used;
        for (const auto& k : keep)
            for (const auto& ci : keyContents(db, k)) used.insert(fmt::bytesToHex(ci.content_id.c, 0x10));
        bool changed = false;
        for (const auto& k : drop) {
            for (const auto& ci : keyContents(db, k)) {
                if (used.count(fmt::bytesToHex(ci.content_id.c, 0x10))) continue;
                bool has = false;
                ncmContentStorageHas(&cs, &has, &ci.content_id);
                if (!has) continue;
                u64 sz = 0;
                ncmContentInfoSizeToU64(&ci, &sz);
                if (R_SUCCEEDED(ncmContentStorageDelete(&cs, &ci.content_id))) freed += sz;
            }
            Result rc = ncmContentMetaDatabaseRemove(&db, &k);
            log("delete %016llX v%u type 0x%02X (storage %d) -> 0x%X", (unsigned long long)k.id, k.version, k.type, (int)st,
                (unsigned)rc);
            if (R_SUCCEEDED(rc)) {
                removed++;
                changed = true;
                if (k.type == NcmContentMetaType_Application) baseGone = true;
            } else if (err.empty()) {
                err = rcStr("刪除內容紀錄", rc);
            }
        }
        if (changed) ncmContentMetaDatabaseCommit(&db);
        ncmContentStorageClose(&cs);
        ncmContentMetaDatabaseClose(&db);
    }

    // HOME record: gone with the base game, otherwise exactly what is still installed.
    bool anyBase = false;
    std::vector<ContentStorageRecord> rest = recordsFromNcm(appId);
    for (const auto& r : rest) anyBase = anyBase || r.key.type == NcmContentMetaType_Application;
    if (baseGone || !anyBase || rest.empty()) {
        Result rc = nsDeleteApplicationRecord(appId);
        log("record %016llX: deleted -> 0x%X", (unsigned long long)appId, (unsigned)rc);
    } else {
        nsDeleteApplicationRecord(appId);
        Result rc = nsPushApplicationRecord(appId, rest.data(), rest.size());
        log("record %016llX: %zu entries left -> 0x%X", (unsigned long long)appId, rest.size(), (unsigned)rc);
        if (R_FAILED(rc) && err.empty()) err = rcStr("更新遊戲紀錄", rc);
        syncLaunchVersion(appId, true);  // e.g. update deleted: the base version is enough again
    }
    nsInvalidateApplicationControlCache(appId);
    log("deleted %d item(s) of %016llX, freed %llu bytes", removed, (unsigned long long)appId, (unsigned long long)freed);
    return removed > 0 && err.empty();
}

Result deleteApplication(u64 appId) {
    std::vector<ContentPiece> all;
    for (NcmStorageId st : kStorages) {
        NcmContentMetaDatabase db;
        if (R_FAILED(ncmOpenContentMetaDatabase(&db, st))) continue;
        for (const auto& k : appKeys(db, appId)) {
            ContentPiece p;
            p.id = k.id;
            p.version = k.version;
            p.type = k.type;
            p.storage = (u8)st;
            all.push_back(p);
        }
        ncmContentMetaDatabaseClose(&db);
    }
    uint64_t freed = 0;
    std::string err;
    if (all.empty()) {  // only a HOME record left
        Result rc = nsDeleteApplicationRecord(appId);
        log("delete application %016llX: record only -> 0x%X", (unsigned long long)appId, (unsigned)rc);
        return rc;
    }
    bool ok = deletePieces(appId, all, freed, err);
    log("delete application %016llX -> %s", (unsigned long long)appId, ok ? "ok" : err.c_str());
    return ok ? 0 : MAKERESULT(Module_Libnx, LibnxError_IoError);
}

bool isGamecardInstall(u64 appId) {
    const uint8_t* key = headerKey();
    if (!key) return false;
    const NcmStorageId storages[] = {NcmStorageId_SdCard, NcmStorageId_BuiltInUser};
    for (NcmStorageId st : storages) {
        NcmContentMetaDatabase db;
        if (R_FAILED(ncmOpenContentMetaDatabase(&db, st))) continue;
        NcmContentMetaKey keys[4];
        s32 total = 0, written = 0;
        bool card = false, found = false;
        if (R_SUCCEEDED(ncmContentMetaDatabaseList(&db, &total, &written, keys, 4, NcmContentMetaType_Application, 0,
                                                   appId, appId, NcmContentInstallType_Full)) && written > 0) {
            NcmContentId pid;
            if (R_SUCCEEDED(ncmContentMetaDatabaseGetContentIdByType(&db, &pid, &keys[0], NcmContentType_Program))) {
                NcmContentStorage cs;
                if (R_SUCCEEDED(ncmOpenContentStorage(&cs, st))) {
                    uint8_t hdr[0x400];
                    if (R_SUCCEEDED(ncmContentStorageReadContentIdFile(&cs, hdr, sizeof(hdr), &pid, 0))) {
                        crypto::xtsNintendo(key, hdr, sizeof(hdr), 0, false);
                        if (!memcmp(hdr + 0x200, "NCA3", 4) || !memcmp(hdr + 0x200, "NCA2", 4)) {
                            found = true;
                            card = hdr[0x204] == 1;
                        }
                    }
                    ncmContentStorageClose(&cs);
                }
            }
        }
        ncmContentMetaDatabaseClose(&db);
        if (found) return card;
    }
    return false;
}

void scanGamecardInstalls(const titles::Installed& installed, std::set<u64>& out) {
    out.clear();
    if (!headerKey()) return;
    for (const auto& kv : installed)
        if ((kv.first & 0xFFF) == 0 && isGamecardInstall(kv.first)) out.insert(kv.first);
    if (!out.empty()) log("cartridge-type installs: %zu", out.size());
}

bool scanInstalled(titles::Installed& out) {
    out.clear();
    bool any = false;
    const NcmStorageId storages[] = {NcmStorageId_SdCard, NcmStorageId_BuiltInUser};
    for (NcmStorageId st : storages) {
        NcmContentMetaDatabase db;
        if (R_FAILED(ncmOpenContentMetaDatabase(&db, st))) continue;
        std::vector<NcmContentMetaKey> keys(2048);
        s32 total = 0, written = 0;
        Result rc = ncmContentMetaDatabaseList(&db, &total, &written, keys.data(), (s32)keys.size(),
                                               NcmContentMetaType_Unknown, 0, 0, UINT64_MAX, NcmContentInstallType_Full);
        if (R_SUCCEEDED(rc) && total > written) {
            keys.resize((size_t)total);
            rc = ncmContentMetaDatabaseList(&db, &total, &written, keys.data(), (s32)keys.size(),
                                            NcmContentMetaType_Unknown, 0, 0, UINT64_MAX, NcmContentInstallType_Full);
        }
        if (R_SUCCEEDED(rc)) {
            any = true;
            for (s32 i = 0; i < written; i++) {
                const NcmContentMetaKey& k = keys[(size_t)i];
                if (k.type != NcmContentMetaType_Application && k.type != NcmContentMetaType_Patch &&
                    k.type != NcmContentMetaType_AddOnContent)
                    continue;
                auto it = out.find(k.id);
                if (it == out.end() || it->second < k.version) out[k.id] = k.version;
            }
        }
        ncmContentMetaDatabaseClose(&db);
    }
    return any;
}

void exitServices() {
    if (g_avmOpen) { serviceClose(&g_avm); g_avmOpen = false; }
    serviceClose(&g_es);
    if (g_nsAmOwned) { serviceClose(&g_nsAm); g_nsAmOwned = false; }
    nsExit();
    ncmExit();
}

// Encoded system version (major << 26 | minor << 20 | micro << 16) of this console, 0 if unknown.
static u32 currentSystemVersion() {
    static u32 cached = 0xFFFFFFFF;
    if (cached != 0xFFFFFFFF) return cached;
    cached = 0;
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
            cached = ((u32)fw.major << 26) | ((u32)fw.minor << 20) | ((u32)fw.micro << 16);
        setsysExit();
    }
    return cached;
}

static std::string sysVerStr(u32 v) {
    char b[32];
    snprintf(b, sizeof(b), "%u.%u.%u", v >> 26, (v >> 20) & 0x3F, (v >> 16) & 0xF);
    return b;
}

static u32 extU32(const std::vector<uint8_t>& e, size_t off) {
    if (e.size() < off + 4) return 0;
    return (u32)e[off] | ((u32)e[off + 1] << 8) | ((u32)e[off + 2] << 16) | ((u32)e[off + 3] << 24);
}

// Does the game need a ticket it will not find? Reads each installed NCA's header: a rights id
// means its key comes from a ticket; that ticket must be in the package or already on the console.
// Logged either way; only the missing ones are recorded.
static void checkTickets(Ctx& c, const fmt::ContentMeta& meta, const std::set<std::string>& pkgRights) {
    const uint8_t* key = c.opt.headerKey ? c.opt.headerKey : headerKey();
    if (!key) { log("  ticket check: no header key, skipped"); return; }
    std::set<std::string> needed;
    for (const auto& ct : meta.contents) {
        if (ct.type == fmt::CT_DeltaFragment) continue;
        NcmContentId id;
        memcpy(id.c, ct.contentId, 0x10);
        uint8_t hdr[0x400];
        if (R_FAILED(ncmContentStorageReadContentIdFile(&c.cs, hdr, sizeof(hdr), &id, 0))) continue;
        crypto::xtsNintendo(key, hdr, sizeof(hdr), 0, false);
        if (memcmp(hdr + 0x200, "NCA3", 4) != 0 && memcmp(hdr + 0x200, "NCA2", 4) != 0) continue;
        bool any = false;
        for (int i = 0; i < 0x10; i++) any = any || hdr[0x230 + i];
        if (any) needed.insert(fmt::bytesToHex(hdr + 0x230, 0x10));
    }
    if (needed.empty()) { log("  ticket: not needed (no rights id)"); return; }
    std::set<std::string> console;
    bool consoleRead = false;
    for (const auto& r : needed) {
        if (pkgRights.count(r)) { log("  ticket %s: in the package", r.c_str()); continue; }
        if (!consoleRead) { console = esTicketRights(); consoleRead = true; }
        if (console.count(r)) { log("  ticket %s: already on the console", r.c_str()); continue; }
        log("  ticket %s: MISSING (in neither the package nor the console: the game cannot start)", r.c_str());
        if (c.opt.findings) c.opt.findings->missingTicket.push_back(r);
    }
}

bool installPackage(const std::string& url, const Options& opt, const ProgressFn& progress, std::string& err,
                    std::string& note) {
    Ctx c;
    // "<package>#gs-title=<TID>": one title of a merged package, shown as a file of its own
    uint64_t onlyTitle = 0;
    std::string fetchUrl = splitTitleUrl(url, onlyTitle);
    c.url = fetchUrl;
    c.opt = opt;
    c.progress = progress;
    c.lastReportTick = c.speedTick = armGetSystemTick();
    note.clear();

    log("install start: %s", http::redact(url).c_str());
    // 1. Open the container (NSP/NSZ = PFS0, XCI/XCZ = secure HFS0 partition).
    c.p.stage = "正在讀取封裝檔標頭";
    report(c, true);
    container::Kind kind;
    std::vector<fmt::Pfs0Entry> files;
    container::ReadFn reader = [&](uint64_t off, uint64_t size, std::vector<uint8_t>& out, std::string& e) {
        return http::getToMemory(fetchUrl, off, size, out, e);
    };
    if (!container::open(reader, kind, files, err)) { log("install FAILED: %s", err.c_str()); return false; }
    for (const auto& f : files)
        if (isContentFile(f.name)) c.p.total += f.size;
    if (onlyTitle) {
        log("  only title %016llX of this package", (unsigned long long)onlyTitle);
        if (opt.onlyTotal && opt.onlyTotal < c.p.total) c.p.total = opt.onlyTotal;
    }

    Result rc;
    if (R_FAILED(rc = ncmOpenContentStorage(&c.cs, opt.storage))) { err = rcStr("開啟內容儲存空間", rc); return false; }
    if (R_FAILED(rc = ncmOpenContentMetaDatabase(&c.db, opt.storage))) {
        ncmContentStorageClose(&c.cs);
        err = rcStr("開啟內容資料庫", rc);
        return false;
    }

    // Progress of an earlier attempt of this same package (same URL and same contents).
    if (opt.keepProgress) {
        c.rsPath = resume::pathFor(kResumeDir, url);
        resume::State old;
        if (resume::load(c.rsPath, old)) {
            if (old.url == url && old.total == c.p.total) {
                c.rs = old;
                int before = c.rs.percent();
                if (dropMissing(c.cs, c.rs)) {
                    c.wiped = c.rs.completed() == 0;
                    log("  resume: data of the earlier attempt was cleared by the system (%d%% -> %d%%)", before,
                        c.rs.percent());
                    saveResume(c);
                }
                log("  resume: continuing an earlier attempt (%d%% done)", c.rs.percent());
            } else {
                log("  resume: package changed on the server, starting over");
                for (const auto& pt : old.parts) {
                    NcmPlaceHolderId ph;
                    if (fmt::hexToBytes(pt.second.placeholder, ph.uuid.uuid, 0x10)) ncmContentStorageDeletePlaceHolder(&c.cs, &ph);
                }
                resume::remove(c.rsPath);
            }
        }
        c.rs.url = url;
        c.rs.total = c.p.total;
        if (!c.rs.started) c.rs.started = (int64_t)time(nullptr);
        c.rs.name = opt.displayName.empty() ? url.substr(url.rfind('/') + 1) : opt.displayName;
    }

    bool ok = true;
    int metas = 0, tickets = 0;
    std::string warn;
    std::set<std::string> pkgRights;  // rights ids of the tickets in this package

    // 2. Tickets + certificates (eShop titles use rights-id crypto; gamecard dumps have none).
    for (const auto& f : files) {
        if (!ok || !fmt::endsWith(f.name, ".tik")) continue;
        std::string stem = f.name.substr(0, f.name.size() - 4);
        const fmt::Pfs0Entry* cert = findByName(files, stem + ".cert");
        if (!cert)
            for (const auto& g : files)
                if (fmt::endsWith(g.name, ".cert")) { cert = &g; break; }
        if (!cert) { err = "封裝檔中的 ticket 缺少憑證"; ok = false; break; }
        c.p.stage = "正在匯入 ticket " + f.name;
        report(c, true);
        std::vector<uint8_t> tik, crt;
        if (!download(c, f, tik, err) || !download(c, *cert, crt, err)) { ok = false; break; }
        rc = esImportTicket(tik.data(), tik.size(), crt.data(), crt.size());
        tickets++;
        std::string rights = tik.size() >= 0x2B0 ? fmt::bytesToHex(&tik[0x2A0], 0x10) : "?";
        if (tik.size() >= 0x2B0) pkgRights.insert(rights);
        log("  ticket %s (%zu bytes, cert %zu bytes) rights=%s keygen=%u keytype=%s -> 0x%X", f.name.c_str(),
            tik.size(), crt.size(), rights.c_str(), tik.size() > 0x285 ? (unsigned)tik[0x285] : 0u,
            tik.size() > 0x281 ? (tik[0x281] == 0 ? "common" : tik[0x281] == 1 ? "personalized" : "other") : "?",
            (unsigned)rc);
        if (R_FAILED(rc)) {
            err = rcStr("匯入 ticket", rc);
            ok = false;
        }
    }
    if (ok) log("  tickets imported: %d", tickets);

    // 3. Every content meta in the package (an XCI can hold base game + update + DLC).
    for (const auto& f : files) {
        if (!ok) break;
        if (!fmt::endsWith(f.name, ".cnmt.nca")) continue;
        metas++;

        NcmContentId cnmtId;
        if (!fmt::hexToBytes(f.name, cnmtId.c, 0x10)) { err = "檔名格式錯誤：" + f.name; ok = false; break; }
        // The meta NCA is left untouched so it can still be mounted without sigpatches.
        if (!installContent(c, f, cnmtId, f.size, nullptr, false, err)) { ok = false; break; }

        c.p.stage = "正在讀取內容描述";
        report(c, true);
        std::vector<uint8_t> raw;
        fmt::ContentMeta meta;
        if (!readInstalledCnmt(c, cnmtId, raw, err) || !fmt::parseCnmt(raw, meta, err)) { ok = false; break; }
        {   // Not wanted from this package (a newer one comes from another file), or older than what the
            // console already has: never install over it (no downgrade).
            bool skip = opt.skipTitles.count(meta.titleId) > 0 || (onlyTitle && meta.titleId != onlyTitle);
            bool have = false;
            u32 haveVer = 0;
            if (!skip && (meta.type == 0x81 || meta.type == 0x82)) {
                haveVer = installedTitleVersion(meta.titleId, &have);
                skip = have && haveVer > meta.version;
            }
            if (skip) {
                log("  skip %016llX v%u type 0x%02X (%s)", (unsigned long long)meta.titleId, meta.version, meta.type,
                    have ? "console already has a newer version"
                         : (onlyTitle && meta.titleId != onlyTitle) ? "another title of the package" : "newer one comes from another file");
                // drop the meta NCA just written, unless the very same meta is installed (shares it)
                NcmContentMetaKey same;
                memset(&same, 0, sizeof(same));
                same.id = meta.titleId;
                same.version = meta.version;
                same.type = meta.type;
                same.install_type = NcmContentInstallType_Full;
                bool shared = false;
                ncmContentMetaDatabaseHas(&c.db, &shared, &same);
                if (!shared) ncmContentStorageDelete(&c.cs, &cnmtId);
                if (have) {
                    char b[160];
                    snprintf(b, sizeof(b), "已有較新版本，略過%s v%u（主機上 v%u）。",
                             meta.type == 0x81 ? "更新" : "DLC", meta.version, haveVer);
                    warn += b;
                }
                continue;
            }
        }
        {
            uint32_t reqSys = meta.extendedHeader.size() >= 0xC ? (uint32_t)meta.extendedHeader[8] |
                              ((uint32_t)meta.extendedHeader[9] << 8) | ((uint32_t)meta.extendedHeader[10] << 16) |
                              ((uint32_t)meta.extendedHeader[11] << 24) : 0;
            log("  cnmt %016llX v%u type 0x%02X ext=%zu extData=%zu metaInfos=%zu reqSys=%u (%u.%u.%u)",
                (unsigned long long)meta.titleId, meta.version, meta.type, meta.extendedHeader.size(),
                meta.extendedData.size(), meta.metaInfos.size() / 0x10, reqSys, reqSys >> 26, (reqSys >> 20) & 0x3F,
                (reqSys >> 16) & 0xF);
            for (const auto& ct : meta.contents)
                log("    content %s type %u size %llu", fmt::bytesToHex(ct.contentId, 0x10).c_str(), ct.type,
                    (unsigned long long)ct.size);
            bool needsTicket = meta.type == 0x80 || meta.type == 0x81 || meta.type == 0x82;
            if (needsTicket && tickets == 0) log("  NOTE: package has no .tik (fine only for non-rights-id dumps)");
        }
        // Base games and updates name the lowest system version they run on: refuse early instead of
        // installing something that can never start.
        if (meta.type == 0x80 || meta.type == 0x81) {
            u32 need = extU32(meta.extendedHeader, 8), have = currentSystemVersion();
            log("  system version: need %s, console %s", sysVerStr(need).c_str(), have ? sysVerStr(have).c_str() : "?");
            if (have && need > have) {
                ncmContentStorageDelete(&c.cs, &cnmtId);
                err = "此" + std::string(meta.type == 0x81 ? "更新" : "遊戲") + "需要系統 " + sysVerStr(need) +
                      " 以上，你的主機是 " + sysVerStr(have) + "，裝了也無法啟動。";
                ok = false;
                break;
            }
        }
        if (!opt.siblingsBusy && (meta.type == 0x81 || meta.type == 0x82) && isGamecardInstall(meta.applicationId())) {
            log("  base game is a cartridge-type install");
            warn += "這款遊戲的本體是卡帶版（XCI）原樣安裝，更新／DLC 無法在上面啟動：請用本軟體重新安裝一次本體（會自動轉換）。";
        }
        // DLC names the game version it needs. Install anyway, but say so.
        if (meta.type == 0x82 && !opt.siblingsBusy) {
            u32 need = extU32(meta.extendedHeader, 8), have = 0;
            u64 app = meta.applicationId();
            for (const auto& r : recordsFromNcm(app))
                if ((r.key.id == app || r.key.id == (app | 0x800)) && r.key.version > have) have = r.key.version;
            log("  dlc needs game v%u, installed v%u", need, have);
            if (need > have) {
                char b[160];
                snprintf(b, sizeof(b), "此 DLC 需要遊戲更新到 v%u（主機上是 v%u），請先安裝遊戲更新，否則可能無法啟動。",
                         need, have);
                warn += b;
            }
        }

        for (const auto& content : meta.contents) {
            if (content.type == fmt::CT_DeltaFragment) continue;
            std::string hex = fmt::bytesToHex(content.contentId, 0x10);
            const fmt::Pfs0Entry* e = findByName(files, hex + ".nca");
            if (!e) e = findByName(files, hex + ".ncz");
            if (!e) { err = "封裝檔缺少 " + hex + ".nca"; ok = false; break; }
            NcmContentId id;
            memcpy(id.c, content.contentId, 0x10);
            if (!installContent(c, *e, id, content.size, content.hash, true, err)) { ok = false; break; }
        }
        if (!ok) break;
        checkTickets(c, meta, pkgRights);

        c.p.stage = "正在註冊遊戲";
        report(c, true);
        NcmContentMetaKey key;
        memset(&key, 0, sizeof(key));
        key.id = meta.titleId;
        key.version = meta.version;
        key.type = meta.type;
        key.install_type = NcmContentInstallType_Full;
        std::vector<uint8_t> installMeta = fmt::buildInstallMeta(meta, cnmtId.c, f.size, 0);
        log("  meta set %zu bytes", installMeta.size());
        Lock reg(g_regLock);  // one registration at a time across parallel installs
        if (R_FAILED(rc = ncmContentMetaDatabaseSet(&c.db, &key, installMeta.data(), installMeta.size()))) {
            err = rcStr("寫入內容資料庫", rc);
            ok = false;
            break;
        }
        if (R_FAILED(rc = ncmContentMetaDatabaseCommit(&c.db))) {
            err = rcStr("提交內容資料庫", rc);
            ok = false;
            break;
        }
        if (!pushRecord(c, meta, key, err)) { ok = false; break; }
    }

    if (ok && metas == 0) { err = "封裝檔中找不到 .cnmt.nca"; ok = false; }
    log("install %s%s", ok ? "ok" : "FAILED: ", ok ? "" : err.c_str());
    if (!c.rsPath.empty()) {
        if (ok || (c.rs.done.empty() && c.rs.parts.empty())) resume::remove(c.rsPath);
        else log("  progress kept for later (%d%% done)", c.rs.percent());
    }

    ncmContentMetaDatabaseClose(&c.db);
    ncmContentStorageClose(&c.cs);
    if (ok && c.patchedNcas) log("  converted %d cartridge NCA(s) to eShop type", c.patchedNcas);
    if (ok) {
        // successful cartridge conversion is silent (only logged); warn only when it could not be done
        if (!c.patchedNcas && kind == container::Kind::Xci && (!opt.headerKey || !opt.convertGamecard))
            note = "卡帶版（XCI/XCZ）已照原樣安裝：找不到 SD 卡上的 prod.keys，無法轉換，日後裝更新或 DLC 會無法啟動。"
                   "請把 Lockpick_RCM 匯出的 prod.keys 放在 switch 資料夾後重裝本體。";
        if (!warn.empty()) note = note.empty() ? warn : note + " " + warn;
        c.p.stage = "完成";
        c.p.done = c.p.total;
        report(c, true);
    }
    return ok;
}

// ---------------------------------------------------------------- misc for the UI
void prepare() {
    headerKey();
    currentSystemVersion();
}

uint64_t sdFreeBytes() {
    FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc");
    s64 avail = 0;
    if (!fs || R_FAILED(fsFsGetFreeSpace(fs, "/", &avail))) return 0;
    return (uint64_t)avail;
}

std::string installedDisplayVersion(u64 appId) {
    NsApplicationControlData* d = (NsApplicationControlData*)malloc(sizeof(NsApplicationControlData));
    if (!d) return "";
    u64 size = 0;
    std::string v;
    if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, appId, d, sizeof(*d), &size)) &&
        size >= sizeof(d->nacp)) {
        char buf[0x11] = {0};
        memcpy(buf, d->nacp.display_version, 0x10);
        v = buf;
    }
    free(d);
    return v;
}

std::vector<PendingInstall> pendingInstalls() {
    std::vector<PendingInstall> out;
    NcmContentStorage cs;
    bool csOk = R_SUCCEEDED(ncmOpenContentStorage(&cs, NcmStorageId_SdCard));
    for (const std::string& path : resume::list(kResumeDir)) {
        resume::State st;
        if (!resume::load(path, st)) { resume::remove(path); continue; }
        bool had = !st.parts.empty() || !st.done.empty();
        bool lost = csOk && dropMissing(cs, st);
        if (lost) resume::save(path, st);
        PendingInstall p;
        p.wiped = had && lost && st.completed() == 0;
        p.path = path;
        p.url = st.url;
        p.name = st.name;
        p.percent = st.percent();
        p.total = st.total;
        p.kept = st.completed();
        p.started = st.started;
        out.push_back(p);
    }
    if (csOk) ncmContentStorageClose(&cs);
    return out;
}

uint64_t discardPendingUrl(const std::string& url) {
    std::string path = resume::pathFor(kResumeDir, url);
    resume::State st;
    if (!resume::load(path, st)) return 0;
    return discardPending(path);
}

uint64_t discardPending(const std::string& path) {
    resume::State st;
    if (!resume::load(path, st)) { resume::remove(path); return 0; }
    uint64_t freed = 0;
    Lock reg(g_regLock);
    NcmContentStorage cs;
    if (R_SUCCEEDED(ncmOpenContentStorage(&cs, NcmStorageId_SdCard))) {
        for (const auto& pt : st.parts) {
            NcmPlaceHolderId ph;
            if (fmt::hexToBytes(pt.second.placeholder, ph.uuid.uuid, 0x10)) {
                s64 sz = 0;  // the placeholder takes its full size on the SD card from the start
                if (R_FAILED(ncmContentStorageGetSizeFromPlaceHolderId(&cs, &sz, &ph))) sz = 0;
                if (R_SUCCEEDED(ncmContentStorageDeletePlaceHolder(&cs, &ph))) freed += (uint64_t)std::max<s64>(0, sz);
            }
        }
        // Contents finished by the attempt but never tied to a game: delete them, unless an
        // installed game uses them (a reinstall over an installed copy).
        std::set<std::string> used;
        u64 app = titles::parse(st.name).baseId;
        NcmContentMetaDatabase db;
        if (app && R_SUCCEEDED(ncmOpenContentMetaDatabase(&db, NcmStorageId_SdCard))) {
            NcmContentMetaKey keys[256];
            s32 total = 0, written = 0;
            if (R_SUCCEEDED(ncmContentMetaDatabaseList(&db, &total, &written, keys, 256, NcmContentMetaType_Unknown, 0,
                                                       app, app + 0x1FFF, NcmContentInstallType_Full))) {
                for (s32 k = 0; k < written; k++) {
                    NcmContentInfo infos[64];
                    s32 n = 0;
                    if (R_SUCCEEDED(ncmContentMetaDatabaseListContentInfo(&db, &n, infos, 64, &keys[k], 0)))
                        for (s32 j = 0; j < n; j++) used.insert(fmt::bytesToHex(infos[j].content_id.c, 0x10));
                }
            }
            ncmContentMetaDatabaseClose(&db);
        }
        for (const auto& d : st.done) {
            if (used.count(d.first) || !app) continue;
            NcmContentId id;
            if (fmt::hexToBytes(d.first, id.c, 0x10) && R_SUCCEEDED(ncmContentStorageDelete(&cs, &id))) freed += d.second;
        }
        ncmContentStorageClose(&cs);
    }
    log("discarded unfinished install: %s (%llu bytes freed)", st.name.c_str(), (unsigned long long)freed);
    resume::remove(path);
    return freed;
}

}  // namespace install
