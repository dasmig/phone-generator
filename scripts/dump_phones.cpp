// Prints "region<TAB>type<TAB>e164" for generated numbers, for scripts/validate_phones.py.
//   dump_phones [count] [seed]
#include "../dasmig/phonegen.hpp"
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv)
{
    const std::uint64_t count = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 100000;
    const std::uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;
    dasmig::phg gen;
    if (!gen.load(dasmig::dataset::lite)) return 1;
    gen.for_each_phone({}, {.count = count, .threads = 0}, seed, [](std::uint64_t, const dasmig::phone& p) {
        std::cout << p.country_code << '\t' << (p.type == dasmig::phone_type::mobile ? "mobile" : "fixed") << '\t'
                  << p.e164() << '\n';
    });
    return 0;
}
