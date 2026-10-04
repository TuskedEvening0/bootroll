#include "core/fat/FatVolume.h"
#include "core/bcd/Utf16.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

namespace bootroll {

namespace {

uint16_t rd16(const uint8_t* p)
{
    return uint16_t(p[0] | (p[1] << 8));
}

uint32_t rd32(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

bool isPow2(uint32_t v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

char lowerCh(char c)
{
    return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

// ASCII case-insensitive byte compare (FAT name matching semantics).
bool nameEquals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (lowerCh(a[i]) != lowerCh(b[i])) {
            return false;
        }
    }
    return true;
}

// LFN checksum over the 11-byte 8.3 name.
uint8_t lfnChecksumBytes(const uint8_t* shortName)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; ++i) {
        sum = uint8_t(((sum >> 1) | ((sum & 1) << 7)) + shortName[i]);
    }
    return sum;
}

// Decode an 8.3 short name (11 raw bytes) honoring the lowercase NT flags.
std::string decodeShortName(const uint8_t* b)
{
    std::string name;
    const uint8_t* rawName = b;
    if (rawName[0] == 0x05) { // 0xE5 escaped in first byte
        name += '\xE5';
        rawName++;
    }
    int nameLen = 8 - int(rawName - b);
    for (int i = 0; i < nameLen && rawName[i] != ' '; ++i) {
        name += char(rawName[i]);
    }
    std::string ext;
    for (int i = 8; i < 11 && b[i] != ' '; ++i) {
        ext += char(b[i]);
    }
    if (!ext.empty()) {
        name += '.';
        name += ext;
    }
    const uint8_t nt = b[12];
    if (nt & 0x08) { // base lowercase
        for (size_t i = 0; i < name.size(); ++i) {
            if (name[i] == '.') {
                break;
            }
            name[i] = lowerCh(name[i]);
        }
    }
    if (nt & 0x10) { // extension lowercase
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos) {
            for (size_t i = dot + 1; i < name.size(); ++i) {
                name[i] = lowerCh(name[i]);
            }
        }
    }
    return name;
}

// One LFN slot contributes 13 UTF-16 code units.
std::string decodeLfnSlot(const uint8_t* b)
{
    uint16_t units[13];
    units[0] = rd16(b + 1);
    units[1] = rd16(b + 3);
    units[2] = rd16(b + 5);
    units[3] = rd16(b + 7);
    units[4] = rd16(b + 9);
    for (int i = 0; i < 6; ++i) {
        units[5 + i] = rd16(b + 14 + i * 2);
    }
    units[11] = rd16(b + 28);
    units[12] = rd16(b + 30);
    int count = 13;
    for (int i = 0; i < 13; ++i) {
        if (units[i] == 0x0000) {
            count = i;
            break; // NUL terminates the name inside this slot
        }
    }
    // U+FFFF padding only appears after a NUL; if no NUL, keep all 13.
    return utf16leToUtf8(reinterpret_cast<const uint8_t*>(units),
                         size_t(count) * 2);
}

// FAT date/time (local) -> unix time. date: yyyymm mmmddddd bits.
int64_t fatTimeToUnix(uint16_t date, uint16_t time)
{
    if (date == 0) {
        return 0;
    }
    const int64_t year = 1980 + (date >> 9);
    const int64_t month = (date >> 5) & 0xF;
    const int64_t day = date & 0x1F;
    if (month < 1 || month > 12 || day < 1 || day > 31) {
        return 0;
    }
    // Days from civil (Howard Hinnant's algorithm).
    int64_t y = year - (month <= 2 ? 1 : 0);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + doe - 719468;
    return days * 86400 + (time >> 11) * 3600 + ((time >> 5) & 0x3F) * 60 +
           (time & 0x1F) * 2;
}

} // namespace

