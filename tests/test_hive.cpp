// Hive roundtrip + malformed-input tests (synthetic fixtures, no real data).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/bcd/Hive.h"

#include <cstring>
#include <sstream>

using namespace bootroll;
namespace R = RegTypes;

namespace {

std::vector<uint8_t> bytes(const char* s)
{
    return std::vector<uint8_t>(s, s + strlen(s));
}

std::vector<uint8_t> dwordLE(uint32_t v)
{
    return { uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24) };
}

Hive makeSyntheticHive()
{
    Hive hive;
    hive.meta().hiveName = "CMI-CreateHive{11111111-2222-3333-4444-555555555555}";
    hive.meta().lastWriteTime = 0x01D4000000000000ull;

    HiveKey& root = *hive.root();
    root.setValue("RootValue", R::Sz, bytes("hello"));

    HiveKey& objects = root.getOrCreateChild("Objects");
    HiveKey& obj = objects.getOrCreateChild("{01234567-89ab-cdef-0123-456789abcdef}");
    HiveKey& desc = obj.getOrCreateChild("Description");
    desc.setValue("Type", R::Dword, dwordLE(0x10200003));
    desc.setValue("Name", R::Sz, bytes("Boot Manager"));
    HiveKey& elems = obj.getOrCreateChild("Elements");
    HiveKey& elem = elems.getOrCreateChild("12000002");
    elem.setValue("Element", R::Sz, bytes("Windows 引导管理器"));

    HiveKey& child2 = objects.getOrCreateChild("{ffffffff-ffff-ffff-ffff-ffffffffffff}");
    child2.setValue("Blob", R::Binary, std::vector<uint8_t>{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 });
    // A value > 4 bytes forces a separate data cell; > 4KB forces a bigger bin.
    std::vector<uint8_t> big(6000, 0xAB);
    child2.setValue("Big", R::Binary, big);
    return hive;
}

bool sameTree(const HiveKey& a, const HiveKey& b)
{
    if (a.name != b.name || a.values.size() != b.values.size() ||
        a.children.size() != b.children.size())
        return false;
    for (size_t i = 0; i < a.values.size(); ++i) {
        if (a.values[i].name != b.values[i].name || a.values[i].type != b.values[i].type ||
            a.values[i].data != b.values[i].data)
            return false;
    }
    for (size_t i = 0; i < a.children.size(); ++i)
        if (!sameTree(*a.children[i], *b.children[i]))
            return false;
    return true;
}

} // namespace

TEST_CASE("synthetic hive: parse -> serialize -> reparse roundtrip")
{
    Hive hive1 = makeSyntheticHive();
    std::vector<uint8_t> image = hive1.serialize();
    CHECK(image.size() % 4096 == 0);
    CHECK(memcmp(image.data(), "regf", 4) == 0);

    Hive hive2;
    std::string err;
    REQUIRE_MESSAGE(hive2.parse(image, &err), "parse error: " << err);
    CHECK(err.empty());

    // Serialize again must be byte-identical (deterministic writer).
    CHECK(hive2.serialize() == image);

    REQUIRE(sameTree(*hive1.root(), *hive2.root()));
}

TEST_CASE("synthetic hive: value accessors & case-insensitive lookup")
{
    Hive hive = makeSyntheticHive();
    HiveKey& root = *hive.root();

    HiveKey* obj = root.findChild("objects"); // lowercase lookup
    REQUIRE(obj != nullptr);
    CHECK(obj->name == "Objects");

    HiveKey* desc = obj->findChild("{01234567-89AB-CDEF-0123-456789ABCDEF}");
    REQUIRE(desc != nullptr);
    const HiveValue* type = desc->findChild("Description")->findValue("Type");
    REQUIRE(type != nullptr);
    CHECK(type->type == R::Dword);
    REQUIRE(type->data.size() == 4);

    // Modify via API, reserialize, verify.
    desc->getOrCreateChild("Description").setValue("Name", R::Sz, bytes("Changed"));
    std::vector<uint8_t> image = hive.serialize();
    Hive hive2;
    REQUIRE(hive2.parse(image, nullptr));
    CHECK(hive2.root()
              ->findChild("Objects")
              ->findChild("{01234567-89ab-cdef-0123-456789abcdef}")
              ->findChild("Description")
              ->findValue("Name")
              ->data == bytes("Changed"));
}

TEST_CASE("rejects garbage and truncation")
{
    std::string err;

    SUBCASE("not a hive") {
        std::vector<uint8_t> junk(4096, 0x42);
        Hive h;
        CHECK_FALSE(h.parse(junk, &err));
        CHECK_FALSE(err.empty());
    }
    SUBCASE("truncated base block") {
        Hive good = makeSyntheticHive();
        std::vector<uint8_t> image = good.serialize();
        image.resize(1024);
        Hive h;
        CHECK_FALSE(h.parse(image, &err));
    }
    SUBCASE("checksum corruption") {
        Hive good = makeSyntheticHive();
        std::vector<uint8_t> image = good.serialize();
        image[0x40] ^= 0xFF;
        Hive h;
        CHECK_FALSE(h.parse(image, &err));
    }
    SUBCASE("empty image") {
        Hive h;
        CHECK_FALSE(h.parse({}, &err));
    }
}

TEST_CASE("fresh empty hive serializes and reparses")
{
    Hive h1;
    std::vector<uint8_t> image = h1.serialize();
    Hive h2;
    std::string err;
    REQUIRE(h2.parse(image, &err));
    CHECK(h2.root() != nullptr);
    CHECK(h2.root()->children.empty());
    CHECK(h2.root()->values.empty());
}

#if defined(BOOTROLL_TEST_REAL_BCD)
// Optional fixture: BOOTROLL_TEST_REAL_BCD points at a copy of a real BCD file.
// Validates the parser against actual Windows output (read-only; never written back).
TEST_CASE("real BCD: parse and roundtrip")
{
    const char* path = getenv("BOOTROLL_TEST_REAL_BCD");
    if (!path) {
        MESSAGE("skipped: set BOOTROLL_TEST_REAL_BCD to a BCD copy to enable");
        return;
    }

    FILE* f = fopen(path, "rb");
    if (!f) {
        MESSAGE("skipped: cannot open " << path);
        return;
    }
    std::vector<uint8_t> image;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        image.insert(image.end(), buf, buf + n);
    fclose(f);
    REQUIRE(image.size() > 8192);

    Hive h1;
    std::string err;
    REQUIRE(h1.parse(image, &err));

    const HiveKey* objects = h1.root()->findChild("Objects");
    REQUIRE(objects != nullptr);
    CHECK(objects->children.size() > 0);

    std::vector<uint8_t> rebuilt = h1.serialize();
    Hive h2;
    REQUIRE(h2.parse(rebuilt, &err));
    REQUIRE(sameTree(*h1.root(), *h2.root()));
}
#endif
