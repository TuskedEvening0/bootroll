#include "core/bcd/BcdStore.h"

#include "core/bcd/Hive.h"
#include "core/bcd/Utf16.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <set>

namespace bootroll {

namespace {

std::vector<uint8_t> dwordBytes(uint32_t v)
{
    return { uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24) };
}

uint32_t dwordFromBytes(const uint8_t* p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// The narrow CRT file APIs interpret paths in the ANSI code page, which breaks
// for BCD files under non-ASCII folders. Convert our UTF-8 paths to wide chars
// (UTF-16 on Windows) and use the CRT's wide variants instead.
#ifdef _WIN32
std::wstring widePath(const std::string& path)
{
    if (path.empty())
        return {};
    const std::vector<uint8_t> u16 = utf8ToUtf16le(path);
    return std::wstring(reinterpret_cast<const wchar_t*>(u16.data()),
                        u16.size() / sizeof(wchar_t));
}

FILE* openFileUtf8(const std::string& path, const wchar_t* mode)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, widePath(path).c_str(), mode) != 0)
        return nullptr;
    return f;
}

int removeFileUtf8(const std::string& path)
{
    return _wremove(widePath(path).c_str());
}

int renameFileUtf8(const std::string& from, const std::string& to)
{
    return _wrename(widePath(from).c_str(), widePath(to).c_str());
}
#else
FILE* openFileUtf8(const std::string& path, const char* mode)
{
    return fopen(path.c_str(), mode); // POSIX takes UTF-8 paths natively
}

int removeFileUtf8(const std::string& path)
{
    return remove(path.c_str());
}

int renameFileUtf8(const std::string& from, const std::string& to)
{
    return rename(from.c_str(), to.c_str());
}
#endif

std::string guidFromWideParts(const HiveKey& key)
{
    // Object key names are already "{guid}" strings; normalize to lowercase.
    std::string s = key.name;
    if (!s.empty() && s.front() != '{')
        s = "{" + s + "}";
    for (char& c : s)
        c = char(tolower((unsigned char)c));
    return s;
}