bool FatVolume::open(FatVolumeReader reader, std::string* error)
{
    m_isOpen = false;
    m_reader = std::move(reader);

    uint8_t bpb[512] = {};
    if (!m_reader || !m_reader(0, 512, bpb)) {
        *error = "Cannot read the boot sector of the volume.";
        return false;
    }

    m_bytesPerSector = rd16(bpb + 11);
    m_sectorsPerCluster = bpb[13];
    m_reservedSectors = rd16(bpb + 14);
    m_fatCount = bpb[16];
    const uint16_t rootEntries = rd16(bpb + 17);
    const uint16_t total16 = rd16(bpb + 19);
    const uint16_t fatSize16 = rd16(bpb + 22);
    const uint32_t total32 = rd32(bpb + 32);
    const uint32_t fatSize32 = rd32(bpb + 36);
    m_rootCluster = rd32(bpb + 44);
    const uint16_t extFlags = rd16(bpb + 40);

    // Sanity: accept only the sector sizes FAT supports and self-consistent BPBs.
    const bool bpsOk = m_bytesPerSector >= 512 && m_bytesPerSector <= 4096 &&
                       isPow2(m_bytesPerSector);
    const bool spcOk = isPow2(m_sectorsPerCluster) && m_sectorsPerCluster <= 128;
    if (!bpsOk || !spcOk || m_reservedSectors == 0 || m_fatCount == 0 ||
        m_fatCount > 8) {
        *error = "Not a FAT volume (invalid BPB).";
        return false;
    }
    m_totalSectors = total16 != 0 ? total16 : total32;
    m_fatSizeSectors = fatSize16 != 0 ? fatSize16 : fatSize32;
    if (m_totalSectors == 0 || m_fatSizeSectors == 0) {
        *error = "Not a FAT volume (invalid sizes).";
        return false;
    }
    if (bpb[510] != 0x55 || bpb[511] != 0xAA) {
        *error = "Not a FAT volume (missing 0x55AA signature).";
        return false;
    }

    m_rootDirSectors = uint32_t((uint64_t(rootEntries) * 32 + m_bytesPerSector - 1) /
                                m_bytesPerSector);
    const uint64_t dataSectors =
        m_totalSectors - (m_reservedSectors + uint64_t(m_fatCount) * m_fatSizeSectors +
                          m_rootDirSectors);
    m_totalClusters = uint32_t(dataSectors / m_sectorsPerCluster);
    m_firstDataSector =
        m_reservedSectors + m_fatCount * m_fatSizeSectors + m_rootDirSectors;

    // FAT32 is identified by the BPB fields that only it uses (rootEntries and
    // fatSize16 must both be 0) - real ESPs are often smaller than the
    // spec-recommended 65525 clusters, so the cluster count alone misjudges.
    if (rootEntries == 0 && fatSize16 == 0) {
        m_fatType = "FAT32";
        if (m_rootCluster < 2) {
            *error = "Not a FAT volume (invalid root cluster).";
            return false;
        }
    } else {
        m_fatType = m_totalClusters <= 4085 ? "FAT12" : "FAT16";
        if (m_totalClusters < 2) {
            *error = "Not a FAT volume (too small).";
            return false;
        }
    }

    // Keep a copy of the active FAT: extFlags bit 7 selects it when mirroring
    // is off, otherwise FAT #0 (kept consistent on all firmware formats).
    const uint32_t activeFat = (m_fatType == "FAT32" && (extFlags & 0x80))
                                   ? (extFlags & 0xF) % m_fatCount
                                   : 0;
    m_fat.resize(size_t(m_fatSizeSectors) * m_bytesPerSector);
    for (uint32_t s = 0; s < m_fatSizeSectors; ++s) {
        if (!m_reader(uint64_t(m_reservedSectors + activeFat * m_fatSizeSectors + s) *
                          m_bytesPerSector,
                      m_bytesPerSector, m_fat.data() + size_t(s) * m_bytesPerSector)) {
            *error = "Cannot read the file allocation table.";
            return false;
        }
    }

    m_isOpen = true;
    return true;
}

uint64_t FatVolume::totalBytes() const
{
    return m_totalSectors * m_bytesPerSector;
}

