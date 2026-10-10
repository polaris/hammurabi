#ifndef HAMMURABI_WIRE_CODEC_H
#define HAMMURABI_WIRE_CODEC_H

#include <hammurabi/attributes.h>

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hammurabi::wire {

enum class decode_error : std::uint8_t {
    truncated,
    trailing_bytes,
    invalid_value,
    too_large // a frame header announces more than the limit; produced by step 7.3
};

/// the u32 length prefix for a field of size bytes
/// throws std::length_error if it doesn't fit: the caller violated the wire format
[[nodiscard]] std::uint32_t wire_length(std::size_t size);

class writer {
public:
    void u8(std::uint8_t value) { put(value); }
    void u32(std::uint32_t value) { put(value); }
    void u64(std::uint64_t value) { put(value); }
    void boolean(bool value) { u8(value ? 1 : 0); }
    void bytes(std::span<const std::byte> data);
    void string(std::string_view text);
    [[nodiscard]] std::size_t size() const { return buffer_.size(); }
    [[nodiscard]] std::vector<std::byte> take() && { return std::move(buffer_); }

private:
    template<std::unsigned_integral T>
    void put(T value) {
        if constexpr (std::endian::native == std::endian::little) {
            value = std::byteswap(value); // the wire is big-endian
        }
        const auto raw = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
        buffer_.insert(buffer_.end(), raw.begin(), raw.end());
    }

    std::vector<std::byte> buffer_;
};

class reader {
public:
    /// input must outlive the reader
    explicit reader(std::span<const std::byte> input HAMMURABI_LIFETIMEBOUND) : input_{input} {}
    /// a temporary container would die before the reader is used
    template<std::ranges::range R>
        requires(std::convertible_to<R, std::span<const std::byte>> && !std::ranges::borrowed_range<R>)
    explicit reader(R&&) = delete;

    std::uint8_t u8() { return get<std::uint8_t>(); }
    std::uint32_t u32() { return get<std::uint32_t>(); }
    std::uint64_t u64() { return get<std::uint64_t>(); }
    bool boolean();
    std::vector<std::byte> bytes();
    std::string string();
    [[nodiscard]] std::size_t remaining() const { return input_.size(); }
    void fail(decode_error error);
    [[nodiscard]] std::expected<void, decode_error> finish() const;

private:
    template<std::unsigned_integral T>
    T get() {
        const auto input = take_bytes(sizeof(T));
        if (input.size() != sizeof(T)) {
            return T{};
        }
        T value;
        std::memcpy(&value, input.data(), sizeof(T));
        if constexpr (std::endian::native == std::endian::little) {
            value = std::byteswap(value);
        }
        return value;
    }

    std::span<const std::byte> take_bytes(std::size_t n);

    std::span<const std::byte> input_;
    std::optional<decode_error> error_;
};

} // namespace hammurabi::wire

#endif // HAMMURABI_WIRE_CODEC_H
