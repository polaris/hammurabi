#include <hammurabi/kv/command.h>
#include <hammurabi/kv/state_machine.h>
#include <hammurabi/wire/codec.h>

#include <optional>
#include <string>
#include <utility>
#include <variant>

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

} // namespace

result state_machine::apply(const operation& op) {
    return std::visit([this](const auto& o) { return execute(o); }, op);
}

result state_machine::read(const get& op) const {
    return execute(op);
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

std::vector<std::byte> encode(const operation& op) {
    wire::writer w;
    write(w, op);
    return std::move(w).take();
}

std::expected<operation, wire::decode_error> decode_operation(std::span<const std::byte> bytes) {
    wire::reader r{bytes};
    auto op = read_operation(r);
    const auto done = r.finish();
    if (!done) {
        return std::unexpected(done.error());
    }
    return op;
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
    }
    r.fail(wire::decode_error::invalid_value);
    return get{}; // never used: finish() reports the error
}

} // namespace hammurabi::kv
