#ifndef HAMMURABI_TIMER_H
#define HAMMURABI_TIMER_H

#include <boost/asio.hpp>

namespace hammurabi {

class deadline_timer {
public:
    deadline_timer(boost::asio::io_service& ioc, std::function<void ()> callback);

    void start(const boost::posix_time::milliseconds &timeout);
    void stop();
    void reset(const boost::posix_time::milliseconds &timeout);

private:
    boost::asio::deadline_timer timer_;
    std::function<void ()> callback_;
};

}

#endif //HAMMURABI_TIMER_H
