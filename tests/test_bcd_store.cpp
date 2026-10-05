// BcdStore model tests (synthetic, no real BCD needed).
#include <doctest.h>

#include "core/bcd/BcdElements.h"
#include "core/bcd/BcdStore.h"
#include "core/bcd/Hive.h"
#include "core/bcd/Utf16.h"
#include "core/util/HexText.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

using namespace bootroll;
namespace R = RegTypes;

TEST_CASE("store roundtrip: create objects/elements, save, reload")
{
    BcdStore store;
    std::string bootmgrGuid = store.createObject(0x10200003, "Boot Manager");
    std::string os1 = store.createObject(0x10200007, "Windows 10");
    std::string os2 = store.createObject(0x10200007, "Windows 11");

    // bootmgr-level options
    store.setElementDword(bootmgrGuid, 0x25000004, 30); // Timeout
    store.setElementString(bootmgrGuid, 0x23000003, os1); // Default
    store.setElementGuidList(bootmgrGuid, 0x24000001, { os1, os2 }); // DisplayOrder

    // osloader entries
    store.setElementString(os1, 0x12000002, "Windows 10");
    store.setElementString(os1, 0x12000004, "\\Windows\\system32\\winload.exe");

    std::vector<uint8_t> image = store.saveBytes();
    CHECK(image.size() % 4096 == 0);

    BcdStore reloaded;
    std::string err;
    REQUIRE(reloaded.loadBytes(image, &err));

    REQUIRE(reloaded.objects().size() == 3);
    const BcdStore::Object* bootmgr = reloaded.findFirstOfType(0x10200003);
    REQUIRE(bootmgr != nullptr);
    CHECK(bootmgr->name == "Boot Manager");

    uint32_t timeout = 0;
    REQUIRE(bootmgr->elements.count(0x25000004) == 1);
    CHECK(BcdStore::elementAsDword(bootmgr->elements.at(0x25000004), &timeout));
    CHECK(timeout == 30);

    REQUIRE(bootmgr->elements.count(0x24000001) == 1);
    std::vector<std::string> order =
        BcdStore::elementAsGuidList(bootmgr->elements.at(0x24000001));
    REQUIRE(order.size() == 2);
    CHECK(order[0] == os1);
    CHECK(order[1] == os2);

    CHECK(BcdStore::stringElement(*bootmgr, 0x23000003) == os1);
    CHECK(BcdStore::stringElement(*reloaded.findObject(os1), 0x12000004) ==
          "\\Windows\\system32\\winload.exe");
}

TEST_CASE("store file save/load with atomic replace and .bak")
{
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "bootroll_tests";
    std::filesystem::create_directories(dir);
    std::filesystem::path file = dir / "test.bcd";
    std::error_code ec;
    std::filesystem::remove(file, ec);
    std::filesystem::remove(file.string() + ".bak", ec);

    BcdStore store;
    store.createObject(0x10200003, "BM");
    REQUIRE(store.saveFile(file.string()));
    CHECK(std::filesystem::exists(file));
    // Second save produces a .bak of the first version.
    REQUIRE(store.saveFile(file.string()));
    CHECK(std::filesystem::exists(file.string() + ".bak"));

    BcdStore loaded;
    std::string err;
    REQUIRE(loaded.loadFile(file.string(), &err));
    CHECK(loaded.objects().size() == 1);
    CHECK(loaded.objects()[0].type == 0x10200003);
    CHECK(loaded.objects()[0].name == "BM");

    std::filesystem::remove(file, ec);
    std::filesystem::remove(file.string() + ".bak", ec);
}

