#include "core/uefi/UefiVars.h"
#include "core/bcd/Utf16.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace bootroll {

namespace {

uint16_t rd16(const uint8_t* p)
{
    return uint16_t(p[0] | (p[1] << 8));
}

uint32_t rd32(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

uint64_t rd64(const uint8_t* p)
{
    return uint64_t(rd32(p)) | (uint64_t(rd32(p + 4)) << 32);
}

void push16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back(uint8_t(x & 0xFF));
    v.push_back(uint8_t(x >> 8));
}

void push32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(uint8_t(x & 0xFF));
    v.push_back(uint8_t((x >> 8) & 0xFF));
    v.push_back(uint8_t((x >> 16) & 0xFF));
    v.push_back(uint8_t((x >> 24) & 0xFF));
}

void push64(std::vector<uint8_t>& v, uint64_t x)
{
    push32(v, uint32_t(x & 0xFFFFFFFF));
    push32(v, uint32_t(x >> 32));
}

int hexVal(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool hexDecode(const std::string& s, std::vector<uint8_t>* out)
{
    if (s.size() % 2 != 0) {
        return false;
    }
    out->clear();
    out->reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        const int hi = hexVal(s[i]);
        const int lo = hexVal(s[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out->push_back(uint8_t((hi << 4) | lo));
    }
    return true;
}

std::string hexStr(uint32_t v, int width)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), width == 4 ? "%04X" : "%08X", v);
    return buf;
}

char lowerCh(char c)
{
    return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

} // namespace

bool parseLoadOption(const std::vector<uint8_t>& raw, BootEntry* out)
{
    *out = BootEntry{};
    if (raw.size() < 6) {
        return false;
    }
    out->raw = raw;
    out->attrs = rd32(raw.data());
    const size_t fpLen = rd16(raw.data() + 4);

    // Description: UTF-16LE, NUL-terminated, starting right after the header.
    // It must end inside the declared FilePathList region; a missing NUL
    // truncates it there instead of running into unrelated bytes.
    const size_t fpEnd = std::min(raw.size(), size_t(6) + fpLen);
    size_t descEnd = fpEnd;
    bool foundNul = false;
    for (size_t i = 6; i + 1 < fpEnd; i += 2) {
        if (raw[i] == 0 && raw[i + 1] == 0) {
            descEnd = i;
            foundNul = true;
            break;
        }
    }
    out->desc = utf16leToUtf8(raw.data() + 6, descEnd - 6);

    // Device path: walk the nodes inside the declared FilePathList region.
    const size_t pathStart = foundNul ? descEnd + 2 : descEnd;
    const size_t dpEnd = std::min(raw.size(), pathStart + fpLen);
    size_t p = pathStart;
    while (p + 4 <= dpEnd) {
        const uint8_t type = raw[p];
        const uint8_t subType = raw[p + 1];
        const size_t len = rd16(raw.data() + p + 2);
        if (len < 4 || p + len > dpEnd) {
            break; // malformed node: keep what we have
        }
        if (!out->hasDp) {
            out->hasDp = true;
            out->firstDpType = type;
            out->firstDpSubType = subType;
        }
        if (type == 0x7F) {
            break; // end-of-device-path node
        }
        if (type == 4 && (subType == 1 || subType == 3) && len >= 42) {
            // MEDIA_HARDDRIVE_DP: partition, start, size, signature[16],
            // MBR type, signature type. subType 1 is the documented value
            // (UEFI spec "Media Device Path"); 3 is accepted for entries
            // written by older bootroll builds, which used the wrong byte.
            const uint8_t* n = raw.data() + p + 4;
            out->hasHdNode = true;
            out->hdPartition = rd32(n);
            out->hdPartStart = rd64(n + 4);
            out->hdPartSize = rd64(n + 12);
            out->hdSignature.assign(n + 20, n + 36);
            out->hdSignatureType = n[37];
        }
        if (type == 4 && subType == 4) {
            out->path += utf16leToUtf8(raw.data() + p + 4, len - 4);
        }
        p += len;
    }
    return true;
}

