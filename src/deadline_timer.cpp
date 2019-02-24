#include "../include/hammurabi/deadline_timer.h"

namespace hammurabi {

deadline_timer::deadline_timer(boost::asio::io_service &ioc, std::function<void ()> callback)
: timer_{ioc}
, callback_{std::move(callback)} {
}

void deadline_timer::start(const boost::posix_time::milliseconds &timeout) {
    timer_.expires_from_now(timeout);
    timer_.async_wait([this](const boost::system::error_code& ec) {
        if (ec != boost::asio::error::operation_aborted) {
            callback_();
        }
    });
}

void deadline_timer::stop() {
    boost::system::error_code ec;
    timer_.cancel(ec);
}

void deadline_timer::reset(const boost::posix_time::milliseconds &timeout) {
    stop();
    start(timeout);
}

}
