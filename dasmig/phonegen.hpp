// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Dasso Migotto

#ifndef DASMIG_PHONEGEN_HPP
#define DASMIG_PHONEGEN_HPP

#include "core.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef DASMIG_PHONE_EMBED
#include "phonegen_data.hpp"
#endif

/// @file phonegen.hpp
/// @brief Phone Generator library — phone numbers valid in each country's numbering plan for C++23.
/// @author Diego Dasso Migotto (diegomigotto at hotmail dot com)
/// @see See doc/usage.md for the narrative tutorial.

DASMIG_CORE_REQUIRE(1, 1);

namespace dasmig
{

/// @brief The kinds of phone number generated.
enum class phone_type : std::uint8_t
{
    mobile, ///< A mobile number.
    fixed   ///< A fixed-line number.
};

/// @brief What a phone call asks for.
///
/// Every generation call takes one; `phone_request{}` asks for the
/// defaults. Choices are never stored on the generator, so one call cannot
/// change the next.
struct phone_request
{
    /// @brief ISO-3166 two-letter country code (any case), or a country
    ///        drawn when absent.
    std::optional<std::string> country{std::nullopt};

    /// @brief Mobile or fixed line; drawn when absent (mobile seven times in
    ///        ten, or the only type the country has).
    std::optional<phone_type> type{std::nullopt};

    /// @brief Use numbers the regulators set aside for fiction.
    ///
    /// Where the country has such a range the number comes from it and
    /// phone::reserved is true. Elsewhere the number is drawn outside the
    /// country's allocated ranges and phone::reserved is false; no error, so
    /// batches over random countries do not fail.
    bool safe{false};

    /// @brief Weighted selection (default): countries by population and
    ///        ranges by their weight: within a length every valid number is
    ///        equally likely, and a plan with several lengths is drawn mostly
    ///        at the length of its example number. False selects countries
    ///        and ranges uniformly.
    bool weighted{true};
};

/// @brief Return type for phone number generation.
///
/// Supports implicit conversion to std::string (the international format),
/// streaming via operator<< and JSON output via to_json().
class phone
{
  public:
    std::string country;         ///< ISO 3166-1 alpha-2 of the region the number belongs to.
    std::string country_code;    ///< Calling code without the plus sign ("55").
    std::string national_number; ///< The digits after the calling code, without a national prefix.
    phone_type type{phone_type::mobile}; ///< Mobile or fixed line.
    bool reserved{false};        ///< True when the number comes from a range set aside for fiction.

    /// @brief The number in E.164.
    /// @return For example `+5511987654321`.
    [[nodiscard]] std::string e164() const { return "+" + country_code + national_number; }

    /// @brief The number as written for international dialling.
    /// @return For example `+55 11 98765-4321`.
    [[nodiscard]] std::string international() const
    {
        if (_international == "-") return e164();
        return "+" + country_code + " " + apply_(_international);
    }

    /// @brief The number as written inside its country.
    /// @return For example `(11) 98765-4321`; with the national prefix where
    ///         the country dials one.
    [[nodiscard]] std::string national() const { return apply_(_national); }

    /// @brief The number as an RFC 3966 URI.
    /// @return For example `tel:+55-11-98765-4321`.
    [[nodiscard]] std::string rfc3966() const
    {
        std::string out = "tel:+" + country_code;
        const std::string body = _international == "-" ? national_number : apply_(_international);
        bool gap = true; // a separator is pending
        for (const char c : body)
        {
            if (c >= '0' && c <= '9')
            {
                if (gap) out += '-';
                gap = false;
                out += c;
            }
            else
            {
                gap = true;
            }
        }
        return out;
    }

    /// @brief Retrieve the random seed used to generate this phone number.
    /// @return The call seed; pass it to get_phone(request, seed) with the
    ///         same request and data to replay it.
    [[nodiscard]] std::uint64_t seed() const { return _signature.seed; }

    /// @brief The seed and the data this number was generated from.
    /// @return The replay signature; `data` is phg::data_hash() at the time
    ///         of generation.
    [[nodiscard]] const core::seed_signature& signature() const { return _signature; }

    /// @brief Human-readable summary string.
    /// @return The international format.
    [[nodiscard]] std::string to_string() const { return international(); }

    /// @brief Write this number as a JSON object.
    /// @param w Writer positioned where a value is allowed.
    void to_json(core::json_writer& w) const
    {
        w.begin_object()
            .field("country", country)
            .field("type", type == phone_type::mobile ? "mobile" : "fixed")
            .field("e164", e164())
            .field("international", international())
            .field("national", national())
            .field("reserved", reserved)
            .key("signature");
        _signature.to_json(w);
        w.end_object();
    }

    /// @brief This number as a JSON document.
    /// @return Compact JSON, for example
    ///   `{"country":"BR","type":"mobile","e164":"+55...",...}`.
    [[nodiscard]] std::string to_json() const
    {
        core::json_writer w;
        to_json(w);
        return w.str();
    }

