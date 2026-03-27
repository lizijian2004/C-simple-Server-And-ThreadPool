#include "servertask.hpp"
#include "threadpool.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

void EpollServer::ReleaseConn(int fd) {
  std::lock_guard<std::mutex> lock(conn_map_mux_);
  auto it = conn_map_.find(fd);
  if (it == conn_map_.end())
    return;
  epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
  conn_map_.erase(it);
}

bool EpollServer::EpollCTL(int op, uint32_t events,
                           std::shared_ptr<ConnContext> ctx) {
  epoll_event ev{};
  ev.events = events;
  ev.data.ptr = ctx ? ctx.get() : nullptr;
  if (!ctx)
    return false;
  std::lock_guard<std::mutex> lock(ctx->conn_mux_);
  if (-1 == epoll_ctl(epfd_, op, ctx->fd_, &ev)) {
    return false;
  }
  return true;
}

void EpollServer::HandleAccept() {
  sockaddr_in addr;
  socklen_t addr_len = sizeof(addr);
  int client_fd =
      accept4(server_fd_, (sockaddr *)&addr, &addr_len, SOCK_NONBLOCK);
  if (client_fd == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return;
    std::cerr << "accept4 failed: " << strerror(errno) << '\n';
    return;
  }

  auto ctx = std::make_shared<ConnContext>(epfd_, client_fd, addr);
  if (!EpollCTL(EPOLL_CTL_ADD, EPOLLIN | EPOLLET | EPOLLONESHOT, ctx)) {
    close(client_fd);
    return;
  }
  std::lock_guard<std::mutex> lock(conn_map_mux_);
  conn_map_[client_fd] = ctx;
}

size_t EpollServer::SendData(std::shared_ptr<ConnContext> ctx) {
  if (ctx->write_buf_.empty())
    return 0;
  const char *send_buf = ctx->write_buf_.data();
  size_t sent = 0;
  size_t buf_len = ctx->write_buf_.size();
  while (buf_len > 0) {
    // 返回值为0意味着发送了0个数据
    int send_ret = send(ctx->fd_, send_buf, buf_len, MSG_NOSIGNAL);
    if (send_ret > 0) {
      sent += send_ret;
      send_buf += send_ret;
      buf_len -= send_ret;
    } else if (send_ret == 0) {
      break;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      } else {
        std::cerr << "send failed: " << strerror(errno) << '\n';
        return 0;
      }
    }
  }
  if (sent > 0)
    ctx->write_buf_.erase(0, sent);
  return sent;
}

bool EpollServer::ResetOneShot(std::shared_ptr<ConnContext> ctx,
                               uint32_t events) {
  if (!ctx || ctx->fd_ < 0)
    return false;
  return EpollCTL(EPOLL_CTL_MOD, events | EPOLLET | EPOLLONESHOT, ctx);
}

void EpollServer::HandleWrite(std::shared_ptr<ConnContext> ctx) {
  if (!ctx || ctx->fd_ == -1)
    return;
  size_t ret;
  {
    std::lock_guard<std::mutex> lock(ctx->conn_mux_);
    ret = SendData(ctx);
  }
  if (ret == 0 && !ctx->write_buf_.empty()) {
    ReleaseConn(ctx->fd_);
    return;
  }
  if (ctx->write_buf_.empty()) {
    ResetOneShot(ctx, EPOLLIN);
  } else {
    ResetOneShot(ctx, EPOLLOUT | EPOLLIN);
  }
}

