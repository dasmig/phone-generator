#include "catch_amalgamated.hpp"
#include "../dasmig/phonegen.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>

// ---------------------------------------------------------------------------
// Recorded answers (see "Known answers" below); re-record them with the data.
// ---------------------------------------------------------------------------

#define KAT_DATA_HASH 0x82d22110ee3bc395ULL
#define KAT_JSON_1 R"({"country_code":"EG","type":"fixed","e164":"+20928653194","international":"+20 92 8653194","national":"092 8653194","reserved":false,"signature":{"seed":"0000000000000001","data":"82d22110ee3bc395"}})"
#define KAT_JSON_2 R"({"country_code":"US","type":"fixed","e164":"+17759545740","international":"+1 775-954-5740","national":"(775) 954-5740","reserved":false,"signature":{"seed":"0000000000000002","data":"82d22110ee3bc395"}})"
#define KAT_JSON_3 R"({"country_code":"GB","type":"mobile","e164":"+447700900222","international":"+44 7700 900222","national":"07700 900222","reserved":true,"signature":{"seed":"0000000000000003","data":"82d22110ee3bc395"}})"
#define KAT_HASH 0x46b24ac8eacaf16fULL
#define KAT_BATCH_HASH 0x8aa51a38b7aa6e6bULL

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static dasmig::phg make_loaded_generator()
{
    dasmig::phg gen;
    if (!gen.load(dasmig::dataset::lite))
    {
        for (const auto* candidate : {"resources/lite", "../resources/lite"})
        {
            if (std::filesystem::is_directory(candidate))
            {
                gen.load(std::filesystem::path{candidate});
                break;
            }
        }
    }
    REQUIRE(gen.has_data());
    return gen;
}

// The libphonenumber patterns the ranges were built from (test data only).
static const std::map<std::pair<std::string, std::string>, std::regex>& patterns()
{
    static const auto table = [] {
        std::ifstream in{"tests/data/phone_regex.tsv"};
        if (!in) in.open("../tests/data/phone_regex.tsv");
        REQUIRE(in.good());
        std::map<std::pair<std::string, std::string>, std::regex> out;
        std::string line;
        std::getline(in, line); // header
        while (std::getline(in, line))
        {
            const auto a = line.find('\t');
            const auto b = line.find('\t', a + 1);
            out.emplace(std::pair{line.substr(0, a), line.substr(a + 1, b - a - 1)},
                        std::regex{line.substr(b + 1), std::regex::optimize});
        }
        return out;
    }();
    return table;
}

static std::string digits_of(const std::string& text)
{
    std::string out;
    for (const char c : text)
    {
        if (c >= '0' && c <= '9') out += c;
    }
    return out;
}

