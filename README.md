# Phone Generator for C++

> **Requires C++23** (e.g., `-std=c++23` for GCC/Clang, `/std:c++latest` for MSVC).

[![Phone Generator for C++](https://raw.githubusercontent.com/dasmig/phone-generator/main/doc/phone-generator.png)](https://github.com/dasmig/phone-generator/releases)

[![GitHub license](https://img.shields.io/badge/license-MIT-blue.svg)](https://raw.githubusercontent.com/dasmig/phone-generator/main/LICENSE.MIT)
[![CI](https://github.com/dasmig/phone-generator/actions/workflows/ci.yml/badge.svg)](https://github.com/dasmig/phone-generator/actions/workflows/ci.yml)
[![GitHub Releases](https://img.shields.io/github/release/dasmig/phone-generator.svg)](https://github.com/dasmig/phone-generator/releases)
[![C++23](https://img.shields.io/badge/standard-C%2B%2B23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Header-only](https://img.shields.io/badge/type-header--only-green.svg)](https://github.com/dasmig/phone-generator#integration)
[![Documentation](https://img.shields.io/badge/docs-GitHub%20Pages-blue.svg)](https://dasmig.github.io/phone-generator/)

**[API Reference](https://dasmig.github.io/phone-generator/)** · **[Usage Guide](doc/usage.md)** · **[Data and Method](doc/data.md)** · **[Releases](https://github.com/dasmig/phone-generator/releases)**

Phone numbers that are valid in each country's numbering plan, for tests, fixtures and generated users. Numbers come from libphonenumber's plans (mobile and fixed line, 195 countries), so they pass a validator, and are written the way the country writes them. A `safe` option uses the ranges regulators set aside for fiction where there are any.

## Features

- **Valid Numbers**. Every number is drawn from the ranges of the country's own plan (libphonenumber), so it passes a validator; 100,000 draws checked against Python's `phonenumbers` were all valid.

- **Written Like the Country Writes Them**. E.164, international, national and RFC 3966 forms, with the country's grouping and national prefix.

- **Safe for Tests**. `safe` uses the numbers regulators reserve for fiction where they exist (US/CA 555-01xx, GB 07700 900xxx, DE, AU) and says so in `reserved`; elsewhere it avoids the allocated ranges.

- **Deterministic Seeding**. Per-call `get_phone(request, seed)` for reproducible results, and `seed()` on the result for replaying a previous generation with the same request and data.

- **Batch Generation**. Generate thousands of results from one seed, on several threads with the same output for any thread count, optionally unique by a key, and stream them as JSON Lines or CSV (see the usage guide).

- **Identical Everywhere**. A seed gives the same result on every compiler, standard library and CPU (GCC, Clang, MSVC, Emscripten; x86-64 and ARM64), checked by known-answer tests.

- **Extensible Data**. Load your own TSV layers on top of the shipped data to add, change or remove entries (`extend()`), checked before they are applied.

- **JSON and Replay**. `to_json()` on every result, and a `signature()` recording the seed and the data it came from.

- **Compiled-in Data**. Define `DASMIG_PHONE_EMBED` to build the `lite` tier into the program, for WebAssembly and other builds without files.

- **Multi-Instance Support**. Construct independent `phg` instances with their own data and random engine.

## Integration

[`phonegen.hpp`](https://github.com/dasmig/phone-generator/blob/main/dasmig/phonegen.hpp) is the library [released here](https://github.com/dasmig/phone-generator/releases). Keep [`core.hpp`](https://github.com/dasmig/phone-generator/blob/main/dasmig/core.hpp) and the [`core/`](https://github.com/dasmig/phone-generator/tree/main/dasmig/core) directory ([dasmig-core](https://github.com/dasmig/dasmig-core)) in the same directory. Add

```cpp
#include <dasmig/phonegen.hpp>
```

to the files you want to use it in, and supply the [`resources`](https://github.com/dasmig/phone-generator/tree/main/resources) folder (or define `DASMIG_PHONE_EMBED`).

## Usage

```cpp
#include <dasmig/phonegen.hpp>
#include <iostream>

auto& gen = dasmig::phg::instance(); // loads the lite tier

// Any country, or one you name.
auto p = gen.get_phone({.country = "BR", .type = dasmig::phone_type::mobile});
std::cout << p.international() << '\n'; // +55 11 91234-5678
std::cout << p.national() << '\n';      // (11) 91234-5678
std::cout << p.e164() << '\n';          // +5511912345678

// A number that reaches nobody, for tests.
auto safe = gen.get_phone({.country = "US", .safe = true});
std::cout << safe << ' ' << safe.reserved << '\n'; // +1 202-555-0143 1

// The same seed gives the same result everywhere.
auto again = gen.get_phone({.country = "BR", .type = dasmig::phone_type::mobile}, p.seed());
```

See the [usage guide](doc/usage.md) for everything else.

## Building

```bash
make            # example
make test       # tests
make bench      # benchmarks, results in build/bench.tsv
make test-embed # tests with the lite tier compiled in
make coverage   # code coverage
make lint       # clang-tidy
make docs       # API docs
make embed      # regenerate dasmig/phonegen_data.hpp from resources/lite
```

## Compiler Support

Tested with:
- Clang 18+ (`-std=c++23`)
- GCC 14+ (`-std=c++23`)
- MSVC 19.38+ (`/std:c++latest`)
- Emscripten 3.1.74

## Dependencies

| Dependency | Version | Bundled | Purpose |
|-----------|---------|---------|---------|
| [dasmig-core](https://github.com/dasmig/dasmig-core) | 1.3 | Yes (`core.hpp`, `core/`) | Platform-independent random engine and distributions, data layers, JSON |
| [Catch2](https://github.com/catchorg/Catch2) | 3.x | Yes (amalgamated) | Unit testing |

## Related Libraries

| Library | Description |
|---------|-------------|
| [address-generator](https://github.com/dasmig/address-generator) | Postal addresses with real streets by place |
| [biodata-generator](https://github.com/dasmig/biodata-generator) | Procedural human physical characteristics |
| [birth-generator](https://github.com/dasmig/birth-generator) | Demographically plausible birthdays |
| [city-generator](https://github.com/dasmig/city-generator) | Weighted city selection by population |
| [country-generator](https://github.com/dasmig/country-generator) | Weighted country selection by population |
| [device-generator](https://github.com/dasmig/device-generator) | Devices, browsers, user agents and IP addresses that fit together |
| [email-generator](https://github.com/dasmig/email-generator) | Email addresses in each country's style |
| [entity-generator](https://github.com/dasmig/entity-generator) | ECS-based entity generation |
| [interest-generator](https://github.com/dasmig/interest-generator) | Religion, hobbies and favourite media by country, age and sex |
| [name-generator](https://github.com/dasmig/name-generator) | Culturally appropriate full names |
| [nickname-generator](https://github.com/dasmig/nickname-generator) | Gamer-style nicknames |
| [occupation-generator](https://github.com/dasmig/occupation-generator) | Employment status, education and occupation by country, age and sex |
| [persona-generator](https://github.com/dasmig/persona-generator) | Complete characters composed from the other generators |
| [relationship-generator](https://github.com/dasmig/relationship-generator) | Families around a person: parents, siblings, partners and children |
| [trait-generator](https://github.com/dasmig/trait-generator) | Personality traits with conflicts, synergies and Big Five axes |
| [user-generator](https://github.com/dasmig/user-generator) | Complete user records: account, username, email, phone and address |

## License

This library is released under the [MIT License](https://opensource.org/licenses/MIT).

```
MIT License

Copyright (c) 2026 Diego Dasso Migotto
```
