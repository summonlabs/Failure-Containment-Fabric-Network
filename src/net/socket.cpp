// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fcfn/net/socket.hpp"

#include <atomic>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fcfn::net {

#ifdef _WIN32
namespace {

SOCKET to_socket(std::uintptr_t handle) { return static_cast<SOCKET>(handle); }
std::uintptr_t from_socket(SOCKET socket) { return static_cast<std::uintptr_t>(socket); }
constexpr std::uintptr_t kInvalidHandle = static_cast<std::uintptr_t>(INVALID_SOCKET);

std::once_flag g_startup_once;
int g_startup_result = 0;

}  // namespace
#else
namespace {
std::uintptr_t to_handle(int fd) { return static_cast<std::uintptr_t>(fd); }
int to_fd(std::uintptr_t handle) { return static_cast<int>(handle); }
constexpr std::uintptr_t kInvalidHandle = static_cast<std::uintptr_t>(-1);
}  // namespace
#endif

VoidResult ensure_socket_layer() {
#ifdef _WIN32
  std::call_once(g_startup_once, []() {
    WSADATA data{};
    g_startup_result = ::WSAStartup(MAKEWORD(2, 2), &data);
  });
  if (g_startup_result != 0) {
    return Status{StatusCode::IoError, "Winsock initialisation failed"};
  }
#endif
  return ok_result();
}

struct Socket::Impl {
  std::atomic<std::uintptr_t> handle{kInvalidHandle};
};

struct Listener::Impl {
  std::atomic<std::uintptr_t> handle{kInvalidHandle};
};

Listener::~Listener() { shutdown(); }

Listener::Listener(Listener&& other) noexcept : impl_(std::move(other.impl_)), port_(other.port_) {
  other.port_ = 0;
}

Listener& Listener::operator=(Listener&& other) noexcept {
  if (this != &other) {
    shutdown();
    impl_ = std::move(other.impl_);
    port_ = other.port_;
    other.port_ = 0;
  }
  return *this;
}

bool Listener::valid() const noexcept {
  return impl_ != nullptr && impl_->handle.load() != kInvalidHandle;
}

Result<Listener> Listener::bind_loopback(std::uint16_t port) {
  const VoidResult initialized = ensure_socket_layer();
  if (!initialized.ok()) {
    return initialized.status();
  }
#ifdef _WIN32
  SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == INVALID_SOCKET) {
    return Status{StatusCode::IoError, "cannot create listening socket"};
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = ::htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::closesocket(socket);
    return Status{StatusCode::IoError, "cannot bind loopback listener"};
  }
  if (::listen(socket, SOMAXCONN) != 0) {
    ::closesocket(socket);
    return Status{StatusCode::IoError, "cannot listen on loopback"};
  }
  sockaddr_in bound{};
  int bound_length = sizeof(bound);
  if (::getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0) {
    ::closesocket(socket);
    return Status{StatusCode::IoError, "cannot determine bound port"};
  }
  Listener listener;
  listener.impl_ = std::make_shared<Impl>();
  listener.impl_->handle.store(from_socket(socket));
  listener.port_ = ::ntohs(bound.sin_port);
  return listener;
#else
  const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    return Status{StatusCode::IoError, "cannot create listening socket"};
  }
  const int enable = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = ::htons(port);
  address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return Status{StatusCode::IoError, "cannot bind loopback listener"};
  }
  if (::listen(fd, SOMAXCONN) != 0) {
    ::close(fd);
    return Status{StatusCode::IoError, "cannot listen on loopback"};
  }
  sockaddr_in bound{};
  socklen_t bound_length = sizeof(bound);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0) {
    ::close(fd);
    return Status{StatusCode::IoError, "cannot determine bound port"};
  }
  Listener listener;
  listener.impl_ = std::make_shared<Impl>();
  listener.impl_->handle.store(to_handle(fd));
  listener.port_ = ::ntohs(bound.sin_port);
  return listener;
#endif
}

Result<std::unique_ptr<Socket>> Listener::accept() {
  if (!valid()) {
    return Status{StatusCode::Interrupted, "listener is closed"};
  }
  const std::uintptr_t handle = impl_->handle.load();
#ifdef _WIN32
  SOCKET client = ::accept(to_socket(handle), nullptr, nullptr);
  if (client == INVALID_SOCKET) {
    return Status{StatusCode::Interrupted, "accept released"};
  }
  auto socket = std::make_unique<Socket>();
  socket->impl_ = std::make_shared<Socket::Impl>();
  socket->impl_->handle.store(from_socket(client));
  return socket;
#else
  const int fd = ::accept(to_fd(handle), nullptr, nullptr);
  if (fd < 0) {
    return Status{StatusCode::Interrupted, "accept released"};
  }
  auto socket = std::make_unique<Socket>();
  socket->impl_ = std::make_shared<Socket::Impl>();
  socket->impl_->handle.store(to_handle(fd));
  return socket;
#endif
}

