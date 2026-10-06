#include "hammurabi/connector.h"

#include <algorithm>

namespace hammurabi {

connector::connector(asio::io_context& ioc, unsigned short port)
: socket_{ioc, asio::ip::udp::endpoint{asio::ip::udp::v4(), port}} {}

void connector::send(const asio::ip::udp::endpoint& endpoint, uint8_t* data, std::size_t length) {
    std::fill(output_data_, output_data_ + max_length, 0);
    std::memcpy(output_data_, data, std::min(static_cast<std::size_t>(max_length), length));
    try {
        socket_.send_to(asio::buffer(output_data_, length), endpoint);
    } catch (const std::exception& ex) {
        throw;
    }
}

void connector::receive(const std::function<void(uint8_t*, std::size_t)>& callback) {
    socket_.async_receive_from(asio::buffer(input_data_, max_length), sender_endpoint_,
                               [this, callback](asio::error_code ec, std::size_t bytes_received) {
                                   if (!ec && bytes_received > 0) {
                                       callback(input_data_, bytes_received);
                                   }
                                   receive(callback);
                               });
}

} // namespace hammurabi
