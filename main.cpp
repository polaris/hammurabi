#include "connector.h"
#include "raft.h"
#include "proto/raft.pb.h"

#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/lexical_cast.hpp>

std::vector<std::string> splitString(const std::string &s);
template<typename T>
std::vector<T> convertTo(const std::vector<std::string>& v);

int main(int argc, char* argv[]) {
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    if (argc < 3) {
        std::cout << argv[0] << " <index>\n";
        return -1;
    }

    const auto port = boost::lexical_cast<unsigned short>(argv[1]);
    const auto peerPorts = convertTo<unsigned short>(splitString(argv[2]));

    hammurabi::endpoint_map_t peers;
    for (const auto peer : peerPorts) {
        peers[peer] = hammurabi::endpoint_t{boost::asio::ip::address::from_string("127.0.0.1"), peer};
    }

    boost::asio::io_service io;
    hammurabi::raft server{io, port, peers};
    std::thread t([&io](){
        io.run();
    });

    getchar();

    io.stop();

    t.join();

    google::protobuf::ShutdownProtobufLibrary();
}

std::vector<std::string> splitString(const std::string &s) {
    std::vector<std::string> result;
    std::stringstream ss{s};
    while (ss.good()) {
        std::string x;
        getline(ss, x, ',');
        result.push_back(x);
    }
    return result;
}

template<typename T>
std::vector<T> convertTo(const std::vector<std::string>& v) {
    std::vector<T> result;
    for (const auto& s : v) {
        result.push_back(boost::lexical_cast<T>(s));
    }
    return result;
}
