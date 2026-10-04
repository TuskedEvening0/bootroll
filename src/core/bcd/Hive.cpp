#include "core/bcd/Hive.h"
#include "core/bcd/Utf16.h"

#include <algorithm>
#include <cwctype>
#include <cstring>

namespace bootroll {

namespace {

// ---------------------------------------------------------------------------
// regf constants & layout
// ---------------------------------------------------------------------------
constexpr uint32_t kBaseBlockSize = 4096;
constexpr uint32_t kBinHeaderSize = 32;   // "hbin" header
constexpr uint32_t kNkHeaderSize = 0x4C;  // named key header (name follows)
constexpr uint32_t kVkHeaderSize = 0x14;  // value key header (name follows)
constexpr int kMaxRecursion = 512;
constexpr uint32_t kMaxNameLen = 0x8000;  // bytes
constexpr uint32_t kMaxValueData = 0x400000; // 4 MiB cap per value
constexpr uint32_t kMaxValueCount = 0x10000;
constexpr uint32_t kMaxSubkeyCount = 0x100000;

constexpr uint16_t kNkFlagHiveEntry = 0x0004;       // root key
constexpr uint16_t kNkFlagNoDelete = 0x0008;
constexpr uint16_t kNkFlagCompressedName = 0x0020;  // single-byte (latin-1) name
constexpr uint32_t kVkDataInline = 0x80000000;      // data lives in offset field
constexpr uint32_t kVkDataBig = 0x40000000;         // "db" indirection (unsupported)

inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
inline uint32_t rd32(const uint8_t* p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint64_t rd64(const uint8_t* p) { return uint64_t(rd32(p)) | uint64_t(rd32(p + 4)) << 32; }
inline void wr16(uint8_t* p, uint16_t v)
{
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
inline void wr32(uint8_t* p, uint32_t v)
{
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}
inline uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

// ---------------------------------------------------------------------------
// Shared UTF conversion lives in core/bcd/Utf16.h. Only hive-specific
// latin-1 ("compressed" registry names) helpers remain here.
// ---------------------------------------------------------------------------
// latin-1 (single byte) -> UTF-8, for "compressed" registry names.
std::string latin1ToUtf8(const uint8_t* p, size_t n)
{
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        uint32_t c = p[i];
        if (c < 0x80) {
            out.push_back(char(c));
        } else {
            out.push_back(char(0xC0 | (c >> 6)));
            out.push_back(char(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

bool utf8ToLatin1(const std::string& s, std::vector<uint8_t>& out)
{
    std::vector<uint16_t> w;
    utf8ToWide(s, w);
    out.clear();
    out.reserve(w.size());
    for (uint16_t c : w) {
        if (c > 0xFF)
            return false;
        out.push_back(uint8_t(c));
    }
    return true;
}

// "lh" subkey list name hash: h = h*37 + towupper(ch) over the wide name.
uint32_t lhHash(const std::string& nameUtf8)
{
    std::vector<uint16_t> w;
    utf8ToWide(nameUtf8, w);
    uint32_t h = 0;
    for (uint16_t c : w)
        h = h * 37 + uint32_t(std::towupper(wint_t(c)));
    return h;
}

bool iNameLess(const std::string& a, const std::string& b)
{
    std::vector<uint16_t> wa, wb;
    utf8ToWide(a, wa);
    utf8ToWide(b, wb);
    size_t n = std::min(wa.size(), wb.size());
    for (size_t i = 0; i < n; ++i) {
        uint16_t la = uint16_t(std::towlower(wint_t(wa[i])));
        uint16_t lb = uint16_t(std::towlower(wint_t(wb[i])));
        if (la != lb)
            return la < lb;
    }
    return wa.size() < wb.size();
}

bool iNameEqual(const std::string& a, const std::string& b)
{
    return !iNameLess(a, b) && !iNameLess(b, a);
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
struct HiveReader {
    const uint8_t* img = nullptr;
    uint32_t imgSize = 0;    // whole image
    uint32_t dataLen = 0;    // bins area length (from file offset 0x1000)
    HiveMeta metaOut;
    HiveKeyPtr rootOut;

    bool run(std::string* error)
    {
        if (imgSize < kBaseBlockSize + kBinHeaderSize + 8)
            return fail(error, "image too small for a hive");

        const uint8_t* base = img;
        if (memcmp(base, "regf", 4) != 0)
            return fail(error, "not a regf hive (missing 'regf')");

        // Checksum: XOR of the first 127 dwords must equal the dword at 0x1FC.
        uint32_t xsum = 0;
        for (int i = 0; i < 127; ++i)
            xsum ^= rd32(base + i * 4);
        if (xsum != rd32(base + 0x1FC))
            return fail(error, "base block checksum mismatch (corrupt hive)");

        metaOut.majorVersion = rd32(base + 0x14);
        metaOut.minorVersion = rd32(base + 0x18);
        metaOut.lastWriteTime = rd64(base + 0x0C);

        metaOut.hiveName = utf16leToUtf8(base + 0x30, 128);

        dataLen = rd32(base + 0x28);
        if (dataLen < kBinHeaderSize || 0x1000 + dataLen > imgSize)
            return fail(error, "hive data length out of range");

        uint32_t rootOff = rd32(base + 0x24);
        if (rootOff == 0 || rootOff == 0xFFFFFFFF || rootOff >= dataLen)
            return fail(error, "bad root cell offset");

        // Structural walk of the bins (cells are addressed flat).
        uint32_t off = 0;
        while (off < dataLen) {
            if (off + kBinHeaderSize > dataLen)
                return fail(error, "truncated bin header");
            const uint8_t* bin = img + 0x1000 + off;
            if (memcmp(bin, "hbin", 4) != 0)
                return fail(error, "missing 'hbin' signature");
            uint32_t binSize = rd32(bin + 8);
            if (binSize < kBinHeaderSize || off + binSize > dataLen)
                return fail(error, "bin size out of range");
            off += binSize;
        }

        rootOut = std::make_unique<HiveKey>();
        if (!readKey(rootOff, rootOut.get(), 0, error))
            return false;
        return true;
    }

    bool fail(std::string* error, const char* msg) const
    {
        if (error)
            *error = msg;
        return false;
    }

    // Cell offsets address from the first hbin (file offset 0x1000) and point
    // at the cell's size field; record data starts at off+4. Returns nullptr
    // when the size field is out of range.
    const uint8_t* cellAt(uint32_t off) const
    {
        if (off + 4 > dataLen)
            return nullptr;
        return img + 0x1000 + off;
    }

    bool readKey(uint32_t cellOff, HiveKey* key, int depth, std::string* error)
    {
        if (depth > kMaxRecursion)
            return fail(error, "key tree too deep");
        const uint8_t* cell = cellAt(cellOff);
        if (!cell)
            return fail(error, "nk cell out of range");
        if (int32_t(rd32(cell)) >= 0)
            return fail(error, "nk cell is free");
        if (cellOff + 4 + kNkHeaderSize > dataLen)
            return fail(error, "truncated nk");
        const uint8_t* nk = cell + 4;
        if (memcmp(nk, "nk", 2) != 0)
            return fail(error, "expected 'nk' signature");

        uint16_t flags = rd16(nk + 0x02);
        uint32_t subkeyCount = rd32(nk + 0x10);
        uint32_t subkeyListOff = rd32(nk + 0x18);
        uint32_t valueCount = rd32(nk + 0x20);
        uint32_t valueListOff = rd32(nk + 0x24);
        uint16_t nameLen = rd16(nk + 0x44); // bytes

        if (nameLen > kMaxNameLen || cellOff + 4 + kNkHeaderSize + nameLen > dataLen)
            return fail(error, "key name out of range");
        const uint8_t* nameP = nk + kNkHeaderSize;
        key->name = (flags & kNkFlagCompressedName)
                        ? latin1ToUtf8(nameP, nameLen)
                        : utf16leToUtf8(nameP, nameLen);

        if (!readValues(key, valueCount, valueListOff, error))
            return false;
        if (!readSubkeys(key, subkeyCount, subkeyListOff, depth, error))
            return false;
        return true;
    }

    bool readValues(HiveKey* key, uint32_t count, uint32_t listOff, std::string* error)
    {
        if (count == 0)
            return true;
        if (listOff == 0 || listOff == 0xFFFFFFFF || count > kMaxValueCount)
            return fail(error, "bad value list");
        const uint8_t* list = cellAt(listOff);
        if (!list)
            return fail(error, "value list out of range");
        if (int32_t(rd32(list)) >= 0)
            return fail(error, "value list cell is free");
        if (listOff + 4 + uint64_t(count) * 4 > dataLen)
            return fail(error, "value list truncated");

        key->values.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t vkOff = rd32(list + 4 + i * 4);
            const uint8_t* vkCell = cellAt(vkOff);
            if (!vkCell || int32_t(rd32(vkCell)) >= 0)
                return fail(error, "bad vk cell");
            if (vkOff + 4 + kVkHeaderSize > dataLen)
                return fail(error, "truncated vk");
            const uint8_t* vk = vkCell + 4;
            if (memcmp(vk, "vk", 2) != 0)
                return fail(error, "expected 'vk' signature");

            uint16_t nameLen = rd16(vk + 0x02); // bytes
            uint32_t dataSize = rd32(vk + 0x04);
            uint32_t dataOff = rd32(vk + 0x08);
            uint32_t type = rd32(vk + 0x0C);
            uint16_t vflags = rd16(vk + 0x10);

            if (nameLen > kMaxNameLen ||
                vkOff + 4 + kVkHeaderSize + uint64_t(nameLen) > dataLen)
                return fail(error, "value name out of range");

            HiveValue val;
            val.type = type;
            if (vflags & 0x0001)
                val.name = latin1ToUtf8(vk + kVkHeaderSize, nameLen);
            else
                val.name = utf16leToUtf8(vk + kVkHeaderSize, nameLen);

            if (dataSize & kVkDataBig) {
                return fail(error, "big-data (db) values are not supported");
            } else if (dataSize & kVkDataInline) {
                uint32_t len = dataSize & ~kVkDataInline;
                if (len > 4)
                    return fail(error, "bad inline value size");
                val.data.resize(len);
                memcpy(val.data.data(), &dataOff, len);
            } else {
                if (dataSize > kMaxValueData)
                    return fail(error, "value data too large");
                if (dataOff >= dataLen || dataOff + 4 + uint64_t(dataSize) > dataLen)
                    return fail(error, "value data out of range");
                const uint8_t* dcell = cellAt(dataOff);
                if (!dcell || int32_t(rd32(dcell)) > 0)
                    return fail(error, "value data cell is free");
                val.data.resize(dataSize);
                memcpy(val.data.data(), dcell + 4, dataSize);
            }
            key->values.push_back(std::move(val));
        }
        return true;
    }

    bool readSubkeys(HiveKey* key, uint32_t count, uint32_t listOff, int depth,
                     std::string* error)
    {
        if (count == 0)
            return true;
        if (count > kMaxSubkeyCount)
            return fail(error, "too many subkeys");
        if (listOff == 0 || listOff == 0xFFFFFFFF)
            return fail(error, "subkey count without list");

        std::vector<uint32_t> childOffs;
        if (!collectSubkeyOffsets(listOff, &childOffs, 0, error))
            return false;

        key->children.reserve(childOffs.size());
        for (uint32_t off : childOffs) {
            auto child = std::make_unique<HiveKey>();
            if (!readKey(off, child.get(), depth + 1, error))
                return false;
            key->children.push_back(std::move(child));
        }
        std::sort(key->children.begin(), key->children.end(),
                  [](const HiveKeyPtr& a, const HiveKeyPtr& b) {
                      return iNameLess(a->name, b->name);
                  });
        return true;
    }

    bool collectSubkeyOffsets(uint32_t listOff, std::vector<uint32_t>* out, int depth,
                              std::string* error)
    {
        if (depth > 8)
            return fail(error, "subkey index nesting too deep");
        const uint8_t* cell = cellAt(listOff);
        if (!cell || int32_t(rd32(cell)) >= 0)
            return fail(error, "subkey list cell is free");
        if (listOff + 4 + 4 > dataLen)
            return fail(error, "subkey list truncated");
        const uint8_t* rec = cell + 4;
        char sig0 = char(rec[0]), sig1 = char(rec[1]);
        uint16_t count = rd16(rec + 2);
        if (count == 0xFFFF)
            return fail(error, "bad subkey list count");

        if (sig0 == 'l' && (sig1 == 'f' || sig1 == 'h')) {
            if (listOff + 4 + 4 + uint64_t(count) * 8 > dataLen)
                return fail(error, "subkey list truncated");
            out->reserve(out->size() + count);
            for (uint32_t i = 0; i < count; ++i)
                out->push_back(rd32(rec + 4 + i * 8 + 4));
        } else if (sig0 == 'l' && sig1 == 'i') {
            if (listOff + 4 + 4 + uint64_t(count) * 4 > dataLen)
                return fail(error, "subkey list truncated");
            out->reserve(out->size() + count);
            for (uint32_t i = 0; i < count; ++i)
                out->push_back(rd32(rec + 4 + i * 4));
        } else if (sig0 == 'r' && sig1 == 'i') {
            if (listOff + 4 + 4 + uint64_t(count) * 4 > dataLen)
                return fail(error, "subkey index truncated");
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t sub = rd32(rec + 4 + i * 4);
                if (!collectSubkeyOffsets(sub, out, depth + 1, error))
                    return false;
            }
        } else {
            return fail(error, "unknown subkey list type");
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// Writer (full rebuild): packed bins, "lh" subkey lists, no free cells except
// the tail padding of each bin.
// ---------------------------------------------------------------------------
class HiveWriter {
public:
    explicit HiveWriter(const HiveMeta& meta)
        : m_meta(meta)
    {
    }

    std::vector<uint8_t> run(const HiveKey* root)
    {
        m_image.assign(kBaseBlockSize, 0);
        startBin(0x1000);
        uint32_t rootOff = emitKey(root, 0, true);
        patch32(0x1000 + rootOff + 4 + 0x0C, rootOff); // root's parent is itself
        finishBins();
        writeBaseBlock(rootOff);
        return std::move(m_image);
    }

private:
    // --- bin management -----------------------------------------------------
    // The image always covers complete bins; m_used tracks allocated bytes
    // inside the current bin so tail padding writes stay in bounds.
    void startBin(uint32_t binSize)
    {
        m_binStart = uint32_t(m_image.size());        // file offset of the bin header
        m_binDataStart = m_binStart + kBinHeaderSize; // file offset of first cell
        m_binSize = binSize;                          // total, header included
        m_used = 0;
        m_image.resize(m_binStart + binSize, 0);
        uint8_t* hdr = m_image.data() + m_binStart;
        memcpy(hdr, "hbin", 4);
        wr32(hdr + 4, m_binStart - 0x1000); // self offset, hive-relative
        wr32(hdr + 8, binSize);
    }

    // Allocate a cell holding `dataLen` payload bytes; returns the hive-relative
    // offset of the cell (pointing at its size field).
    uint32_t allocCell(const void* data, uint32_t dataLen)
    {
        uint32_t total = alignUp(4 + dataLen, 8);
        uint32_t usable = m_binSize - kBinHeaderSize;
        if (m_used + total > usable) {
            uint32_t rem = usable - m_used;
            if (rem >= 8)
                wr32(m_image.data() + m_binDataStart + m_used, rem); // positive = free
            uint32_t newBin = std::max<uint32_t>(0x1000, alignUp(total + kBinHeaderSize, 0x1000));
            startBin(newBin);
        }
        uint32_t off = (m_binDataStart - 0x1000) + m_used;
        wr32(m_image.data() + 0x1000 + off, uint32_t(0) - total); // negative = allocated
        if (dataLen)
            memcpy(m_image.data() + 0x1000 + off + 4, data, dataLen);
        m_used += total;
        return off;
    }

    void patch32(uint32_t filePos, uint32_t v)
    {
        if (filePos + 4 <= m_image.size())
            wr32(m_image.data() + filePos, v);
    }

    // --- record emission ----------------------------------------------------
    uint32_t emitKey(const HiveKey* key, uint32_t parentOff, bool isRoot)
    {
        // 1) nk record (list offsets/values patched after emission).
        std::vector<uint8_t> nameRaw;
        bool nameCompressed = utf8ToLatin1(key->name, nameRaw);
        std::vector<uint8_t> nk(kNkHeaderSize, 0);
        memcpy(nk.data(), "nk", 2);
        uint16_t flags = 0;
        if (nameCompressed)
            flags |= kNkFlagCompressedName;
        if (isRoot)
            flags |= kNkFlagHiveEntry | kNkFlagNoDelete;
        wr16(nk.data() + 0x02, flags);
        wr32(nk.data() + 0x04, uint32_t(m_meta.lastWriteTime));
        wr32(nk.data() + 0x08, uint32_t(m_meta.lastWriteTime >> 32));
        wr16(nk.data() + 0x44, uint16_t(nameRaw.size()));
        nk.insert(nk.end(), nameRaw.begin(), nameRaw.end());
        uint32_t myOff = allocCell(nk.data(), uint32_t(nk.size()));
        uint32_t myNk = 0x1000 + myOff + 4; // file position of the nk record

        // 2) values: data cells first, then vk records, then the value list.
        std::vector<uint32_t> vkOffs;
        vkOffs.reserve(key->values.size());
        for (const HiveValue& v : key->values) {
            uint32_t dataOff = 0;
            uint32_t sizeField = uint32_t(v.data.size());
            if (v.data.size() <= 4) {
                uint32_t tmp = 0;
                if (!v.data.empty())
                    memcpy(&tmp, v.data.data(), v.data.size());
                dataOff = tmp;
                sizeField |= kVkDataInline;
            } else {
                dataOff = allocCell(v.data.data(), uint32_t(v.data.size()));
            }
            std::vector<uint8_t> nameRaw2;
            bool vnameCompressed = utf8ToLatin1(v.name, nameRaw2);
            uint16_t nameField;
            if (vnameCompressed) {
                nameField = uint16_t(nameRaw2.size());
            } else {
                std::vector<uint16_t> w;
                utf8ToWide(v.name, w);
                nameRaw2.resize(w.size() * 2);
                memcpy(nameRaw2.data(), w.data(), nameRaw2.size());
                nameField = uint16_t(nameRaw2.size());
            }
            std::vector<uint8_t> vk(kVkHeaderSize, 0);
            memcpy(vk.data(), "vk", 2);
            wr16(vk.data() + 0x02, nameField);
            wr32(vk.data() + 0x04, sizeField);
            wr32(vk.data() + 0x08, dataOff);
            wr32(vk.data() + 0x0C, v.type);
            wr16(vk.data() + 0x10, vnameCompressed ? 0x0001 : 0x0000);
            vk.insert(vk.end(), nameRaw2.begin(), nameRaw2.end());
            vkOffs.push_back(allocCell(vk.data(), uint32_t(vk.size())));
        }
        if (!vkOffs.empty()) {
            std::vector<uint8_t> list(vkOffs.size() * 4, 0);
            memcpy(list.data(), vkOffs.data(), list.size());
            uint32_t listOff = allocCell(list.data(), uint32_t(list.size()));
            patch32(myNk + 0x24, listOff);
            patch32(myNk + 0x20, uint32_t(vkOffs.size()));
        }

        // 3) children: full subtrees first, then the "lh" subkey list.
        if (!key->children.empty()) {
            std::vector<uint32_t> childOffs;
            childOffs.reserve(key->children.size());
            for (const HiveKeyPtr& child : key->children)
                childOffs.push_back(emitKey(child.get(), myOff, false));
            std::vector<uint8_t> lh(4 + childOffs.size() * 8, 0);
            lh[0] = 'l';
            lh[1] = 'h';
            wr16(lh.data() + 2, uint16_t(childOffs.size()));
            for (size_t i = 0; i < childOffs.size(); ++i) {
                wr32(lh.data() + 4 + i * 8, lhHash(key->children[i]->name));
                wr32(lh.data() + 4 + i * 8 + 4, childOffs[i]);
            }
            uint32_t listOff = allocCell(lh.data(), uint32_t(lh.size()));
            patch32(myNk + 0x18, listOff);
            patch32(myNk + 0x10, uint32_t(childOffs.size()));
        }

        // 4) parent pointer (the root patches itself after return).
        if (parentOff != 0)
            patch32(myNk + 0x0C, parentOff);

        return myOff;
    }

    void finishBins()
    {
        uint32_t rem = m_binSize - kBinHeaderSize - m_used;
        if (rem >= 8)
            wr32(m_image.data() + m_binDataStart + m_used, rem); // positive = free
    }

    void writeBaseBlock(uint32_t rootOff)
    {
        uint8_t* base = m_image.data();
        memcpy(base, "regf", 4);
        wr32(base + 0x04, 1); // primary sequence
        wr32(base + 0x08, 1); // secondary sequence
        wr32(base + 0x0C, uint32_t(m_meta.lastWriteTime));
        wr32(base + 0x10, uint32_t(m_meta.lastWriteTime >> 32));
        wr32(base + 0x14, m_meta.majorVersion);
        wr32(base + 0x18, m_meta.minorVersion);
        wr32(base + 0x1C, 0); // file type: normal
        wr32(base + 0x20, 1); // file format: direct
        wr32(base + 0x24, rootOff);
        wr32(base + 0x28, uint32_t(m_image.size()) - 0x1000);
        std::vector<uint16_t> w;
        utf8ToWide(m_meta.hiveName, w);
        if (w.size() > 64)
            w.resize(64);
        memcpy(base + 0x30, w.data(), w.size() * 2);
        uint32_t xsum = 0;
        for (int i = 0; i < 127; ++i)
            xsum ^= rd32(base + i * 4);
        wr32(base + 0x1FC, xsum);
    }

    const HiveMeta& m_meta;
    std::vector<uint8_t> m_image;
    uint32_t m_binStart = 0;
    uint32_t m_binDataStart = 0;
    uint32_t m_binSize = 0;
    uint32_t m_used = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// HiveKey helpers (case-insensitive registry semantics)
// ---------------------------------------------------------------------------
const HiveKey* HiveKey::findChild(std::string_view keyName) const
{
    for (const HiveKeyPtr& c : children)
        if (iNameEqual(c->name, std::string(keyName)))
            return c.get();
    return nullptr;
}

HiveKey* HiveKey::findChild(std::string_view keyName)
{
    return const_cast<HiveKey*>(const_cast<const HiveKey*>(this)->findChild(keyName));
}

HiveKey& HiveKey::getOrCreateChild(std::string_view keyName)
{
    if (HiveKey* c = findChild(keyName))
        return *c;
    auto child = std::make_unique<HiveKey>();
    child->name = std::string(keyName);
    HiveKey* raw = child.get();
    children.push_back(std::move(child));
    std::sort(children.begin(), children.end(),
              [](const HiveKeyPtr& a, const HiveKeyPtr& b) { return iNameLess(a->name, b->name); });
    return *raw;
}

bool HiveKey::removeChild(std::string_view keyName)
{
    for (auto it = children.begin(); it != children.end(); ++it) {
        if (iNameEqual((*it)->name, std::string(keyName))) {
            children.erase(it);
            return true;
        }
    }
    return false;
}

const HiveValue* HiveKey::findValue(std::string_view valueName) const
{
    for (const HiveValue& v : values)
        if (v.name == valueName)
            return &v;
    return nullptr;
}

void HiveKey::setValue(std::string valueName, uint32_t type, std::vector<uint8_t> data)
{
    for (HiveValue& v : values) {
        if (v.name == valueName) {
            v.type = type;
            v.data = std::move(data);
            return;
        }
    }
    values.push_back(HiveValue{ std::move(valueName), type, std::move(data) });
}

bool HiveKey::removeValue(std::string_view valueName)
{
    for (auto it = values.begin(); it != values.end(); ++it) {
        if (it->name == valueName) {
            values.erase(it);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Hive facade
// ---------------------------------------------------------------------------
Hive::Hive()
    : m_root(std::make_unique<HiveKey>())
{
}

Hive::~Hive() = default;
Hive::Hive(Hive&&) noexcept = default;
Hive& Hive::operator=(Hive&&) noexcept = default;

bool Hive::parse(const std::vector<uint8_t>& image, std::string* error)
{
    HiveReader reader;
    reader.img = image.data();
    reader.imgSize = uint32_t(image.size());
    if (!reader.run(error))
        return false;
    m_meta = std::move(reader.metaOut);
    m_root = std::move(reader.rootOut);
    return true;
}

std::vector<uint8_t> Hive::serialize() const
{
    HiveWriter writer(m_meta);
    return writer.run(m_root.get());
}

} // namespace bootroll
