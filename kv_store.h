#ifndef HAMMURABI_KV_STORE_H
#define HAMMURABI_KV_STORE_H

#include "detail/hashmap.h"

#include <mutex>

#include <boost/any.hpp>

namespace hammurabi {

class kv_store {
public:
    explicit kv_store(std::size_t bucket_count);
    ~kv_store() = default;

    template <typename T>
    bool get(const char* key, T& value);

    template <typename T>
    void set(const char* key, const T& value);

    bool exists(const char* key);

    void remove(const char* key);

private:
    std::mutex m;
    detail::hashmap hm;
};

template <typename T>
bool kv_store::get(const char *key, T& value) {
    std::lock_guard<std::mutex> lock{m};

    boost::any v;
    if (hm.get(key, v)) {
        try {
            value = boost::any_cast<T>(v);
            return true;
        } catch (const boost::bad_any_cast&) {
            return false;
        }
    }
    return false;
}

template <typename T>
void kv_store::set(const char* key, const T& value) {
    std::lock_guard<std::mutex> lock{m};

    hm.set(key, value);
}

}   // namespace hammurabi

#endif //HAMMURABI_KV_STORE_H
