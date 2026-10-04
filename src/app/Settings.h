#pragma once
// Persistent app settings stored in bootroll.ini next to the exe (key=value).
#include <string>

namespace bootroll {

struct Settings {
    std::string language = "zh_CN"; // tinygettext language code; "" = English msgid
    bool autoBackup = true;         // backup before disk writes (M3+)
    int windowWidth = 980;
    int windowHeight = 640;
    bool windowSizeFromUser = false; // ini already carried a saved size

    static Settings& instance();

    void load(const std::string& iniPath);
    void save(const std::string& iniPath) const;
};

} // namespace bootroll
