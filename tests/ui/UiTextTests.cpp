// SPDX-License-Identifier: GPL-3.0-only
// Fluvi ui: every text the screens show comes from the language files. Scans assets/ui/*.rml and the C++ of the UI
// for keys, checks each exists in pt-BR, and that en-US translates exactly the same set.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "fluvi/common/I18n.h"

namespace fs = std::filesystem;
using namespace fluvi;

namespace {

// Tests run from the repository root.
const fs::path kUi = fs::path("assets") / "ui";
const fs::path kI18n = fs::path("assets") / "i18n";

std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

std::set<std::string> KeysOf(const fs::path& json) {
    i18n::Catalog catalog;
    Status status;
    REQUIRE_MESSAGE(catalog.AddJson(ReadFile(json), json.string(), status), status.message);
    std::set<std::string> keys;
    for (const auto& [key, value] : catalog.Entries()) keys.insert(key);
    return keys;
}

// Text between tags, outside <head>, <style> and comments.
std::vector<std::string> TextNodes(const std::string& rml) {
    std::vector<std::string> nodes;
    std::string text;
    auto flush = [&] {
        const size_t a = text.find_first_not_of(" \t\r\n");
        if (a != std::string::npos) nodes.push_back(text.substr(a, text.find_last_not_of(" \t\r\n") - a + 1));
        text.clear();
    };
    size_t i = 0;
    auto skipTo = [&](const char* end) {
        const size_t at = rml.find(end, i);
        i = at == std::string::npos ? rml.size() : at + std::char_traits<char>::length(end);
    };
    while (i < rml.size()) {
        if (rml[i] != '<') {
            text += rml[i++];
            continue;
        }
        flush();
        if (rml.compare(i, 4, "<!--") == 0) skipTo("-->");
        else if (rml.compare(i, 5, "<head") == 0) skipTo("</head>");
        else if (rml.compare(i, 6, "<style") == 0) skipTo("</style>");
        else {
            // A tag: '>' may appear inside quoted attribute values ("conn.stage > 0").
            char quote = 0;
            for (++i; i < rml.size(); ++i) {
                const char c = rml[i];
                if (quote) {
                    if (c == quote) quote = 0;
                } else if (c == '"' || c == '\'') {
                    quote = c;
                } else if (c == '>') {
                    ++i;
                    break;
                }
            }
        }
    }
    flush();
    return nodes;
}

const std::regex kKey(R"(\[([a-z][a-z0-9_]*(?:\.[a-z0-9_]+)+)\])");

} // namespace

TEST_CASE("every visible text in the screens is a language key that exists in pt-BR") {
    REQUIRE(fs::exists(kUi / "theme.rcss"));
    const std::set<std::string> pt = KeysOf(kI18n / "pt-BR" / "ui.json");
    const std::regex dataExpr(R"(\{\{[^}]*\}\})");
    const std::regex entity(R"(&#?[A-Za-z0-9]+;)");
    const std::regex letters(R"([A-Za-zÀ-ÿ]{2,})");
    int files = 0, keys = 0;
    for (const auto& entry : fs::directory_iterator(kUi)) {
        if (entry.path().extension() != ".rml") continue;
        ++files;
        const std::string name = entry.path().filename().string();
        for (const std::string& node : TextNodes(ReadFile(entry.path()))) {
            for (std::sregex_iterator it(node.begin(), node.end(), kKey), end; it != end; ++it) {
                ++keys;
                CHECK_MESSAGE(pt.contains((*it)[1].str()), name << ": key missing in pt-BR/ui.json: " << (*it)[1].str());
            }
            // What is left after keys, data expressions and entities (icons) must not be words: "Fluvi", button
            // letters ("A", "B") and step numbers are the only literal text allowed.
            std::string rest = std::regex_replace(node, kKey, "");
            rest = std::regex_replace(rest, dataExpr, "");
            rest = std::regex_replace(rest, entity, "");
            if (rest.find("Fluvi") != std::string::npos) rest.erase(rest.find("Fluvi"), 5);
            CHECK_MESSAGE(!std::regex_search(rest, letters), name << ": untranslated text: \"" << node << "\"");
        }
    }
    CHECK(files == 7);
    CHECK(keys > 100);
}

TEST_CASE("every key the UI code asks for exists in pt-BR") {
    const std::set<std::string> pt = KeysOf(kI18n / "pt-BR" / "ui.json");
    const std::regex tr(R"_(\bTr(Count)?\("([a-z][a-z0-9_]*(?:\.[a-z0-9_]+)+)")_");
    int found = 0;
    for (const auto& entry : fs::directory_iterator(fs::path("src") / "ui")) {
        if (entry.path().extension() != ".cpp") continue;
        const std::string code = ReadFile(entry.path());
        for (std::sregex_iterator it(code.begin(), code.end(), tr), end; it != end; ++it) {
            ++found;
            const std::string key = (*it)[2].str();
            if ((*it)[1].matched) {
                CHECK_MESSAGE(pt.contains(key + ".one"), entry.path().filename().string() << ": " << key << ".one");
                CHECK_MESSAGE(pt.contains(key + ".other"), entry.path().filename().string() << ": " << key << ".other");
            } else {
                CHECK_MESSAGE(pt.contains(key), entry.path().filename().string() << ": key missing in pt-BR/ui.json: " << key);
            }
        }
    }
    CHECK(found > 30);
    for (int band = 0; band < 4; ++band) CHECK(pt.contains("stream.band_" + std::to_string(band)));   // built with std::format
}

TEST_CASE("en-US translates exactly the keys of pt-BR") {
    const std::set<std::string> pt = KeysOf(kI18n / "pt-BR" / "ui.json");
    const std::set<std::string> en = KeysOf(kI18n / "en-US" / "ui.json");
    for (const std::string& k : pt) CHECK_MESSAGE(en.contains(k), "missing in en-US/ui.json: " << k);
    for (const std::string& k : en) CHECK_MESSAGE(pt.contains(k), "only in en-US/ui.json: " << k);
}
