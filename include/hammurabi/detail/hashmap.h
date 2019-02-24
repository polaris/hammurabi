#ifndef KIWI_HASHMAP_H
#define KIWI_HASHMAP_H

#include <string>
#include <unordered_map>

#include <boost/any.hpp>

namespace hammurabi {

namespace detail {

using map = std::unordered_map<std::string, boost::any>;

struct hashmap {
    explicit hashmap(std::size_t bucket_count)
            : m{bucket_count} {}

    bool get(const char *key, boost::any &value) {
        auto itr = m.find(key);
        if (itr != m.end()) {
            value = itr->second;
            return true;
        }
        return false;
    }

    void set(const char *key, const boost::any &value) {
        m[key] = value;
    }

    bool exists(const char *key) {
        return m.find(key) != m.end();
    }

    void remove(const char *key) {
        auto itr = m.find(key);
        if (itr != m.end()) {
            m.erase(itr);
        }
    }

    map m;
};

}

}

#endif //KIWI_HASHMAP_H
