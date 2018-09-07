#pragma once

// `krbn::local_datagram::client` can be used safely in a multi-threaded environment.

#include "boost_defs.hpp"

BEGIN_BOOST_INCLUDE
#include <boost/asio.hpp>
#include <boost/signals2.hpp>
END_BOOST_INCLUDE

#include "logger.hpp"
#include "thread_utility.hpp"

namespace krbn {
namespace local_datagram {
class client final {
public:
  // Signals

  boost::signals2::signal<void(void)> connected;
  boost::signals2::signal<void(const boost::system::error_code&)> connect_failed;
  boost::signals2::signal<void(void)> closed;

  // Methods

  client(const client&) = delete;

  client(void) : io_service_(),
                 work_(std::make_unique<boost::asio::io_service::work>(io_service_)),
                 socket_(io_service_),
                 connected_(false) {
    io_service_thread_ = std::thread([this] {
      (this->io_service_).run();
    });
  }

  ~client(void) {
    async_close();

    if (io_service_thread_.joinable()) {
      work_ = nullptr;
      io_service_thread_.join();
    }
  }

  void async_connect(const std::string& path,
                     boost::optional<std::chrono::milliseconds> server_check_interval) {
    async_close();

    io_service_.post([this, path, server_check_interval] {
      connected_ = false;

      // Open

      {
        boost::system::error_code error_code;
        socket_.open(boost::asio::local::datagram_protocol::socket::protocol_type(),
                     error_code);
        if (error_code) {
          connect_failed(error_code);
          return;
        }
      }

      // Connect

      socket_.async_connect(boost::asio::local::datagram_protocol::endpoint(path),
                            [this, server_check_interval](auto&& error_code) {
                              if (error_code) {
                                connect_failed(error_code);
                              } else {
                                connected_ = true;

                                stop_server_check_timer();
                                start_server_check_timer(server_check_interval);

                                connected();
                              }
                            });
    });
  }

  void async_close(void) {
    io_service_.post([this] {
      stop_server_check_timer();

      // Close socket

      boost::system::error_code error_code;

      socket_.cancel(error_code);
      socket_.close(error_code);

      // Signal

      if (connected_) {
        connected_ = false;
        closed();
      }
    });
  }

  void async_send(const std::vector<uint8_t>& v) {
    auto ptr = std::make_shared<buffer>(v);
    io_service_.post([this, ptr] {
      do_async_send(ptr);
    });
  }

  void async_send(const uint8_t* _Nonnull p, size_t length) {
    auto ptr = std::make_shared<buffer>(p, length);
    io_service_.post([this, ptr] {
      do_async_send(ptr);
    });
  }

private:
  class buffer final {
  public:
    buffer(const std::vector<uint8_t> v) {
      v_ = v;
    }

    buffer(const uint8_t* _Nonnull p, size_t length) {
      v_.resize(length);
      memcpy(&(v_[0]), p, length);
    }

    const std::vector<uint8_t>& get_vector(void) const { return v_; }

  private:
    std::vector<uint8_t> v_;
  };

  void do_async_send(std::shared_ptr<buffer> ptr) {
    socket_.async_send(boost::asio::buffer(ptr->get_vector()),
                       [this, ptr](auto&& error_code,
                                   auto&& bytes_transferred) {
                         if (error_code) {
                           async_close();
                         }
                       });
  }

  void start_server_check_timer(boost::optional<std::chrono::milliseconds> server_check_interval) {
    if (!server_check_interval) {
      return;
    }

    if (server_check_timer_) {
      return;
    }

    server_check_timer_ = std::make_unique<thread_utility::timer>(
        *server_check_interval,
        thread_utility::timer::mode::repeat,
        [this] {
          io_service_.post([this] {
            std::vector<uint8_t> data;
            async_send(data);
          });
        });
  }

  void stop_server_check_timer(void) {
    if (!server_check_timer_) {
      return;
    }

    server_check_timer_->cancel();
    server_check_timer_ = nullptr;
  }

  boost::asio::io_service io_service_;
  std::unique_ptr<boost::asio::io_service::work> work_;
  boost::asio::local::datagram_protocol::socket socket_;
  std::thread io_service_thread_;
  bool connected_;

  std::unique_ptr<thread_utility::timer> server_check_timer_;
};
} // namespace local_datagram
} // namespace krbn
