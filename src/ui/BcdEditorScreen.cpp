#include "ui/BcdEditorScreen.h"

#include "app/App.h"
#include "app/I18n.h"
#include "core/bcd/BcdElements.h"
#include "ui/BcdProfessionalScreen.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace bootroll {

namespace {

std::string trimCopy(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string toLower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower((unsigned char)c));
    return s;
}

// Entry display label: Description element, falling back to the key name.
std::string entryLabel(const BcdStore::Object& obj)
{
    std::string d = BcdStore::stringElement(obj, BcdElementId::Description);
    return d.empty() ? obj.name : d;
}

// Launchable OS entries (what the boot menu lists).
std::vector<const BcdStore::Object*> osEntries(const BcdStore& store)
{
    std::vector<const BcdStore::Object*> out;
    for (const BcdStore::Object& o : store.objects())
        if (o.type == BcdObjectType::OsLoader || o.type == BcdObjectType::LegacyNtldr)
            out.push_back(&o);
    return out;
}

bool boolElement(const BcdStore::Object& obj, uint32_t id, bool dflt)
{
    const BcdStore::Element* e = BcdStore::findElement(obj, id);
    uint32_t v = 0;
    return (e && BcdStore::elementAsDword(*e, &v)) ? v != 0 : dflt;
}

std::vector<std::string> displayOrder(const BcdStore& store)
{
    const BcdStore::Object* bm = store.findFirstOfType(BcdObjectType::BootManager);
    if (!bm)
        return {};
    const BcdStore::Element* e = BcdStore::findElement(*bm, BcdElementId::DisplayOrder);
    return e ? BcdStore::elementAsGuidList(*e) : std::vector<std::string>{};
}

void writeDisplayOrder(BcdStore& store, const std::vector<std::string>& order)
{
    const BcdStore::Object* bm = store.findFirstOfType(BcdObjectType::BootManager);
    if (!bm)
        return;
    if (order.empty())
        store.removeElement(bm->guid, BcdElementId::DisplayOrder);
    else
        store.setElementGuidList(bm->guid, BcdElementId::DisplayOrder, order);
}

} // namespace

