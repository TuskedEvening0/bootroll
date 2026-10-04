#pragma once
// gettext-style i18n via tinygettext. .po/.mo catalogs are embedded at build time
// and exposed as in-memory files, so the exe stays fully portable (single file).
#include <string>
#include <vector>

#include <tinygettext/dictionary_manager.hpp>

namespace bootroll {

class I18n {
public:
    static I18n& instance();

    // Register embedded catalogs. Safe to call once at startup.
    void init();

    // "zh_CN", "en_US" or "" (msgid passthrough).
    void setLanguage(const std::string& lang);
    const std::string& language() const { return m_language; }
    std::vector<std::string> availableLanguages() const;

    std::string translate(const std::string& msgid) const;

private:
    I18n();
    tinygettext::DictionaryManager m_mgr;
    std::string m_language;
};

} // namespace bootroll

// Returns const char* (valid for the duration of the full expression), so the
// result can go straight into printf-style ImGui varargs.
#define T_(msgid) bootroll::I18n::instance().translate(msgid).c_str()
