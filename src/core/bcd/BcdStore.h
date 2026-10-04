#pragma once
// BCD store model on top of the generic regf Hive.
//
// Layout of a BCD file (a registry hive):
//   \                      root (name is informational)
//   \Objects\{guid}\Description\   values: Type (REG_DWORD), Name (REG_SZ, optional)
//   \Objects\{guid}\Elements\{idHex}\  single value "Element" with the typed payload
//
// All strings are UTF-8 at this boundary. Writes go through the full-rebuild
// hive serializer (saveFile uses temp + .bak + atomic replace).
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace bootroll {

// --- GUID helpers (uppercase/lowercase "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}") ---
// Bytes follow the Windows mixed-endian binary GUID layout (as stored in REG_BINARY).
std::string guidFromBytes(const uint8_t* b);      // -> lowercase "{...}"
bool guidToBytes(std::string_view guid, uint8_t out[16]); // accepts "{...}" or bare
bool guidIsValid(std::string_view guid);
// Random v4 GUID (cross-platform, no OS crypto), lowercase "{...}".
std::string guidGenerate();

class BcdStore {
public:
    struct Element {
        uint32_t id = 0;       // e.g. 0x12000002 (Description)
        uint32_t dataType = 0; // REG_* of the "Element" value
        std::vector<uint8_t> data;
    };

    struct Object {
        std::string guid;  // lowercase "{...}"
        uint32_t type = 0; // Description\Type, e.g. 0x10200003 (boot manager)
        std::string name;  // Description\Name (REG_SZ), may be empty
        std::map<uint32_t, Element> elements;
    };

    // --- file I/O -----------------------------------------------------------
    bool loadFile(const std::string& path, std::string* error = nullptr);
    // Save as: write temp -> verify by reparse -> backup old to .bak -> replace.
    bool saveFile(const std::string& path, std::string* error = nullptr);

    bool loadBytes(const std::vector<uint8_t>& image, std::string* error = nullptr);
    std::vector<uint8_t> saveBytes() const;

    bool empty() const { return m_objects.empty(); }
    const std::vector<Object>& objects() const { return m_objects; }

    // --- object management ---------------------------------------------------
    const Object* findObject(std::string_view guid) const;
    Object* findObject(std::string_view guid);
    // First object of the given type (e.g. the boot manager), or nullptr.
    const Object* findFirstOfType(uint32_t type) const;
    // Creates an object with a fresh GUID; returns the new guid.
    std::string createObject(uint32_t type, const std::string& name = {});
    bool deleteObject(std::string_view guid);

    // --- element management ---------------------------------------------------
    void setElement(std::string_view guid, uint32_t id, uint32_t dataType,
                    std::vector<uint8_t> data);
    void setElementString(std::string_view guid, uint32_t id, const std::string& value);
    void setElementDword(std::string_view guid, uint32_t id, uint32_t value);
    bool removeElement(std::string_view guid, uint32_t id);

    // Typed getters: false when the element is absent or malformed.
    static bool elementAsBytes(const Element& e, std::vector<uint8_t>* out);
    static bool elementAsString(const Element& e, std::string* out);
    static bool elementAsDword(const Element& e, uint32_t* out);

    // GUID list elements: Default (0x23000003, REG_SZ single guid) and
    // DisplayOrder/ToolsDisplayOrder (0x24000001/2, REG_MULTI_SZ of guids).
    // Stored values are UTF-16LE; these helpers hide that.
    static std::vector<std::string> elementAsGuidList(const Element& e);
    void setElementGuidList(std::string_view guid, uint32_t id,
                            const std::vector<std::string>& guids);

    // Convenience accessors over an object's elements (may be absent).
    static const Element* findElement(const Object& obj, uint32_t id);
    static std::string stringElement(const Object& obj, uint32_t id); // "" if absent

private:
    std::vector<Object> m_objects; // sorted by guid
};

} // namespace bootroll
