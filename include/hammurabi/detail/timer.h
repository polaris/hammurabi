#ifndef HAMMURABI_TIMER_H
#define HAMMURABI_TIMER_H

#include <asio.hpp>

#include <chrono>



namespace hammurabi::detail {

class timer {
public:
    timer(asio::io_context& ioc, std::function<void()> callback) : timer_{ioc}, callback_{std::move(callback)} {}

    void start(const std::chrono::milliseconds& timeout) {
        timer_.expires_after(timeout);
        timer_.async_wait([this](const asio::error_code& ec) {
            if (ec != asio::error::operation_aborted) {
                callback_();
            }
        });
    }

    void stop() { timer_.cancel(); }

    void reset(const std::chrono::milliseconds& timeout) {
        stop();
        start(timeout);
    }

private:
    asio::steady_timer timer_;
    std::function<void()> callback_;
};

} // namespace hammurabi::detail



#endif // HAMMURABI_TIMER_H
