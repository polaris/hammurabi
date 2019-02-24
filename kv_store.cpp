#include "kv_store.h"

#include <string>
#include <unordered_map>

namespace hammurabi {

kv_store::kv_store(std::size_t bucket_count)
: m{}
, hm{bucket_count} {
}

bool kv_store::exists(const char *key) {
    std::lock_guard<std::mutex> lock{m};

    return hm.exists(key);
}

void kv_store::remove(const char *key) {
    std::lock_guard<std::mutex> lock{m};

    hm.remove(key);
}

}