// Element subkey name <-> id. Names are 8 hex digits (case-insensitive on disk).
bool elementIdFromName(std::string_view name, uint32_t* id)
{
    if (name.size() != 8)
        return false;
    uint32_t v = 0;
    for (char c : name) {
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= uint32_t(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= uint32_t(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= uint32_t(c - 'A' + 10);
        else
            return false;
    }
    *id = v;
    return true;
}

std::string elementNameFromId(uint32_t id)
{
    char buf[9];
    snprintf(buf, sizeof(buf), "%08x", id);
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------
// GUID helpers
// ---------------------------------------------------------------------------
std::string guidFromBytes(const uint8_t* b)
{
    char buf[40];
    snprintf(buf, sizeof(buf),
             "{%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
             b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11],
             b[12], b[13], b[14], b[15]);
    return buf;
}

bool guidToBytes(std::string_view guid, uint8_t out[16])
{
    uint8_t nib[32];
    int n = 0;
    for (char c : guid) {
        uint8_t v;
        if (c >= '0' && c <= '9')
            v = uint8_t(c - '0');
        else if (c >= 'a' && c <= 'f')
            v = uint8_t(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v = uint8_t(c - 'A' + 10);
        else
            continue; // skip braces/dashes
        if (n >= 32)
            return false;
        nib[n++] = v;
    }
    if (n != 32)
        return false;
    auto pairAt = [&](int i) { return uint8_t(nib[i] << 4 | nib[i + 1]); };
    // Windows binary GUID: first three fields little-endian, rest big-endian.
    out[0] = pairAt(6), out[1] = pairAt(4), out[2] = pairAt(2), out[3] = pairAt(0);
    out[4] = pairAt(10), out[5] = pairAt(8);
    out[6] = pairAt(14), out[7] = pairAt(12);
    for (int i = 8; i < 16; ++i)
        out[i] = pairAt(2 * i);
    return true;
}

bool guidIsValid(std::string_view guid)
{
    uint8_t b[16];
    return guidToBytes(guid, b);
}

std::string guidGenerate()
{
    static std::mt19937_64 rng{ std::random_device{}() };
    uint64_t a = rng(), b = rng();
    uint8_t bytes[16];
    memcpy(bytes, &a, 8);
    memcpy(bytes + 8, &b, 8);
    bytes[7] = uint8_t((bytes[7] & 0x0F) | 0x40); // version 4
    bytes[8] = uint8_t((bytes[8] & 0x3F) | 0x80); // variant 10
    return guidFromBytes(bytes);
}

// ---------------------------------------------------------------------------
// load / save
// ---------------------------------------------------------------------------
bool BcdStore::loadBytes(const std::vector<uint8_t>& image, std::string* error)
{
    Hive hive;
    if (!hive.parse(image, error))
        return false;
    const HiveKey* root = hive.root();
    const HiveKey* objects = root->findChild("Objects");
    if (!objects) {
        if (error)
            *error = "not a BCD store (no \\Objects key)";
        return false; // a hive without \Objects is nothing we can edit
    }

    m_objects.clear();
    std::set<std::string> seen;
    for (const HiveKeyPtr& objKey : objects->children) {
        if (!objKey->name.empty() && objKey->name.front() != '{')
            continue; // tolerate stray keys

        Object obj;
        obj.guid = guidFromWideParts(*objKey);
        if (!seen.insert(obj.guid).second)
            continue;

        if (const HiveKey* desc = objKey->findChild("Description")) {
            if (const HiveValue* t = desc->findValue("Type"); t && t->data.size() == 4)
                obj.type = dwordFromBytes(t->data.data());
            if (const HiveValue* n = desc->findValue("Name"))
                obj.name = utf16leToUtf8(n->data);
        }
        if (const HiveKey* elems = objKey->findChild("Elements")) {
            for (const HiveKeyPtr& elemKey : elems->children) {
                uint32_t id;
                if (!elementIdFromName(elemKey->name, &id))
                    continue;
                const HiveValue* v = elemKey->findValue("Element");
                if (!v)
                    continue;
                Element e;
                e.id = id;
                e.dataType = v->type;
                e.data = v->data;
                obj.elements[id] = std::move(e);
            }
        }
        m_objects.push_back(std::move(obj));
    }
    std::sort(m_objects.begin(), m_objects.end(),
              [](const Object& a, const Object& b) { return a.guid < b.guid; });
    return true;
}

std::vector<uint8_t> BcdStore::saveBytes() const
{
    Hive hive;
    hive.meta().hiveName = "CMI-CreateHive{00000000-0000-0000-0000-000000000000}";
    hive.meta().lastWriteTime = 0; // deterministic; file timestamp conveys the time

    HiveKey& root = *hive.root();
    HiveKey& objects = root.getOrCreateChild("Objects");
    for (const Object& obj : m_objects) {
        HiveKey& objKey = objects.getOrCreateChild(obj.guid);
        HiveKey& desc = objKey.getOrCreateChild("Description");
        desc.setValue("Type", RegTypes::Dword, dwordBytes(obj.type));
        if (!obj.name.empty())
            desc.setValue("Name", RegTypes::Sz, utf8ToUtf16le(obj.name));
        if (!obj.elements.empty()) {
            HiveKey& elems = objKey.getOrCreateChild("Elements");
            for (const auto& [id, e] : obj.elements) {
                HiveKey& elemKey = elems.getOrCreateChild(elementNameFromId(id));
                elemKey.setValue("Element", e.dataType, e.data);
            }
        }
    }
    return hive.serialize();
}

bool BcdStore::loadFile(const std::string& path, std::string* error)
{
    FILE* f = openFileUtf8(path, L"rb");
    if (!f) {
        if (error)
            *error = "cannot open " + path;
        return false;
    }
    std::vector<uint8_t> image;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        image.insert(image.end(), buf, buf + n);
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) {
        if (error)
            *error = "read failed: " + path;
        return false;
    }
    return loadBytes(image, error);
}

bool BcdStore::saveFile(const std::string& path, std::string* error)
{
    std::vector<uint8_t> image = saveBytes();

    // Verify the fresh image parses back before touching the original.
    BcdStore verify;
    std::string verr;
    if (!verify.loadBytes(image, &verr)) {
        if (error)
            *error = "internal: rebuilt BCD failed to reparse: " + verr;
        return false;
    }

    const std::string tmp = path + ".tmp";
    const std::string bak = path + ".bak";
    FILE* f = openFileUtf8(tmp, L"wb");
    if (!f) {
        if (error)
            *error = "cannot create " + tmp;
        return false;
    }
    size_t written = fwrite(image.data(), 1, image.size(), f);
    bool ok = written == image.size() && fflush(f) == 0;
    fclose(f);
    if (!ok) {
        removeFileUtf8(tmp);
        if (error)
            *error = "write failed: " + tmp;
        return false;
    }

    // Back up the original, then move the temp over it.
    removeFileUtf8(bak);      // best effort; may not exist yet
    renameFileUtf8(path, bak);
    if (renameFileUtf8(tmp, path) != 0) {
        // Roll the backup back so the store is not lost.
        renameFileUtf8(bak, path);
        if (error)
            *error = "replace failed: " + path;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// object / element management
// ---------------------------------------------------------------------------
const BcdStore::Object* BcdStore::findObject(std::string_view guid) const
{
    std::string g(guid);
    for (char& c : g)
        c = char(tolower((unsigned char)c));
    if (!g.empty() && g.front() != '{')
        g = "{" + g + "}";
    auto it = std::lower_bound(m_objects.begin(), m_objects.end(), g,
                               [](const Object& o, const std::string& key) {
                                   return o.guid < key;
                               });
    if (it != m_objects.end() && it->guid == g)
        return &*it;
    return nullptr;
}

BcdStore::Object* BcdStore::findObject(std::string_view guid)
{
    return const_cast<Object*>(const_cast<const BcdStore*>(this)->findObject(guid));
}

const BcdStore::Object* BcdStore::findFirstOfType(uint32_t type) const
{
    for (const Object& o : m_objects)
        if (o.type == type)
            return &o;
    return nullptr;
}

std::string BcdStore::createObject(uint32_t type, const std::string& name)
{
    Object obj;
    obj.guid = guidGenerate();
    obj.type = type;
    obj.name = name;
    std::string guid = obj.guid; // copy before the move below
    auto it = std::lower_bound(m_objects.begin(), m_objects.end(), obj.guid,
                               [](const Object& o, const std::string& g) {
                                   return o.guid < g;
                               });
    m_objects.insert(it, std::move(obj));
    return guid;
}

bool BcdStore::deleteObject(std::string_view guid)
{
    for (auto it = m_objects.begin(); it != m_objects.end(); ++it) {
        if (it->guid == guid) {
            m_objects.erase(it);
            return true;
        }
    }
    return false;
}

void BcdStore::setElement(std::string_view guid, uint32_t id, uint32_t dataType,
                          std::vector<uint8_t> data)
{
    Object* obj = findObject(guid);
    if (!obj)
        return;
    Element e;
    e.id = id;
    e.dataType = dataType;
    e.data = std::move(data);
    obj->elements[id] = std::move(e);
}

void BcdStore::setElementString(std::string_view guid, uint32_t id, const std::string& value)
{
    // Registry strings are UTF-16LE on disk.
    setElement(guid, id, RegTypes::Sz, utf8ToUtf16le(value));
}

std::vector<std::string> BcdStore::elementAsGuidList(const Element& e)
{
    if (e.dataType == RegTypes::MultiSz)
        return regMultiFromBytes(e.data.data(), e.data.size());
    if (e.dataType == RegTypes::Sz || e.dataType == RegTypes::ExpandSz) {
        std::string one = utf16leToUtf8(e.data);
        if (one.empty())
            return {};
        return { one };
    }
    return {};
}

void BcdStore::setElementGuidList(std::string_view guid, uint32_t id,
                                  const std::vector<std::string>& guids)
{
    // Display-order style lists are REG_MULTI_SZ (single entries included).
    setElement(guid, id, RegTypes::MultiSz, regMultiToBytes(guids));
}

void BcdStore::setElementDword(std::string_view guid, uint32_t id, uint32_t value)
{
    setElement(guid, id, RegTypes::Dword, dwordBytes(value));
}

bool BcdStore::removeElement(std::string_view guid, uint32_t id)
{
    Object* obj = findObject(guid);
    return obj && obj->elements.erase(id) > 0;
}

// ---------------------------------------------------------------------------
// typed element access
// ---------------------------------------------------------------------------
bool BcdStore::elementAsBytes(const Element& e, std::vector<uint8_t>* out)
{
    *out = e.data;
    return true;
}

bool BcdStore::elementAsString(const Element& e, std::string* out)
{
    if (e.dataType != RegTypes::Sz && e.dataType != RegTypes::ExpandSz &&
        e.dataType != RegTypes::MultiSz)
        return false;
    // Registry strings are UTF-16LE on disk.
    *out = utf16leToUtf8(e.data);
    return true;
}

bool BcdStore::elementAsDword(const Element& e, uint32_t* out)
{
    if (e.dataType != RegTypes::Dword || e.data.size() != 4)
        return false;
    *out = dwordFromBytes(e.data.data());
    return true;
}

const BcdStore::Element* BcdStore::findElement(const Object& obj, uint32_t id)
{
    auto it = obj.elements.find(id);
    return it == obj.elements.end() ? nullptr : &it->second;
}

std::string BcdStore::stringElement(const Object& obj, uint32_t id)
{
    const Element* e = findElement(obj, id);
    std::string s;
    return e && elementAsString(*e, &s) ? s : std::string();
}

} // namespace bootroll
