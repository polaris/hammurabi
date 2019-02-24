#include "hashmap.h"

namespace hammurabi {

namespace detail {

using map = std::unordered_map<std::string, boost::any>;

bool hashmap::get(const char *key, boost::any &value) {
    auto itr = m.find(key);
    if (itr != m.end()) {
        value = itr->second;
        return true;
    }
    return false;
}

void hashmap::set(const char *key, const boost::any &value) {
    m[key] = value;
}

bool hashmap::exists(const char *key) {
    return m.find(key) != m.end();
}

void hashmap::remove(const char *key) {
    auto itr = m.find(key);
    if (itr != m.end()) {
        m.erase(itr);
    }
}

}

}
