# Usage Guide

This guide covers every feature of the phone-generator library. For a quick overview, see the [README](../README.md). For the data and how it was built, see [Data and Method](data.md). For the full API reference, run `doxygen Doxyfile` from the repository root and open `doc/api/html/index.html`.

[TOC]

<!-- GitHub-rendered TOC (Doxygen uses [TOC] above instead) -->
<ul>
<li><a href="#quick-start">Quick Start</a></li>
<li><a href="#installation">Installation</a></li>
<li><a href="#loading-resources">Loading Resources</a></li>
<li><a href="#generating">Generating</a></li>
<li><a href="#safe-numbers-for-tests">Safe Numbers for Tests</a></li>
<li><a href="#seeding-and-deterministic-generation">Seeding and Deterministic Generation</a></li>
<li><a href="#extending-data">Extending Data</a></li>
<li><a href="#replay-and-json">Replay and JSON</a></li>
<li><a href="#compiled-in-data">Compiled-in Data</a></li>
<li><a href="#thread-safety">Thread Safety</a></li>
<li><a href="#batch-generation">Batch Generation</a></li>
<li><a href="#error-reference">Error Reference</a></li>
</ul>

## Quick Start

```cpp
#include <dasmig/phonegen.hpp>
#include <iostream>

int main()
{
    auto& gen = dasmig::phg::instance();
    std::cout << gen.get_phone(dasmig::phone_request{}) << '\n';
    // +55 11 91234-5678
}
```

## Installation

Copy `dasmig/phonegen.hpp`, `dasmig/core.hpp` and the `dasmig/core/` directory into your include path, keeping them together. Supply `resources/` next to the program, or define `DASMIG_PHONE_EMBED` to compile the `lite` tier in.

## Loading Resources

`instance()` loads the `lite` tier on first use (or `full` if there is no `lite`), from the compiled-in copy or from the first of `resources/`, `../resources`, `phone-generator/resources` and `../phone-generator/resources` that has it. `lite` holds the 195 sovereign states, `full` the territories as well (the ones libphonenumber has a plan for).

```cpp
dasmig::phg gen;                            // an empty, independent instance
bool ok = gen.load(dasmig::dataset::lite);  // false when the tier is not found
gen.load(std::filesystem::path{"path/to/resources/lite"}); // a directory
gen.has_data();
gen.country_count();
gen.has_country("br");
gen.has_reserved("US", dasmig::phone_type::fixed);
```

Each `load` replaces the data and every layer loaded before.

## Generating

```cpp
auto p = gen.get_phone(dasmig::phone_request{});               // any country
auto br = gen.get_phone({.country = "BR", .type = dasmig::phone_type::mobile});

br.country;          // "BR"
br.country_code;     // "55"
br.national_number;  // "11912345678"
br.type;             // phone_type::mobile
br.reserved;         // false (see the safe option)
br.e164();           // "+5511912345678"
br.international();  // "+55 11 91234-5678"
br.national();       // "(11) 91234-5678"
br.rfc3966();        // "tel:+55-11-91234-5678"
```

- **Valid numbers.** Each country's mobile and fixed-line plan is stored as prefix ranges built from libphonenumber. A number is a prefix plus digits drawn from the range's masks, so it passes a validator. The draw is uniform over the valid numbers of the country, so a prefix with more numbers is more likely.
- **Country.** Absent, it is drawn by population among the countries that have numbers of the type. `weighted = false` draws countries and ranges uniformly.
- **Type.** Absent, mobile is drawn seven times in ten, or the only type the country has. An explicit type the country lacks throws `std::invalid_argument`.
- **Formats** are libphonenumber's. They can differ from the digits: an Argentinian mobile is `+54 9 11 5811-5808` internationally and `11 15-5811-5808` nationally.
- Toll-free, premium-rate, shared-cost, VoIP and other number types are not generated.

## Safe Numbers for Tests

`safe = true` makes a number that is unlikely to reach a real person:

- Where the country's regulator has set a range aside for fiction the number comes from it and `reserved` is true. Confirmed ranges are in the US and Canada (555-0100 to 0199 under every area code where those numbers are valid), GB (mobile 07700 900000 to 900999), Germany (Bundesnetzagentur drama numbers) and Australia ((02) 5550 xxxx). `has_reserved(country, type)` tells which countries have a range.
- Everywhere else `reserved` is false and the number is drawn outside the country's allocated ranges: it will usually fail validation and is unlikely to be anyone's, but nothing guarantees it. There is no error, so a batch over random countries does not fail.
- With no type, a country with a reserved range of one type only gives that type.

## Seeding and Deterministic Generation

- `get_phone(request, seed)` gives the same result for the same request, seed and data, on every platform, without advancing the instance engine.
- `get_phone(request)` draws a fresh seed from the instance engine, initially seeded from system entropy. There are no generator-level `seed()` or `unseed()` controls; for deterministic sequences, keep a `dasmig::core::rng` in the caller and pass its next value as the seed.
- `p.seed()` is the seed a result was generated from: it replays it with the same request and data.
- The call draws from streams split from the call seed (`"country"`, `"phone-type"`, `"phone-range"`, `"phone-digits"`), so adding a field later does not change the others.

## Extending Data

