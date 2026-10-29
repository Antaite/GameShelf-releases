#pragma once
// Opens NSP / NSZ (PFS0) and XCI / XCZ (gamecard image -> HFS0 "secure" partition) through a
// random-access read callback, returning the files inside with absolute offsets.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "formats.hpp"

namespace container {

using ReadFn = std::function<bool(uint64_t offset, uint64_t size, std::vector<uint8_t>& out, std::string& err)>;

enum class Kind { Pfs0, Xci };

bool open(const ReadFn& read, Kind& kind, std::vector<fmt::Pfs0Entry>& entries, std::string& err);

// Space the package takes once installed: the NCA sizes (an .ncz counts as the NCA it unpacks to,
// read from its section table). Tickets / certificates / xml are not counted.
bool installSize(const ReadFn& read, uint64_t& total, std::string& err);

// The title ids the package really holds (from the header of each content meta NCA, decrypted
// with the user's header key), whatever its file name says. Base game, update and DLC ids as they are.
bool realTitleIds(const ReadFn& read, const uint8_t headerKey[0x20], std::vector<uint64_t>& ids, std::string& err);

// What a package really holds, from its content meta (cnmt) files.
struct Meta {
    uint64_t titleId = 0;
    uint32_t version = 0;
    uint8_t type = 0;           // 0x80 base, 0x81 update, 0x82 DLC
    bool versionKnown = false;  // false: only the title id could be read (no key area key)
    uint64_t size = 0;          // bytes once installed (its contents + the meta NCA), 0 = unknown
};
// Key area key (key_area_key_application_XX) for a key generation index; false when unknown.
using KeyAreaKeyFn = std::function<bool(int index, uint8_t out[0x10])>;
// metaCount: how many content metas the package has (1 read of its directory). With `deep`, each
// one is read and decrypted: title id, version and type. Without, `metas` stays empty.
bool readMetas(const ReadFn& read, const uint8_t headerKey[0x20], const KeyAreaKeyFn& kaek, bool deep,
               int& metaCount, std::vector<Meta>& metas, std::string& err);

}  // namespace container