uint32_t FatVolume::fatEntry(uint32_t cluster) const
{
    if (m_fatType == "FAT12") {
        const size_t off = size_t(cluster + cluster / 2);
        if (off + 1 >= m_fat.size()) {
            return 0xFFF; // treat out-of-range as end-of-chain
        }
        const uint16_t v = rd16(m_fat.data() + off);
        return (cluster & 1) ? (v >> 4) : (v & 0xFFF);
    }
    if (m_fatType == "FAT16") {
        const size_t off = size_t(cluster) * 2;
        if (off + 1 >= m_fat.size()) {
            return 0xFFFF;
        }
        return rd16(m_fat.data() + off);
    }
    const size_t off = size_t(cluster) * 4;
    if (off + 3 >= m_fat.size()) {
        return 0x0FFFFFFF;
    }
    return rd32(m_fat.data() + off) & 0x0FFFFFFF;
}

bool FatVolume::isEoc(uint32_t v) const
{
    if (m_fatType == "FAT12") {
        return v >= 0xFF8;
    }
    if (m_fatType == "FAT16") {
        return v >= 0xFFF8;
    }
    return v >= 0x0FFFFFF8;
}

bool FatVolume::isBad(uint32_t v) const
{
    if (m_fatType == "FAT12") {
        return v == 0xFF7;
    }
    if (m_fatType == "FAT16") {
        return v == 0xFFF7;
    }
    return v == 0x0FFFFFF7;
}

bool FatVolume::readSectors(uint64_t firstSector, uint32_t count, uint8_t* out,
                            std::string* error)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (!m_reader((firstSector + i) * m_bytesPerSector, m_bytesPerSector,
                      out + size_t(i) * m_bytesPerSector)) {
            *error = "Cannot read volume sectors.";
            return false;
        }
    }
    return true;
}

bool FatVolume::readCluster(uint32_t cluster, std::vector<uint8_t>* out,
                            std::string* error)
{
    out->resize(size_t(m_sectorsPerCluster) * m_bytesPerSector);
    const uint64_t first = m_firstDataSector + uint64_t(cluster - 2) * m_sectorsPerCluster;
    return readSectors(first, m_sectorsPerCluster, out->data(), error);
}

uint32_t FatVolume::nextCluster(uint32_t cluster, bool* end, std::string* error)
{
    *end = false;
    if (cluster < 2 || cluster >= 2 + m_totalClusters) {
        *end = true; // out of range: stop (defensive)
        return 0;
    }
    const uint32_t v = fatEntry(cluster);
    if (isBad(v)) {
        *error = "Bad cluster in the file system.";
        *end = true;
        return 0;
    }
    if (isEoc(v)) {
        *end = true;
        return 0;
    }
    return v;
}

bool FatVolume::forEachDirSlot(const DirSource& src, std::string* error,
                               const std::function<bool(const uint8_t*)>& fn)
{
    auto handleBuffer = [&](const uint8_t* buf, size_t bytes) {
        for (size_t off = 0; off + 32 <= bytes; off += 32) {
            if (!fn(buf + off)) {
                return false; // stop requested
            }
        }
        return true;
    };

    if (src.isRootFixed) {
        std::vector<uint8_t> buf(size_t(src.rootSectors) * m_bytesPerSector);
        if (!readSectors(src.rootSector, src.rootSectors, buf.data(), error)) {
            return false;
        }
        handleBuffer(buf.data(), buf.size()); // a stop by fn is not an error
        return true;
    }

    std::vector<uint8_t> buf;
    uint32_t cluster = src.firstCluster;
    if (cluster < 2) {
        *error = "Empty directory chain.";
        return false;
    }
    for (uint32_t steps = 0; steps <= m_totalClusters; ++steps) {
        if (!readCluster(cluster, &buf, error)) {
            return false;
        }
        if (!handleBuffer(buf.data(), buf.size())) {
            return true; // stop requested by fn, not an error
        }
        bool end = false;
        cluster = nextCluster(cluster, &end, error);
        if (!error->empty()) {
            return false;
        }
        if (end) {
            return true;
        }
    }
    *error = "Cluster chain loop detected.";
    return false;
}

