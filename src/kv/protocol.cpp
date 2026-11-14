#include <hammurabi/kv/command.h>
#include <hammurabi/kv/protocol.h>
#include <hammurabi/kv/state_machine.h>
#include <hammurabi/wire/codec.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace hammurabi::kv {

namespace {

enum class response_type : std::uint8_t { result = 1, not_leader = 2, rejection = 3 };

void write(wire::writer& w, const result& r) {
    w.u8(std::to_underlying(response_type::result));
    w.u8(std::to_underlying(r.code));
    w.optional_string(r.value);
}

void write(wire::writer& w, const not_leader& r) {
    w.u8(std::to_underlying(response_type::not_leader));
    w.optional_string(r.leader_address);
}

void write(wire::writer& w, const rejection& r) {
    w.u8(std::to_underlying(response_type::rejection));
    w.u8(std::to_underlying(r));
}

status read_status(wire::reader& r) {
    const auto code = static_cast<status>(r.u8());
    switch (code) {
        case status::ok:
        case status::not_found:
        case status::cas_failed:
            return code;
    }
    r.fail(wire::decode_error::invalid_value);
    return status::ok; // never used: finish() reports the error
}

rejection read_rejection(wire::reader& r) {
    const auto rej = static_cast<rejection>(r.u8());
    switch (rej) {
        case rejection::malformed:
        case rejection::session_expired:
        case rejection::too_large:
            return rej;
    }
    r.fail(wire::decode_error::invalid_value);
    return rejection::too_large; // never used: finish() reports the error
}

std::unexpected<wire::decode_error> reject(wire::reader& r, wire::decode_error error) {
    r.fail(error);
    return std::unexpected(r.finish().error());
}

} // namespace

std::vector<std::byte> encode(const request& req) {
    wire::writer w;
    w.u8(client_wire_version);
    write(w, req.op);
    return std::move(w).take();
}

std::vector<std::byte> encode(const response& resp) {
    wire::writer w;
    w.u8(client_wire_version);
    std::visit([&w](const auto& r) { write(w, r); }, resp);
    return std::move(w).take();
}

std::expected<request, wire::decode_error> decode_request(std::span<const std::byte> bytes) {
    wire::reader r{bytes};
    const auto version = r.u8();
    if (version != client_wire_version) {
        return reject(r, wire::decode_error::invalid_value);
    }
    auto op = read_operation(r);
    const auto done = r.finish();
    if (!done) {
        return std::unexpected(done.error());
    }
    return request{std::move(op)};
}

std::expected<response, wire::decode_error> decode_response(std::span<const std::byte> bytes) {
    wire::reader r{bytes};
    const auto version = r.u8();
    if (version != client_wire_version) {
        return reject(r, wire::decode_error::invalid_value);
    }
    const auto type = static_cast<response_type>(r.u8());
    response resp{};
    switch (type) {
        case response_type::result: // a braced initializer is evaluated left to right
            resp = result{.code = read_status(r), .value = r.optional_string()};
            break;
        case response_type::not_leader:
            resp = not_leader{.leader_address = r.optional_string()};
            break;
        case response_type::rejection:
            resp = read_rejection(r);
            break;
        default:
            return reject(r, wire::decode_error::invalid_value);
    }
    const auto done = r.finish();
    if (!done) {
        return std::unexpected(done.error());
    }
    return resp;
}

} // namespace hammurabi::kv
