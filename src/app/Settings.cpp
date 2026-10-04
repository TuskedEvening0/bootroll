#include "app/Settings.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <string>

namespace bootroll {

namespace {

std::map<std::string, std::string> readIni(const std::string& path)
{
    std::map<std::string, std::string> kv;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return kv;
    }
    std::string line;
    while (std::getline(in, line)) {
        // strip BOM / CR
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (line.rfind("\xEF\xBB\xBF", 0) == 0) {
            line.erase(0, 3);
        }
        if (line.empty() || line[0] == '#' || line[0] == '[') {
            continue;
        }
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            kv[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    return kv;
}

} // namespace

Settings& Settings::instance()
{
    static Settings s;
    return s;
}

void Settings::load(const std::string& path)
{
    auto kv = readIni(path);
    if (auto it = kv.find("language"); it != kv.end()) {
        language = it->second;
    }
    if (auto it = kv.find("autoBackup"); it != kv.end()) {
        autoBackup = it->second == "1" || it->second == "true";
    }
    if (auto it = kv.find("windowWidth"); it != kv.end()) {
        int v = std::atoi(it->second.c_str());
        if (v >= 320 && v <= 8192) {
            windowWidth = v;
            windowSizeFromUser = true;
        }
    }
    if (auto it = kv.find("windowHeight"); it != kv.end()) {
        int v = std::atoi(it->second.c_str());
        if (v >= 240 && v <= 8192) {
            windowHeight = v;
        }
    }
}

void Settings::save(const std::string& path) const
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return;
    }
    out << "# bootroll settings\n";
    out << "language=" << language << "\n";
    out << "autoBackup=" << (autoBackup ? 1 : 0) << "\n";
    out << "windowWidth=" << windowWidth << "\n";
    out << "windowHeight=" << windowHeight << "\n";
}

} // namespace bootroll
