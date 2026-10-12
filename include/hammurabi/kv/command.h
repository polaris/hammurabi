#ifndef HAMMURABI_KV_COMMAND_H
#define HAMMURABI_KV_COMMAND_H

#include <optional>
#include <string>
#include <variant>

namespace hammurabi::kv {

struct get {
    std::string key;
    bool operator==(const get&) const = default;
};

struct put {
    std::string key;
    std::string value;
    bool operator==(const put&) const = default;
};

struct remove {
    std::string key;
    bool operator==(const remove&) const = default;
};

struct cas {
    std::string key;
    std::optional<std::string> expected;
    std::string desired;
    bool operator==(const cas&) const = default;
};

using operation = std::variant<get, put, remove, cas>;

} // namespace hammurabi::kv

#endif // HAMMURABI_KV_COMMAND_H