The shipped data is a base layer in four tables (see [Data and Method](data.md)): `countries`, `ranges`, `formats` and `reserved`. A layer adds, changes or removes rows of one table without editing the shipped files.

```cpp
using dasmig::phone_table;

// Add a country with one mobile range and a format, from text.
gen.extend_from_string(phone_table::countries, "region\tcode\tpopulation\nXX\t999\t1\n", "xx");
gen.extend_from_string(phone_table::formats, "id\tgroups\tnational\tinternational\n9000\t3-4\t$1 $2\t$1-$2\n", "xx");
gen.extend_from_string(phone_table::ranges,
    "region\ttype\tprefix\tlength\tmask\tweight\tformat\nXX\tmobile\t7\t7\t[1-3]#{5}\t300000\t9000\n", "xx");

// Remove rows: a layer needs only the key and a disabled column.
gen.extend_from_string(phone_table::ranges, "region\ttype\tprefix\tlength\tdisabled\nXX\tmobile\t7\t7\t1\n", "cleanup");

// A whole directory of tables, or one file named like a table.
gen.extend("mods/phone-data");
gen.extend("mods/reserved.tsv", dasmig::core::layer_mode::add);
```

- Every value in a layer is checked before anything changes: masks must add up to the length, formats must group exactly the digits of the ranges that use them, and regions and formats must exist. A bad layer throws `dasmig::core::tsv_error` with the file, line and column, and the data stays as it was.
- `layers(table)` lists what is loaded for a table: source, mode, rows added, replaced and removed, and warnings such as unknown columns.
- `load(...)` replaces the base and drops every layer.

## Replay and JSON

Every result records the seed and the data it came from:

```cpp
auto v = gen.get_phone(dasmig::phone_request{});
v.signature().seed; // the call seed
v.signature().data; // gen.data_hash() when it was generated
```

`to_json()` gives the result as JSON, the form the WebAssembly layer and the game consume. Seeds are strings because JavaScript numbers cannot hold every 64-bit value. `to_json(writer)` writes into an existing `dasmig::core::json_writer`, for composing larger documents.

## Compiled-in Data

Define `DASMIG_PHONE_EMBED` before including the header to compile the `lite` tier into the program. `load(dataset::lite)` and `instance()` then use it and need no files. The compiled-in data has the same `data_hash()` as the files, so seeds replay the same either way.

`dasmig/phonegen_data.hpp` is generated from `resources/lite` by `make embed`; CI checks it is up to date. It is about 670 KB.

## Thread Safety

Each `phg` instance is independent. Concurrent calls to `get_phone(request, seed)` on the **same** instance are safe; calls that use the instance engine (`get_phone(request)`), loading and extending require external synchronisation (e.g. a `std::mutex`).

## Batch Generation

`get_phone_batch` returns a `std::vector`, and `for_each_phone` streams the items to a sink in index order without holding the batch.

```cpp
auto& gen = dasmig::phg::instance();
const dasmig::phone_request request{.country = "US", .safe = true};

// 1,000 numbers from one batch seed, on four threads.
auto items = gen.get_phone_batch(request, {.count = 1000, .threads = 4}, 42);

// Item 1 replays on its own: the batch seed and the index give its seed.
auto again = gen.get_phone(request, dasmig::core::item_seed(42, 1));

// Unique numbers.
auto rule = dasmig::core::unique_rule::by<dasmig::phone>([](const dasmig::phone& p) { return p.e164(); });
auto unique = gen.get_phone_batch({.country = "GB"}, {.count = 5000, .unique = rule}, 7);

// A million numbers to a file as JSON Lines, never all in memory.
std::ofstream out{"phones.jsonl"};
dasmig::core::jsonl_writer lines{out};
gen.for_each_phone(request, {.count = 1'000'000, .threads = 0}, 42, lines);
```

- **Item seeds.** Item `i` is exactly `get_phone(request, core::item_seed(batch_seed, first + i))`. A range can be generated directly (`first = 1000` starts at item 1000), two batches that follow each other equal one, and the output is the same for any `threads` (0 means the hardware count). Without a `batch_seed` one is drawn like the single getter draws its own.
- **Options.** `core::batch_options` has `count`, `first`, `threads` and `unique`. The request describes one item; the options describe the batch.
- **Uniqueness.** `.unique = core::unique_rule::by<phone>(key)` regenerates an item whose key already appeared in the batch, with a deterministic retry seed, and throws `std::runtime_error` naming the item when the attempts run out. A small country has far fewer valid numbers than a million, and the reserved ranges hold only 100 to 1,000 each.
- **Sinks.** `core::jsonl_writer` writes one `to_json()` per line, `core::csv_writer` flattens each result's JSON into dotted columns. Any callable `(std::uint64_t index, phone&)` works.
- Batch calls use the same const getters from several threads, which the thread-safety rules above allow; the CI runs the batch tests under ThreadSanitizer.

## Error Reference

| Exception | When |
|-----------|------|
| `std::runtime_error` | Any generation call before data is loaded; a unique batch that runs out of attempts |
| `std::invalid_argument` | An unknown country; a type the country has no numbers of; a country with no numbers |
| `dasmig::core::tsv_error` | A malformed table or layer (message gives file, line and column); the data is unchanged |
