#include <hammurabi/kv/command.h>
#include <hammurabi/kv/state_machine.h>
#include <hammurabi/overloaded.h>
#include <hammurabi/wire/codec.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace hammurabi::kv {

namespace {

void write(wire::writer& w, const get& op) {
    w.string(op.key);
}

void write(wire::writer& w, const put& op) {
    w.string(op.key);
    w.string(op.value);
}

void write(wire::writer& w, const remove& op) {
    w.string(op.key);
}

void write(wire::writer& w, const cas& op) {
    w.string(op.key);
    w.optional_string(op.expected);
    w.string(op.desired);
}

void write(wire::writer& /*w*/, const register_client& /*op*/) {
    // no fields: the type byte says it all
}

constexpr operation_type type_of(const get&) {
    return operation_type::get;
}

constexpr operation_type type_of(const put&) {
    return operation_type::put;
}

constexpr operation_type type_of(const remove&) {
    return operation_type::remove;
}

constexpr operation_type type_of(const cas&) {
    return operation_type::cas;
}

constexpr operation_type type_of(const register_client&) {
    return operation_type::register_client;
}

} // namespace

state_machine::state_machine(std::size_t max_sessions) : max_sessions_{max_sessions} {}

std::expected<reply, rejection> state_machine::apply(std::uint64_t index, const command& cmd) {
    if (std::holds_alternative<register_client>(cmd.op)) {
        if (cmd.client_id != 0) {
            return std::unexpected(rejection::malformed);
        }
        // The log index is the client id: every node agrees on it, and no two entries share it.
        [[maybe_unused]] const auto [_, inserted] = sessions_.try_emplace(index, session{.last_used = index});
        assert(inserted);
        evict_if_full();
        return registered{.client_id = index};
    }

    if (cmd.client_id == 0) {
        // Without a session only get may run: it changes nothing, so running it twice is harmless.
        if (const auto* op = std::get_if<get>(&cmd.op)) {
            return execute(*op);
        }
        return std::unexpected(rejection::malformed);
    }

    const auto itr = sessions_.find(cmd.client_id);
    if (itr == sessions_.end()) {
        return std::unexpected(rejection::session_expired); // never execute: it may have run before eviction
    }
    auto& s = itr->second;

    // sequence 0 is never a retry: right after registering, last_sequence is 0 and there is no result to repeat.
    if (cmd.sequence != 0 && cmd.sequence == s.last_sequence) {
        s.last_used = index; // a retry: the client is still alive
        return s.last_result;
    }
    if (cmd.sequence != s.last_sequence + 1) {
        return std::unexpected(rejection::malformed); // a gap or an old number: a client bug, not executed
    }

    s.last_result = std::visit(overloaded{
                                   [this](const auto& op) -> result { return execute(op); },
                                   [](const register_client&) -> result {
                                       throw std::logic_error("register_client is handled before the session lookup");
                                   },
                               },
                               cmd.op);
    s.last_sequence = cmd.sequence;
    s.last_used = index;
    return s.last_result;
}

result state_machine::read(const get& op) const {
    return execute(op);
}

void state_machine::evict_if_full() {
    if (sessions_.size() <= max_sessions_) {
        return;
    }
    // O(n), but only register_client grows the map, so only it gets here.
    const auto oldest =
        std::ranges::min_element(sessions_, {}, [](const auto& entry) { return entry.second.last_used; });
    sessions_.erase(oldest);
}

result state_machine::execute(const get& op) const {
    const auto itr = data_.find(op.key);
    if (itr == data_.end()) {
        return {.code = status::not_found, .value = std::nullopt};
    }
    return {.code = status::ok, .value = itr->second};
}

result state_machine::execute(const put& op) {
    data_.insert_or_assign(op.key, op.value);
    return {.code = status::ok, .value = std::nullopt};
}

result state_machine::execute(const remove& op) {
    const auto itr = data_.find(op.key);
    if (itr == data_.end()) {
        return {.code = status::not_found, .value = std::nullopt};
    }
    data_.erase(itr);
    return {.code = status::ok, .value = std::nullopt};
}

result state_machine::execute(const cas& op) {
    const auto itr = data_.find(op.key);
    std::optional<std::string> current;
    if (itr != data_.end()) {
        current = itr->second;
    }
    if (current != op.expected) {
        return {.code = status::cas_failed, .value = current};
    }
    data_.insert_or_assign(op.key, op.desired);
    return {.code = status::ok, .value = std::nullopt};
}

std::vector<std::byte> encode(const command& cmd) {
    wire::writer w;
    w.u64(cmd.client_id);
    w.u64(cmd.sequence);
    write(w, cmd.op);
    return std::move(w).take();
}

std::expected<command, wire::decode_error> decode_command(std::span<const std::byte> bytes) {
    wire::reader r{bytes};
    // Separate statements: the members must be read in this order.
    const auto client_id = r.u64();
    const auto sequence = r.u64();
    auto op = read_operation(r);
    const auto done = r.finish();
    if (!done) {
        return std::unexpected(done.error());
    }
    return command{.client_id = client_id, .sequence = sequence, .op = std::move(op)};
}

void write(wire::writer& w, const operation& op) {
    w.u8(std::to_underlying(std::visit([](const auto& m) { return type_of(m); }, op)));
    std::visit([&w](const auto& m) { write(w, m); }, op);
}

operation read_operation(wire::reader& r) {
    // No default: -Wswitch then points here when operation_type gets a new value.
    switch (static_cast<operation_type>(r.u8())) {
        case operation_type::get:
            return get{.key = r.string()};
        case operation_type::put:
            return put{.key = r.string(), .value = r.string()};
        case operation_type::remove:
            return remove{.key = r.string()};
        case operation_type::cas:
            return cas{.key = r.string(), .expected = r.optional_string(), .desired = r.string()};
        case operation_type::register_client:
            return register_client{};
    }
    r.fail(wire::decode_error::invalid_value);
    return get{}; // never used: finish() reports the error
}

} // namespace hammurabi::kv
