#ifndef HAMMURABI_CONNECTOR_H
#define HAMMURABI_CONNECTOR_H

#include <boost/asio.hpp>

#include <functional>

namespace hammurabi {

class connector {
public:
    connector(boost::asio::io_service& ioc, unsigned short port);

    void send(const boost::asio::ip::udp::endpoint& endpoint, uint8_t* data, std::size_t length);
    void receive(std::function<void (uint8_t*, std::size_t)> callback);

private:
    boost::asio::ip::udp::socket socket_;
    boost::asio::ip::udp::endpoint sender_endpoint_;
    enum { max_length = 1024 };
    uint8_t input_data_[max_length];
    uint8_t output_data_[max_length];
};

}

#endif //HAMMURABI_CONNECTOR_H
