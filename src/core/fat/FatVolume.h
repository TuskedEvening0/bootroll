#pragma once
// Read-only FAT12/FAT16/FAT32 volume reader (BPB parse, FAT chains, directory
// entries with long-name support, case-insensitive path lookup). Windows does
// not mount ESPs, so the ESP file browser walks the raw partition instead.
// Pure logic: the caller supplies sectors via the FatVolumeReader callback -
// no OS calls here (mirrors the core/uefi seam style).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bootroll {

// Reader contract: copy `bytes` bytes starting at volume-relative `byteOffset`
// into *out. Both are always multiples of the volume's bytes-per-sector.
// Returns false on an IO failure (error string comes from the caller's cause).
using FatVolumeReader =
    std::function<bool(uint64_t byteOffset, uint32_t bytes, uint8_t* out)>;

struct FatDirEntry {
    std::string name;       // UTF-8 long name (8.3 fallback), no path prefix
    bool isDir = false;
    bool isVolumeLabel = false;
    uint64_t sizeBytes = 0; // 0 for directories
    int64_t mtime = 0;      // last write time as unix time (0 = unset)
};

class FatVolume {
public:
    // Parse the BPB and load the FAT. Returns false on an unusable or
    // unsupported volume and fills *error.
    bool open(FatVolumeReader reader, std::string* error);

    bool isOpen() const { return m_isOpen; }
    const std::string& fatType() const { return m_fatType; } // FAT12/FAT16/FAT32
    uint32_t bytesPerSector() const { return m_bytesPerSector; }
    uint64_t totalBytes() const;

    // List a directory. path uses '\' or '/' separators; "" or "\" is the root.
    // Volume-label entries are included with isVolumeLabel set.
    bool listDir(const std::string& path, std::vector<FatDirEntry>* out,
                 std::string* error);

    // Case-insensitive lookup of a file/directory (long name or 8.3 alias).
    bool findEntry(const std::string& path, FatDirEntry* out, std::string* error);

private:
    struct DirSource {
        bool isRootFixed = false; // FAT12/16 root: fixed sector region
        uint32_t rootSector = 0;  // first sector when isRootFixed
        uint32_t rootSectors = 0;
        uint32_t firstCluster = 0; // when !isRootFixed
    };

    bool readSectors(uint64_t firstSector, uint32_t count, uint8_t* out,
                     std::string* error);
    bool readCluster(uint32_t cluster, std::vector<uint8_t>* out, std::string* error);
    uint32_t fatEntry(uint32_t cluster) const; // raw entry value
    bool isEoc(uint32_t v) const;
    bool isBad(uint32_t v) const;
    uint32_t nextCluster(uint32_t cluster, bool* end, std::string* error);

    // Walk all directory data of a DirSource (chain or fixed region) and call
    // fn per 32-byte slot; fn returning false stops the iteration.
    bool forEachDirSlot(const DirSource& src, std::string* error,
                        const std::function<bool(const uint8_t*)>& fn);
    // Locate a subdirectory of `dir` by (case-insensitive) name.
    bool findInDir(const DirSource& dir, const std::string& name,
                   FatDirEntry* out, uint32_t* firstCluster, std::string* error);
    // Directory data source for a path ("" or "\" = root).
    bool dirSourceFor(const std::string& path, DirSource* out, std::string* error);

    FatVolumeReader m_reader;
    bool m_isOpen = false;
    std::string m_fatType;
    uint32_t m_bytesPerSector = 512;
    uint32_t m_sectorsPerCluster = 1;
    uint32_t m_reservedSectors = 0;
    uint32_t m_fatCount = 0;
    uint32_t m_fatSizeSectors = 0;  // one FAT, in sectors
    uint32_t m_rootDirSectors = 0;  // FAT12/16 only
    uint32_t m_rootCluster = 0;     // FAT32 only
    uint32_t m_firstDataSector = 0;
    uint32_t m_totalClusters = 0;   // count of data clusters
    uint64_t m_totalSectors = 0;
    std::vector<uint8_t> m_fat;     // active FAT copy
};

} // namespace bootroll
