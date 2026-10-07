// Unit tests for the M11 quick-add loader detection (core/uefi/LoaderPresets).
// Pure logic: the ESP is simulated by a set of existing backslash paths with
// case-insensitive lookup, mirroring FatVolume::findEntry semantics.
#include "core/uefi/LoaderPresets.h"

#include "doctest.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <set>
#include <string>

using namespace bootroll;

namespace {

// Case-insensitive ESP simulation: lookup by lowercase path.
class FakeEsp {
public:
    FakeEsp(std::initializer_list<const char*> paths)
    {
        for (const char* p : paths) {
            m_paths.insert(lower(p));
        }
    }

    bool exists(const std::string& path) const
    {
        return m_paths.count(lower(path)) != 0;
    }

private:
    static std::string lower(std::string s)
    {
        for (char& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    }
    std::set<std::string> m_paths;
};

std::vector<std::string> ids(const std::vector<LoaderHit>& hits)
{
    std::vector<std::string> out;
    for (const LoaderHit& h : hits) {
        out.push_back(h.id);
    }
    return out;
}

bool contains(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

TEST_CASE("systemd-boot is detected at its primary path")
{
    FakeEsp esp = {"\\EFI\\systemd\\systemd-bootx64.efi"};
    const auto hits = detectEspLoaders(
        [&esp](const std::string& p) { return esp.exists(p); });
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].id == "systemd-boot");
    CHECK(hits[0].description == "Linux Boot Manager");
    CHECK(hits[0].efiPath == "\\EFI\\systemd\\systemd-bootx64.efi");
    CHECK(hits[0].note.empty()); // primary-path match carries no caveat
}

TEST_CASE("Limine is detected at its dedicated directory")
{
    FakeEsp esp = {"\\EFI\\LIMINE\\LIMINE.EFI"};
    const auto hits = detectEspLoaders(
        [&esp](const std::string& p) { return esp.exists(p); });
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].id == "limine");
    CHECK(hits[0].description == "Limine");
    CHECK(hits[0].efiPath == "\\EFI\\LIMINE\\LIMINE.EFI");
    CHECK(hits[0].note.empty());
}

TEST_CASE("rEFInd is detected at its dedicated directory")
{
    FakeEsp esp = {"\\EFI\\refind\\refind_x64.efi"};
    const auto hits = detectEspLoaders(
        [&esp](const std::string& p) { return esp.exists(p); });
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].id == "refind");
    CHECK(hits[0].description == "rEFInd Boot Manager");
}

TEST_CASE("BOOTX64.EFI alone becomes a verify-flagged systemd fallback")
{
    // ROADMAP M11: BOOTX64.EFI is systemd-boot's documented fallback path -
    // reported when nothing else matches, with a note asking the user to
    // verify the description (any loader may be named BOOTX64.EFI).
    FakeEsp esp = {"\\EFI\\BOOT\\BOOTX64.EFI", "\\readme.txt"};
    const auto hits = detectEspLoaders(
        [&esp](const std::string& p) { return esp.exists(p); });
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].id == "systemd-boot");
    CHECK(hits[0].efiPath == "\\EFI\\BOOT\\BOOTX64.EFI");
    CHECK(!hits[0].note.empty()); // asks the user to verify
}

TEST_CASE("BOOTX64.EFI + limine.conf confirms a Limine hit")
{
    FakeEsp esp = {"\\EFI\\BOOT\\BOOTX64.EFI", "\\limine.conf"};
    const auto hits = detectEspLoaders(
        [&esp](const std::string& p) { return esp.exists(p); });
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].id == "limine");
    CHECK(hits[0].efiPath == "\\EFI\\BOOT\\BOOTX64.EFI");
    CHECK(!hits[0].note.empty()); // evidence note present
}

TEST_CASE("limine.cfg next to the fallback copy is also accepted as evidence")
{
    FakeEsp esp = {"\\EFI\\BOOT\\BOOTX64.EFI", "\\EFI\\BOOT\\LIMINE.CFG"};
    const auto hits = detectEspLoaders(
        [&esp](const std::string& p) { return esp.exists(p); });
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].id == "limine");
}

TEST_CASE("limine config evidence beats the systemd fallback guess")
{
    // BOOTX64.EFI could be either loader; the documented Limine marker wins.
    FakeEsp esp = {"\\EFI\\BOOT\\BOOTX64.EFI", "\\EFI\\LIMINE\\LIMINE.CONF"};
    const auto hits = detectEspLoaders(
        [&esp](const std::string& p) { return esp.exists(p); });
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].id == "limine");
}

TEST_CASE("primary systemd path suppresses the BOOTX64 fallback attribution")
{
    // systemd installs to EFI/systemd AND copies to EFI/BOOT; one entry only.
    FakeEsp esp = {"\\EFI\\systemd\\systemd-bootx64.efi", "\\EFI\\BOOT\\BOOTX64.EFI"};
    const auto hits = detectEspLoaders(
        [&esp](const std::string& p) { return esp.exists(p); });
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].id == "systemd-boot");
    CHECK(hits[0].efiPath == "\\EFI\\systemd\\systemd-bootx64.efi");
    CHECK(hits[0].note.empty());
}

TEST_CASE("multiple loaders coexist with one hit each")
{
    FakeEsp esp = {"\\EFI\\systemd\\systemd-bootx64.efi",
                   "\\EFI\\LIMINE\\LIMINE.EFI",
                   "\\EFI\\refind\\refind_x64.efi",
                   "\\EFI\\BOOT\\BOOTX64.EFI"};
    const auto hits = detectEspLoaders(
        [&esp](const std::string& p) { return esp.exists(p); });
    REQUIRE(hits.size() == 3);
    const auto v = ids(hits);
    CHECK(contains(v, "systemd-boot"));
    CHECK(contains(v, "limine"));
    CHECK(contains(v, "refind"));
}

TEST_CASE("an empty or foreign ESP yields no hits")
{
    FakeEsp esp = {"\\EFI\\Microsoft\\Boot\\bootmgfw.efi", "\\bootmgr"};
    const auto hits = detectEspLoaders(
        [&esp](const std::string& p) { return esp.exists(p); });
    CHECK(hits.empty());
}

TEST_CASE("the probe receives backslash-separated paths")
{
    std::set<std::string> seen;
    FakeEsp esp = {"\\EFI\\systemd\\systemd-bootx64.efi"};
    (void)esp;
    (void)detectEspLoaders([&seen](const std::string& p) {
        seen.insert(p);
        return false;
    });
    REQUIRE(!seen.empty());
    for (const std::string& p : seen) {
        CHECK(p.find('/') == std::string::npos);
        CHECK(p.front() == '\\');
    }
}
