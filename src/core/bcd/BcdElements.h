#pragma once
// Public BCD element/object metadata (from Microsoft's documented BCD schema).
//
// Element id layout (doc: "BCD Element Types"):
//   0xAABBCCCC  AA = data class (11 device, 12 string, 22 integer, 23 boolean,
//                          24 guid-list, 25 int64, 26 int-list, ...),
//               rest = element id proper.
// Object types encode the application kind, e.g. 0x10200003 = boot manager.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bootroll {

// --- object types ----------------------------------------------------------
namespace BcdObjectType {
inline constexpr uint32_t BootManager = 0x10200003;
inline constexpr uint32_t OsLoader = 0x10200007;
inline constexpr uint32_t MemoryDiagnostic = 0x10200009;
inline constexpr uint32_t ResumeApplication = 0x10300006;
inline constexpr uint32_t LegacyNtldr = 0x10300009; // {ntldr}
inline constexpr uint32_t LegacyVhdBoot = 0x1030000A;

inline bool isApplication(uint32_t t) { return (t >> 24) == 0x10; }
inline bool isInheritable(uint32_t t) { return (t >> 24) == 0x00; }
} // namespace BcdObjectType

// --- element ids (the subset bootroll uses) --------------------------------
namespace BcdElementId {
// device
inline constexpr uint32_t Device = 0x11000001;   // boot device (application)
inline constexpr uint32_t OsDevice = 0x21000001; // OS partition (osloader)

// string
inline constexpr uint32_t Description = 0x12000002;
inline constexpr uint32_t Path = 0x12000004;
inline constexpr uint32_t Locale = 0x12000005;
inline constexpr uint32_t SystemRoot = 0x12000007;
inline constexpr uint32_t OsLoadOptions = 0x12000030;
inline constexpr uint32_t KernelPath = 0x12000036;
inline constexpr uint32_t HalPath = 0x12000037;

// list / guid-list
inline constexpr uint32_t DisplayOrder = 0x24000001;
inline constexpr uint32_t ToolsDisplayOrder = 0x24000002;

// integer / int64
inline constexpr uint32_t Timeout = 0x25000004;
inline constexpr uint32_t SafeBoot = 0x25000080;

// boolean
inline constexpr uint32_t DefaultObject = 0x23000003; // bootmgr: default entry (REG_SZ guid)
inline constexpr uint32_t DisplayBootMenu = 0x23000005;
inline constexpr uint32_t NoErrorUi = 0x23000006;

// dword policies (osloader)
inline constexpr uint32_t NxPolicy = 0x22000005;
inline constexpr uint32_t PaePolicy = 0x2200000E;
// int-list boolean: present => on ("detecthal")
inline constexpr uint32_t DetectKernelAndHal = 0x26000010;
} // namespace BcdElementId

// --- element classification -------------------------------------------------
enum class BcdValueClass {
    Unknown,
    Device,    // 0x11xxxxxx binary device element
    String,    // 0x12xxxxxx
    Integer,   // 0x22xxxxxx (dword)
    Boolean,   // 0x23xxxxxx
    GuidList,  // 0x24xxxxxx
    Integer64, // 0x25xxxxxx
    IntList,   // 0x26xxxxxx
};

BcdValueClass bcdClassify(uint32_t elementId);

// English label for an element id ("" when unknown); the UI passes it through
// T_() so translated catalogs can cover it.
std::string_view bcdElementLabel(uint32_t elementId);
// Schema group an element belongs to ("" when unknown): "Library", "Boot
// manager" or "OS loader". Drives the professional-mode grouping.
std::string_view bcdElementGroup(uint32_t elementId);
// English label for an object type ("" when unknown).
std::string_view bcdObjectTypeLabel(uint32_t type);

// English label for a registry value type ("REG_SZ", ...; "" when unknown).
std::string_view bcdRegTypeLabel(uint32_t regType);

// One-line decoded preview of an element payload for tables/tooltips:
// strings decode from UTF-16LE, control chars become '?', lists join with
// " | ", integers show "dec (0xHEX)", everything else hex-dumps (truncated
// with the total byte count). Never throws; malformed data degrades to hex.
std::string bcdElementValuePreview(uint32_t regType, const uint8_t* data, size_t size);
inline std::string bcdElementValuePreview(uint32_t regType, const std::vector<uint8_t>& d)
{
    return bcdElementValuePreview(regType, d.data(), d.size());
}

} // namespace bootroll
