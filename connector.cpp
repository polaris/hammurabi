#include "connector.h"

#include <algorithm>

namespace hammurabi {

connector::connector(boost::asio::io_service &ioc, unsigned short port)
: socket_{ioc, boost::asio::ip::udp::endpoint{boost::asio::ip::udp::v4(), port}} {
}

void connector::send(const boost::asio::ip::udp::endpoint &endpoint, uint8_t *data, std::size_t length) {
    memcpy(output_data_, data, std::min(std::size_t(max_length), length));
    try {
        socket_.send_to(boost::asio::buffer(output_data_, length), endpoint);
    } catch (const std::exception &ex) {
        throw;
    }
}

void connector::receive(std::function<void(uint8_t *, std::size_t)> callback) {
    socket_.async_receive_from(boost::asio::buffer(input_data_, max_length), sender_endpoint_,
        [this, callback](boost::system::error_code ec, std::size_t bytes_received) {
           if (!ec && bytes_received > 0) {
               callback(input_data_, bytes_received);
           }
           receive(callback);
        });
}

}
