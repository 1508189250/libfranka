#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <sstream>

#include <franka/exception.h>

#include <Poco/Net/DatagramSocket.h>
#include <Poco/Net/NetException.h>
#include <Poco/Net/StreamSocket.h>

namespace franka {

class Network {
 public:
  Network(const std::string& franka_address,
          uint16_t franka_port,
          std::chrono::milliseconds tcp_timeout = std::chrono::seconds(60),
          std::chrono::milliseconds udp_timeout = std::chrono::seconds(1),
          std::tuple<bool, int, int, int> tcp_keepalive = std::make_tuple(true, 1, 3, 1));
  ~Network();

  uint16_t udpPort() const noexcept;

  int udpAvailableData();

  template <typename T>
  T udpRead();

  template <typename T>
  void udpSend(const T& data);

  void tcpThrowIfConnectionClosed();

  void tcpReceiveIntoBuffer(uint8_t* buffer, size_t read_size);

  template <typename T>
  typename T::Response tcpBlockingReceiveResponse(uint32_t command_id);

  template <typename T>
  bool tcpReceiveResponse(uint32_t command_id,
                          std::function<void(const typename T::Response&)> handler);

  template <typename T, typename... TArgs>
  void tcpSendRequest(TArgs&&... args);

 private:
  template <typename T>
  bool tcpPeekHeader(T* header);

  Poco::Net::StreamSocket tcp_socket_;
  Poco::Net::DatagramSocket udp_socket_;
  Poco::Net::SocketAddress udp_server_address_;

  std::recursive_mutex udp_mutex_;
  std::recursive_mutex tcp_mutex_;
};

template <typename T>
T Network::udpRead() try {
  std::lock_guard<std::recursive_mutex> _(udp_mutex_);

  std::array<uint8_t, sizeof(T)> buffer;

  int bytes_received =
      udp_socket_.receiveFrom(buffer.data(), static_cast<int>(buffer.size()), udp_server_address_);

  if (bytes_received != buffer.size()) {
    throw ProtocolException("libfranka: incorrect object size");
  }

  return *reinterpret_cast<T*>(buffer.data());
} catch (const Poco::Exception& e) {
  using namespace std::string_literals;  // NOLINT (google-build-using-namespace)
  throw NetworkException("libfranka: UDP read: "s + e.what());
}

template <typename T>
void Network::udpSend(const T& data) try {
  std::lock_guard<std::recursive_mutex> _(udp_mutex_);

  int bytes_sent = udp_socket_.sendTo(&data, sizeof(data), udp_server_address_);
  if (bytes_sent != sizeof(data)) {
    throw NetworkException("libfranka: could not send UDP data");
  }
} catch (const Poco::Exception& e) {
  using namespace std::string_literals;  // NOLINT (google-build-using-namespace)
  throw NetworkException("libfranka: UDP send: "s + e.what());
}

template <typename T, typename... TArgs>
void Network::tcpSendRequest(TArgs&&... args) try {
  std::lock_guard<std::recursive_mutex> _(tcp_mutex_);

  typename T::Request request(std::forward<TArgs>(args)...);
  tcp_socket_.sendBytes(&request, sizeof(request));
} catch (const Poco::Exception& e) {
  using namespace std::string_literals;  // NOLINT (google-build-using-namespace)
  throw NetworkException("libfranka: TCP send bytes: "s + e.what());
}

template <typename T>
bool Network::tcpPeekHeader(T* header) try {
  std::lock_guard<std::recursive_mutex> _(tcp_mutex_);

  if (tcp_socket_.poll(0, Poco::Net::Socket::SELECT_READ) &&
      tcp_socket_.available() >= static_cast<int>(sizeof(T))) {
    int rv = tcp_socket_.receiveBytes(header, sizeof(T), MSG_PEEK);
    if (rv != sizeof(T)) {
      throw NetworkException("libfranka: Could not read data.");
    }
    return true;
  }
  return false;
} catch (const Poco::Exception& e) {
  using namespace std::string_literals;  // NOLINT (google-build-using-namespace)
  throw NetworkException("libfranka: "s + e.what());
}

template <typename T>
bool Network::tcpReceiveResponse(uint32_t command_id,
                                 std::function<void(const typename T::Response&)> handler) {
  std::lock_guard<std::recursive_mutex> _(tcp_mutex_);

  typename T::Header header;
  if (!tcpPeekHeader(&header) || header.command != T::kCommand || header.command_id != command_id) {
    return false;
  }

  // We received the header, so now we have to block and wait for the rest of the message.
  std::array<uint8_t, sizeof(typename T::Response)> buffer;
  tcpReceiveIntoBuffer(buffer.data(), buffer.size());

  typename T::Response response = *reinterpret_cast<typename T::Response*>(buffer.data());

  handler(response);
  return true;
}

template <typename T>
typename T::Response Network::tcpBlockingReceiveResponse(uint32_t command_id) {
  std::array<uint8_t, sizeof(typename T::Response)> buffer;

  // Wait until we receive a packet with the right function header.
  std::unique_lock<std::recursive_mutex> lock(tcp_mutex_, std::defer_lock);
  typename T::Header header;
  while (true) {
    lock.lock();
    if (tcpPeekHeader(&header) && header.command == T::kCommand &&
        header.command_id == command_id) {
      break;
    }
    lock.unlock();
  }
  tcpReceiveIntoBuffer(buffer.data(), buffer.size());
  return *reinterpret_cast<const typename T::Response*>(buffer.data());
}

template <typename T, uint16_t kLibraryVersion>
void connect(Network& network, uint32_t command_id, uint16_t* ri_version) {
  typename T::Request connect_request(command_id, network.udpPort());
  network.tcpSendRequest<T>(connect_request);

  typename T::Response connect_response = network.tcpBlockingReceiveResponse<T>(command_id);
  switch (connect_response.status) {
    case (T::Status::kIncompatibleLibraryVersion): {
      std::stringstream message;
      message << "libfranka: incompatible library version. " << std::endl
              << "Server version: " << connect_response.version << std::endl
              << "Library version: " << kLibraryVersion;
      throw IncompatibleVersionException(message.str());
    }
    case (T::Status::kSuccess): {
      *ri_version = connect_response.version;
      break;
    }
    default:
      throw ProtocolException(
          "libfranka gripper: protocol error during gripper connection attempt");
  }
}

}  // namespace franka
