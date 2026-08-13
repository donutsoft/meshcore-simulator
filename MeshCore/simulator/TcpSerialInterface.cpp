#include "TcpSerialInterface.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace simulator {
namespace {
constexpr size_t kMaximumFrame = MAX_FRAME_SIZE;
std::mutex frame_log_mutex;

void logFrame(uint16_t port, const char* direction, const uint8_t* data, size_t size) {
  std::ostringstream line;
  line << "TCP_FRAME port=" << port << " direction=" << direction
       << " length=" << size << " hex=" << std::hex << std::uppercase
       << std::setfill('0');
  for (size_t index = 0; index < size; ++index) {
    if (index) line << ' ';
    line << std::setw(2) << static_cast<unsigned int>(data[index]);
  }
  std::lock_guard<std::mutex> lock(frame_log_mutex);
  std::cout << line.str() << std::endl;
}

void closeSocket(int& descriptor) {
  if (descriptor < 0) return;
  ::shutdown(descriptor, SHUT_RDWR);
  ::close(descriptor);
  descriptor = -1;
}
bool sendAll(int descriptor, const uint8_t* data, size_t size) {
  while (size) {
    const ssize_t sent = ::send(descriptor, data, size, 0);
    if (sent < 0 && errno == EINTR) continue;
    if (sent <= 0) return false;
    data += sent;
    size -= static_cast<size_t>(sent);
  }
  return true;
}
}  // namespace

TcpSerialInterface::TcpSerialInterface(std::string bind_address, uint16_t port)
    : bind_address_(std::move(bind_address)), port_(port) {}
TcpSerialInterface::~TcpSerialInterface() { stop(); }

void TcpSerialInterface::start() {
  listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listener_ < 0) throw std::runtime_error("cannot create TCP companion socket: " + std::string(std::strerror(errno)));
  int enabled = 1;
  ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port_);
  if (::inet_pton(AF_INET, bind_address_.c_str(), &address.sin_addr) != 1) {
    closeSocket(listener_);
    throw std::runtime_error("invalid companion bind address: " + bind_address_);
  }
  if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    const std::string error = std::strerror(errno);
    closeSocket(listener_);
    throw std::runtime_error("cannot bind companion port " + std::to_string(port_) + ": " + error);
  }
  if (::listen(listener_, 1) != 0) {
    const std::string error = std::strerror(errno);
    closeSocket(listener_);
    throw std::runtime_error("cannot listen on companion port " + std::to_string(port_) + ": " + error);
  }
  thread_ = std::thread(&TcpSerialInterface::runServer, this);
}

void TcpSerialInterface::stop() {
  if (stopping_.exchange(true)) return;
  closeSockets();
  if (thread_.joinable()) thread_.join();
}
void TcpSerialInterface::disable() {
  enabled_.store(false);
  std::lock_guard<std::mutex> lock(mutex_);
  closeSocket(client_);
}
void TcpSerialInterface::closeSockets() {
  std::lock_guard<std::mutex> lock(mutex_);
  closeSocket(client_);
  closeSocket(listener_);
}
bool TcpSerialInterface::isWriteBusy() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !output_queue_.empty();
}
size_t TcpSerialInterface::writeFrame(const uint8_t source[], size_t length) {
  if (!enabled_.load() || length == 0 || length > kMaximumFrame) return 0;
  std::lock_guard<std::mutex> lock(mutex_);
  output_queue_.emplace_back(source, source + length);
  return length;
}
size_t TcpSerialInterface::checkRecvFrame(uint8_t destination[]) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (input_queue_.empty()) return 0;
  auto frame = std::move(input_queue_.front());
  input_queue_.pop_front();
  std::memcpy(destination, frame.data(), frame.size());
  return frame.size();
}

void TcpSerialInterface::runServer() {
  while (!stopping_.load()) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(listener_, &readable);
    timeval timeout{0, 200'000};
    const int ready = ::select(listener_ + 1, &readable, nullptr, nullptr, &timeout);
    if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0 || !FD_ISSET(listener_, &readable) || !enabled_.load()) continue;
    const int accepted = ::accept(listener_, nullptr, nullptr);
    if (accepted < 0) continue;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      client_ = accepted;
      input_queue_.clear();
      output_queue_.clear();
    }
    connected_.store(true);
    std::vector<uint8_t> input;

    while (!stopping_.load() && enabled_.load()) {
      std::deque<std::vector<uint8_t>> output;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        output.swap(output_queue_);
      }
      bool write_ok = true;
      for (const auto& frame : output) {
        // Keep the serial-style frame contiguous in one TCP write. TCP clients
        // must still support arbitrary stream fragmentation, but the MeshCore
        // app's network transport expects the marker, length, and small payload
        // to normally arrive together.
        std::vector<uint8_t> wire_frame;
        wire_frame.reserve(frame.size() + 3);
        wire_frame.push_back('>');
        wire_frame.push_back(static_cast<uint8_t>(frame.size()));
        wire_frame.push_back(static_cast<uint8_t>(frame.size() >> 8));
        wire_frame.insert(wire_frame.end(), frame.begin(), frame.end());
        if (!sendAll(accepted, wire_frame.data(), wire_frame.size())) {
          write_ok = false;
          break;
        }
        logFrame(port_, "companion_to_app", frame.data(), frame.size());
      }
      if (!write_ok) break;

      fd_set client_readable;
      FD_ZERO(&client_readable);
      FD_SET(accepted, &client_readable);
      timeval client_timeout{0, 20'000};
      const int client_ready = ::select(accepted + 1, &client_readable, nullptr, nullptr, &client_timeout);
      if (client_ready < 0 && errno == EINTR) continue;
      if (client_ready < 0) break;
      if (client_ready > 0) {
        uint8_t buffer[512];
        const ssize_t received = ::recv(accepted, buffer, sizeof(buffer), 0);
        if (received <= 0) break;
        input.insert(input.end(), buffer, buffer + received);
      }
      while (input.size() >= 3) {
        if (input[0] != '<') { input.erase(input.begin()); continue; }
        const size_t length = static_cast<size_t>(input[1]) | (static_cast<size_t>(input[2]) << 8);
        if (length == 0 || length > kMaximumFrame) { input.erase(input.begin()); continue; }
        if (input.size() < length + 3) break;
        logFrame(port_, "app_to_companion", input.data() + 3, length);
        {
          std::lock_guard<std::mutex> lock(mutex_);
          input_queue_.emplace_back(input.begin() + 3, input.begin() + 3 + length);
        }
        input.erase(input.begin(), input.begin() + 3 + length);
      }
    }
    connected_.store(false);
    std::lock_guard<std::mutex> lock(mutex_);
    input_queue_.clear();
    output_queue_.clear();
    if (client_ == accepted) closeSocket(client_);
  }
}

}  // namespace simulator
