#include "ui/BcdProfessionalScreen.h"

#include "app/App.h"
#include "app/I18n.h"
#include "core/bcd/BcdElements.h"
#include "core/bcd/BcdStore.h"
#include "core/bcd/Hive.h"
#include "core/bcd/Utf16.h"
#include "core/util/HexText.h"
#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace bootroll {

namespace {

namespace RT = RegTypes;

std::string tr(std::string_view msgid)
{
    return I18n::instance().translate(std::string(msgid));
}

// Registry type assumed for a brand-new element id (mirrors real BCD stores).
uint32_t inferRegType(uint32_t id)
{
    if (id == BcdElementId::DefaultObject)
        return RT::Sz; // bootmgr object reference (guid string)
    switch (bcdClassify(id)) {
    case BcdValueClass::String: return RT::Sz;
    case BcdValueClass::GuidList:
    case BcdValueClass::IntList: return RT::MultiSz;
    case BcdValueClass::Integer:
    case BcdValueClass::Boolean:
    case BcdValueClass::Integer64: return RT::Dword;
    case BcdValueClass::Device:
    default: return RT::Binary;
    }
}

enum class EditKind { String, List, Integer, Boolean, Hex };

EditKind kindFor(uint32_t id, const BcdStore::Element* e)
{
    uint32_t t = e ? e->dataType : inferRegType(id);
    if (t == RT::Sz || t == RT::ExpandSz)
        return EditKind::String;
    if (t == RT::MultiSz)
        return EditKind::List;
    if (t == RT::Dword || t == RT::DwordBigEndian || t == RT::Qword) {
        if (t != RT::Qword && bcdClassify(id) == BcdValueClass::Boolean)
            return EditKind::Boolean;
        return EditKind::Integer;
    }
    return EditKind::Hex;
}

// ---------------------------------------------------------------------------
// editor buffers (reload on selection change so edits never leak across elements)
// ---------------------------------------------------------------------------
struct EditBufs {
    std::string objGuid;
    uint32_t elemId = 0;
    bool loaded = false;
    bool raw = false;   // force hex editing of the selected element
    char text[1024];    // REG_SZ / REG_EXPAND_SZ / integer input
    char list[8192];    // REG_MULTI_SZ, one item per line
    char rawtext[8192]; // hex bytes
    std::string err;    // msgid key of the last validation error

    EditBufs()
    {
        text[0] = list[0] = rawtext[0] = '\0';
    }
};

std::vector<uint8_t> dwordLe(uint32_t v)
{
    return { uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24) };
}

EditBufs& editBufs(const std::string& guid, uint32_t id, const BcdStore::Element* e)
{
    static EditBufs s;
    if (s.loaded && s.objGuid == guid && s.elemId == id)
        return s;
    s = EditBufs();
    s.objGuid = guid;
    s.elemId = id;
    s.loaded = true;
    if (!e)
        return s;

    std::snprintf(s.rawtext, sizeof(s.rawtext), "%s", bytesToHexText(e->data).c_str());
    switch (kindFor(id, e)) {
    case EditKind::String:
        std::snprintf(s.text, sizeof(s.text), "%s", utf16leToUtf8(e->data).c_str());
        break;
    case EditKind::List: {
        std::string joined;
        for (const std::string& g : BcdStore::elementAsGuidList(*e)) {
            if (!joined.empty())
                joined += '\n';
            joined += g;
        }
        std::snprintf(s.list, sizeof(s.list), "%s", joined.c_str());
        break;
    }
    case EditKind::Integer: {
        char buf[32] = "";
        if (e->dataType == RT::Qword && e->data.size() == 8) {
            uint64_t v = 0;
            for (int i = 7; i >= 0; --i)
                v = v << 8 | e->data[i];
            std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
        } else if (e->data.size() == 4) {
            uint32_t v = uint32_t(e->data[0]) | uint32_t(e->data[1]) << 8 |
                         uint32_t(e->data[2]) << 16 | uint32_t(e->data[3]) << 24;
            if (e->dataType == RT::DwordBigEndian)
                v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) |
                    (v << 24);
            std::snprintf(buf, sizeof(buf), "%u", v);
        }
        std::snprintf(s.text, sizeof(s.text), "%s", buf);
        break;
    }
    case EditKind::Boolean:
    case EditKind::Hex:
        break;
    }
    return s;
}

