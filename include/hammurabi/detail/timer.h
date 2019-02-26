#ifndef HAMMURABI_TIMER_H
#define HAMMURABI_TIMER_H

#include <boost/asio.hpp>

namespace hammurabi {

namespace detail {

class timer {
public:
    timer(boost::asio::io_service &ioc, std::function<void()> callback)
            : timer_{ioc}
            , callback_{std::move(callback)} {
    }


    void start(const boost::posix_time::milliseconds &timeout) {
        timer_.expires_from_now(timeout);
        timer_.async_wait([this](const boost::system::error_code& ec) {
            if (ec != boost::asio::error::operation_aborted) {
                callback_();
            }
        });
    }

    void stop()  {
        boost::system::error_code ec;
        timer_.cancel(ec);
    }

    void reset(const boost::posix_time::milliseconds &timeout) {
        stop();
        start(timeout);
    }

private:
    boost::asio::deadline_timer timer_;
    std::function<void()> callback_;
};

}

}

#endif //HAMMURABI_TIMER_H