TEST_CASE("GUID tools")
{
    std::string g = guidGenerate();
    CHECK(g.size() == 38);
    CHECK(g.front() == '{');
    CHECK(g.back() == '}');
    CHECK(guidIsValid(g));

    uint8_t bytes[16];
    REQUIRE(guidToBytes(g, bytes));
    std::string g2 = guidFromBytes(bytes);
    CHECK(g == g2);

    // Known mixed-endian layout: {00112233-4455-6677-8899-aabbccddeeff}
    uint8_t expect[16] = { 0x33, 0x22, 0x11, 0x00, 0x55, 0x44, 0x77, 0x66,
                           0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
    uint8_t got[16];
    REQUIRE(guidToBytes("{00112233-4455-6677-8899-AABBCCDDEEFF}", got));
    CHECK(memcmp(expect, got, 16) == 0);

    CHECK_FALSE(guidIsValid("not-a-guid"));
}

TEST_CASE("store rejects non-BCD hive")
{
    Hive plain;
    plain.meta().hiveName = "PlainHive";
    std::vector<uint8_t> image = plain.serialize();

    BcdStore store;
    std::string err;
    CHECK_FALSE(store.loadBytes(image, &err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("string elements are UTF-16LE on disk; MULTI_SZ roundtrip")
{
    BcdStore store;
    std::string os = store.createObject(0x10200007, "中文系统");
    store.setElementString(os, 0x12000002, "中文描述：测试");

    std::vector<uint8_t> image = store.saveBytes();

    BcdStore reloaded;
    std::string err;
    REQUIRE(reloaded.loadBytes(image, &err));
    const BcdStore::Object* obj = reloaded.findObject(os);
    REQUIRE(obj != nullptr);
    CHECK(obj->name == "中文系统");
    CHECK(BcdStore::stringElement(*obj, 0x12000002) == "中文描述：测试");

    // MULTI_SZ payload layout: items NUL-separated, double NUL at the end.
    std::vector<uint8_t> payload = regMultiToBytes({ "a", "b" });
    CHECK(payload.size() == 10);
    auto parts = regMultiFromBytes(payload.data(), payload.size());
    REQUIRE(parts.size() == 2);
    CHECK(parts[0] == "a");
    CHECK(parts[1] == "b");
    // Empty list -> single NUL terminator; parses back to zero strings.
    CHECK(regMultiToBytes({}).size() == 2);
    CHECK(regMultiFromBytes(regMultiToBytes({}).data(), 2).empty());
}

TEST_CASE("BCD element/object labels and classification")
{
    CHECK(bcdClassify(0x11000001) == BcdValueClass::Device);
    CHECK(bcdClassify(0x12000002) == BcdValueClass::String);
    CHECK(bcdClassify(0x23000005) == BcdValueClass::Boolean);
    CHECK(bcdClassify(0x24000001) == BcdValueClass::GuidList);
    CHECK(bcdClassify(0x25000004) == BcdValueClass::Integer64);
    CHECK(bcdClassify(0x99999999) == BcdValueClass::Unknown);

    CHECK(bcdElementLabel(0x12000002) == "Description");
    CHECK(bcdElementLabel(0xDEADBEEF).empty());
    CHECK(bcdObjectTypeLabel(0x10200003) == "Windows Boot Manager");
    CHECK(bcdObjectTypeLabel(0x12345678).empty());

    CHECK(BcdObjectType::isApplication(0x10200007));
    CHECK_FALSE(BcdObjectType::isApplication(0x00100000));
}

TEST_CASE("element groups and extended labels (M2)")
{
    CHECK(bcdElementGroup(0x12000002) == "Library");
    CHECK(bcdElementGroup(0x25000004) == "Boot manager");
    CHECK(bcdElementGroup(0x21000001) == "OS loader");
    CHECK(bcdElementGroup(0xDEADBEEF).empty());

    CHECK(bcdElementLabel(0x12000036) == "Kernel path");
    CHECK(bcdElementLabel(0x12000037) == "HAL path");
    CHECK(bcdElementLabel(0x22000005) == "NX policy");
    CHECK(bcdElementLabel(0x23000003) == "Default object");
    CHECK(bcdObjectTypeLabel(0x10100002) == "Firmware Boot Manager");
    CHECK(bcdObjectTypeLabel(0x00200004) == "Inherit (device options)");
    CHECK(bcdRegTypeLabel(RegTypes::MultiSz) == "REG_MULTI_SZ");
    CHECK(bcdRegTypeLabel(0x1234).empty());
}

TEST_CASE("value preview: strings, lists, integers, hex fallback")
{
    // REG_SZ / REG_EXPAND_SZ decode UTF-16LE.
    CHECK(bcdElementValuePreview(RegTypes::Sz, utf8ToUtf16le("abc")) == "abc");
    // Control chars (incl. NUL) flatten to '?' so rows stay single-line.
    std::vector<uint8_t> ctrl = { 'a', 0, 0x01, 0, 0, 0 };
    CHECK(bcdElementValuePreview(RegTypes::ExpandSz, ctrl) == "a??");

    // REG_MULTI_SZ joins with " | ".
    CHECK(bcdElementValuePreview(RegTypes::MultiSz, regMultiToBytes({ "x", "y" })) ==
          "x | y");

    // Integers print "dec (0xHEX)".
    std::vector<uint8_t> dw = { 0x2A, 0, 0, 0 };
    CHECK(bcdElementValuePreview(RegTypes::Dword, dw) == "42 (0x0000002A)");
    std::vector<uint8_t> qw(8);
    qw[0] = 1;
    CHECK(bcdElementValuePreview(RegTypes::Qword, qw) ==
          "1 (0x0000000000000001)");

    // Payload that does not fit the declared type degrades to a hex dump.
    std::vector<uint8_t> bad = { 1, 2 };
    CHECK(bcdElementValuePreview(RegTypes::Dword, bad) == "01 02 (2 B)");
    CHECK(bcdElementValuePreview(RegTypes::Binary, {}).empty());
}

TEST_CASE("hex text <-> bytes")
{
    std::vector<uint8_t> out;
    REQUIRE(hexTextToBytes("0A 1B 2C", &out));
    CHECK((out == std::vector<uint8_t>{0x0A, 0x1B, 0x2C}));

    REQUIRE(hexTextToBytes("0a,1b_2c", &out)); // separators
    CHECK(out.size() == 3);
    REQUIRE(hexTextToBytes("0A1B2C", &out)); // contiguous
    CHECK(out.size() == 3);
    REQUIRE(hexTextToBytes("0A 0x1B", &out)); // "0x" between pairs
    CHECK(out.size() == 2);

    CHECK(hexTextToBytes("", &out));
    CHECK(out.empty());
    CHECK_FALSE(hexTextToBytes("0A1", &out));  // odd digit count
    CHECK_FALSE(hexTextToBytes("0G", &out));   // non-hex digit
    CHECK_FALSE(hexTextToBytes("0A 1Z", &out));

    CHECK(bytesToHexText(std::vector<uint8_t>{0x0A, 0x1B}, true) == "0a 1b");
    CHECK(bytesToHexText(std::vector<uint8_t>{0x0A, 0x1B}, false) == "0a1b");
}
