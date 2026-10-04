#pragma once
// Self-contained regf (Windows registry hive) parser + full-rebuild serializer.
// Scope: BCD files and similar small hives. No volatile keys, no transaction
// logs, no big-data ("db") indirection — BCD never uses them.
//
// Read : parse() -> HiveKey tree (values carry raw bytes + registry type).
// Write: serialize() rebuilds the whole hive from the tree (garbage-collecting
//        and repacking), which is the safest write strategy for BCD (<1MB).
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bootroll {

// Registry value type constants (REG_*).
namespace RegTypes {
inline constexpr uint32_t None = 0;
inline constexpr uint32_t Sz = 1;
inline constexpr uint32_t ExpandSz = 2;
inline constexpr uint32_t Binary = 3;
inline constexpr uint32_t Dword = 4;
inline constexpr uint32_t DwordBigEndian = 5;
inline constexpr uint32_t Link = 6;
inline constexpr uint32_t MultiSz = 7;
inline constexpr uint32_t ResourceList = 8;
inline constexpr uint32_t FullResourceDescriptor = 9;
inline constexpr uint32_t ResourceRequirementsList = 10;
inline constexpr uint32_t Qword = 11;
} // namespace RegTypes

struct HiveValue {
    std::string name; // UTF-8; empty name = the key's default value
    uint32_t type = RegTypes::None;
    std::vector<uint8_t> data;
};

struct HiveKey;
using HiveKeyPtr = std::unique_ptr<HiveKey>;

struct HiveKey {
    std::string name; // UTF-8
    std::vector<HiveValue> values;
    std::vector<HiveKeyPtr> children;

    // Children are kept sorted case-insensitively (registry semantics).
    const HiveKey* findChild(std::string_view keyName) const;
    HiveKey* findChild(std::string_view keyName);
    HiveKey& getOrCreateChild(std::string_view keyName);
    bool removeChild(std::string_view keyName);

    const HiveValue* findValue(std::string_view valueName) const; // "" = default value
    void setValue(std::string name, uint32_t type, std::vector<uint8_t> data);
    bool removeValue(std::string_view name);
};

struct HiveMeta {
    std::string hiveName;        // base-block hive name (UTF-8)
    uint32_t majorVersion = 3;
    uint32_t minorVersion = 1;
    uint64_t lastWriteTime = 0;  // FILETIME
};

class Hive {
public:
    Hive(); // starts with an empty root key, ready to build a fresh store
    ~Hive();
    Hive(Hive&&) noexcept;
    Hive& operator=(Hive&&) noexcept;
    // Parse a full hive image ("regf" ... bins ...). On failure returns false
    // and, when provided, fills *error with a short reason.
    bool parse(const std::vector<uint8_t>& image, std::string* error = nullptr);

    // Serialize the tree into a fresh, packed hive image.
    std::vector<uint8_t> serialize() const;

    bool valid() const { return m_root != nullptr; }
    HiveKey* root() { return m_root.get(); }
    const HiveKey* root() const { return m_root.get(); }
    const HiveMeta& meta() const { return m_meta; }
    HiveMeta& meta() { return m_meta; }

private:
    HiveMeta m_meta;
    HiveKeyPtr m_root;
};

} // namespace bootroll
