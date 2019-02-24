#ifndef HAMMURABI_TYPES_H
#define HAMMURABI_TYPES_H

#include <boost/asio.hpp>

#include <cstdint>
#include <unordered_map>

namespace hammurabi {
    using term_t = uint32_t;
    using server_id_t = uint32_t;
    using log_index_t = uint32_t;

    using endpoint_t = boost::asio::ip::udp::endpoint;

    using endpoint_map_t = std::unordered_map<server_id_t, endpoint_t>;
}

#endif // HAMMURABI_TYPES_H
