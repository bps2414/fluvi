// SPDX-License-Identifier: GPL-3.0-only
#include <doctest/doctest.h>

#include "fluvi/core/LatestQueue.h"
#include "fluvi/core/Log.h"
#include "fluvi/core/Time.h"
#include "fluvi/common/I18n.h"

#include <algorithm>
#include <filesystem>

TEST_CASE("LatestQueue drops the oldest item when full") {
    fluvi::LatestQueue<int, 3> q;
    CHECK_FALSE(q.Push(1));
    CHECK_FALSE(q.Push(2));
    CHECK_FALSE(q.Push(3));
    CHECK(q.Push(4));
    CHECK(q.Dropped() == 1);
    CHECK(q.TryPop() == 2);
    CHECK(q.TryPop() == 3);
    CHECK(q.TryPop() == 4);
    CHECK_FALSE(q.TryPop().has_value());
}

TEST_CASE("LatestQueue WaitPop returns after Close") {
    fluvi::LatestQueue<int, 2> q;
    q.Close();
    CHECK_FALSE(q.WaitPop(std::chrono::milliseconds(1000)).has_value());
}

TEST_CASE("QPC conversions round-trip") {
    CHECK(fluvi::time::QpcFrequency() > 0);
    const int64_t a = fluvi::time::NowQpc();
    CHECK(fluvi::time::NowQpc() >= a);
    CHECK(fluvi::time::QpcToMs(fluvi::time::MsToQpc(16.0)) == doctest::Approx(16.0).epsilon(0.001));
}

TEST_CASE("Log writes without a file") {
    fluvi::log::SetFile(L"");
    FLUVI_INFO("test", "value {}", 42);
}

// ---- i18n ----

namespace i18n = fluvi::i18n;

TEST_CASE("i18n catalog rejects duplicates and non-string values") {
    i18n::Catalog c;
    fluvi::Status s;
    CHECK(c.AddJson(R"({"a.b": "x"})", "one.json", s));
    CHECK_FALSE(c.AddJson(R"({"a.b": "y"})", "two.json", s));
    CHECK(s.message.find("two.json") != std::string::npos);
    CHECK_FALSE(c.AddJson(R"({"n": 3})", "three.json", s));
    CHECK_FALSE(c.AddJson("not json", "four.json", s));
    REQUIRE(c.Find("a.b"));
    CHECK(*c.Find("a.b") == "x");
}

TEST_CASE("i18n placeholders, fallback and plurals") {
    i18n::Catalog en, pt;
    fluvi::Status s;
    REQUIRE(pt.AddJson(R"({"hi": "Olá, {name}", "only.pt": "só pt", "cons.count.one": "{n} console", "cons.count.other": "{n} consoles"})", "pt", s));
    REQUIRE(en.AddJson(R"({"hi": "Hi, {name}", "cons.count.one": "{n} console", "cons.count.other": "{n} consoles"})", "en", s));

    i18n::Install("en-US", en, pt);
    CHECK(i18n::Tr("hi", {{"name", "Bryan"}}) == "Hi, Bryan");
    CHECK(i18n::Tr("only.pt") == "só pt");           // falls back to the source language
    CHECK(i18n::Tr("nope.key") == "nope.key");      // then to the key itself
    CHECK(i18n::TrCount("cons.count", 0) == "0 consoles");
    CHECK(i18n::TrCount("cons.count", 1) == "1 console");
    CHECK(i18n::FormatNumber(1.5, 1) == "1.5");

    i18n::Install("pt-BR", pt, {});
    CHECK(i18n::TrCount("cons.count", 0) == "0 console");   // CLDR pt: 0 and 1 are "one"
    CHECK(i18n::TrCount("cons.count", 2) == "2 consoles");
    CHECK(i18n::FormatNumber(1.5, 1) == "1,5");
    CHECK(i18n::Substitute("{a} e {b} {c}", {{"a", "1"}, {"b", "2"}}) == "1 e 2 {c}");
}

TEST_CASE("i18n resolves languages to existing folders") {
    const std::filesystem::path dir = "assets/i18n";
    REQUIRE(std::filesystem::is_directory(dir));
    CHECK(i18n::ResolveLanguage("pt-BR", dir) == "pt-BR");
    CHECK(i18n::ResolveLanguage("pt-PT", dir) == "pt-BR");
    CHECK(i18n::ResolveLanguage("en-GB", dir) == "en-US");
    CHECK(i18n::ResolveLanguage("xx-YY", dir) == "pt-BR");
    const std::string automatic = i18n::ResolveLanguage("auto", dir);
    CHECK((automatic == "pt-BR" || automatic == "en-US"));
}

TEST_CASE("i18n shipped languages load and share one key set") {
    const std::filesystem::path dir = "assets/i18n";
    const auto languages = i18n::AvailableLanguages(dir);
    REQUIRE(std::find(languages.begin(), languages.end(), "pt-BR") != languages.end());
    fluvi::Status s;
    i18n::Catalog source;
    REQUIRE_MESSAGE(source.AddFolder(dir / "pt-BR", s), s.message);
    for (const auto& lang : languages) {
        if (lang == "pt-BR") continue;
        i18n::Catalog other;
        REQUIRE_MESSAGE(other.AddFolder(dir / lang, s), s.message);
        for (const auto& [key, value] : source.Entries()) CHECK_MESSAGE(other.Find(key), lang << " lacks " << key);
        for (const auto& [key, value] : other.Entries()) CHECK_MESSAGE(source.Find(key), lang << " has extra " << key);
    }
    CHECK(i18n::Load(dir, "en-US", s));
    CHECK(i18n::CurrentLanguage() == "en-US");
    CHECK(i18n::Load(dir, "pt-BR", s));
}
