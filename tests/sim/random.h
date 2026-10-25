#ifndef HAMMURABI_SIM_RANDOM_H
#define HAMMURABI_SIM_RANDOM_H

#include <cstdint>
#include <random>

namespace hammurabi::sim {

inline constexpr int min_per_mille = 0;
inline constexpr int max_per_mille = 1000;

[[nodiscard]] inline bool chance(std::mt19937_64& rng, int per_mille) {
    return rng() % max_per_mille < static_cast<std::uint64_t>(per_mille);
}

[[nodiscard]] inline std::uint64_t random_between(std::mt19937_64& rng, std::uint64_t lo, std::uint64_t hi) {
    return lo + rng() % (hi - lo + 1);
}

} // namespace hammurabi::sim

#endif // HAMMURABI_SIM_RANDOM_H