bool FatVolume::findInDir(const DirSource& dir, const std::string& name,
                          FatDirEntry* out, uint32_t* firstCluster, std::string* error)
{
    // Streaming LFN assembly state.
    bool lfnActive = false;
    uint8_t lfnChecksum = 0;
    int lfnRemaining = 0; // slots still expected (seq counts down)
    std::map<int, std::string> lfnParts; // seq -> fragment

    FatDirEntry current;
    bool found = false;

    const bool ok = forEachDirSlot(dir, error, [&](const uint8_t* b) {
        if (b[0] == 0x00) {
            return false; // end of directory
        }
        if (b[0] == 0xE5) { // deleted
            lfnActive = false;
            return true;
        }
        const uint8_t attr = b[11];
        if (attr == 0x0F) { // long-name slot
            const int seq = b[0] & 0x3F;
            if (b[0] & 0x40) { // last (highest) part: starts a new record
                lfnActive = true;
                lfnChecksum = b[13];
                lfnParts.clear();
                lfnRemaining = seq;
            }
            if (!lfnActive || seq != lfnRemaining || lfnRemaining <= 0) {
                lfnActive = false; // inconsistent chain: ignore
                return true;
            }
            lfnParts[seq] = decodeLfnSlot(b);
            lfnRemaining--;
            return true;
        }

        // Short entry.
        const std::string shortName = decodeShortName(b);
        std::string longName;
        if (lfnActive && lfnRemaining == 0 && lfnChecksum == lfnChecksumBytes(b)) {
            for (int s = 1; s <= int(lfnParts.size()); ++s) {
                longName += lfnParts[s];
            }
        }
        lfnActive = false;

        current = FatDirEntry{};
        current.isDir = (attr & 0x10) != 0;
        current.isVolumeLabel = (attr & 0x08) != 0 && !current.isDir;
        current.sizeBytes = rd32(b + 28);
        current.mtime = fatTimeToUnix(rd16(b + 24), rd16(b + 22));
        current.name = !longName.empty() ? longName : shortName;

        const bool match = nameEquals(shortName, name) ||
                           (!longName.empty() && nameEquals(longName, name));
        if (match) {
            if (out != nullptr) {
                *out = current;
            }
            if (firstCluster != nullptr) {
                *firstCluster = rd16(b + 26) | (uint32_t(rd16(b + 20)) << 16);
            }
            found = true;
            return false;
        }
        return true;
    });
    if (!ok) {
        return false;
    }
    if (!found && error != nullptr) {
        *error = "Path not found: " + name;
    }
    return found;
}

bool FatVolume::listDir(const std::string& path, std::vector<FatDirEntry>* out,
                        std::string* error)
{
    out->clear();
    if (!m_isOpen) {
        *error = "Volume is not open.";
        return false;
    }

    // Resolve to the directory's data source.
    DirSource src;
    if (!dirSourceFor(path, &src, error)) {
        return false;
    }

    // Enumerate all slots into entries.
    std::vector<FatDirEntry> result;
    bool lfnActive = false;
    uint8_t lfnChecksum = 0;
    int lfnRemaining = 0;
    std::map<int, std::string> lfnParts;

    const bool ok = forEachDirSlot(src, error, [&](const uint8_t* b) {
        if (b[0] == 0x00) {
            return false;
        }
        if (b[0] == 0xE5) {
            lfnActive = false;
            return true;
        }
        const uint8_t attr = b[11];
        if (attr == 0x0F) {
            const int seq = b[0] & 0x3F;
            if (b[0] & 0x40) {
                lfnActive = true;
                lfnChecksum = b[13];
                lfnParts.clear();
                lfnRemaining = seq;
            }
            if (!lfnActive || seq != lfnRemaining || lfnRemaining <= 0) {
                lfnActive = false;
                return true;
            }
            lfnParts[seq] = decodeLfnSlot(b);
            lfnRemaining--;
            return true;
        }

        const std::string shortName = decodeShortName(b);
        std::string longName;
        if (lfnActive && lfnRemaining == 0 && lfnChecksum == lfnChecksumBytes(b)) {
            for (int s = 1; s <= int(lfnParts.size()); ++s) {
                longName += lfnParts[s];
            }
        }
        lfnActive = false;

        // "." and ".." are on-disk navigation artifacts, never real files:
        // keep them out of listings (the UI navigates up explicitly).
        if (shortName == "." || shortName == "..") {
            return true;
        }

        FatDirEntry e;
        e.isDir = (attr & 0x10) != 0;
        e.isVolumeLabel = (attr & 0x08) != 0 && !e.isDir;
        e.sizeBytes = rd32(b + 28);
        e.mtime = fatTimeToUnix(rd16(b + 24), rd16(b + 22));
        e.name = !longName.empty() ? longName : shortName;
        result.push_back(std::move(e));
        return true;
    });
    if (!ok) {
        return false;
    }
    *out = std::move(result);
    return true;
}

