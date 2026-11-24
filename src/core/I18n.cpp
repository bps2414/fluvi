// SPDX-License-Identifier: GPL-3.0-only
// Fluvi core: language catalogs and the Tr() lookup behind fluvi/common/I18n.h.
#include "fluvi/common/I18n.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>

#include <nlohmann/json.hpp>
#include <windows.h>

#include "fluvi/core/Log.h"

namespace fluvi::i18n {

namespace {

struct State {
    std::string language{kSourceLanguage};
    Catalog current;
    Catalog fallback;
    bool hasFallback = false;   // false: `current` is the source language
};

std::shared_mutex g_mutex;
std::shared_ptr<const State> g_state = std::make_shared<State>();
std::mutex g_missingMutex;
std::set<std::string, std::less<>> g_missing;

std::shared_ptr<const State> Snapshot() {
    std::shared_lock lock(g_mutex);
    return g_state;
}

std::string PrimaryLanguage(std::string_view tag) {
    const size_t dash = tag.find('-');
    std::string out(tag.substr(0, dash));
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool SameTag(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

std::string SystemLanguage() {
    // Display language first, then the regional format as a fallback.
    wchar_t name[LOCALE_NAME_MAX_LENGTH * 4] = {};
    ULONG count = 0, size = static_cast<ULONG>(std::size(name));
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, name, &size) || count == 0 || !name[0]) {
        if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) <= 0) return std::string(kSourceLanguage);
    }
    std::string out;
    for (const wchar_t* p = name; *p; ++p) out.push_back(static_cast<char>(*p < 128 ? *p : '?'));
    return out;
}

const std::string* Lookup(const State& s, std::string_view key) {
    if (const std::string* v = s.current.Find(key)) return v;
    if (s.hasFallback) {
        if (const std::string* v = s.fallback.Find(key)) return v;
    }
    return nullptr;
}

std::string Missing(std::string_view key) {
#ifndef NDEBUG
    std::lock_guard lock(g_missingMutex);
    if (g_missing.insert(std::string(key)).second) FLUVI_WARN("i18n", "missing text key '{}'", key);
#endif
    return std::string(key);
}

} // namespace

bool Catalog::AddJson(std::string_view json, std::string_view source, Status& status) {
    const nlohmann::json doc = nlohmann::json::parse(json, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        status = Status::Fail(std::format("{}: not a JSON object", source));
        return false;
    }
    for (const auto& [key, value] : doc.items()) {
        if (!value.is_string()) {
            status = Status::Fail(std::format("{}: value of '{}' is not a string", source, key));
            return false;
        }
        if (entries_.contains(key)) {
            status = Status::Fail(std::format("{}: key '{}' is defined twice", source, key));
            return false;
        }
        entries_.emplace(key, value.get<std::string>());
    }
    return true;
}

bool Catalog::AddFolder(const std::filesystem::path& folder, Status& status) {
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) {
        status = Status::Fail(std::format("language folder not found: {}", folder.string()));
        return false;
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            status = Status::Fail(std::format("cannot read {}", file.string()));
            return false;
        }
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (!AddJson(text, file.filename().string(), status)) return false;
    }
    return true;
}

const std::string* Catalog::Find(std::string_view key) const {
    const auto it = entries_.find(key);
    return it == entries_.end() ? nullptr : &it->second;
}

std::vector<std::string> AvailableLanguages(const std::filesystem::path& i18nDir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(i18nDir, ec)) {
        if (entry.is_directory()) out.push_back(entry.path().filename().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string ResolveLanguage(std::string_view setting, const std::filesystem::path& i18nDir) {
    const std::string wanted = (setting.empty() || SameTag(setting, "auto")) ? SystemLanguage() : std::string(setting);
    const std::vector<std::string> available = AvailableLanguages(i18nDir);
    for (const auto& lang : available) {
        if (SameTag(lang, wanted)) return lang;
    }
    const std::string primary = PrimaryLanguage(wanted);
    // Prefer the source language when it shares the primary subtag (pt-PT -> pt-BR).
    if (primary == PrimaryLanguage(kSourceLanguage)) return std::string(kSourceLanguage);
    for (const auto& lang : available) {
        if (PrimaryLanguage(lang) == primary) return lang;
    }
    return std::string(kSourceLanguage);
}

bool Load(const std::filesystem::path& i18nDir, std::string_view language, Status& status) {
    Catalog fallback;
    if (!fallback.AddFolder(i18nDir / std::string(kSourceLanguage), status)) return false;
    if (SameTag(language, kSourceLanguage)) {
        Install(std::string(kSourceLanguage), std::move(fallback), {});
        return true;
    }
    Catalog current;
    if (!current.AddFolder(i18nDir / std::string(language), status)) return false;
    Install(std::string(language), std::move(current), std::move(fallback));
    return true;
}

void Install(std::string language, Catalog current, Catalog fallback) {
    auto state = std::make_shared<State>();
    state->hasFallback = !SameTag(language, kSourceLanguage);
    state->language = std::move(language);
    state->current = std::move(current);
    state->fallback = std::move(fallback);
    {
        std::unique_lock lock(g_mutex);
        g_state = std::move(state);
    }
    std::lock_guard lock(g_missingMutex);
    g_missing.clear();
}

std::string CurrentLanguage() { return Snapshot()->language; }

std::string Substitute(std::string_view text, std::initializer_list<Arg> args) {
    return SubstituteRange(text, args.begin(), args.size());
}

std::string SubstituteRange(std::string_view text, const Arg* args, size_t count) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '{') {
            const size_t close = text.find('}', i + 1);
            if (close != std::string_view::npos) {
                const std::string_view name = text.substr(i + 1, close - i - 1);
                const Arg* match = nullptr;
                for (size_t k = 0; k < count; ++k) {
                    if (args[k].name == name) { match = &args[k]; break; }
                }
                if (match) {
                    out += match->value;
                    i = close + 1;
                    continue;
                }
            }
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

std::string Tr(std::string_view key) {
    const auto state = Snapshot();
    if (const std::string* v = Lookup(*state, key)) return *v;
    return Missing(key);
}

std::string Tr(std::string_view key, std::initializer_list<Arg> args) {
    const auto state = Snapshot();
    if (const std::string* v = Lookup(*state, key)) return Substitute(*v, args);
    return Missing(key);
}

bool IsPluralOne(std::string_view language, int64_t count) {
    // CLDR cardinal "one": pt -> i = 0..1; en and the default -> n = 1.
    if (PrimaryLanguage(language) == "pt") return count == 0 || count == 1;
    return count == 1;
}

std::string TrCount(std::string_view key, int64_t count, std::initializer_list<Arg> args) {
    const auto state = Snapshot();
    const std::string full = std::string(key) + (IsPluralOne(state->language, count) ? ".one" : ".other");
    std::vector<Arg> all(args);
    all.push_back({"n", std::to_string(count)});
    const std::string* v = Lookup(*state, full);
    if (!v) return Missing(full);
    return SubstituteRange(*v, all.data(), all.size());
}

bool Has(std::string_view key) { return Lookup(*Snapshot(), key) != nullptr; }

char DecimalSeparator() { return PrimaryLanguage(Snapshot()->language) == "en" ? '.' : ','; }

std::string FormatNumber(double value, int decimals) {
    if (!std::isfinite(value)) return "-";
    decimals = std::clamp(decimals, 0, 6);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
    std::string out(buf);
    if (const char sep = DecimalSeparator(); sep != '.') std::replace(out.begin(), out.end(), '.', sep);
    return out;
}

} // namespace fluvi::i18n