static std::uint64_t fnv(const std::string& text)
{
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const char c : text)
    {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ULL;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

TEST_CASE("load(dataset::lite) loads the sovereign states", "[loading]")
{
    const auto gen = make_loaded_generator();
    REQUIRE(gen.country_count() == 195);
    REQUIRE(gen.has_country("br"));
    REQUIRE_FALSE(gen.has_country("ZZ"));
    REQUIRE(gen.layers(dasmig::phone_table::countries).size() == 1);
    REQUIRE(gen.layers(dasmig::phone_table::ranges).size() == 1);
}

TEST_CASE("the full tier loads the territories too and every country draws both types", "[loading][full]")
{
    dasmig::phg gen;
    for (const auto* candidate : {"resources/full", "../resources/full"})
    {
        if (std::filesystem::is_directory(candidate))
        {
            gen.load(std::filesystem::path{candidate});
            break;
        }
    }
    REQUIRE(gen.has_data());
    REQUIRE(gen.country_count() == 243);
    REQUIRE(gen.has_country("AX"));
    REQUIRE_FALSE(gen.has_country("AQ")); // no libphonenumber plan
    for (const auto& type : {dasmig::phone_type::mobile, dasmig::phone_type::fixed})
    {
        for (std::uint64_t s = 0; s < 3; ++s)
        {
            for (const auto* c : {"AX", "GI", "PR", "GU", "VA", "BR", "US"})
            {
                REQUIRE(gen.get_phone({.country = c, .type = type}, s).national_number.size() >= 4);
            }
        }
    }
}

TEST_CASE("load - non-existent path is a no-op", "[loading]")
{
    dasmig::phg gen;
    gen.load(std::filesystem::path{"no/such/directory"});
    REQUIRE_FALSE(gen.has_data());
    REQUIRE(gen.data_hash() == 0);
}

#ifndef DASMIG_PHONE_EMBED // compiled-in data is never missing
TEST_CASE("load(dataset) returns false for a missing tier", "[loading]")
{
    const auto old_cwd = std::filesystem::current_path();
    const auto tmp = std::filesystem::temp_directory_path() / "phg_empty_dir";
    std::filesystem::create_directories(tmp);
    std::filesystem::current_path(tmp);
    dasmig::phg gen;
    const bool loaded = gen.load(dasmig::dataset::full);
    std::filesystem::current_path(old_cwd);
    std::filesystem::remove_all(tmp);
    REQUIRE_FALSE(loaded);
    REQUIRE_FALSE(gen.has_data());
}
#endif

TEST_CASE("the singleton loads data on first use", "[loading]")
{
    REQUIRE(dasmig::phg::instance().has_data());
}

// ---------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------

TEST_CASE("get_phone throws when no data is loaded", "[generation]")
{
    dasmig::phg gen;
    REQUIRE_THROWS_AS(gen.get_phone(dasmig::phone_request{}), std::runtime_error);
    REQUIRE_THROWS_AS(gen.get_phone(dasmig::phone_request{}, std::uint64_t{1}), std::runtime_error);
}

TEST_CASE("get_phone rejects an unknown country", "[generation][errors]")
{
    const auto gen = make_loaded_generator();
    REQUIRE_THROWS_AS(gen.get_phone({.country = "ZZ"}, std::uint64_t{1}), std::invalid_argument);
}

TEST_CASE("get_phone with a seed is deterministic", "[generation][seed]")
{
    const auto gen = make_loaded_generator();
    for (std::uint64_t s = 0; s < 100; ++s)
    {
        REQUIRE(gen.get_phone({}, s).to_json() == gen.get_phone({}, s).to_json());
    }
}

TEST_CASE("a result replays from its seed", "[generation][seed]")
{
    auto gen = make_loaded_generator();
    const auto v = gen.get_phone(dasmig::phone_request{});
    REQUIRE(gen.get_phone(dasmig::phone_request{}, v.seed()).to_json() == v.to_json());
}

TEST_CASE("explicit per-call seeds replay across generator instances", "[seed]")
{
    const auto first = make_loaded_generator();
    const auto second = make_loaded_generator();
    REQUIRE(first.get_phone({}, 42).to_json() == second.get_phone({}, 42).to_json());
}

TEST_CASE("the country and the type follow the request", "[generation]")
{
    const auto gen = make_loaded_generator();
    for (std::uint64_t s = 0; s < 200; ++s)
    {
        const auto m = gen.get_phone({.country = "br", .type = dasmig::phone_type::mobile}, s);
        REQUIRE(m.country_code == "BR");
        REQUIRE(m.calling_code == "55");
        REQUIRE(m.type == dasmig::phone_type::mobile);
        REQUIRE(gen.get_phone({.country = "BR", .type = dasmig::phone_type::fixed}, s).type ==
                dasmig::phone_type::fixed);
    }
}

TEST_CASE("every country and type validates against the libphonenumber pattern", "[generation][validity]")
{
    const auto gen = make_loaded_generator();
    const auto& rx = patterns();
    std::size_t checked = 0;
    for (const auto& [key, re] : rx)
    {
        if (!gen.has_country(key.first)) continue;
        const auto type = key.second == "mobile" ? dasmig::phone_type::mobile : dasmig::phone_type::fixed;
        for (std::uint64_t s = 0; s < 300; ++s)
        {
            const auto p = gen.get_phone({.country = key.first, .type = type}, s);
            INFO(key.first << " " << key.second << " " << p.national_number);
            REQUIRE(std::regex_match(p.national_number, re));
            ++checked;
        }
    }
    REQUIRE(checked > 50000);
}

TEST_CASE("no number is longer than E.164 allows, and plans are drawn near their usual length", "[generation][length]")
{
    const auto gen = make_loaded_generator();
    for (std::uint64_t s = 0; s < 20000; ++s)
    {
        const auto p = gen.get_phone({}, s);
        INFO(p.to_json());
        REQUIRE(p.e164().size() <= 16); // '+' and at most 15 digits
        REQUIRE(p.national_number.size() >= 4);
    }
    // Germany lists lengths from 5 to 13 digits: most fixed-line numbers have 8 or 9 (the example's length is 8).
    int near = 0;
    constexpr int draws = 3000;
    for (std::uint64_t s = 0; s < draws; ++s)
    {
        const auto n = gen.get_phone({.country = "DE", .type = dasmig::phone_type::fixed}, s).national_number.size();
        near += (n >= 7 && n <= 9) ? 1 : 0;
    }
    REQUIRE(near > draws * 0.85);
}

TEST_CASE("the formats keep every digit in order", "[generation][format]")
{
    const auto gen = make_loaded_generator();
    for (std::uint64_t s = 0; s < 3000; ++s)
    {
        const auto p = gen.get_phone({}, s);
        INFO(p.to_json());
        REQUIRE(p.e164() == "+" + p.calling_code + p.national_number);
        REQUIRE(digits_of(p.international()) == p.calling_code + p.national_number);
        REQUIRE(p.rfc3966().starts_with("tel:+" + p.calling_code));
        REQUIRE(digits_of(p.rfc3966()) == p.calling_code + p.national_number);
        // The national format ends with the subscriber digits. It may drop or add leading ones, as
        // libphonenumber does for Argentinian mobiles (+54 9 11 5811-5808 is 11 15-5811-5808).
        REQUIRE(digits_of(p.national()).ends_with(p.national_number.substr(p.national_number.size() - 4)));
        REQUIRE(p.rfc3966().find("--") == std::string::npos);
    }
}

TEST_CASE("known formats", "[generation][format]")
{
    const auto gen = make_loaded_generator();
    const auto p = gen.get_phone({.country = "US", .type = dasmig::phone_type::fixed}, std::uint64_t{1});
    REQUIRE(p.international().starts_with("+1 "));
    REQUIRE(p.national().size() == 14); // (212) 555-0123
    REQUIRE(p.national()[0] == '(');
    REQUIRE(p.rfc3966().size() == 5 + 2 + 12); // tel:+1-212-555-0123
}

TEST_CASE("countries are drawn by population by default, uniformly on request", "[generation][stats]")
{
    const auto gen = make_loaded_generator();
    constexpr int draws = 6000;
    std::map<std::string, int> weighted;
    std::map<std::string, int> uniform;
    for (std::uint64_t s = 0; s < draws; ++s)
    {
        ++weighted[gen.get_phone({}, s).country_code];
        ++uniform[gen.get_phone({.weighted = false}, s).country_code];
    }
    REQUIRE(weighted["IN"] > draws / 25);     // about 17% of the population of the 195
    REQUIRE(weighted["IS"] < draws / 200);    // about 0.005%
    REQUIRE(uniform["IN"] < draws / 60);      // about 1 in 195
    REQUIRE(uniform.size() > 180);
}

TEST_CASE("without a type, mobile is the common draw", "[generation][stats]")
{
    const auto gen = make_loaded_generator();
    int mobile = 0;
    constexpr int draws = 4000;
    for (std::uint64_t s = 0; s < draws; ++s)
    {
        mobile += gen.get_phone({.country = "DE"}, s).type == dasmig::phone_type::mobile ? 1 : 0;
    }
    REQUIRE(mobile == Catch::Approx(draws * 0.7).margin(draws * 0.03));
}

// ---------------------------------------------------------------------------
// The safe option
// ---------------------------------------------------------------------------

TEST_CASE("safe numbers come from the regulators' ranges where there are any", "[safe]")
{
    const auto gen = make_loaded_generator();
    for (const char* c : {"US", "CA", "GB", "DE", "AU"})
    {
        REQUIRE((gen.has_reserved(c, dasmig::phone_type::mobile) || gen.has_reserved(c, dasmig::phone_type::fixed)));
    }
    REQUIRE_FALSE(gen.has_reserved("BR", dasmig::phone_type::mobile));

    for (std::uint64_t s = 0; s < 500; ++s)
    {
        const auto us = gen.get_phone({.country = "US", .safe = true}, s);
        REQUIRE(us.reserved);
        REQUIRE(us.national_number.substr(3, 4) == "5550");
        REQUIRE(us.national_number[7] == '1');

        const auto ca = gen.get_phone({.country = "CA", .safe = true}, s);
        REQUIRE(ca.reserved);
        REQUIRE(ca.national_number.substr(3, 5) == "55501");

        const auto gb = gen.get_phone({.country = "GB", .safe = true}, s);
        REQUIRE(gb.reserved);
        REQUIRE(gb.type == dasmig::phone_type::mobile);
        REQUIRE(gb.national_number.starts_with("7700900"));
        REQUIRE(gb.international().starts_with("+44 7700 900"));

        const auto au = gen.get_phone({.country = "AU", .safe = true}, s);
        REQUIRE(au.reserved);
        REQUIRE(au.national_number.starts_with("25550"));

        const auto de = gen.get_phone({.country = "DE", .type = dasmig::phone_type::fixed, .safe = true}, s);
        REQUIRE(de.reserved);
        const std::set<std::string> prefixes{"3023125", "6990009", "4066969", "2214710", "8999998"};
        REQUIRE(prefixes.contains(de.national_number.substr(0, 7)));
    }
}

TEST_CASE("safe numbers elsewhere avoid the allocated ranges and say so", "[safe]")
{
    const auto gen = make_loaded_generator();
    const auto& rx = patterns();
    int outside = 0;
    constexpr int draws = 300;
    for (std::uint64_t s = 0; s < draws; ++s)
    {
        const auto p = gen.get_phone({.country = "BR", .type = dasmig::phone_type::mobile, .safe = true}, s);
        REQUIRE_FALSE(p.reserved);
        outside += std::regex_match(p.national_number, rx.at({"BR", "mobile"})) ? 0 : 1;
    }
    REQUIRE(outside > draws * 0.95);
}

TEST_CASE("safe batches over random countries never fail", "[safe][batch]")
{
    const auto gen = make_loaded_generator();
    const auto batch = gen.get_phone_batch({.safe = true}, {.count = 2000, .threads = 4}, 9);
    REQUIRE(batch.size() == 2000);
    REQUIRE(std::ranges::any_of(batch, [](const dasmig::phone& p) { return p.reserved; }));
    REQUIRE(std::ranges::any_of(batch, [](const dasmig::phone& p) { return !p.reserved; }));
}

// ---------------------------------------------------------------------------
// Conversion
// ---------------------------------------------------------------------------

TEST_CASE("string conversion and streaming give the international format", "[conversion]")
{
    const auto gen = make_loaded_generator();
    const auto v = gen.get_phone({}, std::uint64_t{5});
    const std::string s = v;
    std::ostringstream os;
    os << v;
    REQUIRE(s == v.international());
    REQUIRE(os.str() == v.international());
}

// ---------------------------------------------------------------------------
// Known answers (rule 2): the same seed and data give these exact results on
// every compiler and platform. A change here is a change of output and needs
// a major version.
// ---------------------------------------------------------------------------

TEST_CASE("known answers for fixed seeds", "[kat]")
{
    dasmig::phg gen;
    gen.load(std::filesystem::path{"resources/lite"});
    if (!gen.has_data()) gen.load(std::filesystem::path{"../resources/lite"});
    REQUIRE(gen.has_data());
    REQUIRE(gen.data_hash() == KAT_DATA_HASH);

    REQUIRE(gen.get_phone({}, std::uint64_t{1}).to_json() == KAT_JSON_1);
    REQUIRE(gen.get_phone({.country = "US"}, std::uint64_t{2}).to_json() == KAT_JSON_2);
    REQUIRE(gen.get_phone({.country = "GB", .safe = true}, std::uint64_t{3}).to_json() == KAT_JSON_3);

    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (std::uint64_t s = 0; s < 10000; ++s)
    {
        h = dasmig::core::hash_combine(h, fnv(gen.get_phone({}, s).to_json()));
    }
    REQUIRE(h == KAT_HASH);
}

// ---------------------------------------------------------------------------
// JSON and signatures
// ---------------------------------------------------------------------------

TEST_CASE("to_json and signature", "[json]")
{
    const auto gen = make_loaded_generator();
    const auto v = gen.get_phone({}, std::uint64_t{99});
    REQUIRE(v.signature().seed == 99);
    REQUIRE(v.seed() == 99);
    REQUIRE(v.signature().data == gen.data_hash());
    const auto json = v.to_json();
    REQUIRE(json.starts_with(R"({"country_code":")"));
    REQUIRE(json.find(R"("e164":"+)") != std::string::npos);
    REQUIRE(json.find(R"("signature":{"seed":"0000000000000063")") != std::string::npos);

    dasmig::core::json_writer w;
    w.begin_array();
    v.to_json(w);
    w.end_array();
    REQUIRE(w.str() == "[" + json + "]");
}

// ---------------------------------------------------------------------------
// Extending data (rule 8)
// ---------------------------------------------------------------------------

TEST_CASE("extend adds a country, replaces ranges and removes them", "[extend]")
{
    auto gen = make_loaded_generator();
    const auto base_hash = gen.data_hash();
    const auto before = gen.get_phone({}, std::uint64_t{7});

    // A made-up region with one mobile range and a format.
    gen.extend_from_string(dasmig::phone_table::countries, "region\tcode\tpopulation\nXX\t999\t1\n", "xx.tsv");
    gen.extend_from_string(dasmig::phone_table::formats, "id\tgroups\tnational\tinternational\n9000\t3-4\t$1 $2\t$1-$2\n", "fmt.tsv");
    gen.extend_from_string(dasmig::phone_table::ranges,
                           "region\ttype\tprefix\tlength\tmask\tweight\tformat\nXX\tmobile\t7\t7\t[1-3]#{5}\t300000\t9000\n",
                           "xx-ranges.tsv");
    REQUIRE(gen.has_country("xx"));
    REQUIRE(gen.data_hash() != base_hash);
    for (std::uint64_t s = 0; s < 100; ++s)
    {
        const auto p = gen.get_phone({.country = "XX"}, s);
        REQUIRE(p.calling_code == "999");
        REQUIRE(p.national_number.size() == 7);
        REQUIRE(p.national_number[0] == '7');
        REQUIRE(p.national_number[1] >= '1');
        REQUIRE(p.national_number[1] <= '3');
        REQUIRE(p.international() == "+999 " + p.national_number.substr(0, 3) + "-" + p.national_number.substr(3));
    }
    REQUIRE_THROWS_AS(gen.get_phone({.country = "XX", .type = dasmig::phone_type::fixed}, std::uint64_t{1}),
                      std::invalid_argument);

    gen.extend_from_string(dasmig::phone_table::ranges,
                           "region\ttype\tprefix\tlength\tdisabled\nXX\tmobile\t7\t7\t1\n", "off.tsv");
    REQUIRE_THROWS_AS(gen.get_phone({.country = "XX"}, std::uint64_t{1}), std::invalid_argument);

    // Reloading the base drops every layer.
    REQUIRE(gen.load(dasmig::dataset::lite));
    REQUIRE(gen.data_hash() == base_hash);
    REQUIRE_FALSE(gen.has_country("XX"));
    REQUIRE(gen.get_phone({}, std::uint64_t{7}).to_json() == before.to_json());
}

TEST_CASE("a bad layer is rejected and leaves the data unchanged", "[extend][errors]")
{
    using dasmig::phone_table;
    auto gen = make_loaded_generator();
    const auto hash = gen.data_hash();

    const auto header = std::string{"region\ttype\tprefix\tlength\tmask\tweight\tformat\n"};
    REQUIRE_THROWS_AS(gen.extend_from_string(phone_table::ranges, header + "BR\tmobile\t9x\t9\t#{7}\t1\t1\n", "a.tsv"),
                      dasmig::core::tsv_error); // prefix not digits
    REQUIRE_THROWS_AS(gen.extend_from_string(phone_table::ranges, header + "BR\tmobile\t9\t9\t#{3}\t1\t1\n", "b.tsv"),
                      dasmig::core::tsv_error); // digits do not add up to the length
    REQUIRE_THROWS_AS(gen.extend_from_string(phone_table::ranges, header + "BR\tmobile\t9\t4\t[3-1]##\t1\t1\n", "c.tsv"),
                      dasmig::core::tsv_error); // backwards class
    REQUIRE_THROWS_AS(gen.extend_from_string(phone_table::ranges, header + "BR\tcell\t9\t4\t###\t1\t1\n", "d.tsv"),
                      dasmig::core::tsv_error); // unknown type
    REQUIRE_THROWS_AS(gen.extend_from_string(phone_table::ranges, header + "QQ\tmobile\t9\t4\t###\t1\t1\n", "e.tsv"),
                      dasmig::core::tsv_error); // unknown region
    REQUIRE_THROWS_AS(gen.extend_from_string(phone_table::ranges, header + "BR\tmobile\t9\t4\t###\t1\t424242\n", "f.tsv"),
                      dasmig::core::tsv_error); // unknown format
    REQUIRE_THROWS_AS(gen.extend_from_string(phone_table::ranges, header + "BR\tmobile\t9\t4\t###\t-1\t1\n", "g.tsv"),
                      dasmig::core::tsv_error); // negative weight
    REQUIRE_THROWS_AS(gen.extend_from_string(phone_table::formats, "id\tgroups\tnational\n7\t3-x\t$1\n", "h.tsv"),
                      dasmig::core::tsv_error);
    REQUIRE_THROWS_AS(gen.extend_from_string(phone_table::countries, "region\tcode\nBRA\t55\n", "i.tsv"),
                      dasmig::core::tsv_error);

    REQUIRE(gen.data_hash() == hash);
    REQUIRE(gen.layers(phone_table::ranges).size() == 1);
}

#ifdef DASMIG_PHONE_EMBED
TEST_CASE("compiled-in data needs no files and matches them", "[embed]")
{
    const auto old_cwd = std::filesystem::current_path();
    const auto tmp = std::filesystem::temp_directory_path() / "phg_embed_dir";
    std::filesystem::create_directories(tmp);
    std::filesystem::current_path(tmp);
    dasmig::phg gen;
    const bool loaded = gen.load(dasmig::dataset::lite);
    std::filesystem::current_path(old_cwd);
    std::filesystem::remove_all(tmp);

    REQUIRE(loaded);
    REQUIRE(gen.layers(dasmig::phone_table::ranges).front().source == "embedded:lite/ranges.tsv");
    REQUIRE(gen.data_hash() == KAT_DATA_HASH); // same as the files
    REQUIRE(gen.get_phone({}, std::uint64_t{1}).to_json() == KAT_JSON_1);
}
#endif

// ---------------------------------------------------------------------------
// Batch generation
// ---------------------------------------------------------------------------

TEST_CASE("phone batch: items equal the single calls for any thread count", "[batch]")
{
    auto gen = make_loaded_generator();
    const auto request = dasmig::phone_request{};
    const auto batch = gen.get_phone_batch(request, {.count = 200, .threads = 1}, 77);
    REQUIRE(batch.size() == 200);
    for (std::uint64_t i = 0; i < batch.size(); ++i)
    {
        REQUIRE(batch[i].to_json() == gen.get_phone(request, dasmig::core::item_seed(77, i)).to_json());
    }

    // A range is computed directly, on any number of threads.
    const auto tail = gen.get_phone_batch(request, {.count = 100, .first = 100, .threads = 4}, 77);
    REQUIRE(tail.size() == 100);
    for (std::size_t i = 0; i < tail.size(); ++i)
    {
        REQUIRE(tail[i].to_json() == batch[100 + i].to_json());
    }
    for (const std::size_t threads : {2U, 7U, 0U})
    {
        const auto other = gen.get_phone_batch(request, {.count = 200, .threads = threads}, 77);
        REQUIRE(other.size() == batch.size());
        for (std::size_t i = 0; i < other.size(); ++i)
        {
            REQUIRE(other[i].to_json() == batch[i].to_json());
        }
    }

    // Without a seed the batch still has the requested size.
    REQUIRE(gen.get_phone_batch(request, {.count = 5}).size() == 5);
}

TEST_CASE("phone batch: unique numbers", "[batch]")
{
    auto gen = make_loaded_generator();
    const auto rule = dasmig::core::unique_rule::by<dasmig::phone>([](const dasmig::phone& p) { return p.e164(); });
    const auto batch = gen.get_phone_batch({.country = "IS"}, {.count = 2000, .unique = rule}, 3);
    std::set<std::string> seen;
    for (const auto& p : batch) seen.insert(p.e164());
    REQUIRE(seen.size() == batch.size());
}

TEST_CASE("phone batch: streaming gives the same items in order", "[batch]")
{
    auto gen = make_loaded_generator();
    const auto request = dasmig::phone_request{};
    const auto batch = gen.get_phone_batch(request, {.count = 300, .first = 10, .threads = 3}, 5);
    std::uint64_t next = 10;
    std::size_t seen = 0;
    gen.for_each_phone(request, {.count = 300, .first = 10, .threads = 3}, 5,
                       [&](std::uint64_t index, const dasmig::phone& item)
                       {
                           REQUIRE(index == next);
                           REQUIRE(item.to_json() == batch[seen].to_json());
                           ++next;
                           ++seen;
                       });
    REQUIRE(seen == batch.size());
}

TEST_CASE("phone batch: known answer of 10,000 items", "[batch][kat]")
{
    auto gen = make_loaded_generator();
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    gen.for_each_phone({}, {.count = 10000, .threads = 4}, 1,
                       [&](std::uint64_t, const dasmig::phone& item)
                       { hash = dasmig::core::hash_combine(hash, dasmig::core::fnv1a64(item.to_json())); });
    REQUIRE(hash == KAT_BATCH_HASH);
}
