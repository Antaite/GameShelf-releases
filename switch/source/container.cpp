#include "container.hpp"

#include <algorithm>
#include <cstring>

#include "crypto.hpp"

namespace container {

static uint64_t rd64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static bool readHfs0(const ReadFn& read, uint64_t at, std::vector<fmt::Pfs0Entry>& out, std::string& err) {
    std::vector<uint8_t> h;
    if (!read(at, 0x10, h, err)) return false;
    uint64_t size = fmt::hfs0HeaderSize(h.data());
    if (!size) { err = "XCI 中的 HFS0 分割區損壞"; return false; }
    if (!read(at, size, h, err)) return false;
    return fmt::parseHfs0(h, out, err, at);
}

static bool openXci(const ReadFn& read, uint64_t start, std::vector<fmt::Pfs0Entry>& entries, std::string& err) {
    std::vector<uint8_t> h;
    if (!read(start + 0x100, 0x40, h, err)) return false;
    if (memcmp(h.data(), "HEAD", 4) != 0) { err = "不是有效的 NSP / XCI 檔案"; return false; }
    uint64_t rootOffset = rd64(&h[0x30]);  // XCI header +0x130

    std::vector<fmt::Pfs0Entry> root;
    if (!readHfs0(read, start + rootOffset, root, err)) return false;
    for (const auto& p : root) {
        if (p.name != "secure") continue;
        return readHfs0(read, p.offset, entries, err);
    }
    err = "XCI 中找不到 secure 分割區";
    return false;
}

bool open(const ReadFn& read, Kind& kind, std::vector<fmt::Pfs0Entry>& entries, std::string& err) {
    std::vector<uint8_t> h;
    if (!read(0, 0x10, h, err)) return false;
    if (memcmp(h.data(), "PFS0", 4) == 0) {
        kind = Kind::Pfs0;
        uint64_t size = fmt::pfs0HeaderSize(h.data());
        if (!size) { err = "PFS0 標頭損壞"; return false; }
        if (!read(0, size, h, err)) return false;
        return fmt::parsePfs0(h, entries, err, 0);
    }
    kind = Kind::Xci;
    std::string e1;
    if (openXci(read, 0, entries, e1)) return true;
    // some dumps carry an extra 0x1000-byte key area in front of the header
    if (openXci(read, 0x1000, entries, err)) return true;
    err = e1;
    return false;
}

static bool endsWith(const std::string& s, const char* e) {
    size_t n = strlen(e);
    return s.size() >= n && s.compare(s.size() - n, n, e) == 0;
}

bool installSize(const ReadFn& read, uint64_t& total, std::string& err) {
    total = 0;
    Kind kind;
    std::vector<fmt::Pfs0Entry> files;
    if (!open(read, kind, files, err)) return false;
    for (const auto& f : files) {
        if (endsWith(f.name, ".nca")) { total += f.size; continue; }
        if (!endsWith(f.name, ".ncz")) continue;
        // .ncz: the first 0x4000 bytes of the NCA as they are, then "NCZSECTN" + section count +
        // 0x40-byte sections (offset, size, ...) that cover the NCA: its size is the last section end.
        std::vector<uint8_t> h;
        if (f.size < 0x4010 || !read(f.offset + 0x4000, 0x10, h, err)) return false;
        if (h.size() < 0x10 || memcmp(h.data(), "NCZSECTN", 8) != 0) { err = f.name + "：不是有效的 NCZ 檔案"; return false; }
        uint64_t count = rd64(&h[8]);
        if (count == 0 || count > (1u << 20)) { err = f.name + "：NCZ 區段數量錯誤"; return false; }
        std::vector<uint8_t> sec;
        if (!read(f.offset + 0x4010, count * 0x40, sec, err) || sec.size() < count * 0x40) return false;
        uint64_t end = 0x4000;
        for (uint64_t i = 0; i < count; i++) {
            uint64_t o = rd64(&sec[i * 0x40]), z = rd64(&sec[i * 0x40 + 8]);
            if (o + z > end) end = o + z;
        }
        total += end;
    }
    return true;
}

bool realTitleIds(const ReadFn& read, const uint8_t headerKey[0x20], std::vector<uint64_t>& ids, std::string& err) {
    ids.clear();
    Kind kind;
    std::vector<fmt::Pfs0Entry> files;
    if (!open(read, kind, files, err)) return false;
    bool sawMeta = false;
    for (const auto& f : files) {
        if (!endsWith(f.name, ".cnmt.nca") && !endsWith(f.name, ".cnmt.ncz")) continue;
        sawMeta = true;
        std::vector<uint8_t> h;
        if (f.size < 0x400 || !read(f.offset, 0x400, h, err) || h.size() < 0x400) return false;
        crypto::xtsNintendo(headerKey, h.data(), 0x400, 0, false);
        if (memcmp(&h[0x200], "NCA3", 4) != 0 && memcmp(&h[0x200], "NCA2", 4) != 0) {
            err = "內容描述 NCA 標頭無法解開（header_key 不對？）";
            return false;
        }
        uint64_t tid = rd64(&h[0x210]);
        if (tid) ids.push_back(tid);
    }
    if (!sawMeta) { err = "封裝檔中找不到 .cnmt.nca"; return false; }
    return !ids.empty();
}

static uint32_t rd32le(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

// One content meta NCA, fully in memory: decrypt its header, key area and first section (a PFS0
// holding the .cnmt), then parse the cnmt.
static bool decodeMetaNca(std::vector<uint8_t>& nca, const uint8_t headerKey[0x20], const KeyAreaKeyFn& kaek, Meta& m,
                          std::string& err) {
    if (nca.size() < 0xC00) { err = "內容描述 NCA 太小"; return false; }
    crypto::xtsNintendo(headerKey, nca.data(), 0xC00, 0, false);
    const uint8_t* h = nca.data();
    if (memcmp(h + 0x200, "NCA3", 4) != 0 && memcmp(h + 0x200, "NCA2", 4) != 0) {
        err = "內容描述 NCA 標頭無法解開（header_key 不對？）";
        return false;
    }
    m.titleId = rd64(h + 0x210);
    m.type = 0;
    m.versionKnown = false;
    int gen = std::max(h[0x206], h[0x220]);
    int idx = gen > 0 ? gen - 1 : 0;
    uint8_t kak[0x10], keys[0x40];
    if (h[0x207] != 0 || !kaek || !kaek(idx, kak)) return true;  // title id only
    memcpy(keys, h + 0x300, 0x40);
    crypto::aesEcbDecrypt(kak, keys, sizeof keys);
    const uint8_t* fs = h + 0x400;  // section 0
    uint64_t start = (uint64_t)rd32le(h + 0x240) * 0x200, end = (uint64_t)rd32le(h + 0x244) * 0x200;
    if (fs[0x2] != 1 || end <= start || end > nca.size()) return true;  // not a PFS0 section
    uint64_t pfsOff = rd64(fs + 0x40), pfsSize = rd64(fs + 0x48);
    if (pfsOff + pfsSize > end - start || pfsSize < 0x10) return true;
    uint64_t a = start + pfsOff;
    std::vector<uint8_t> pfs(nca.begin() + (long)a, nca.begin() + (long)(a + pfsSize));
    if (fs[0x4] == 3 || fs[0x4] == 4) {  // AES-CTR
        uint8_t ctr[0x10];
        for (int j = 0; j < 8; j++) ctr[j] = fs[0x140 + 7 - j];
        uint64_t blk = a >> 4;
        for (int j = 0; j < 8; j++) ctr[15 - j] = (uint8_t)(blk >> (8 * j));
        crypto::AesCtr aes(keys + 0x20);
        aes.crypt(pfs.data(), pfs.size(), ctr, (size_t)(a & 15));
    }
    std::vector<fmt::Pfs0Entry> files;
    std::string e;
    if (memcmp(pfs.data(), "PFS0", 4) != 0 || !fmt::parsePfs0(pfs, files, e, 0)) return true;
    for (const auto& f : files) {
        if (f.name.size() < 5 || f.name.compare(f.name.size() - 5, 5, ".cnmt") != 0) continue;
        if (f.offset + f.size > pfs.size()) break;
        std::vector<uint8_t> raw(pfs.begin() + (long)f.offset, pfs.begin() + (long)(f.offset + f.size));
        fmt::ContentMeta cm;
        if (!fmt::parseCnmt(raw, cm, e)) break;
        m.titleId = cm.titleId;
        m.version = cm.version;
        m.type = cm.type;
        m.versionKnown = true;
        m.size = 0;
        for (const auto& ct : cm.contents)
            if (ct.type != fmt::CT_DeltaFragment) m.size += ct.size;
        break;
    }
    return true;
}

bool readMetas(const ReadFn& read, const uint8_t headerKey[0x20], const KeyAreaKeyFn& kaek, bool deep, int& metaCount,
               std::vector<Meta>& metas, std::string& err) {
    metaCount = 0;
    metas.clear();
    Kind kind;
    std::vector<fmt::Pfs0Entry> files;
    if (!open(read, kind, files, err)) return false;
    for (const auto& f : files) {
        if (!endsWith(f.name, ".cnmt.nca") && !endsWith(f.name, ".cnmt.ncz")) continue;
        metaCount++;
        if (!deep) continue;
        if (f.size > (4u << 20)) { err = f.name + "：內容描述檔過大"; return false; }
        std::vector<uint8_t> nca;
        if (!read(f.offset, f.size, nca, err) || nca.size() < f.size) return false;
        Meta m;
        if (!decodeMetaNca(nca, headerKey, kaek, m, err)) return false;
        if (!m.type) {  // no key area key: the kind follows from the title id
            uint64_t low = m.titleId & 0xFFF;
            m.type = low == 0 ? 0x80 : low == 0x800 ? 0x81 : 0x82;
        }
        if (m.size) m.size += f.size;  // the meta NCA itself is installed too
        metas.push_back(m);
    }
    if (!metaCount) { err = "封裝檔中找不到 .cnmt.nca"; return false; }
    return true;
}

}  // namespace container
