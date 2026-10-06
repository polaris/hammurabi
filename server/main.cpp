#include "hammurabi/connector.h"
#include "hammurabi/raft.h"

#include <asio.hpp>

#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

std::vector<std::string> splitString(const std::string& s);
std::vector<asio::ip::port_type> convertTo(const std::vector<std::string>& v);

int main(int argc, char* argv[]) {
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    if (argc < 3) {
        std::cout << argv[0] << " <PORT>\n";
        return -1;
    }

    const auto port = asio::ip::port_type{static_cast<unsigned short>(std::stoi(argv[1]))};
    const auto peerPorts = convertTo(splitString(argv[2]));

    hammurabi::endpoint_map_t peers;
    for (const auto port : peerPorts) {
        peers[port] = hammurabi::endpoint_t{asio::ip::make_address("127.0.0.1"), port};
    }

    asio::io_context io;
    hammurabi::raft server{io, port, peers};
    std::thread t([&io]() { io.run(); });

    getchar();

    io.stop();

    t.join();

    google::protobuf::ShutdownProtobufLibrary();
}

std::vector<std::string> splitString(const std::string& s) {
    std::vector<std::string> result;
    std::stringstream ss{s};
    while (ss.good()) {
        std::string x;
        getline(ss, x, ',');
        result.push_back(x);
    }
    return result;
}

std::vector<asio::ip::port_type> convertTo(const std::vector<std::string>& v) {
    std::vector<asio::ip::port_type> result;
    result.reserve(v.size());
    for (const auto& s : v) {
        result.emplace_back(static_cast<asio::ip::port_type>(std::stoi(s)));
    }
    return result;
}