    /// @brief Implicit conversion to std::string.
    /// @return Summary string.
    operator std::string() const // NOLINT(hicpp-explicit-conversions)
    {
        return to_string();
    }

    /// @brief Stream the summary to an output stream.
    /// @param os Output stream.
    /// @param v The number to stream.
    /// @return The stream.
    friend std::ostream& operator<<(std::ostream& os, const phone& v)
    {
        os << v.to_string();
        return os;
    }

  private:
    // Fills a template such as "($1) $2-$3" with the digit groups.
    [[nodiscard]] std::string apply_(std::string_view tmpl) const
    {
        std::string out;
        for (std::size_t i = 0; i < tmpl.size(); ++i)
        {
            if (tmpl[i] == '$' && i + 1 < tmpl.size() && tmpl[i + 1] >= '1' && tmpl[i + 1] <= '9')
            {
                const auto g = static_cast<std::size_t>(tmpl[++i] - '1');
                std::size_t from = 0;
                for (std::size_t k = 0; k < g && k < _groups.size(); ++k) from += _groups[k];
                if (g < _groups.size()) out.append(national_number, from, _groups[g]);
            }
            else
            {
                out += tmpl[i];
            }
        }
        return out;
    }

    core::seed_signature _signature;
    std::vector<std::uint8_t> _groups; // digit group lengths
    std::string _national;             // template, "$1" the first group
    std::string _international;        // template, or "-" for none
    friend class phg;
};

/// @brief The tables of the phone data, for extending it.
enum class phone_table : std::uint8_t
{
    countries,    ///< `countries.tsv`: calling codes, national prefixes and populations.
    ranges,       ///< `ranges.tsv`: the allocated number ranges, by country and type.
    formats,      ///< `formats.tsv`: the digit grouping and templates the ranges refer to.
    reserved      ///< `reserved.tsv`: the ranges set aside for fiction (the `safe` option).
};

/// @brief Phone Generator: phone numbers valid in each country's numbering plan.
///
/// A call draws a country (by population), a type, a range of that country
/// (by weight: even over the numbers of a length) and the remaining digits, each
/// from a stream split from the call seed (`"country"`, `"phone-type"`,
/// `"phone-range"`, `"phone-digits"`). Output is identical on every compiler
/// and platform (dasmig-core).
///
/// Can be used as a singleton via instance() or constructed
/// independently.
///
/// @par Thread safety
/// Each instance is independent.  Concurrent calls to get_phone(request, seed) on the
/// **same** instance are safe; calls that use the instance engine, loading and
/// extending require external synchronisation.
class phg
{
  public:
    /// @brief Default constructor — creates an empty generator with no
    ///        data.
    phg() : _tables{make_tables_()} {}

    phg(const phg&) = delete;            ///< Not copyable.
    phg& operator=(const phg&) = delete; ///< Not copyable.
    phg(phg&&) noexcept = default;       ///< Movable.
    phg& operator=(phg&&) noexcept = default; ///< Movable.
    ~phg() = default;                       ///< Destructor.

    /// @brief Access the global singleton instance.
    ///
    /// Loads the `lite` tier (or `full` if `lite` is missing) on first
    /// access, from compiled-in data or an auto-probed `resources/`.
    /// @return The singleton.
    static phg& instance()
    {
        static phg inst{auto_probe_tag{}};
        return inst;
    }

    // -- Generation -------------------------------------------------------

    /// @brief Generate a random phone number.
    ///        Use phone_request{} for the defaults.
    /// @param request What to generate.
    /// @return The phone number.
    /// @throws std::runtime_error If no data has been loaded.
    /// @throws std::invalid_argument If the country is unknown or has no
    ///         number of the requested type.
    [[nodiscard]] phone get_phone(const phone_request& request)
    {
        return get_phone(request, draw_seed_());
    }

    /// @brief Generate a deterministic phone number.
    /// @param request What to generate.
    /// @param call_seed Seed; the same request, seed and data give the same
    ///        result. Does not touch the instance engine.
    /// @return The phone number.
    /// @throws std::runtime_error If no data has been loaded.
    /// @throws std::invalid_argument If the country is unknown or has no
    ///         number of the requested type.
    [[nodiscard]] phone get_phone(const phone_request& request, std::uint64_t call_seed) const
    {
        require_data_();
        const core::rng call{call_seed};
        const std::size_t want = request.type ? static_cast<std::size_t>(*request.type) : any_;
        const auto& country = _model.countries[pick_country_(request, want, call)];
        const std::size_t ti = pick_type_(request, country, want, call);
        const auto& by_type = country.types.at(ti);

        auto rr = call.split("phone-range");
        auto dr = call.split("phone-digits");
        const bool reserved = request.safe && !by_type.reserved.empty();
        const range_data& range = reserved ? by_type.reserved[request.weighted ? by_type.reserved_weights.pick(rr)
                                                                               : pick_uniform_(rr, by_type.reserved.size())]
                                           : by_type.ranges[request.weighted ? by_type.weights.pick(rr)
                                                                             : pick_uniform_(rr, by_type.ranges.size())];

        phone v;
        v._signature = {.seed = call_seed, .data = data_hash()};
        v.country = country.region;
        v.country_code = country.code;
        v.type = ti == 0 ? phone_type::mobile : phone_type::fixed;
        v.reserved = reserved;
        v.national_number = draw_number_(request.safe && !reserved, country, range, dr);
        apply_format_(v, reserved ? nearest_format_(by_type, v.national_number) : range.format);
        return v;
    }

