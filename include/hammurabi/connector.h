#ifndef HAMMURABI_CONNECTOR_H
#define HAMMURABI_CONNECTOR_H

#include <asio.hpp>

#include <functional>

namespace hammurabi {

class connector {
public:
    connector(asio::io_context& ioc, unsigned short port);

    void send(const asio::ip::udp::endpoint& endpoint, uint8_t* data, std::size_t length);
    void receive(const std::function<void(uint8_t*, std::size_t)>& callback);

private:
    asio::ip::udp::socket socket_;
    asio::ip::udp::endpoint sender_endpoint_;
    enum { max_length = 1024 };
    uint8_t input_data_[max_length]{};
    uint8_t output_data_[max_length]{};
};

} // namespace hammurabi

#endif // HAMMURABI_CONNECTOR_H
