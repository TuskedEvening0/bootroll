#include "core/bcd/BcdElements.h"

#include "core/bcd/Hive.h"
#include "core/bcd/Utf16.h"
#include "core/util/HexText.h"

#include <cstdio>
#include <utility>

namespace bootroll {

BcdValueClass bcdClassify(uint32_t elementId)
{
    switch (elementId >> 24) {
    case 0x11: return BcdValueClass::Device;
    case 0x12: return BcdValueClass::String;
    case 0x22: return BcdValueClass::Integer;
    case 0x23: return BcdValueClass::Boolean;
    case 0x24: return BcdValueClass::GuidList;
    case 0x25: return BcdValueClass::Integer64;
    case 0x26: return BcdValueClass::IntList;
    default: return BcdValueClass::Unknown;
    }
}

namespace {

struct ElemLabel {
    uint32_t id;
    std::string_view label;
    std::string_view group; // "Library" | "Boot manager" | "OS loader"
};

// English source strings for the elements bootroll exposes. Unknown elements
// fall back to "%08X". Groups follow the BCD schema sections (library elements
// are shared by all application types; the others are manager/osloader picks).
constexpr ElemLabel kElemLabels[] = {
    // library (application-common)
    { 0x11000001, "Device", "Library" },
    { 0x12000002, "Description", "Library" },
    { 0x12000004, "Path", "Library" },
    { 0x12000005, "Locale", "Library" },
    // boot manager
    { 0x23000003, "Default object", "Boot manager" },
    { 0x24000001, "Display order", "Boot manager" },
    { 0x24000002, "Tools display order", "Boot manager" },
    { 0x25000004, "Timeout", "Boot manager" },
    { 0x23000005, "Display boot menu", "Boot manager" },
    { 0x23000006, "Boot status policy: no error UI", "Boot manager" },
    // osloader
    { 0x21000001, "OS device", "OS loader" },
    { 0x12000007, "System root", "OS loader" },
    { 0x12000030, "OS load options", "OS loader" },
    { 0x12000036, "Kernel path", "OS loader" },
    { 0x12000037, "HAL path", "OS loader" },
    { 0x22000005, "NX policy", "OS loader" },
    { 0x2200000E, "PAE policy", "OS loader" },
    { 0x25000080, "Safe boot", "OS loader" },
    { 0x26000010, "Detect kernel and HAL", "OS loader" },
};

struct ObjLabel {
    uint32_t type;
    std::string_view label;
};

constexpr ObjLabel kObjLabels[] = {
    { 0x10100002, "Firmware Boot Manager" },
    { 0x10200003, "Windows Boot Manager" },
    { 0x10200007, "Windows Boot Loader" },
    { 0x10200009, "Windows Memory Diagnostic" },
    { 0x10300006, "Windows Resume Application" },
    { 0x10300009, "Legacy Windows (NTLDR)" },
    { 0x1030000A, "Legacy VHD Boot" },
    // inheritable settings objects found in real stores
    { 0x00200000, "Inherit (generic)" },
    { 0x00200001, "Inherit (custom)" },
    { 0x00200002, "Inherit (debugger)" },
    { 0x00200003, "Inherit (EMS)" },
    { 0x00200004, "Inherit (device options)" },
};

} // namespace

std::string_view bcdElementLabel(uint32_t elementId)
{
    for (const ElemLabel& e : kElemLabels)
        if (e.id == elementId)
            return e.label;
    return {};
}

std::string_view bcdElementGroup(uint32_t elementId)
{
    for (const ElemLabel& e : kElemLabels)
        if (e.id == elementId)
            return e.group;
    return {};
}

std::string_view bcdObjectTypeLabel(uint32_t type)
{
    for (const ObjLabel& o : kObjLabels)
        if (o.type == type)
            return o.label;
    return {};
}

std::string_view bcdRegTypeLabel(uint32_t regType)
{
    namespace RC = RegTypes;
    switch (regType) {
    case RC::None: return "REG_NONE";
    case RC::Sz: return "REG_SZ";
    case RC::ExpandSz: return "REG_EXPAND_SZ";
    case RC::Binary: return "REG_BINARY";
    case RC::Dword: return "REG_DWORD";
    case RC::DwordBigEndian: return "REG_DWORD_BIG_ENDIAN";
    case RC::Link: return "REG_LINK";
    case RC::MultiSz: return "REG_MULTI_SZ";
    case RC::Qword: return "REG_QWORD";
    default: return {};
    }
}

namespace {

// Flatten control characters so a value can be shown on a single table line
// without breaking UTF-8 multibyte sequences.
std::string sanitizeLine(std::string s)
{
    for (char& c : s)
        if ((unsigned char)c < 0x20 || c == 0x7f)
            c = '?';
    return s;
}

std::string hexFallback(const uint8_t* data, size_t size)
{
    if (size == 0)
        return {};
    // Payload that does not match its declared registry type: dump what is
    // there instead of guessing.
    size_t shown = size < 16 ? size : 16;
    std::string s = bytesToHexText(data, shown, true);
    if (size > shown)
        s += " ...";
    char tail[32];
    std::snprintf(tail, sizeof(tail), " (%zu B)", size);
    s += tail;
    return s;
}

} // namespace

std::string bcdElementValuePreview(uint32_t regType, const uint8_t* data, size_t size)
{
    namespace RC = RegTypes;
    switch (regType) {
    case RC::Sz:
    case RC::ExpandSz:
        return sanitizeLine(utf16leToUtf8(data, size));
    case RC::MultiSz: {
        std::vector<std::string> parts = regMultiFromBytes(data, size);
        std::string out;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i)
                out += " | ";
            out += sanitizeLine(parts[i]);
        }
        return out;
    }
    case RC::Dword: {
        if (size != 4)
            break;
        uint32_t v = uint32_t(data[0]) | uint32_t(data[1]) << 8 |
                     uint32_t(data[2]) << 16 | uint32_t(data[3]) << 24;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%u (0x%08X)", v, v);
        return buf;
    }
    case RC::DwordBigEndian: {
        if (size != 4)
            break;
        uint32_t v = uint32_t(data[3]) | uint32_t(data[2]) << 8 |
                     uint32_t(data[1]) << 16 | uint32_t(data[0]) << 24;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%u (0x%08X)", v, v);
        return buf;
    }
    case RC::Qword: {
        if (size != 8)
            break;
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i)
            v = v << 8 | data[i];
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%llu (0x%016llX)",
                      (unsigned long long)v, (unsigned long long)v);
        return buf;
    }
    default:
        break;
    }
    return hexFallback(data, size);
}

} // namespace bootroll
