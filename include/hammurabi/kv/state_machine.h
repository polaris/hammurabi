#ifndef HAMMURABI_KV_STATE_MACHINE_H
#define HAMMURABI_KV_STATE_MACHINE_H

#include <hammurabi/kv/command.h>
#include <hammurabi/wire/codec.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hammurabi::kv {

enum class operation_type : std::uint8_t { get = 1, put = 2, remove = 3, cas = 4 };

enum class status : std::uint8_t { ok = 1, not_found = 2, cas_failed = 3 };

struct result {
    status code = status::ok;
    std::optional<std::string> value;
};

class state_machine {
public:
    result apply(const operation& op);              // phase 5.2 changes this to take a command
    [[nodiscard]] result read(const get& op) const; // used by ReadIndex in phase 8

private:
    result execute(const get& op) const;
    result execute(const put& op);
    result execute(const remove& op);
    result execute(const cas& op);

    std::map<std::string, std::string> data_;
};

[[nodiscard]] std::vector<std::byte> encode(const operation& op);
[[nodiscard]] std::expected<operation, wire::decode_error> decode_operation(std::span<const std::byte> bytes);

} // namespace hammurabi::kv

#endif // HAMMURABI_KV_STATE_MACHINE_H
