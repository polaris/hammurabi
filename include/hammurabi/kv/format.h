#ifndef HAMMURABI_KV_FORMAT_H
#define HAMMURABI_KV_FORMAT_H

#include <hammurabi/kv/command.h>
#include <hammurabi/kv/protocol.h>

#include <format>
#include <optional>
#include <string>
#include <utility>
#include <variant>

// std::formatters for operations, requests and responses: readable test failures now, CLI output later.
// Strings are printed in quotes, so an empty value is visible. ("{:?}" would escape them, but GCC 14 lacks it.)
namespace hammurabi::kv::detail {

struct no_spec_formatter {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
};

inline std::string quoted(const std::optional<std::string>& s) {
    return s ? std::format("\"{}\"", *s) : std::string{"absent"};
}

} // namespace hammurabi::kv::detail

template<>
struct std::formatter<hammurabi::kv::get> : hammurabi::kv::detail::no_spec_formatter {
    auto format(const hammurabi::kv::get& op, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "Get{{\"{}\"}}", op.key);
    }
};

template<>
struct std::formatter<hammurabi::kv::put> : hammurabi::kv::detail::no_spec_formatter {
    auto format(const hammurabi::kv::put& op, std::format_context& ctx) const {
        return std::format_to(ctx.out(), R"(Put{{"{}"="{}"}})", op.key, op.value);
    }
};

template<>
struct std::formatter<hammurabi::kv::remove> : hammurabi::kv::detail::no_spec_formatter {
    auto format(const hammurabi::kv::remove& op, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "Remove{{\"{}\"}}", op.key);
    }
};

template<>
struct std::formatter<hammurabi::kv::cas> : hammurabi::kv::detail::no_spec_formatter {
    auto format(const hammurabi::kv::cas& op, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "Cas{{\"{}\": {}→\"{}\"}}", op.key, hammurabi::kv::detail::quoted(op.expected),
                              op.desired);
    }
};

template<>
struct std::formatter<hammurabi::kv::operation> : hammurabi::kv::detail::no_spec_formatter {
    auto format(const hammurabi::kv::operation& op, std::format_context& ctx) const {
        return std::visit([&ctx](const auto& o) { return std::format_to(ctx.out(), "{}", o); }, op);
    }
};

template<>
struct std::formatter<hammurabi::kv::request> : hammurabi::kv::detail::no_spec_formatter {
    auto format(const hammurabi::kv::request& req, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "Request{{{}}}", req.op);
    }
};

template<>
struct std::formatter<hammurabi::kv::status> : hammurabi::kv::detail::no_spec_formatter {
    auto format(hammurabi::kv::status s, std::format_context& ctx) const {
        using enum hammurabi::kv::status;
        switch (s) {
            case ok:
                return std::format_to(ctx.out(), "ok");
            case not_found:
                return std::format_to(ctx.out(), "not_found");
            case cas_failed:
                return std::format_to(ctx.out(), "cas_failed");
        }
        return std::format_to(ctx.out(), "status({})", std::to_underlying(s)); // a value outside the enum
    }
};

template<>
struct std::formatter<hammurabi::kv::result> : hammurabi::kv::detail::no_spec_formatter {
    auto format(const hammurabi::kv::result& r, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "Result{{{}, {}}}", r.code, hammurabi::kv::detail::quoted(r.value));
    }
};

template<>
struct std::formatter<hammurabi::kv::not_leader> : hammurabi::kv::detail::no_spec_formatter {
    auto format(const hammurabi::kv::not_leader& n, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "NotLeader{{{}}}", hammurabi::kv::detail::quoted(n.leader_address));
    }
};

template<>
struct std::formatter<hammurabi::kv::rejection> : hammurabi::kv::detail::no_spec_formatter {
    auto format(hammurabi::kv::rejection r, std::format_context& ctx) const {
        using enum hammurabi::kv::rejection;
        switch (r) {
            case too_large:
                return std::format_to(ctx.out(), "Rejected{{too_large}}");
            case malformed:
                return std::format_to(ctx.out(), "Rejected{{malformed}}");
            case session_expired:
                return std::format_to(ctx.out(), "Rejected{{session_expired}}");
        }
        return std::format_to(ctx.out(), "Rejected{{{}}}", std::to_underlying(r)); // a value outside the enum
    }
};

template<>
struct std::formatter<hammurabi::kv::response> : hammurabi::kv::detail::no_spec_formatter {
    auto format(const hammurabi::kv::response& resp, std::format_context& ctx) const {
        return std::visit([&ctx](const auto& r) { return std::format_to(ctx.out(), "{}", r); }, resp);
    }
};

#endif // HAMMURABI_KV_FORMAT_H