bool FatVolume::dirSourceFor(const std::string& path, DirSource* out,
                             std::string* error)
{
    // Root: fixed region on FAT12/16, cluster chain on FAT32.
    if (path.empty() || path == "\\" || path == "/") {
        out->isRootFixed = m_rootDirSectors > 0;
        out->rootSector = m_reservedSectors + m_fatCount * m_fatSizeSectors;
        out->rootSectors = m_rootDirSectors;
        out->firstCluster = m_rootCluster;
        return true;
    }

    std::vector<std::string> parts;
    std::string cur;
    for (const char c : path) {
        if (c == '\\' || c == '/') {
            if (!cur.empty()) {
                parts.push_back(cur);
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        parts.push_back(cur);
    }
    if (parts.empty()) {
        *error = "Empty path.";
        return false;
    }

    // Canonicalize "." / ".." against the path before walking. The on-disk
    // dot entries may legally carry cluster 0 when the parent is the root,
    // and resolving one would fail with an empty directory chain; the path
    // itself is the only reliable source for the parent.
    std::vector<std::string> canon;
    for (const std::string& part : parts) {
        if (part == ".") {
            continue;
        }
        if (part == "..") {
            if (!canon.empty()) {
                canon.pop_back();
            }
            continue; // ".." above the root clamps to the root
        }
        canon.push_back(part);
    }
    if (canon.empty()) {
        return dirSourceFor(std::string(), out, error);
    }

    DirSource src;
    dirSourceFor(std::string(), &src, error); // start at the root
    FatDirEntry e;
    uint32_t cluster = 0;
    for (size_t i = 0; i < canon.size(); ++i) {
        if (!findInDir(src, canon[i], &e, &cluster, error)) {
            return false;
        }
        if (i + 1 < canon.size()) {
            if (!e.isDir) {
                *error = "Not a directory.";
                return false;
            }
            src = DirSource{};
            src.firstCluster = cluster;
        }
    }
    if (!e.isDir) {
        *error = "Not a directory.";
        return false;
    }
    out->isRootFixed = false;
    out->rootSector = 0;
    out->rootSectors = 0;
    out->firstCluster = cluster;
    return true;
}

bool FatVolume::findEntry(const std::string& path, FatDirEntry* out, std::string* error)
{
    if (!m_isOpen) {
        *error = "Volume is not open.";
        return false;
    }
    // Parent directory source + last component.
    std::vector<std::string> parts;
    std::string cur;
    for (const char c : path) {
        if (c == '\\' || c == '/') {
            if (!cur.empty()) {
                parts.push_back(cur);
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        parts.push_back(cur);
    }
    if (parts.empty()) {
        *error = "Empty path.";
        return false;
    }
    std::string parent;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        parent += "\\" + parts[i];
    }
    DirSource src;
    if (!dirSourceFor(parent, &src, error)) {
        return false;
    }
    return findInDir(src, parts.back(), out, nullptr, error);
}

} // namespace bootroll