void Listener::shutdown() {
  if (impl_ == nullptr) {
    return;
  }
  const std::uintptr_t handle = impl_->handle.exchange(kInvalidHandle);
  if (handle == kInvalidHandle) {
    return;
  }
#ifdef _WIN32
  ::closesocket(to_socket(handle));
#else
  ::shutdown(to_fd(handle), SHUT_RDWR);
  ::close(to_fd(handle));
#endif
}

Socket::~Socket() { shutdown(); }

Socket::Socket(Socket&& other) noexcept : impl_(std::move(other.impl_)) {}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    shutdown();
    impl_ = std::move(other.impl_);
  }
  return *this;
}

bool Socket::valid() const noexcept {
  return impl_ != nullptr && impl_->handle.load() != kInvalidHandle;
}

Result<std::size_t> Socket::read(std::span<std::byte> bytes) {
  if (!valid()) {
    return Status{StatusCode::Interrupted, "socket is closed"};
  }
  if (bytes.empty()) {
    return static_cast<std::size_t>(0);
  }
  const std::uintptr_t handle = impl_->handle.load();
#ifdef _WIN32
  const int received = ::recv(to_socket(handle), reinterpret_cast<char*>(bytes.data()),
                              static_cast<int>(bytes.size()), 0);
  if (received < 0) {
    return Status{StatusCode::Interrupted, "socket read failed or was released"};
  }
  return static_cast<std::size_t>(received);
#else
  const ssize_t received = ::recv(to_fd(handle), bytes.data(), bytes.size(), 0);
  if (received < 0) {
    return Status{StatusCode::Interrupted, "socket read failed or was released"};
  }
  return static_cast<std::size_t>(received);
#endif
}

VoidResult Socket::write_all(std::span<const std::byte> bytes) {
  if (!valid()) {
    return Status{StatusCode::Interrupted, "socket is closed"};
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::uintptr_t handle = impl_->handle.load();
    if (handle == kInvalidHandle) {
      return Status{StatusCode::Interrupted, "socket was closed during write"};
    }
#ifdef _WIN32
    const int sent = ::send(to_socket(handle), reinterpret_cast<const char*>(bytes.data() + offset),
                            static_cast<int>(bytes.size() - offset), 0);
#else
    const ssize_t sent = ::send(to_fd(handle), bytes.data() + offset, bytes.size() - offset, 0);
#endif
    if (sent <= 0) {
      return Status{StatusCode::Interrupted, "socket write failed or was released"};
    }
    offset += static_cast<std::size_t>(sent);
  }
  return ok_result();
}

void Socket::shutdown() {
  if (impl_ == nullptr) {
    return;
  }
  const std::uintptr_t handle = impl_->handle.exchange(kInvalidHandle);
  if (handle == kInvalidHandle) {
    return;
  }
#ifdef _WIN32
  ::shutdown(to_socket(handle), SD_BOTH);
  ::closesocket(to_socket(handle));
#else
  ::shutdown(to_fd(handle), SHUT_RDWR);
  ::close(to_fd(handle));
#endif
}

Result<std::unique_ptr<Socket>> Socket::connect_loopback(std::uint16_t port) {
  const VoidResult initialized = ensure_socket_layer();
  if (!initialized.ok()) {
    return initialized.status();
  }
#ifdef _WIN32
  SOCKET handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle == INVALID_SOCKET) {
    return Status{StatusCode::IoError, "cannot create client socket"};
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = ::htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (::connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::closesocket(handle);
    return Status{StatusCode::IoError, "cannot connect to the coordinator"};
  }
  auto socket = std::unique_ptr<Socket>(new Socket());
  socket->impl_ = std::make_shared<Socket::Impl>();
  socket->impl_->handle.store(from_socket(handle));
  return socket;
#else
  const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    return Status{StatusCode::IoError, "cannot create client socket"};
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = ::htons(port);
  address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return Status{StatusCode::IoError, "cannot connect to the coordinator"};
  }
  auto socket = std::unique_ptr<Socket>(new Socket());
  socket->impl_ = std::make_shared<Socket::Impl>();
  socket->impl_->handle.store(to_handle(fd));
  return socket;
#endif
}

std::string Socket::peer() const { return "127.0.0.1"; }

}  // namespace fcfn::net
