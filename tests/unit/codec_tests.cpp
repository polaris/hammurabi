#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <hammurabi/wire/codec.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

std::vector<std::byte> to_bytes(std::initializer_list<std::uint8_t> values) {
    return values | std::views::transform([](std::uint8_t v) { return std::byte{v}; }) | std::ranges::to<std::vector>();
}

} // namespace

TEST_CASE("reader refuses to view a temporary container", "[codec][reader]") {
    using hammurabi::wire::reader;
    STATIC_REQUIRE_FALSE(std::is_constructible_v<reader, std::vector<std::byte>>);
    STATIC_REQUIRE_FALSE(std::is_constructible_v<reader, std::array<std::byte, 4>>);
    STATIC_REQUIRE(std::is_constructible_v<reader, std::vector<std::byte>&>);
    STATIC_REQUIRE(std::is_constructible_v<reader, const std::vector<std::byte>&>);
    STATIC_REQUIRE(std::is_constructible_v<reader, std::span<const std::byte>>);
}

TEST_CASE("round trip for u8", "[codec][writer][reader]") {
    const auto value = GENERATE(as<std::uint8_t>{}, 0, 1, 0x7F, 0x80, 0xFF);
    CAPTURE(static_cast<int>(value));

    hammurabi::wire::writer w;
    w.u8(value);
    const auto buffer = std::move(w).take();

    hammurabi::wire::reader r(buffer);
    REQUIRE(r.u8() == value);
    REQUIRE(r.finish());
}

TEST_CASE("round trip for u32", "[codec][writer][reader]") {
    const auto value = GENERATE(as<std::uint32_t>{}, 0, 1, 0x01020304, 0xFFFFFFFF);
    CAPTURE(value);

    hammurabi::wire::writer w;
    w.u32(value);
    const auto buffer = std::move(w).take();

    hammurabi::wire::reader r(buffer);
    REQUIRE(r.u32() == value);
    REQUIRE(r.finish());
}

TEST_CASE("round trip for u64", "[codec][writer][reader]") {
    const auto value = GENERATE(as<std::uint64_t>{}, 0, 1, 0x0102030405060708, 0xFFFFFFFFFFFFFFFF);
    CAPTURE(value);

    hammurabi::wire::writer w;
    w.u64(value);
    const auto buffer = std::move(w).take();

    hammurabi::wire::reader r(buffer);
    REQUIRE(r.u64() == value);
    REQUIRE(r.finish());
}

TEST_CASE("reader decodes u32 as big-endian", "[codec][reader]") {
    const auto input = to_bytes({0x01, 0x02, 0x03, 0x04});
    hammurabi::wire::reader r(input);
    REQUIRE(r.u32() == 0x01020304);
    REQUIRE(r.finish());
}

TEST_CASE("reader decodes u64 as big-endian", "[codec][reader]") {
    const auto input = to_bytes({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08});
    hammurabi::wire::reader r(input);
    REQUIRE(r.u64() == 0x0102030405060708);
    REQUIRE(r.finish());
}

TEST_CASE("reading u8 from empty input reports truncated", "[codec][reader]") {
    std::vector<std::byte> input = {};
    hammurabi::wire::reader r(input);
    r.u8();

    const auto result = r.finish();
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == hammurabi::wire::decode_error::truncated);
}

// The unread bytes would also be trailing_bytes; the read error must win.
TEST_CASE("reading u32 from three bytes reports truncated, not trailing_bytes", "[codec][reader]") {
    const std::vector<std::byte> input(3);
    hammurabi::wire::reader r(input);
    r.u32();

    const auto result = r.finish();
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == hammurabi::wire::decode_error::truncated);
}

TEST_CASE("reading u64 from seven bytes reports truncated, not trailing_bytes", "[codec][reader]") {
    const std::vector<std::byte> input(7);
    hammurabi::wire::reader r(input);
    r.u64();

    const auto result = r.finish();
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == hammurabi::wire::decode_error::truncated);
}

TEST_CASE("finish reports trailing_bytes when input is left unread", "[codec][reader]") {
    const std::vector<std::byte> input(2);
    hammurabi::wire::reader r(input);
    r.u8();

    const auto result = r.finish();
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == hammurabi::wire::decode_error::trailing_bytes);
}

TEST_CASE("writer appends one byte per u8, in order", "[codec][writer]") {
    hammurabi::wire::writer w;
    w.u8(67);
    w.u8(0);
    w.u8(42);
    const auto buffer = std::move(w).take();
    REQUIRE(buffer == to_bytes({67, 0, 42}));
}

TEST_CASE("writer encodes u32 as big-endian", "[codec][writer]") {
    hammurabi::wire::writer w;
    w.u32(0x01020304);
    const auto buffer = std::move(w).take();
    REQUIRE(buffer == to_bytes({0x01, 0x02, 0x03, 0x04}));
}

TEST_CASE("writer encodes u64 as big-endian", "[codec][writer]") {
    hammurabi::wire::writer w;
    w.u64(0x0102030405060708);
    const auto buffer = std::move(w).take();
    REQUIRE(buffer == to_bytes({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}));
}

TEST_CASE("round trip for boolean", "[codec][writer][reader]") {
    const auto value = GENERATE(false, true);
    CAPTURE(value);

    hammurabi::wire::writer w;
    w.boolean(value);
    const auto buffer = std::move(w).take();

    hammurabi::wire::reader r(buffer);
    REQUIRE(r.boolean() == value);
    REQUIRE(r.finish());
}

