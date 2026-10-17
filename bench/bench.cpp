// Benchmarks for phone-generator; see bench.hpp for the options.

#include "../dasmig/phonegen.hpp"
#include "bench.hpp"

int main(int argc, char** argv)
{
    dasmig::bench::runner run{"phone-generator", argc, argv};

    run.load("load", [&] {
        dasmig::phg gen;
        return gen.load(run.tier());
    });

    dasmig::phg gen;
    if (!gen.load(run.tier())) return run.missing_tier();

    run.calls("get_phone", [&](std::uint64_t seed) { return gen.get_phone({}, seed); });
    run.calls("get_phone to_json", [&](std::uint64_t seed) {
        return gen.get_phone({}, seed).to_json();
    });

    run.batches("get_phone_batch", [&](std::size_t count, std::size_t threads, std::uint64_t seed) {
        return gen.get_phone_batch({}, {.count = count, .threads = threads}, seed);
    });

    return run.finish();
}