std::vector<uint8_t> packLoadOptionFull(uint32_t attrs, const std::string& descUtf8,
                                        const HdPathSpec* hd,
                                        const std::string& filePathUtf8)
{
    std::string path = filePathUtf8;
    for (char& c : path) {
        if (c == '/') {
            c = '\\';
        }
    }
    if (!path.empty() && path[0] != '\\') {
        path.insert(path.begin(), '\\');
    }

    std::vector<uint8_t> desc16 = utf8ToUtf16le(descUtf8);
    std::vector<uint8_t> path16 = utf8ToUtf16le(path);

    // MEDIA_HARDDRIVE_DP node (38-byte payload, 42 with the DP header).
    std::vector<uint8_t> hdNode;
    if (hd) {
        hdNode.push_back(4);  // MEDIA_DEVICE_PATH
        hdNode.push_back(1);  // MEDIA_HARDDRIVE_DP (spec subType 1; 3 is vendor)
        push16(hdNode, 42);
        push32(hdNode, hd->partition);
        push64(hdNode, hd->startLba);
        push64(hdNode, hd->sizeLba);
        hdNode.resize(hdNode.size() + 16, 0); // signature
        for (size_t i = 0; i < hd->signature.size() && i < 16; ++i) {
            hdNode[hdNode.size() - 16 + i] = hd->signature[i];
        }
        // MBR type: 1 = PC-AT MBR, 2 = GPT (mirrors the signature type).
        hdNode.push_back(hd->signatureType == 2 ? 2 : 1);
        hdNode.push_back(hd->signatureType);
    }

    std::vector<uint8_t> out;
    out.reserve(6 + desc16.size() + 2 + hdNode.size() + 8 + path16.size());
    push32(out, attrs);
    // FilePathListLength: [HD node] + file-path node + end node.
    push16(out, uint16_t(hdNode.size() + 8 + path16.size()));
    out.insert(out.end(), desc16.begin(), desc16.end());
    out.push_back(0); // description NUL terminator
    out.push_back(0);
    out.insert(out.end(), hdNode.begin(), hdNode.end());
    out.push_back(4); // MEDIA_DEVICE_PATH
    out.push_back(4); // MEDIA_FILEPATH_DP
    push16(out, uint16_t(4 + path16.size()));
    out.insert(out.end(), path16.begin(), path16.end());
    out.push_back(0x7F); // END_DEVICE_PATH
    out.push_back(0xFF); // EndEntire
    push16(out, 4);
    return out;
}

std::vector<uint8_t> packLoadOption(uint32_t attrs, const std::string& descUtf8,
                                    const std::string& filePathUtf8)
{
    return packLoadOptionFull(attrs, descUtf8, nullptr, filePathUtf8);
}

std::string devicePathTypeLabel(uint8_t type, uint8_t subType)
{
    char buf[48];
    switch (type) {
    case 0x01:
        return "Hardware Device Path (0x01)";
    case 0x02:
        return "ACPI Device Path (0x02)";
    case 0x03:
        return "Messaging Device Path (0x03)";
    case 0x04:
        switch (subType) {
        case 0x01: return "Media Device Path (0x04) - Hard Drive";
        case 0x02: return "Media Device Path (0x04) - CD-ROM";
        case 0x04: return "Media Device Path (0x04) - File Path";
        default:
            std::snprintf(buf, sizeof(buf), "Media Device Path (0x04, subType 0x%02X)",
                          subType);
            return buf;
        }
    case 0x05:
        return "BIOS Boot Specification (0x05)";
    case 0x7F:
        return "End of Device Path (0x7F)";
    default:
        std::snprintf(buf, sizeof(buf), "Unknown Device Path (type 0x%02X, subType 0x%02X)",
                      type, subType);
        return buf;
    }
}

std::vector<uint16_t> decodeBootOrder(const std::vector<uint8_t>& bytes)
{
    std::vector<uint16_t> order;
    order.reserve(bytes.size() / 2);
    for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
        order.push_back(rd16(bytes.data() + i)); // odd trailing byte dropped
    }
    return order;
}

std::vector<uint8_t> encodeBootOrder(const std::vector<uint16_t>& order)
{
    std::vector<uint8_t> bytes;
    bytes.reserve(order.size() * 2);
    for (uint16_t n : order) {
        push16(bytes, n);
    }
    return bytes;
}

std::string bootVarName(uint16_t n)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "Boot%04X", n);
    return buf;
}

bool parseBootVarName(const std::string& name, uint16_t* n)
{
    if (name.size() != 8) {
        return false;
    }
    static const char kPrefix[5] = "boot";
    for (int i = 0; i < 4; ++i) {
        if (lowerCh(name[i]) != kPrefix[i]) {
            return false;
        }
    }
    uint32_t v = 0;
    for (int i = 4; i < 8; ++i) {
        const int d = hexVal(name[i]);
        if (d < 0) {
            return false;
        }
        v = v * 16 + uint32_t(d);
    }
    *n = uint16_t(v);
    return true;
}

