#ifndef HAMMURABI_TESTS_RAFT_STRING_MAKERS_H
#define HAMMURABI_TESTS_RAFT_STRING_MAKERS_H

#include <catch2/catch_tostring.hpp>
#include <hammurabi/raft/format.h>
#include <hammurabi/raft/messages.h>

#include <format>
#include <string>

// Lets Catch2 print Raft messages in failed assertions instead of "{?}", using the std::formatters from format.h.
//
// Every test file that puts a Raft message into an assertion must include this header. Otherwise that file
// instantiates Catch's default StringMaker for the same type, the linker keeps only one of the two definitions
// (an ODR violation), and the output silently falls back to "{?}". Tools that don't see the specialization being
// used would flag the include as unused, hence "IWYU pragma: keep" at the include sites.
namespace hammurabi::raft::testing {

template<typename T>
struct formatter_string_maker {
    static std::string convert(const T& value) { return std::format("{}", value); }
};

} // namespace hammurabi::raft::testing

template<>
struct Catch::StringMaker<hammurabi::raft::request_vote>
: hammurabi::raft::testing::formatter_string_maker<hammurabi::raft::request_vote> {};

template<>
struct Catch::StringMaker<hammurabi::raft::request_vote_reply>
: hammurabi::raft::testing::formatter_string_maker<hammurabi::raft::request_vote_reply> {};

template<>
struct Catch::StringMaker<hammurabi::raft::append_entries>
: hammurabi::raft::testing::formatter_string_maker<hammurabi::raft::append_entries> {};

template<>
struct Catch::StringMaker<hammurabi::raft::append_entries_reply>
: hammurabi::raft::testing::formatter_string_maker<hammurabi::raft::append_entries_reply> {};

template<>
struct Catch::StringMaker<hammurabi::raft::message>
: hammurabi::raft::testing::formatter_string_maker<hammurabi::raft::message> {};

template<>
struct Catch::StringMaker<hammurabi::raft::envelope>
: hammurabi::raft::testing::formatter_string_maker<hammurabi::raft::envelope> {};

#endif // HAMMURABI_TESTS_RAFT_STRING_MAKERS_H
