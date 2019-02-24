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

    bool get(const char *key, boost::any &value);

    void set(const char *key, const boost::any &value);

    bool exists(const char *key);

    void remove(const char *key);

    map m;
};

}

}

#endif //KIWI_HASHMAP_H