void BcdEditorScreen::drawBody(App& app)
{
    App::BcdEditState& st = app.bcdEdit();
    BcdStore& store = st.store;

    // --- shared file row -------------------------------------------------------
    static char pathBuf[1024];
    ImGui::TextUnformatted(T_("BCD file:"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(360);
    ImGui::InputText("##bcdpath", pathBuf, sizeof(pathBuf));

    ImGui::SameLine();
    if (ImGui::Button(T_("Open"))) {
        std::string p = trimCopy(pathBuf);
        std::string err;
        if (p.empty()) {
            st.error = T_("No file specified.");
        } else if (store.loadFile(p, &err)) {
            st.path = p;
            st.error.clear();
            st.dirty = false;
        } else {
            st.error = err;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(T_("Save"))) {
        std::string p = trimCopy(pathBuf);
        std::string err;
        if (p.empty()) {
            st.error = T_("No file specified.");
        } else if (store.saveFile(p, &err)) {
            st.path = p;
            st.error.clear();
            st.dirty = false;
        } else {
            st.error = err;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(T_("New"))) {
        store = BcdStore{};
        store.createObject(BcdObjectType::BootManager, "Boot Manager");
        st.path.clear();
        st.dirty = true;
        st.error.clear();
        pathBuf[0] = '\0';
    }

    // Status line (error > unsaved > path).
    if (!st.error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", st.error.c_str());
    } else if (st.dirty) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "* %s", T_("Unsaved changes"));
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.55f, 0.6f, 1.0f), "%s", st.path.c_str());
    }

    // --- mode sub-tabs (BOOTICE: Easy / Professional) ---------------------------
    ImGui::Spacing();
    if (ImGui::BeginTabBar("##bcdmodetabs")) {
        if (ImGui::BeginTabItem(T_("Easy"))) {
            drawEasyMode(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(T_("Professional"))) {
            BcdProfessionalScreen::drawBody(app);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void BcdEditorScreen::drawEasyMode(App& app)
{
    App::BcdEditState& st = app.bcdEdit();
    BcdStore& store = st.store;

    const BcdStore::Object* bm = store.findFirstOfType(BcdObjectType::BootManager);
    if (!bm) {
        ImGui::TextWrapped("%s", T_("No boot manager object in this store."));
        return;
    }

    // --- boot manager options -------------------------------------------------
    int timeout = 30;
    if (const BcdStore::Element* e = BcdStore::findElement(*bm, BcdElementId::Timeout)) {
        uint32_t v = 30;
        if (BcdStore::elementAsDword(*e, &v))
            timeout = (int)v;
    }
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputInt(T_("Timeout (s)"), &timeout)) {
        timeout = std::clamp(timeout, 0, 999);
        store.setElementDword(bm->guid, BcdElementId::Timeout, (uint32_t)timeout);
        st.dirty = true;
    }

    bool showMenu = boolElement(*bm, BcdElementId::DisplayBootMenu, true);
    if (ImGui::Checkbox(T_("Display boot menu"), &showMenu)) {
        store.setElementDword(bm->guid, BcdElementId::DisplayBootMenu, showMenu ? 1u : 0u);
        st.dirty = true;
    }
    ImGui::SameLine();
    bool noErrUi = boolElement(*bm, BcdElementId::NoErrorUi, false);
    if (ImGui::Checkbox(T_("No error UI"), &noErrUi)) {
        store.setElementDword(bm->guid, BcdElementId::NoErrorUi, noErrUi ? 1u : 0u);
        st.dirty = true;
    }

    // Default entry picker.
    std::vector<const BcdStore::Object*> entries = osEntries(store);
    const std::string defGuid = toLower(BcdStore::stringElement(*bm, BcdElementId::DefaultObject));
    const BcdStore::Object* defEntry = nullptr;
    for (const BcdStore::Object* o : entries)
        if (o->guid == defGuid) {
            defEntry = o;
            break;
        }
    std::string defLabel = defEntry ? entryLabel(*defEntry) : T_("(none)");

    ImGui::SetNextItemWidth(280);
    if (ImGui::BeginCombo(T_("Default entry"), defLabel.c_str())) {
        for (const BcdStore::Object* o : entries) {
            std::string lbl = entryLabel(*o) + "  (" + o->guid + ")";
            if (ImGui::Selectable(lbl.c_str(), o->guid == defGuid)) {
                store.setElementString(bm->guid, BcdElementId::DefaultObject, o->guid);
                st.dirty = true;
            }
        }
        ImGui::EndCombo();
    }

    // --- entries table ----------------------------------------------------------
    static std::string selGuid;
    ImGui::Spacing();
    ImGui::TextUnformatted(T_("Entries"));
    ImGui::Separator();

    static ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_Resizable |
                                        ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##bcdentries", 4, tableFlags)) {
        ImGui::TableSetupColumn(T_("Description"));
        ImGui::TableSetupColumn(T_("Path"));
        ImGui::TableSetupColumn(T_("Type"), ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn(T_("GUID"), ImGuiTableColumnFlags_WidthFixed, 300);
        ImGui::TableHeadersRow();
        for (const BcdStore::Object* o : entries) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            bool sel = selGuid == o->guid;
            if (ImGui::Selectable(entryLabel(*o).c_str(), &sel,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                selGuid = sel ? o->guid : std::string();
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(BcdStore::stringElement(*o, BcdElementId::Path).c_str());
            ImGui::TableSetColumnIndex(2);
            std::string_view tl = bcdObjectTypeLabel(o->type);
            if (!tl.empty()) {
                ImGui::TextUnformatted(tl.data());
            } else {
                char tbuf[16];
                std::snprintf(tbuf, sizeof(tbuf), "0x%08X", o->type);
                ImGui::TextUnformatted(tbuf);
            }
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(o->guid.c_str());
        }
        ImGui::EndTable();
    }

    // --- entry actions ----------------------------------------------------------
    ImGui::Spacing();
    static char newDesc[128];
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##newentry", T_("New entry description"), newDesc,
                             sizeof(newDesc));
    ImGui::SameLine();
    if (ImGui::Button(T_("Add"))) {
        std::string desc = trimCopy(newDesc);
        if (desc.empty()) {
            st.error = T_("Entry description is empty.");
        } else {
            std::string g = store.createObject(BcdObjectType::OsLoader, desc);
            store.setElementString(g, BcdElementId::Description, desc);
            store.setElementString(g, BcdElementId::Path, "\\Windows\\system32\\winload.exe");
            std::vector<std::string> order = displayOrder(store);
            order.push_back(g);
            writeDisplayOrder(store, order);
            selGuid = g;
            st.dirty = true;
            st.error.clear();
            newDesc[0] = '\0';
        }
    }

    const bool haveSel = !selGuid.empty() && store.findObject(selGuid) != nullptr;
    ImGui::BeginDisabled(!haveSel);
    ImGui::SameLine();
    if (ImGui::Button(T_("Set as default")) && haveSel) {
        store.setElementString(bm->guid, BcdElementId::DefaultObject, selGuid);
        st.dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(T_("Delete")) && haveSel) {
        const std::string text =
            std::string(T_("Delete this entry?")) + "\n" + selGuid;
        if (app.platform()->confirmDialog(T_("Confirm delete"), text)) {
            std::vector<std::string> order = displayOrder(store);
            order.erase(std::remove(order.begin(), order.end(), selGuid), order.end());
            writeDisplayOrder(store, order);
            const BcdStore::Object* bmNow = store.findFirstOfType(BcdObjectType::BootManager);
            if (bmNow && toLower(BcdStore::stringElement(*bmNow, BcdElementId::DefaultObject)) ==
                             selGuid)
                store.removeElement(bmNow->guid, BcdElementId::DefaultObject);
            store.deleteObject(selGuid);
            selGuid.clear();
            st.dirty = true;
            st.error.clear();
        }
    }
    ImGui::EndDisabled();
}

} // namespace bootroll
