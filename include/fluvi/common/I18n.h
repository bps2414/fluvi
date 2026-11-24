// SPDX-License-Identifier: GPL-3.0-only
// Fluvi: user-facing text from language files (assets/i18n/<lang>/*.json), so no screen or module hardcodes it.
//
// Layout: one folder per language, one flat JSON object per owner (ui.json, app.json, ...). The loader merges every
// file in the folder and rejects a key defined twice. pt-BR is the source language and the fallback:
//   missing key in the current language -> pt-BR -> the key itself (logged once in debug builds).
// Keys are grouped by screen ("consoles.play"). Placeholders are named: "Conectado à {console}". Plurals use the
// suffixes ".one" / ".other", chosen by the CLDR rule of the language (pt: 0 and 1 are "one"; en: only 1).
//
// Threading: Tr()/Format*() may be called from any thread; Load()/SetLanguage() swap the catalog atomically.
// Implementation: src/core/I18n.cpp (fluvi_core).
#pragma once
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include "fluvi/core/Status.h"

namespace fluvi::i18n {

inline constexpr std::string_view kSourceLanguage = "pt-BR";

struct Arg {
    std::string_view name;   // placeholder name without braces
    std::string value;
};

// One language's strings. Pure data: testable without files.
class Catalog {
public:
    // Merges one flat JSON object of string values. `source` names the file in errors. Fails on invalid JSON,
    // a non-string value or a key already present.
    bool AddJson(std::string_view json, std::string_view source, Status& status);
    // Merges every *.json in `folder` (sorted by name).
    bool AddFolder(const std::filesystem::path& folder, Status& status);

    const std::string* Find(std::string_view key) const;
    const std::map<std::string, std::string, std::less<>>& Entries() const { return entries_; }

private:
    std::map<std::string, std::string, std::less<>> entries_;
};

// The language folders present under `i18nDir` (e.g. {"en-US", "pt-BR"}).
std::vector<std::string> AvailableLanguages(const std::filesystem::path& i18nDir);

// Maps a setting value to a folder that exists: "auto" (or empty) follows the Windows UI language; an exact folder
// name wins; otherwise the first folder with the same primary language ("pt-PT" -> "pt-BR"); otherwise pt-BR.
std::string ResolveLanguage(std::string_view setting, const std::filesystem::path& i18nDir);

// Loads `language` (already resolved) plus pt-BR as the fallback, and makes them current.
bool Load(const std::filesystem::path& i18nDir, std::string_view language, Status& status);
// Installs catalogs directly (tests, tools). `fallback` is the pt-BR catalog.
void Install(std::string language, Catalog current, Catalog fallback);

// The current language folder name ("pt-BR" before any Load()).
std::string CurrentLanguage();

// Translated text, with {name} placeholders replaced. Unknown placeholders stay as written.
std::string Tr(std::string_view key);
std::string Tr(std::string_view key, std::initializer_list<Arg> args);
// Plural: picks key + ".one" or key + ".other" for `count` and also fills {n} with the formatted count.
std::string TrCount(std::string_view key, int64_t count, std::initializer_list<Arg> args = {});
bool Has(std::string_view key);

// Plural category for a language: true = "one".
bool IsPluralOne(std::string_view language, int64_t count);

// Numbers in the current language: decimal comma in pt-BR, point in en-US.
std::string FormatNumber(double value, int decimals);
char DecimalSeparator();

// Replaces {name} placeholders in `text`. Exposed for tests.
std::string Substitute(std::string_view text, std::initializer_list<Arg> args);
std::string SubstituteRange(std::string_view text, const Arg* args, size_t count);

} // namespace fluvi::i18n
