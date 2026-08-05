#pragma once

#include <bit>
#include <cstdint>

namespace lang::detail {

inline std::int64_t wrapping_add_i64(std::int64_t left,
                                     std::int64_t right) {
    const auto bits =
        std::bit_cast<std::uint64_t>(left) +
        std::bit_cast<std::uint64_t>(right);
    return std::bit_cast<std::int64_t>(bits);
}

} // namespace lang::detail
