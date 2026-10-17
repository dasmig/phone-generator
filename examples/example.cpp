#include "../dasmig/phonegen.hpp"
#include <cstdint>
#include <iostream>

int main()
{
    // The singleton loads the lite tier (compiled in, or from resources/).
    auto& gen = dasmig::phg::instance();
    if (!gen.has_data())
    {
        std::cerr << "Could not load the phone data.\n";
        return 1;
    }
    std::cout << "Loaded " << gen.country_count() << " countries.\n\n";

    std::cout << "Random numbers:\n";
    for (int i = 0; i < 5; ++i)
    {
        const auto p = gen.get_phone(dasmig::phone_request{});
        std::cout << "  " << p.country << "  " << p.international() << "  (" << p.national() << ")\n";
    }

    std::cout << "\nBrazilian mobiles:\n";
    for (int i = 0; i < 3; ++i)
    {
        std::cout << "  " << gen.get_phone({.country = "BR", .type = dasmig::phone_type::mobile}) << '\n';
    }

    std::cout << "\nSafe numbers for tests (reserved for fiction where the regulator has a range):\n";
    for (const char* c : {"US", "GB", "DE", "AU", "BR"})
    {
        const auto p = gen.get_phone({.country = c, .safe = true});
        std::cout << "  " << c << "  " << p.international() << (p.reserved ? "  reserved" : "  not reserved") << '\n';
    }

    std::cout << "\nSeed 42, twice:\n";
    const auto a = gen.get_phone(dasmig::phone_request{}, std::uint64_t{42});
    const auto b = gen.get_phone(dasmig::phone_request{}, std::uint64_t{42});
    std::cout << "  " << a.to_json() << '\n'
              << "  " << b.to_json() << '\n';
    return 0;
}
