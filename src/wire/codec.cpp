#include "hammurabi/wire/codec.h"

#include <format>
#include <limits>
#include <ranges>
#include <stdexcept>

namespace hammurabi::wire {

std::uint32_t wire_length(std::size_t size) {
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error(std::format("wire field of {} bytes exceeds the u32 length prefix", size));
    }
    return static_cast<std::uint32_t>(size);
}

void writer::bytes(std::span<const std::byte> data) {
    u32(wire_length(data.size()));
    buffer_.insert(buffer_.end(), data.begin(), data.end());
}

void writer::string(std::string_view text) {
    bytes(std::as_bytes(std::span{text}));
}

bool reader::boolean() {
    const auto value = u8();
    if (value == 0) {
        return false;
    }
    if (value == 1) {
        return true;
    }
    fail(decode_error::invalid_value);
    return false;
}

std::vector<std::byte> reader::bytes() {
    const auto size = u32();
    const auto s = take_bytes(size);
    return {s.begin(), s.end()};
}

std::string reader::string() {
    const auto size = u32();
    const auto s = take_bytes(size);
    return s | std::views::transform([](std::byte b) { return static_cast<char>(b); }) | std::ranges::to<std::string>();
}

void reader::fail(decode_error error) {
    if (!error_) {
        error_ = error;
    }
}

[[nodiscard]] std::expected<void, decode_error> reader::finish() const {
    if (error_) {
        return std::unexpected(*error_);
    }
    if (remaining() > 0) {
        return std::unexpected(decode_error::trailing_bytes);
    }
    return {};
}

std::span<const std::byte> reader::take_bytes(std::size_t n) {
    if (error_) {
        return {}; // nothing after the first error is trusted, so don't consume more input
    }
    if (n > remaining()) {
        fail(decode_error::truncated);
        return {};
    }
    const auto result = input_.subspan(0, n);
    input_ = input_.subspan(n);
    return result;
}

} // namespace hammurabi::wire
