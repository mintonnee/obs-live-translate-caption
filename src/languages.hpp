#pragma once
#include <string>

namespace lt {
struct LangEntry {
    const char *code;
    const char *name;
};

// Translation target languages offered in the filter's Target Language combo.
// `code` is the BCP-47 value that identifies the language to the translation
// prompt; `name` is what the combo shows.
// Each name is the language's own endonym plus the English name in parentheses,
// so both native speakers and non-speakers can recognize it. Common languages
// are listed first; the rest follow in English-name alphabetical order.
// Names use plain narrow string literals; the Windows build compiles with /utf-8
// (cmake/windows/compilerconfig.cmake) so these are UTF-8 bytes, which is what
// obs_property_list_add_string expects.
static const LangEntry kLanguages[] = {
    // Common (pinned to the top of the dropdown)
    {"en", "English"},
    {"zh", "中文 (Chinese)"},
    {"ja", "日本語 (Japanese)"},
    {"ko", "한국어 (Korean)"},
    {"es", "Español (Spanish)"},
    {"fr", "Français (French)"},
    {"de", "Deutsch (German)"},
    {"pt-BR", "Português (Portuguese, BR)"},
    {"it", "Italiano (Italian)"},
    {"ru", "Русский (Russian)"},
    {"id", "Bahasa Indonesia (Indonesian)"},
    {"th", "ไทย (Thai)"},
    {"vi", "Tiếng Việt (Vietnamese)"},

    // Remaining languages, by English name (A→Z)
    {"af", "Afrikaans"},
    {"ak", "Akan"},
    {"sq", "Shqip (Albanian)"},
    {"am", "አማርኛ (Amharic)"},
    {"ar", "العربية (Arabic)"},
    {"hy", "Հայերեն (Armenian)"},
    {"az", "Azərbaycan (Azerbaijani)"},
    {"eu", "Euskara (Basque)"},
    {"be", "Беларуская (Belarusian)"},
    {"bn", "বাংলা (Bengali)"},
    {"bg", "Български (Bulgarian)"},
    {"my", "မြန်မာ (Burmese)"},
    {"ca", "Català (Catalan)"},
    {"hr", "Hrvatski (Croatian)"},
    {"cs", "Čeština (Czech)"},
    {"da", "Dansk (Danish)"},
    {"nl", "Nederlands (Dutch)"},
    {"et", "Eesti (Estonian)"},
    {"fil", "Filipino"},
    {"fi", "Suomi (Finnish)"},
    {"gl", "Galego (Galician)"},
    {"ka", "ქართული (Georgian)"},
    {"el", "Ελληνικά (Greek)"},
    {"gu", "ગુજરાતી (Gujarati)"},
    {"ha", "Hausa"},
    {"he", "עברית (Hebrew)"},
    {"hi", "हिन्दी (Hindi)"},
    {"hu", "Magyar (Hungarian)"},
    {"is", "Íslenska (Icelandic)"},
    {"jv", "Basa Jawa (Javanese)"},
    {"kn", "ಕನ್ನಡ (Kannada)"},
    {"kk", "Қазақ (Kazakh)"},
    {"km", "ខ្មែរ (Khmer)"},
    {"rw", "Kinyarwanda"},
    {"lo", "ລາວ (Lao)"},
    {"lv", "Latviešu (Latvian)"},
    {"lt", "Lietuvių (Lithuanian)"},
    {"mk", "Македонски (Macedonian)"},
    {"ms", "Bahasa Melayu (Malay)"},
    {"ml", "മലയാളം (Malayalam)"},
    {"mr", "मराठी (Marathi)"},
    {"mn", "Монгол (Mongolian)"},
    {"ne", "नेपाली (Nepali)"},
    {"no", "Norsk (Norwegian)"},
    {"fa", "فارسی (Persian)"},
    {"pl", "Polski (Polish)"},
    {"pt-PT", "Português (Portuguese, Portugal)"},
    {"pa", "ਪੰਜਾਬੀ (Punjabi)"},
    {"ro", "Română (Romanian)"},
    {"sr", "Српски (Serbian)"},
    {"sd", "سنڌي (Sindhi)"},
    {"si", "සිංහල (Sinhala)"},
    {"sk", "Slovenčina (Slovak)"},
    {"sl", "Slovenščina (Slovenian)"},
    {"su", "Basa Sunda (Sundanese)"},
    {"sw", "Kiswahili (Swahili)"},
    {"sv", "Svenska (Swedish)"},
    {"ta", "தமிழ் (Tamil)"},
    {"te", "తెలుగు (Telugu)"},
    {"tr", "Türkçe (Turkish)"},
    {"uk", "Українська (Ukrainian)"},
    {"ur", "اردو (Urdu)"},
    {"uz", "Oʻzbek (Uzbek)"},
    {"zu", "IsiZulu (Zulu)"},
};

static const int kLanguagesCount = sizeof(kLanguages) / sizeof(kLanguages[0]);

// The prompt uses the endonym before the UI's optional English annotation.
// Keep regional codes intact; unknown codes still fall back to the code itself.
inline std::string target_language_name(const std::string &code)
{
    for (const auto &language : kLanguages) {
        if (code != language.code) continue;
        const std::string name = language.name;
        return name.substr(0, name.find(" ("));
    }
    return code;
}
}