void EpollServer::HandleRead(std::shared_ptr<ConnContext> ctx) {
  if (!ctx || ctx->fd_ == -1)
    return;
  std::vector<char> buf(BUF_SIZE);
  bool is_close{false};
  {
    std::lock_guard<std::mutex> lock(ctx->conn_mux_);
    while (true) {
      // recv返回值为0则意味着客户端关闭
      int recv_ret = recv(ctx->fd_, buf.data(), BUF_SIZE - 1, MSG_NOSIGNAL);
      if (recv_ret > 0) {
        ctx->read_buf_.append(buf.data(), recv_ret);
        continue;
      } else if (recv_ret == 0) {
        is_close = true;
        break;
      } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        } else {
          is_close = true;
          break;
        }
      }
    }
  }

  if (is_close) {
    ReleaseConn(ctx->fd_);
    return;
  }
  // 回声
  if (!ctx->read_buf_.empty()) {
    ctx->write_buf_.append(ctx->read_buf_);
    ctx->read_buf_.erase(0, ctx->read_buf_.size());
    ResetOneShot(ctx, EPOLLIN | EPOLLOUT);
  } else {
    EpollCTL(EPOLL_CTL_MOD, EPOLLIN | EPOLLET | EPOLLONESHOT, ctx);
  }
}

EpollServer::EpollServer(uint16_t port, int thread_num)
    : port_(port), pool_(thread_num), is_running_(false) {
  server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd_ < 0) {
    throw std::runtime_error("socket failed: " + std::string(strerror(errno)));
  }
  sockaddr_in server{};
  server.sin_family = AF_INET;
  server.sin_port = htons(port_);
  if (-1 == inet_pton(AF_INET, "0.0.0.0", &server.sin_addr)) {
    close(server_fd_);
    throw std::runtime_error("inet_pton failed: " +
                             std::string(strerror(errno)));
  }
  int opt = 1;
  if (-1 ==
      setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
    close(server_fd_);
    throw std::runtime_error("setsockopt failed: " +
                             std::string(strerror(errno)));
  }
  if (-1 == bind(server_fd_, (sockaddr *)&server, sizeof(server))) {
    close(server_fd_);
    throw std::runtime_error("bind failed: " + std::string(strerror(errno)));
  }
  if (-1 == listen(server_fd_, LISTEN_NUM)) {
    close(server_fd_);
    throw std::runtime_error("listen failed: " + std::string(strerror(errno)));
  }
  epfd_ = epoll_create1(EPOLL_CLOEXEC);
  if (epfd_ < 0) {
    close(server_fd_);
    throw std::runtime_error("epoll_create1 failed: " +
                             std::string(strerror(errno)));
  }
  epoll_event tmp{};
  tmp.events = EPOLLIN | EPOLLET;
  tmp.data.fd = server_fd_;
  if (-1 == epoll_ctl(epfd_, EPOLL_CTL_ADD, server_fd_, &tmp)) {
    close(server_fd_);
    close(epfd_);
    throw std::runtime_error("epoll_ctl failed: " +
                             std::string(strerror(errno)));
  }
}

void EpollServer::Stop() {
  if (!is_running())
    return;
  is_running_ = false;
  pool_.Stop();
  std::lock_guard<std::mutex> lock(conn_map_mux_);
  for (auto &[fd, conn_] : conn_map_)
    epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
  conn_map_.clear();
}

void EpollServer::Start() {
  if (is_running())
    return;
  is_running_ = true;
  epoll_event events[MAX_EPOLL]{0};
  while (is_running()) {
    int ret = epoll_wait(epfd_, events, MAX_EPOLL, 500);
    for (int i = 0; i < ret; ++i) {
      if (events[i].data.fd == server_fd_) {
        HandleAccept();
      } else {
        auto ctx_ptr = static_cast<ConnContext *>(events[i].data.ptr);
        if (!ctx_ptr)
          continue;
        std::shared_ptr<ConnContext> ctx;
        {
          std::lock_guard<std::mutex> lock(conn_map_mux_);
          auto it = conn_map_.find(ctx_ptr->fd_);
          if (it == conn_map_.end())
            continue;
          ctx = it->second;
        }

        if (events[i].events & (EPOLLERR | EPOLLHUP)) {
          pool_.AddTask([this, ctx] { ReleaseConn(ctx->fd_); });
        } else if (events[i].events & (EPOLLIN | EPOLLPRI)) {
          pool_.AddTask([this, ctx] { HandleRead(ctx); });
        } else if (events[i].events & EPOLLOUT) {
          pool_.AddTask([this, ctx] { HandleWrite(ctx); });
        }
      }
    }
  }
}