// ---------------------------------------------------------------------------
// parsing / applying
// ---------------------------------------------------------------------------
bool parseU64(const char* s, uint64_t* out)
{
    while (*s == ' ' || *s == '\t')
        ++s;
    const char* digits = s;
    unsigned base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        digits = s + 2;
        base = 16;
    }
    if (!*digits)
        return false;
    char* end = nullptr;
    unsigned long long v = std::strtoull(digits, &end, (int)base);
    while (end && (*end == ' ' || *end == '\t'))
        ++end;
    if (!end || end == digits || *end != '\0')
        return false;
    *out = v;
    return true;
}

bool parseElemId(const char* s, uint32_t* id)
{
    while (*s == ' ' || *s == '\t')
        ++s;
    const char* d = s;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        d = s + 2;
    int n = 0;
    uint64_t v = 0;
    for (; d[n]; ++n) {
        int h;
        if (d[n] >= '0' && d[n] <= '9')
            h = d[n] - '0';
        else if (d[n] >= 'a' && d[n] <= 'f')
            h = d[n] - 'a' + 10;
        else if (d[n] >= 'A' && d[n] <= 'F')
            h = d[n] - 'A' + 10;
        else
            return false;
        if (n >= 8)
            return false;
        v = v << 4 | uint64_t(h);
    }
    if (n == 0 || v == 0)
        return false;
    *id = uint32_t(v);
    return true;
}