    // -- Batch generation -------------------------------------------------

    /// @brief Generate a batch of phone results from a seed drawn like
    ///        get_phone() draws its own.
    ///
    /// Item `i` is `get_phone(request, core::item_seed(batch_seed, options.first + i))`
    /// for the drawn batch seed; see the seeded overload.
    ///
    /// @param request What every item is generated from.
    /// @param options Count, first index, threads and uniqueness.
    /// @return The results, in index order.
    [[nodiscard]] std::vector<phone> get_phone_batch(const phone_request& request,
                                                     const core::batch_options& options)
    {
        return get_phone_batch(request, options, draw_seed_());
    }

    /// @brief Generate a batch of phone results, deterministically.
    ///
    /// Item `i` is exactly `get_phone(request, core::item_seed(batch_seed,
    /// options.first + i))`, so an item replays on its own, a range can be
    /// generated directly, two batches that follow each other equal one, and
    /// the output does not depend on `options.threads`.
    ///
    /// @param request What every item is generated from.
    /// @param options Count, first index, threads and uniqueness (see
    ///        core::unique_rule; the rule must be made for `phone`).
    /// @param batch_seed Seed of the whole batch.
    /// @return The results, in index order.
    /// @throws std::invalid_argument If the request or options are invalid.
    /// @throws std::runtime_error If no data has been loaded, or a unique
    ///         batch runs out of attempts.
    [[nodiscard]] std::vector<phone> get_phone_batch(const phone_request& request,
                                                     const core::batch_options& options,
                                                     std::uint64_t batch_seed) const
    {
        return core::run_batch(options, batch_seed,
                               [&](std::uint64_t item_seed) { return get_phone(request, item_seed); });
    }

    /// @brief Stream a batch of phone results to a sink without holding the
    ///        whole batch.
    ///
    /// Generates the same items as get_phone_batch() and calls
    /// `sink(index, item)` in index order on the calling thread; `index`
    /// includes `options.first`.
    ///
    /// @tparam Sink Callable taking `(std::uint64_t, phone&)`; for example
    ///         core::jsonl_writer or core::csv_writer.
    /// @param request What every item is generated from.
    /// @param options Count, first index, threads and uniqueness.
    /// @param batch_seed Seed of the whole batch.
    /// @param sink Receives the items.
    template <typename Sink>
    void for_each_phone(const phone_request& request, const core::batch_options& options,
                        std::uint64_t batch_seed, Sink&& sink) const
    {
        core::for_each_batch(options, batch_seed,
                             [&](std::uint64_t item_seed) { return get_phone(request, item_seed); },
                             std::forward<Sink>(sink));
    }

    /// @brief Stream a batch of phone results with a batch seed drawn like
    ///        get_phone() draws its own.
    /// @tparam Sink Callable taking `(std::uint64_t, phone&)`.
    /// @param request What every item is generated from.
    /// @param options Count, first index, threads and uniqueness.
    /// @param sink Receives the items.
    template <typename Sink>
    void for_each_phone(const phone_request& request, const core::batch_options& options, Sink&& sink)
    {
        for_each_phone(request, options, draw_seed_(), std::forward<Sink>(sink));
    }

    // -- Data management --------------------------------------------------

    /// @brief Check whether any data has been loaded.
    /// @return True when at least one country is loaded.
    [[nodiscard]] bool has_data() const { return !_model.countries.empty(); }

    /// @brief Return the number of countries loaded.
    /// @return The count.
    [[nodiscard]] std::size_t country_count() const { return _model.countries.size(); }

    /// @brief Whether a country is loaded.
    /// @param country ISO-3166 two-letter code (any case).
    /// @return True when countries.tsv has the country.
    [[nodiscard]] bool has_country(std::string_view country) const
    {
        return _model.by_region.contains(upper_(country));
    }

    /// @brief Whether a country has a range set aside for fiction.
    /// @param country ISO-3166 two-letter code (any case).
    /// @param type Mobile or fixed line.
    /// @return True when `safe` numbers of that type are reserved ones.
    [[nodiscard]] bool has_reserved(std::string_view country, phone_type type) const
    {
        const auto it = _model.by_region.find(upper_(country));
        return it != _model.by_region.end() &&
               !_model.countries[it->second].types.at(static_cast<std::size_t>(type)).reserved.empty();
    }