TEST_CASE("writer encodes boolean as one byte, 0 or 1", "[codec][writer]") {
    hammurabi::wire::writer w;
    w.boolean(false);
    w.boolean(true);
    const auto buffer = std::move(w).take();
    REQUIRE(buffer == to_bytes({0x00, 0x01}));
}

TEST_CASE("reading boolean from a byte other than 0 or 1 reports invalid_value", "[codec][reader]") {
    const auto byte = GENERATE(as<std::uint8_t>{}, 0x02, 0x80, 0xFF);
    CAPTURE(static_cast<int>(byte));

    const auto input = to_bytes({byte});
    hammurabi::wire::reader r(input);
    r.boolean();

    const auto result = r.finish();
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == hammurabi::wire::decode_error::invalid_value);
}

TEST_CASE("round trip for bytes", "[codec][writer][reader]") {
    const auto value = GENERATE(to_bytes({}), to_bytes({0x00}), to_bytes({0x01, 0x02, 0x03}));
    CAPTURE(value);

    hammurabi::wire::writer w;
    w.bytes(value);
    const auto buffer = std::move(w).take();

    hammurabi::wire::reader r(buffer);
    REQUIRE(r.bytes() == value);
    REQUIRE(r.finish());
}

TEST_CASE("writer encodes bytes as a big-endian u32 length, then the data", "[codec][writer]") {
    hammurabi::wire::writer w;
    w.bytes(to_bytes({0xAA, 0xBB}));
    const auto buffer = std::move(w).take();
    REQUIRE(buffer == to_bytes({0x00, 0x00, 0x00, 0x02, 0xAA, 0xBB}));
}

TEST_CASE("wire_length accepts every size the u32 prefix can hold", "[codec][writer]") {
    REQUIRE(hammurabi::wire::wire_length(0) == 0);
    REQUIRE(hammurabi::wire::wire_length(0xFFFFFFFF) == 0xFFFFFFFF);
}

TEST_CASE("wire_length rejects sizes beyond the u32 prefix", "[codec][writer]") {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint32_t)) {
        constexpr std::size_t too_long = std::size_t{0xFFFFFFFF} + 1;
        REQUIRE_THROWS_MATCHES(
            hammurabi::wire::wire_length(too_long), std::length_error,
            Catch::Matchers::Message("wire field of 4294967296 bytes exceeds the u32 length prefix"));
    } else {
        SKIP("size_t cannot exceed the u32 prefix on this platform");
    }
}

TEST_CASE("reading bytes with a truncated length prefix reports truncated", "[codec][reader]") {
    const auto input = to_bytes({0x00, 0x00});
    hammurabi::wire::reader r(input);
    r.bytes();

    const auto result = r.finish();
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == hammurabi::wire::decode_error::truncated);
}

TEST_CASE("reading bytes whose length exceeds the input reports truncated", "[codec][reader]") {
    const auto input = to_bytes({0x00, 0x00, 0x00, 0x05, 0x01, 0x02});
    hammurabi::wire::reader r(input);
    REQUIRE(r.bytes().empty());

    const auto result = r.finish();
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == hammurabi::wire::decode_error::truncated);
}

// A hostile length must be rejected before anything is allocated for it.
TEST_CASE("reading bytes with a maximal length and no data reports truncated", "[codec][reader]") {
    const auto input = to_bytes({0xFF, 0xFF, 0xFF, 0xFF});
    hammurabi::wire::reader r(input);
    REQUIRE(r.bytes().empty());

    const auto result = r.finish();
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == hammurabi::wire::decode_error::truncated);
}

TEST_CASE("round trip for string", "[codec][writer][reader]") {
    const auto value = GENERATE(as<std::string>{}, "", "hammurabi", std::string("a\0b", 3), "\xC3\xA4\xFF");
    CAPTURE(value);

    hammurabi::wire::writer w;
    w.string(value);
    const auto buffer = std::move(w).take();

    hammurabi::wire::reader r(buffer);
    REQUIRE(r.string() == value);
    REQUIRE(r.finish());
}

TEST_CASE("writer encodes string as a big-endian u32 length, then the raw bytes", "[codec][writer]") {
    hammurabi::wire::writer w;
    w.string("hi");
    const auto buffer = std::move(w).take();
    REQUIRE(buffer == to_bytes({0x00, 0x00, 0x00, 0x02, 'h', 'i'}));
}

TEST_CASE("reading string whose length exceeds the input reports truncated", "[codec][reader]") {
    const auto input = to_bytes({0x00, 0x00, 0x00, 0x03, 'h', 'i'});
    hammurabi::wire::reader r(input);
    REQUIRE(r.string().empty());

    const auto result = r.finish();
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == hammurabi::wire::decode_error::truncated);
}

TEST_CASE("the first decode error sticks", "[codec][reader]") {
    const auto input = to_bytes({0x02});
    hammurabi::wire::reader r(input);
    r.boolean(); // invalid_value
    r.u32();     // truncated, but must not replace the first error

    const auto result = r.finish();
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == hammurabi::wire::decode_error::invalid_value);
}

TEST_CASE("reads after an error return zero values and consume nothing", "[codec][reader]") {
    const auto input = to_bytes({0x02, 0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x01, 0xAA});
    hammurabi::wire::reader r(input);
    r.boolean(); // invalid_value

    REQUIRE(r.u32() == 0);
    REQUIRE(r.bytes().empty());
    REQUIRE(r.remaining() == 9);
}
