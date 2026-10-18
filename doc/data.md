# Data and Method

The data in `resources/` and how phone-generator uses it (rule 6 of the dasmig standard).

## Tiers

| Tier | Regions | Ranges | What it holds |
| --- | --- | --- | --- |
| `lite` | 195 | 11,791 | The sovereign states of `country-generator`'s `lite` tier that libphonenumber has a plan for (all 195) |
| `full` | 243 | 12,890 | The 250 territories of its `full` tier; seven (AQ, BV, GS, HM, PN, TF, UM) have no libphonenumber plan |

## Files

Built by `scripts/prepare_phones.py` from libphonenumber v9.0.41 (Apache-2.0). The mobile and fixed-line number patterns are compiled to automata and walked digit by digit until the rest of the number is a product of per-position digit sets; toll-free, premium and other types are left out. The patterns are test data (`tests/data/phone_regex.tsv`) and are not shipped.

| File | Columns | Source |
| --- | --- | --- |
| `countries.tsv` | `region`, `code`, `intl_prefix`, `national_prefix`, `main`, `population` | libphonenumber; calling codes cross-checked against `country-generator`'s `idd_*` by the script |
| `ranges.tsv` | `region`, `type`, `prefix`, `mask`, `length`, `weight`, `format` | Derived. `mask` is the digits after `prefix` (`#` any digit, `[2-9]` a class, `#{6}` a run); `weight` is the range's share of the draws: even over the numbers of a length, and a plan with several lengths is drawn mostly at the length of libphonenumber's example number, a quarter as often one digit away, and so on (the plans list every allowed length, not how common it is). Lengths that would pass 15 digits with the calling code are left out (E.164) |
| `formats.tsv` | `id`, `groups`, `national`, `international` | libphonenumber `numberFormat`; the national prefix rule is applied to `national`; `-` in `international` means no separators |
| `reserved.tsv` | as `ranges.tsv`, without `format` | Curated from the regulators' pages: DE (Bundesnetzagentur), GB mobile (Ofcom), AU (02) 5550 (ACMA), US and Canada 555-0100 to 0199 under every area code where those numbers are valid. GB geographic, the rest of AU, FR, IE and NZ are unconfirmed and left out |

## Generation Model

A country (by population, stream `"country"`, among the countries with numbers of the type), a type (`"phone-type"`: mobile seven times in ten, or the only type the country has), a range (by weight, `"phone-range"`) and the remaining digits, each drawn from the masks (`"phone-digits"`). With `safe` and a reserved range for the type the range is drawn from `reserved.tsv`; without one, random digits are tried up to 64 times until no allocated range holds the number. A reserved number takes its format from the allocated range of the same length that shares its longest prefix, because reserved numbers need not be allocated (the GB range 07700 900xxx is not in libphonenumber's mobile pattern).

## Rebuilding the Data

`python scripts/prepare_phones.py` downloads libphonenumber `PhoneNumberMetadata.xml` (release pinned in the script, cached under `build/phones/`), reads `../country-generator/resources` for the tiers and populations, and writes `resources/lite` and `resources/full` and `tests/data/phone_regex.tsv` (with `--regex-out`). `python scripts/validate_phones.py` (needs `pip install phonenumbers` and a C++23 compiler) draws 100,000 numbers and reports the share `phonenumbers` calls valid.

## Known Limitations

- Mobile and fixed line only; toll-free, premium-rate, shared-cost, VoIP, pager and the other libphonenumber types are not generated.
- The ranges follow libphonenumber's patterns, which can differ from a carrier's actual allocations; a number is valid, not necessarily assigned.
- Reserved ranges are confirmed for DE, GB (mobile), AU ((02) 5550) and US/CA only. GB geographic, the rest of AU, FR, IE and NZ are unconfirmed and left out; elsewhere `safe` avoids the allocated ranges but cannot promise the number is unassigned.
- How common each length is in a plan with several lengths is a heuristic (the example number's length, then a quarter per digit away), not a measurement; libphonenumber lists the allowed lengths but not their frequencies.
- One national format per range; where a country writes numbers in several accepted ways only libphonenumber's is used.
- No area-code-to-place link: the number does not match the city of an address (planned for 1.1).