// Write the edited buffers into the store. Returns nullptr on success or a
// msgid key describing the problem. Empty values remove the element.
const char* applyEdits(BcdStore& store, const std::string& guid, uint32_t id,
                       const BcdStore::Element* existing, const EditBufs& b,
                       EditKind kind)
{
    uint32_t dataType = existing ? existing->dataType : inferRegType(id);
    switch (kind) {
    case EditKind::Hex: {
        std::vector<uint8_t> bytes;
        if (!hexTextToBytes(b.rawtext, &bytes))
            return "Invalid hex input.";
        if (bytes.empty()) {
            store.removeElement(guid, id);
            return nullptr;
        }
        store.setElement(guid, id, dataType, std::move(bytes));
        return nullptr;
    }
    case EditKind::String: {
        std::string v(b.text);
        if (v.empty()) {
            store.removeElement(guid, id);
            return nullptr;
        }
        store.setElement(guid, id, dataType, utf8ToUtf16le(v));
        return nullptr;
    }
    case EditKind::List: {
        std::vector<std::string> items;
        std::string all(b.list);
        size_t pos = 0;
        while (pos < all.size()) {
            size_t nl = all.find('\n', pos);
            std::string line = all.substr(
                pos, nl == std::string::npos ? std::string::npos : nl - pos);
            size_t b0 = line.find_first_not_of(" \t\r");
            if (b0 != std::string::npos) {
                line = line.substr(b0, line.find_last_not_of(" \t\r") - b0 + 1);
                if (line.front() != '{')
                    line = "{" + line + "}";
                for (char& c : line)
                    if (c >= 'A' && c <= 'Z')
                        c = char(c - 'A' + 'a');
                if (!guidIsValid(line))
                    return "Invalid GUID list (one GUID per line).";
                items.push_back(line);
            }
            if (nl == std::string::npos)
                break;
            pos = nl + 1;
        }
        if (items.empty()) {
            store.removeElement(guid, id);
            return nullptr;
        }
        store.setElementGuidList(guid, id, items);
        return nullptr;
    }
    case EditKind::Integer: {
        uint64_t v;
        if (!parseU64(b.text, &v))
            return "Invalid integer value.";
        if (dataType == RT::Qword) {
            std::vector<uint8_t> bytes(8);
            for (int i = 0; i < 8; ++i)
                bytes[i] = uint8_t(v >> (8 * i));
            store.setElement(guid, id, dataType, std::move(bytes));
        } else if (dataType == RT::Dword || dataType == RT::DwordBigEndian) {
            if (v > 0xFFFFFFFFull)
                return "Value does not fit in 32 bits.";
            std::vector<uint8_t> bytes = dwordLe(uint32_t(v));
            if (dataType == RT::DwordBigEndian)
                std::reverse(bytes.begin(), bytes.end());
            store.setElement(guid, id, dataType, std::move(bytes));
        } else {
            return "This value type cannot be edited as an integer; use raw mode.";
        }
        return nullptr;
    }
    case EditKind::Boolean:
        break; // toggled through a checkbox, not via Apply
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// drawing helpers
// ---------------------------------------------------------------------------
std::string objectDisplayLabel(const BcdStore::Object& o)
{
    std::string_view tl = bcdObjectTypeLabel(o.type);
    std::string type;
    if (!tl.empty()) {
        type = tr(tl);
    } else {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%08X", o.type);
        type = buf;
    }
    std::string desc = o.name.empty()
        ? BcdStore::stringElement(o, BcdElementId::Description)
        : o.name;
    std::string label = type + "  |  ";
    if (!desc.empty())
        label += desc + "  |  ";
    label += o.guid;
    return label;
}

std::string truncatePreview(const std::string& s, size_t maxBytes = 160)
{
    if (s.size() <= maxBytes)
        return s;
    size_t cut = maxBytes;
    while (cut > 0 && (s[cut] & 0xC0) == 0x80)
        --cut; // do not split a UTF-8 sequence
    return s.substr(0, cut) + " ...";
}

// Renders the value widget for one editor instance (`wid` keeps widget IDs
// unique between the edit and add panes). Returns true when a boolean
// checkbox was toggled; the caller writes it through immediately.
bool drawValueEditor(const char* wid, EditKind kind, EditBufs& b, bool* boolOn)
{
    const std::string id(wid);
    switch (kind) {
    case EditKind::Hex:
        ImGui::SetNextItemWidth(620);
        ImGui::InputTextMultiline((id + "raw").c_str(), b.rawtext, sizeof(b.rawtext),
                                  ImVec2(0, 110));
        ImGui::TextDisabled(
            "%s", T_("Hex bytes, e.g. \"0A 1B 2C\". Empty clears the element."));
        return false;
    case EditKind::List:
        ImGui::SetNextItemWidth(620);
        ImGui::InputTextMultiline((id + "list").c_str(), b.list, sizeof(b.list),
                                  ImVec2(0, 110));
        ImGui::TextDisabled("%s", T_("One GUID per line. Empty clears the element."));
        return false;
    case EditKind::Boolean: {
        ImGui::Checkbox((id + "bool").c_str(), boolOn);
        bool edited = ImGui::IsItemEdited();
        ImGui::SameLine();
        ImGui::TextUnformatted(T_("Enabled"));
        return edited;
    }
    default: // String / Integer
        ImGui::SetNextItemWidth(620);
        ImGui::InputText((id + "text").c_str(), b.text, sizeof(b.text));
        if (kind == EditKind::Integer)
            ImGui::TextDisabled("%s", T_("Decimal, or 0x prefix for hex."));
        return false;
    }
}

} // namespace

void BcdProfessionalScreen::drawBody(App& app)
{
    App::BcdEditState& st = app.bcdEdit();
    BcdStore& store = st.store;

    if (store.empty()) {
        ImGui::TextWrapped("%s", T_("No store open. Open or create a BCD file above."));
        return;
    }

    // Keep the object selection valid; default to the first object.
    if (!st.profObjGuid.empty() && !store.findObject(st.profObjGuid)) {
        st.profObjGuid.clear();
        st.profElemId = 0;
    }
    if (st.profObjGuid.empty())
        st.profObjGuid = store.objects().front().guid;
    const BcdStore::Object* obj = store.findObject(st.profObjGuid);
    if (!obj)
        return;

    // --- object picker ----------------------------------------------------------
    std::string curLabel = objectDisplayLabel(*obj);
    ImGui::SetNextItemWidth(620);
    if (ImGui::BeginCombo(T_("Object"), curLabel.c_str())) {
        for (const BcdStore::Object& o : store.objects()) {
            std::string lbl = objectDisplayLabel(o);
            if (ImGui::Selectable(lbl.c_str(), o.guid == st.profObjGuid)) {
                st.profObjGuid = o.guid;
                st.profElemId = 0;
            }
        }
        ImGui::EndCombo();
    }

    // --- element table ------------------------------------------------------------
    ImGui::Spacing();
    if (obj->elements.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.55f, 0.6f, 1.0f), "%s",
                           T_("No elements in this object."));
    } else {
        static ImGuiTableFlags tflags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_Resizable |
                                        ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##profelems", 4, tflags)) {
            ImGui::TableSetupColumn(T_("Element ID"), ImGuiTableColumnFlags_WidthFixed,
                                    110);
            ImGui::TableSetupColumn(T_("Name"), ImGuiTableColumnFlags_WidthFixed, 180);
            ImGui::TableSetupColumn(T_("Reg type"), ImGuiTableColumnFlags_WidthFixed, 150);
            ImGui::TableSetupColumn(T_("Value"));
            ImGui::TableHeadersRow();
            for (const auto& [id, e] : obj->elements) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char idbuf[16];
                std::snprintf(idbuf, sizeof(idbuf), "0x%08X", id);
                bool sel = st.profElemId == id;
                if (ImGui::Selectable(idbuf, &sel, ImGuiSelectableFlags_SpanAllColumns))
                    st.profElemId = sel ? id : 0;
                ImGui::TableSetColumnIndex(1);
                std::string_view lb = bcdElementLabel(id);
                if (!lb.empty())
                    ImGui::TextUnformatted(tr(lb).c_str());
                ImGui::TableSetColumnIndex(2);
                std::string_view rt = bcdRegTypeLabel(e.dataType);
                if (!rt.empty())
                    ImGui::TextUnformatted(rt.data());
                else
                    ImGui::Text("0x%X", e.dataType);
                ImGui::TableSetColumnIndex(3);
                std::string prev = bcdElementValuePreview(e.dataType, e.data);
                ImGui::TextUnformatted(truncatePreview(prev).c_str());
                if (ImGui::IsItemHovered() && prev.size() > 160)
                    ImGui::SetTooltip("%s", prev.c_str());
            }
            ImGui::EndTable();
        }
    }

    // --- selected-element editor ---------------------------------------------------
    if (st.profElemId != 0) {
        const BcdStore::Element* elem = BcdStore::findElement(*obj, st.profElemId);
        if (!elem) {
            st.profElemId = 0; // stale selection (element was deleted elsewhere)
        } else {
            ImGui::Spacing();
            ImGui::Separator();

            char header[16];
            std::snprintf(header, sizeof(header), "0x%08X", st.profElemId);
            ImGui::TextUnformatted(T_("Edit element"));
            ImGui::SameLine();
            ImGui::TextUnformatted(header);
            std::string_view lb = bcdElementLabel(st.profElemId);
            if (!lb.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", tr(lb).c_str());
            }

            EditBufs& b = editBufs(st.profObjGuid, st.profElemId, elem);
            if (!b.err.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s",
                                   T_(b.err.c_str()));

            const EditKind kind = kindFor(st.profElemId, elem);
            const bool hexMode = b.raw || kind == EditKind::Hex;

            bool boolOn = false;
            if (kind == EditKind::Boolean) {
                uint32_t v = 0;
                boolOn = BcdStore::elementAsDword(*elem, &v) ? v != 0 : false;
            }
            const bool toggled =
                drawValueEditor("##profed", hexMode ? EditKind::Hex : kind, b, &boolOn);
            if (toggled) {
                store.setElement(st.profObjGuid, st.profElemId, elem->dataType,
                                 dwordLe(boolOn ? 1u : 0u));
                st.dirty = true;
            }

            std::string_view rt = bcdRegTypeLabel(elem->dataType);
            char info[128];
            if (!rt.empty())
                std::snprintf(info, sizeof(info), "%s, %u bytes", rt.data(),
                              (unsigned)elem->data.size());
            else
                std::snprintf(info, sizeof(info), "type 0x%X, %u bytes",
                              elem->dataType, (unsigned)elem->data.size());
            ImGui::TextDisabled("%s", info);

            if (!(kind == EditKind::Boolean && !hexMode)) {
                if (ImGui::Button(T_("Apply"))) {
                    const char* errKey =
                        applyEdits(store, st.profObjGuid, st.profElemId, elem, b,
                                   hexMode ? EditKind::Hex : kind);
                    if (errKey) {
                        b.err = errKey;
                    } else {
                        b.err.clear();
                        st.dirty = true;
                        // Reload the buffers from the freshly written element.
                        b.loaded = false;
                        const BcdStore::Object* o2 = store.findObject(st.profObjGuid);
                        const BcdStore::Element* e2 =
                            o2 ? BcdStore::findElement(*o2, st.profElemId) : nullptr;
                        editBufs(st.profObjGuid, st.profElemId, e2);
                    }
                }
                ImGui::SameLine();
            }
            if (ImGui::Button(T_("Delete"))) {
                char line[16];
                std::snprintf(line, sizeof(line), "0x%08X", st.profElemId);
                const std::string text =
                    std::string(T_("Delete this element?")) + "\n" + line;
                if (app.platform()->confirmDialog(T_("Confirm delete"), text)) {
                    store.removeElement(st.profObjGuid, st.profElemId);
                    st.profElemId = 0;
                    st.dirty = true;
                }
            }
            if (kind != EditKind::Hex) {
                ImGui::SameLine();
                ImGui::Checkbox(T_("Raw"), &b.raw);
            }
        }
    }

    // --- add / update ---------------------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted(T_("Add / update element"));

    static char addId[16] = "";
    static EditBufs addBufs;
    static std::string addErr;
    static bool addBoolOn = false;
    static std::string lastObjKey;
    static char lastId[16] = "";

    ImGui::SetNextItemWidth(130);
    ImGui::InputTextWithHint("##profaddid", "12000002", addId, sizeof(addId));
    ImGui::SameLine();

    uint32_t newId = 0;
    bool idOk = parseElemId(addId, &newId);
    if (addId[0] != '\0' && !idOk) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s",
                           T_("Invalid element ID."));
    } else if (!idOk) {
        ImGui::TextDisabled(
            "%s", T_("Enter an element ID in hex (e.g. 12000002) to edit it."));
    }
    if (!idOk)
        return;

    // Reset the add-pane state when the id or object changes.
    if (lastObjKey != st.profObjGuid || std::strcmp(lastId, addId) != 0) {
        lastObjKey = st.profObjGuid;
        std::snprintf(lastId, sizeof(lastId), "%s", addId);
        addErr.clear();
        const BcdStore::Element* exist0 = BcdStore::findElement(*obj, newId);
        uint32_t v = 0;
        addBoolOn = (exist0 && BcdStore::elementAsDword(*exist0, &v)) ? v != 0 : true;
    }

    const BcdStore::Element* exist = BcdStore::findElement(*obj, newId);
    const uint32_t dataType = exist ? exist->dataType : inferRegType(newId);
    const EditKind kind = kindFor(newId, exist);

    std::string_view lb = bcdElementLabel(newId);
    std::string_view rt = bcdRegTypeLabel(dataType);
    char info[256];
    if (!lb.empty() && !rt.empty())
        std::snprintf(info, sizeof(info), "0x%08X - %s - %s", newId, tr(lb).c_str(),
                      rt.data());
    else if (!rt.empty())
        std::snprintf(info, sizeof(info), "0x%08X - %s", newId, rt.data());
    else
        std::snprintf(info, sizeof(info), "0x%08X - type 0x%X", newId, dataType);
    ImGui::TextDisabled("%s", info);
    if (exist) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T_("(will replace existing element)"));
    }

    drawValueEditor("##profadd", kind, addBufs, &addBoolOn);

    if (ImGui::Button(T_("Add / Update"))) {
        const char* errKey;
        if (kind == EditKind::Boolean) {
            EditBufs tmp;
            std::snprintf(tmp.text, sizeof(tmp.text), "%u", addBoolOn ? 1u : 0u);
            errKey =
                applyEdits(store, st.profObjGuid, newId, exist, tmp, EditKind::Integer);
        } else {
            errKey = applyEdits(store, st.profObjGuid, newId, exist, addBufs, kind);
        }
        if (errKey) {
            addErr = errKey;
        } else {
            addErr.clear();
            st.dirty = true;
            st.profElemId = newId;
        }
    }
    if (!addErr.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", T_(addErr.c_str()));
    }
}

} // namespace bootroll