    /// @brief Load data from a resource directory, replacing all data and
    ///        layers loaded before.
    ///
    /// The directory must contain `countries.tsv`; the other tables are read
    /// when present. A path without `countries.tsv` is ignored.
    ///
    /// @param dir Directory such as `resources/lite`.
    /// @throws core::tsv_error If a file is malformed; the data is then
    ///         unchanged.
    void load(const std::filesystem::path& dir)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec) ||
            !std::filesystem::is_regular_file(dir / file_name_(phone_table::countries), ec))
        {
            return;
        }
        apply_(read_directory_(dir), core::layer_mode::replace, true);
    }

    /// @brief Load a dataset tier, replacing all data and layers loaded
    ///        before.
    ///
    /// Uses the compiled-in copy when the library is built with
    /// `DASMIG_PHONE_EMBED`, otherwise searches `resources/`, `../resources`,
    /// `phone-generator/resources` and `../phone-generator/resources`.
    ///
    /// @param tier The tier.
    /// @return False when the tier was not found.
    /// @throws core::tsv_error If a file is malformed.
    [[nodiscard]] bool load(dataset tier)
    {
        const std::string tier_dir{core::tier_name(tier)};
        const auto marker = tier_dir + "/" + std::string{file_name_(phone_table::countries)};
        const auto root = core::find_resources(library_name_, marker);
        documents docs;
        for (const auto t : all_tables_)
        {
            docs.at(index_(t)) = core::load_resource(tier_dir + "/" + std::string{file_name_(t)},
                                                     embedded_files_(), root);
        }
        if (!docs.at(index_(phone_table::countries))) return false;
        apply_(std::move(docs), core::layer_mode::replace, true);
        return true;
    }

    /// @brief Add data layers from a directory or a file.
    ///
    /// A directory may hold any of the table files (see phone_table); all
    /// are applied together or, if any fails, none. A file must have one of
    /// those names.
    ///
    /// @param path Directory or file.
    /// @param mode How the layers combine with the data loaded before.
    /// @throws core::tsv_error If a file cannot be read or a row is
    ///         invalid; the data is then unchanged.
    /// @throws std::invalid_argument If @p path is a file with another name.
    void extend(const std::filesystem::path& path, core::layer_mode mode = core::layer_mode::override)
    {
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec))
        {
            apply_(read_directory_(path), mode, false);
            return;
        }
        for (const auto t : all_tables_)
        {
            if (path.filename() == file_name_(t))
            {
                (void)extend(t, core::tsv_document::load(path), mode);
                return;
            }
        }
        throw std::invalid_argument("extend: expected countries.tsv, ranges.tsv, formats.tsv or "
                                    "reserved.tsv, got " + path.filename().string());
    }

    /// @brief Add a data layer to one table.
    ///
    /// Rows are keyed by the table's key columns (see doc/data.md). In the
    /// default override mode a row replaces the loaded row with the same key
    /// and new keys are added; a row with `disabled` set to 1 removes its key.
    ///
    /// @param which The table.
    /// @param doc TSV with that table's columns.
    /// @param mode How the layer combines with the data loaded before.
    /// @return What the layer did (rows added, replaced, removed, warnings).
    /// @throws core::tsv_error If a row is invalid; the data is unchanged.
    const core::layer_info& extend(phone_table which, core::tsv_document doc,
                                   core::layer_mode mode = core::layer_mode::override)
    {
        documents docs;
        docs.at(index_(which)).emplace(std::move(doc));
        apply_(std::move(docs), mode, false);
        return _tables.at(index_(which)).layers().back();
    }

    /// @brief Add a data layer to one table from TSV text (for example
    ///        from a WebAssembly host).
    /// @param which The table.
    /// @param text TSV with that table's columns.
    /// @param source Name used in errors.
    /// @param mode How the layer combines with the data loaded before.
    /// @return What the layer did.
    /// @throws core::tsv_error If a row is invalid; the data is unchanged.
    const core::layer_info& extend_from_string(phone_table which, std::string_view text, std::string source,
                                               core::layer_mode mode = core::layer_mode::override)
    {
        return extend(which, core::tsv_document::parse(text, std::move(source)), mode);
    }

    /// @brief The layers in effect for one table, in order.
    /// @param which The table.
    /// @return The layer records.
    [[nodiscard]] const std::vector<core::layer_info>& layers(phone_table which) const
    {
        return _tables.at(index_(which)).layers();
    }

    /// @brief Hash of the loaded data (all tables), recorded in each
    ///        result's signature().
    /// @return The hash; 0 when nothing is loaded.
    [[nodiscard]] std::uint64_t data_hash() const
    {
        std::uint64_t h = 0;
        bool any = false;
        for (const auto& t : _tables)
        {
            any = any || !t.layers().empty();
            h = core::hash_combine(h, t.data_hash());
        }
        return any ? h : 0;
    }

  private:
    using table = phone_table;

    // -- Tables -------------------------------------------------------------

    static constexpr std::string_view library_name_ = "phone-generator";
    static constexpr std::size_t table_count_ = 4;
    static constexpr std::array<table, table_count_> all_tables_{table::countries, table::ranges,
                                                                 table::formats, table::reserved};

    using documents = std::array<std::optional<core::tsv_document>, table_count_>;
    using tables = std::array<core::layered_table, table_count_>;

    static constexpr std::size_t index_(table t) { return static_cast<std::size_t>(t); }

    static constexpr std::string_view file_name_(table t)
    {
        switch (t)
        {
        case table::countries: return "countries.tsv";
        case table::ranges: return "ranges.tsv";
        case table::formats: return "formats.tsv";
        case table::reserved: return "reserved.tsv";
        }
        return "";
    }

    static core::table_schema schema_(table t)
    {
        switch (t)
        {
        case table::countries:
            return {.key = {"region"},
                    .required = {"code"},
                    .optional = {"intl_prefix", "national_prefix", "main", "population"}};
        case table::ranges:
            return {.key = {"region", "type", "prefix", "length"},
                    .required = {"mask", "weight", "format"},
                    .optional = {}};
        case table::formats:
            return {.key = {"id"}, .required = {"groups", "national"}, .optional = {"international"}};
        case table::reserved:
            return {.key = {"region", "type", "prefix", "length"}, .required = {"mask", "weight"}, .optional = {}};
        }
        return {};
    }

    static tables make_tables_()
    {
        return {core::layered_table{schema_(table::countries)}, core::layered_table{schema_(table::ranges)},
                core::layered_table{schema_(table::formats)}, core::layered_table{schema_(table::reserved)}};
    }

    // -- Parsed rows --------------------------------------------------------

    static constexpr std::size_t any_ = 2; // index of "either type" in the per-type arrays
    static constexpr double mobile_share_ = 0.7;
    static constexpr int outside_attempts_ = 64;

    // `count` digits, each one of the set bits of `digits` (bit d = digit d).
    struct mask_token
    {
        std::uint16_t digits{0x3ff};
        std::uint16_t count{1};
    };

    struct range_data
    {
        std::string prefix;
        std::vector<mask_token> tail;
        std::uint32_t length{0};
        double weight{0.0};
        std::uint32_t format{0};
    };

    struct format_data
    {
        std::vector<std::uint8_t> groups;
        std::size_t total{0};
        std::string national;
        std::string international;
    };

    struct type_data
    {
        std::vector<range_data> ranges;
        core::weighted_table<std::size_t> weights; // range index, by weight
        std::vector<range_data> reserved;
        core::weighted_table<std::size_t> reserved_weights;
    };

    struct country_data
    {
        std::string region;
        std::string code;
        double population{1.0};
        std::array<type_data, 2> types;
    };

    struct model
    {
        std::vector<country_data> countries;
        std::unordered_map<std::string, std::size_t> by_region;
        std::unordered_map<std::uint32_t, format_data> formats;
        std::array<core::weighted_table<std::size_t>, 3> by_population; // mobile, fixed, either
        std::array<core::weighted_table<std::size_t>, 3> by_uniform;
    };

    model _model;
    tables _tables;
    core::rng _engine{core::rng::from_entropy()};

    struct auto_probe_tag {};

    explicit phg(auto_probe_tag /*tag*/) : _tables{make_tables_()}
    {
        if (!load(dataset::lite)) (void)load(dataset::full);
    }

    // -- Steps of get_phone -------------------------------------------------

    // The country: the requested one, or a draw among those with numbers of the type.
    [[nodiscard]] std::size_t pick_country_(const phone_request& request, std::size_t want, const core::rng& call) const
    {
        if (!request.country)
        {
            auto cr = call.split("country");
            const auto& table = request.weighted ? _model.by_population.at(want) : _model.by_uniform.at(want);
            if (table.empty()) throw std::runtime_error("get_phone: no country has numbers of that type");
            return table.pick(cr);
        }
        const auto it = _model.by_region.find(upper_(*request.country));
        if (it == _model.by_region.end())
        {
            throw std::invalid_argument("get_phone: unknown country '" + *request.country + "'");
        }
        if (want != any_ && _model.countries[it->second].types.at(want).ranges.empty())
        {
            throw std::invalid_argument("get_phone: " + _model.countries[it->second].region + " has no " +
                                        (want == 0 ? "mobile" : "fixed-line") + " numbers");
        }
        return it->second;
    }

    // The type: the requested one, or mobile seven times in ten when the country has both.
    [[nodiscard]] static std::size_t pick_type_(const phone_request& request, const country_data& country,
                                                std::size_t want, const core::rng& call)
    {
        if (want != any_) return want;
        bool has_mobile = !country.types[0].ranges.empty();
        bool has_fixed = !country.types[1].ranges.empty();
        if (request.safe && has_mobile && has_fixed)
        {
            // Prefer the type the regulator has a range for.
            const bool reserved_mobile = !country.types[0].reserved.empty();
            const bool reserved_fixed = !country.types[1].reserved.empty();
            if (reserved_mobile != reserved_fixed)
            {
                has_mobile = reserved_mobile;
                has_fixed = reserved_fixed;
            }
        }
        if (!has_mobile && !has_fixed)
        {
            throw std::invalid_argument("get_phone: " + country.region + " has no numbers");
        }
        auto tr = call.split("phone-type");
        return has_mobile && (!has_fixed || core::bernoulli(tr, mobile_share_)) ? 0 : 1;
    }

    // The digits of a range; with @p outside (safe, no reserved range) a number no allocated
    // range of the country holds, when one turns up in a few tries.
    [[nodiscard]] static std::string draw_number_(bool outside, const country_data& country, const range_data& range,
                                                  core::rng& dr)
    {
        std::string digits = draw_digits_(range, dr);
        for (int attempt = 0; outside && attempt < outside_attempts_; ++attempt)
        {
            std::string candidate = random_digits_(range.length, dr);
            if (!country_holds_(country, candidate))
            {
                return candidate;
            }
        }
        return digits;
    }

    // A reserved range has no format of its own and need not be allocated: the format of the
    // allocated range of the same length that shares the longest prefix with the number.
    static std::uint32_t nearest_format_(const type_data& by_type, const std::string& digits)
    {
        std::uint32_t format = 0;
        std::size_t best = 0;
        bool found = false;
        for (const auto& r : by_type.ranges)
        {
            if (r.length != digits.size()) continue;
            std::size_t common = 0;
            while (common < r.prefix.size() && r.prefix[common] == digits[common]) ++common;
            if (!found || common > best)
            {
                best = common;
                format = r.format;
                found = true;
            }
        }
        return format;
    }

    // Writes the format's groups and templates into the number, or one group of every digit.
    void apply_format_(phone& v, std::uint32_t format) const
    {
        const auto it = _model.formats.find(format);
        if (it != _model.formats.end() && it->second.total == v.national_number.size())
        {
            v._groups = it->second.groups;
            v._national = it->second.national;
            v._international = it->second.international;
            return;
        }
        v._groups = {static_cast<std::uint8_t>(v.national_number.size())};
        v._national = "$1";
        v._international = "$1";
    }

    // -- Helpers ----------------------------------------------------------

    static std::span<const core::embedded_file> embedded_files_()
    {
#ifdef DASMIG_PHONE_EMBED
        return embedded::phonegen::files;
#else
        return {};
#endif
    }

    std::uint64_t draw_seed_() { return _engine(); }

    static std::string upper_(std::string_view s)
    {
        std::string out{s};
        for (auto& c : out)
        {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
        return out;
    }

    static std::size_t pick_uniform_(core::rng& r, std::size_t n)
    {
        return core::uniform_int<std::size_t>(r, 0, n - 1);
    }

    static constexpr std::uint16_t digit_bit_(int d) { return static_cast<std::uint16_t>(1U << static_cast<unsigned>(d)); }

    static constexpr bool has_digit_(std::uint16_t set, int d) { return (set & digit_bit_(d)) != 0U; }

    static char draw_digit_(const mask_token& t, core::rng& r)
    {
        const int n = std::popcount(static_cast<unsigned>(t.digits));
        int k = core::uniform_int(r, 0, n - 1);
        for (int d = 0; d < 10; ++d)
        {
            if (has_digit_(t.digits, d) && k-- == 0) return static_cast<char>('0' + d);
        }
        return '0';
    }

    static std::string draw_digits_(const range_data& range, core::rng& r)
    {
        std::string out = range.prefix;
        for (const auto& t : range.tail)
        {
            for (std::uint16_t i = 0; i < t.count; ++i) out += draw_digit_(t, r);
        }
        return out;
    }

    static std::string random_digits_(std::size_t length, core::rng& r)
    {
        std::string out;
        out.reserve(length);
        for (std::size_t i = 0; i < length; ++i) out += static_cast<char>('0' + core::uniform_int(r, 0, 9));
        return out;
    }

    static bool range_holds_(const range_data& range, std::string_view digits)
    {
        if (digits.size() != range.length || !digits.starts_with(range.prefix)) return false;
        std::size_t pos = range.prefix.size();
        for (const auto& t : range.tail)
        {
            for (std::uint16_t i = 0; i < t.count; ++i, ++pos)
            {
                if (!has_digit_(t.digits, digits[pos] - '0')) return false;
            }
        }
        return true;
    }

    static bool country_holds_(const country_data& country, std::string_view digits)
    {
        return std::ranges::any_of(country.types, [&](const type_data& t) {
            return std::ranges::any_of(t.ranges, [&](const range_data& r) { return range_holds_(r, digits); });
        });
    }

    static documents read_directory_(const std::filesystem::path& dir)
    {
        documents docs;
        for (const auto t : all_tables_)
        {
            const auto file = dir / file_name_(t);
            std::error_code ec;
            if (std::filesystem::is_regular_file(file, ec))
            {
                docs.at(index_(t)) = core::tsv_document::load(file);
            }
        }
        return docs;
    }

    // Apply layers to copies of the tables, then commit: all or nothing.
    // With @p reset (load), tables without a document are emptied.
    void apply_(documents docs, core::layer_mode mode, bool reset)
    {
        tables next = _tables;
        for (const auto t : all_tables_)
        {
            auto& doc = docs.at(index_(t));
            auto& tbl = next.at(index_(t));
            if (!doc)
            {
                if (reset) tbl.clear();
                continue;
            }
            (void)tbl.extend(std::move(*doc), mode, [t](const core::tsv_row& row) { validate_(t, row); });
        }
        auto built = build_model_(next);
        _tables = std::move(next);
        _model = std::move(built);
    }

    // -- Row checks and the model -------------------------------------------

    // The digits of a `[...]` class (its @p body, between the brackets), as a set.
    static std::uint16_t parse_class_(const core::tsv_row& row, std::string_view body)
    {
        std::uint16_t bits = 0;
        for (std::size_t k = 0; k < body.size(); ++k)
        {
            if (body[k] < '0' || body[k] > '9') throw row.error("mask", "bad class '[" + std::string{body} + "]'");
            const bool span = k + 2 < body.size() && body[k + 1] == '-';
            const char last = span ? body[k + 2] : body[k];
            if (last < body[k]) throw row.error("mask", "bad range in '[" + std::string{body} + "]'");
            for (char d = body[k]; d <= last; ++d) bits |= digit_bit_(d - '0');
            if (span) k += 2;
        }
        if (bits == 0) throw row.error("mask", "empty class");
        return bits;
    }

    // The count of a `{n}` repeat (its @p body, between the braces).
    static std::uint16_t parse_repeat_(const core::tsv_row& row, std::string_view body)
    {
        unsigned n = 0;
        for (const char c : body)
        {
            if (c < '0' || c > '9') throw row.error("mask", "bad repeat '{" + std::string{body} + "}'");
            n = n * 10 + static_cast<unsigned>(c - '0');
            if (n > 40) throw row.error("mask", "repeat too large");
        }
        if (n == 0) throw row.error("mask", "repeat must be positive");
        return static_cast<std::uint16_t>(n);
    }

    static std::vector<mask_token> parse_mask_(const core::tsv_row& row, std::string_view text)
    {
        std::vector<mask_token> out;
        std::size_t i = 0;
        while (i < text.size())
        {
            mask_token t;
            const char c = text[i];
            if (c == '[')
            {
                const auto close = text.find(']', i);
                if (close == std::string_view::npos) throw row.error("mask", "unclosed '['");
                t.digits = parse_class_(row, text.substr(i + 1, close - i - 1));
                i = close + 1;
            }
            else if (c == '#' || (c >= '0' && c <= '9'))
            {
                t.digits = c == '#' ? std::uint16_t{0x3ff} : digit_bit_(c - '0');
                ++i;
            }
            else
            {
                throw row.error("mask", std::string{"unexpected '"} + c + "' in '" + std::string{text} + "'");
            }
            if (i < text.size() && text[i] == '{')
            {
                const auto close = text.find('}', i);
                if (close == std::string_view::npos) throw row.error("mask", "unclosed '{'");
                t.count = parse_repeat_(row, text.substr(i + 1, close - i - 1));
                i = close + 1;
            }
            out.push_back(t);
        }
        return out;
    }

    static range_data parse_range_(const core::tsv_row& row, bool with_format)
    {
        range_data r;
        r.prefix = std::string{row["prefix"]};
        for (const char c : r.prefix)
        {
            if (c < '0' || c > '9') throw row.error("prefix", "must be digits, got '" + r.prefix + "'");
        }
        r.tail = parse_mask_(row, row["mask"]);
        const auto length = row.get<std::int64_t>("length");
        if (length < 1 || length > 20) throw row.error("length", "must be 1 to 20");
        r.length = static_cast<std::uint32_t>(length);
        std::size_t digits = r.prefix.size();
        for (const auto& t : r.tail) digits += t.count;
        if (digits != r.length) throw row.error("mask", "prefix and mask hold " + std::to_string(digits) + " digits, not " + std::to_string(r.length));
        r.weight = row.get<double>("weight");
        if (!(r.weight >= 0.0)) throw row.error("weight", "must not be negative");
        if (with_format)
        {
            const auto f = row.get<std::int64_t>("format");
            if (f < 0) throw row.error("format", "must not be negative");
            r.format = static_cast<std::uint32_t>(f);
        }
        return r;
    }

    static std::size_t type_index_(const core::tsv_row& row)
    {
        const auto t = row["type"];
        if (t == "mobile") return 0;
        if (t == "fixed") return 1;
        throw row.error("type", "must be 'mobile' or 'fixed', got '" + std::string{t} + "'");
    }

    static void validate_(table t, const core::tsv_row& row)
    {
        switch (t)
        {
        case table::countries:
        {
            if (row["region"].size() != 2) throw row.error("region", "must be an ISO 3166-1 alpha-2 code");
            const auto code = row["code"];
            if (code.empty() || code.size() > 4) throw row.error("code", "must be a calling code of 1 to 4 digits");
            for (const char c : code)
            {
                if (c < '0' || c > '9') throw row.error("code", "must be digits");
            }
            if (row.get_or<double>("population", 1.0) < 0.0) throw row.error("population", "must not be negative");
            break;
        }
        case table::ranges:
            (void)type_index_(row);
            (void)parse_range_(row, true);
            break;
        case table::reserved:
            (void)type_index_(row);
            (void)parse_range_(row, false);
            break;
        case table::formats:
        {
            const auto id = row.get<std::int64_t>("id");
            if (id < 0) throw row.error("id", "must not be negative");
            (void)parse_groups_(row);
            break;
        }
        }
    }

    static std::vector<std::uint8_t> parse_groups_(const core::tsv_row& row)
    {
        std::vector<std::uint8_t> out;
        unsigned n = 0;
        bool digit = false;
        for (const char c : std::string{row["groups"]} + "-")
        {
            if (c >= '0' && c <= '9')
            {
                n = n * 10 + static_cast<unsigned>(c - '0');
                digit = true;
                if (n > 30) throw row.error("groups", "group too long");
            }
            else if (c == '-' && digit)
            {
                if (n == 0) throw row.error("groups", "empty group");
                out.push_back(static_cast<std::uint8_t>(n));
                n = 0;
                digit = false;
            }
            else
            {
                throw row.error("groups", "expected lengths joined with '-', got '" + std::string{row["groups"]} + "'");
            }
        }
        return out;
    }

    static void read_formats_(model& m, const tables& tbls)
    {
        for (const auto& row : tbls.at(index_(table::formats)).rows())
        {
            format_data f;
            f.groups = parse_groups_(row);
            for (const auto g : f.groups) f.total += g;
            f.national = std::string{row["national"]};
            f.international = row.has("international") && !row["international"].empty()
                                  ? std::string{row["international"]}
                                  : f.national;
            m.formats[static_cast<std::uint32_t>(row.get<std::int64_t>("id"))] = std::move(f);
        }
    }

    static void read_countries_(model& m, const tables& tbls)
    {
        for (const auto& row : tbls.at(index_(table::countries)).rows())
        {
            country_data c;
            c.region = upper_(row["region"]);
            c.code = std::string{row["code"]};
            c.population = std::max(row.get_or<double>("population", 1.0), 1.0);
            m.by_region.emplace(c.region, m.countries.size());
            m.countries.push_back(std::move(c));
        }
    }

    static country_data& locate_(model& m, const core::tsv_row& row)
    {
        const auto it = m.by_region.find(upper_(row["region"]));
        if (it == m.by_region.end())
        {
            throw row.error("region", "no country '" + std::string{row["region"]} + "' in countries.tsv");
        }
        return m.countries[it->second];
    }

    static void read_ranges_(model& m, const tables& tbls)
    {
        for (const auto& row : tbls.at(index_(table::ranges)).rows())
        {
            auto& c = locate_(m, row);
            auto r = parse_range_(row, true);
            const auto f = m.formats.find(r.format);
            if (f == m.formats.end()) throw row.error("format", "no format " + std::to_string(r.format) + " in formats.tsv");
            if (f->second.total != r.length)
            {
                throw row.error("format", "format " + std::to_string(r.format) + " groups " + std::to_string(f->second.total) +
                                              " digits, the range has " + std::to_string(r.length));
            }
            c.types.at(type_index_(row)).ranges.push_back(std::move(r));
        }
        for (const auto& row : tbls.at(index_(table::reserved)).rows())
        {
            locate_(m, row).types.at(type_index_(row)).reserved.push_back(parse_range_(row, false));
        }
    }

    // The weighted tables of the draws; only countries with numbers of the type enter them.
    static void build_draw_tables_(model& m)
    {
        for (std::size_t i = 0; i < m.countries.size(); ++i)
        {
            auto& c = m.countries[i];
            std::array<bool, 2> has{};
            for (std::size_t t = 0; t < 2; ++t)
            {
                auto& td = c.types.at(t);
                has.at(t) = !td.ranges.empty();
                for (std::size_t k = 0; k < td.ranges.size(); ++k) td.weights.add(k, td.ranges[k].weight);
                for (std::size_t k = 0; k < td.reserved.size(); ++k) td.reserved_weights.add(k, td.reserved[k].weight);
                if (has.at(t) && !(td.weights.total_weight() > 0.0))
                {
                    throw std::invalid_argument("country " + c.region + ": every range has weight 0");
                }
            }
            const std::array<bool, 3> offered{has.at(0), has.at(1), has.at(0) || has.at(1)};
            for (std::size_t t = 0; t < 3; ++t)
            {
                if (!offered.at(t)) continue;
                m.by_population.at(t).add(i, c.population);
                m.by_uniform.at(t).add(i, 1.0);
            }
        }
    }

    static model build_model_(const tables& tbls)
    {
        model m;
        read_formats_(m, tbls);
        read_countries_(m, tbls);
        read_ranges_(m, tbls);
        build_draw_tables_(m);
        return m;
    }

    void require_data_() const
    {
        if (_model.countries.empty())
        {
            throw std::runtime_error("No phone data loaded. Call load() first.");
        }
    }
};

} // namespace dasmig

#endif // DASMIG_PHONEGEN_HPP