uint16_t nextFreeBootNumber(const std::set<uint16_t>& used)
{
    for (uint32_t n = 0; n <= 0xFFFF; ++n) {
        if (used.count(uint16_t(n)) == 0) {
            return uint16_t(n);
        }
    }
    return 0xFFFF;
}

std::string writeUefiBackupText(const UefiBackup& b)
{
    std::string s = "# bootroll UEFI backup v1\n";
    if (b.hasTimeout) {
        s += "timeout=" + std::to_string(b.timeout) + "\n";
    }
    if (b.hasBootNext) {
        s += "bootnext=" + hexStr(b.bootNext, 4) + "\n";
    }
    s += "bootorder=";
    for (size_t i = 0; i < b.bootOrder.size(); ++i) {
        if (i != 0) {
            s += ",";
        }
        s += hexStr(b.bootOrder[i], 4);
    }
    s += "\n";
    for (const auto& entry : b.entries) {
        s += "[Boot" + hexStr(entry.first, 4) + "]\n";
        s += "attrs=" + hexStr(entry.second.size() >= 4 ? rd32(entry.second.data())
                                                        : 0, 8) +
             "\n";
        s += "raw=";
        static const char* kHex = "0123456789ABCDEF";
        for (uint8_t byte : entry.second) {
            s += kHex[byte >> 4];
            s += kHex[byte & 0xF];
        }
        s += "\n";
    }
    return s;
}

bool parseUefiBackupText(const std::string& text, UefiBackup* out)
{
    *out = UefiBackup{};
    std::istringstream in(text);
    std::string line;
    int current = -1; // [Boot####] header whose raw= line we expect next
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (line.compare(0, 5, "[Boot") == 0) {
            if (line.size() != 10 || line[9] != ']') {
                return false;
            }
            uint32_t v = 0;
            for (int i = 5; i < 9; ++i) {
                const int d = hexVal(line[i]);
                if (d < 0) {
                    return false;
                }
                v = v * 16 + uint32_t(d);
            }
            current = int(v);
            continue;
        }
        if (line.compare(0, 8, "timeout=") == 0) {
            const std::string digits = line.substr(8);
            if (digits.empty() || digits.size() > 5) {
                return false;
            }
            unsigned long v = 0;
            for (char c : digits) {
                if (c < '0' || c > '9') {
                    return false;
                }
                v = v * 10 + unsigned(c - '0');
            }
            if (v > 0xFFFF) {
                return false;
            }
            out->hasTimeout = true;
            out->timeout = uint16_t(v);
            continue;
        }
        if (line.compare(0, 9, "bootnext=") == 0) {
            std::vector<uint8_t> bytes;
            if (!hexDecode(line.substr(9), &bytes) || bytes.size() != 2) {
                return false;
            }
            // 4 hex digits are a plain number, high byte first.
            out->hasBootNext = true;
            out->bootNext = uint16_t((bytes[0] << 8) | bytes[1]);
            continue;
        }
        if (line.compare(0, 10, "bootorder=") == 0) {
            const std::string list = line.substr(10);
            if (!list.empty()) {
                std::istringstream tokens(list);
                std::string token;
                while (std::getline(tokens, token, ',')) {
                    std::vector<uint8_t> bytes;
                    if (!hexDecode(token, &bytes) || bytes.size() != 2) {
                        return false;
                    }
                    out->bootOrder.push_back(
                        uint16_t((bytes[0] << 8) | bytes[1]));
                }
            }
            continue;
        }
        if (line.compare(0, 6, "attrs=") == 0) { // informational; raw is authoritative
            std::vector<uint8_t> bytes;
            if (!hexDecode(line.substr(6), &bytes) || bytes.size() != 4) {
                return false;
            }
            continue;
        }
        if (line.compare(0, 4, "raw=") == 0) {
            if (current < 0) {
                return false;
            }
            std::vector<uint8_t> bytes;
            if (!hexDecode(line.substr(4), &bytes)) {
                return false;
            }
            out->entries[uint16_t(current)] = std::move(bytes);
            continue;
        }
        return false; // unknown line: reject the whole file
    }
    return true;
}

} // namespace bootroll
