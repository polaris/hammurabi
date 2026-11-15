#ifndef HAMMURABI_TESTS_KV_STRING_MAKERS_H
#define HAMMURABI_TESTS_KV_STRING_MAKERS_H

#include <catch2/catch_tostring.hpp>
#include <hammurabi/kv/command.h>
#include <hammurabi/kv/format.h>
#include <hammurabi/kv/protocol.h>

#include <expected>
#include <format>
#include <string>

// Lets Catch2 print KV operations, commands, requests, replies and responses in failed assertions instead of "{?}",
// using the std::formatters from kv/format.h.
//
// Every test file that puts one of these types into an assertion must include this header, for the same reason
// as raft_string_makers.h: otherwise the file instantiates Catch's default StringMaker for the same type (an ODR
// violation), and the output silently falls back to "{?}". Hence "IWYU pragma: keep" at the include sites.
namespace hammurabi::kv::testing {

template<typename T>
struct formatter_string_maker {
    static std::string convert(const T& value) { return std::format("{}", value); }
};

} // namespace hammurabi::kv::testing

template<>
struct Catch::StringMaker<hammurabi::kv::get> : hammurabi::kv::testing::formatter_string_maker<hammurabi::kv::get> {};

template<>
struct Catch::StringMaker<hammurabi::kv::put> : hammurabi::kv::testing::formatter_string_maker<hammurabi::kv::put> {};

template<>
struct Catch::StringMaker<hammurabi::kv::remove>
: hammurabi::kv::testing::formatter_string_maker<hammurabi::kv::remove> {};

template<>
struct Catch::StringMaker<hammurabi::kv::cas> : hammurabi::kv::testing::formatter_string_maker<hammurabi::kv::cas> {};

template<>
struct Catch::StringMaker<hammurabi::kv::register_client>
: hammurabi::kv::testing::formatter_string_maker<hammurabi::kv::register_client> {};

template<>
struct Catch::StringMaker<hammurabi::kv::operation>
: hammurabi::kv::testing::formatter_string_maker<hammurabi::kv::operation> {};

template<>
struct Catch::StringMaker<hammurabi::kv::request>
: hammurabi::kv::testing::formatter_string_maker<hammurabi::kv::request> {};

template<>
struct Catch::StringMaker<hammurabi::kv::command>
: hammurabi::kv::testing::formatter_string_maker<hammurabi::kv::command> {};

template<>
struct Catch::StringMaker<hammurabi::kv::status>
: hammurabi::kv::testing::formatter_string_maker<hammurabi::kv::status> {};

template<>
struct Catch::StringMaker<hammurabi::kv::result>
: hammurabi::kv::testing::formatter_string_maker<hammurabi::kv::result> {};

template<>
struct Catch::StringMaker<hammurabi::kv::registered>
: hammurabi::kv::testing::formatter_string_maker<hammurabi::kv::registered> {};

template<>
struct Catch::StringMaker<hammurabi::kv::reply> : hammurabi::kv::testing::formatter_string_maker<hammurabi::kv::reply> {
};

template<>
struct Catch::StringMaker<hammurabi::kv::not_leader>
: hammurabi::kv::testing::formatter_string_maker<hammurabi::kv::not_leader> {};

template<>
struct Catch::StringMaker<hammurabi::kv::rejection>
: hammurabi::kv::testing::formatter_string_maker<hammurabi::kv::rejection> {};

template<>
struct Catch::StringMaker<hammurabi::kv::response>
: hammurabi::kv::testing::formatter_string_maker<hammurabi::kv::response> {};

// What state_machine::apply returns.
template<>
struct Catch::StringMaker<std::expected<hammurabi::kv::reply, hammurabi::kv::rejection>> {
    static std::string convert(const std::expected<hammurabi::kv::reply, hammurabi::kv::rejection>& value) {
        return value ? std::format("{}", *value) : std::format("{}", value.error());
    }
};

#endif // HAMMURABI_TESTS_KV_STRING_MAKERS_H
