#include "servertask.hpp"
#include "httpparser.hpp"
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
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#define STATIC_FILE "/home/l/code/exercise/serverTask/files"

std::string SafePathJoin(const std::string &path) {
  if (path.find("..") != std::string::npos)
    return "";
  std::string real_path = STATIC_FILE;
  if (path == "/")
    real_path += "/index.html";
  else
    real_path += path;
  return real_path;
}

std::string GetMimeType(const std::string &path) {
  size_t pos = path.find_last_of('.');
  if (pos == std::string::npos)
    return "application/octet-stream";
  if (path.compare(pos, 5, ".html") == 0)
    return "text/html";
  if (path.compare(pos, 4, ".css") == 0)
    return "text/css";
  if (path.compare(pos, 3, ".js") == 0)
    return "application/javascript";
  if (path.compare(pos, 4, ".jpg") == 0)
    return "image/jpeg";
  if (path.compare(pos, 5, ".jpeg") == 0)
    return "image/jpeg";
  if (path.compare(pos, 4, ".png") == 0)
    return "image/png";
  if (path.compare(pos, 4, ".gif") == 0)
    return "image/gif";
  if (path.compare(pos, 4, ".txt") == 0)
    return "text/plain";
  return "application/octet-stream"; // 未知类型
}

bool CheckFile(const std::string &path) {
  // 只能确定存在, 不保证并发安全
  if (access(path.c_str(), F_OK) == 0)
    return true;
  else
    return false;
}

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
  size_t total_sent = 0;
  if (!ctx->write_buf_.empty()) {
    const char *buf = ctx->write_buf_.data();
    size_t len = ctx->write_buf_.size();
    while (len > 0) {
      size_t sent = send(ctx->fd_, buf, len, MSG_NOSIGNAL);
      if (sent <= 0)
        break;
      buf += sent;
      total_sent += sent;
      len -= sent;
    }
    if (total_sent > 0)
      ctx->write_buf_.erase(0, total_sent);
    if (!ctx->write_buf_.empty())
      return total_sent;
  }

  if (ctx->is_sending_file_ && ctx->file_.is_open()) {
    char buf[4096];
    while (true) {
      ctx->file_.read(buf, sizeof(buf));
      size_t read_len = ctx->file_.gcount();
      if (read_len == 0)
        break; // 文件读完
      size_t sent = send(ctx->fd_, buf, read_len, MSG_NOSIGNAL);
      if (sent <= 0)
        break;
      total_sent += sent;
      ctx->file_sent_ += sent;
      if (ctx->file_sent_ >= ctx->file_size_) {
        ctx->file_.close();
        ctx->is_sending_file_ = false;
        break;
      }
    }
  }
  return total_sent;
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
  // 这里需要注意buf的定义, 不是数组而是一个对象
  std::vector<char> buf(BUF_SIZE);
  bool is_close{false};
  {
    std::lock_guard<std::mutex> lock(ctx->conn_mux_);
    while (true) {
      // recv返回值为0则意味着客户端关闭,
      // 最后这个宏定义表示即使客户端发送退出消息也不会关闭服务器
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

  ctx->http_parser_.AttachReadBuffer(&ctx->read_buf_);
  ParserState state = ctx->http_parser_.Parser(nullptr, 0);
  if (state == ParserState::COMPLETE) {
    const auto &req = ctx->http_parser_.GetHttpRequest();
    HttpResponse response;
    std::string file_path = SafePathJoin(req.request_path_);
    size_t file_size = 0;
    if (!file_path.empty() && CheckFile(file_path)) {
      ctx->file_.open(file_path, std::ios::binary | std::ios::in);
      if (ctx->file_.is_open()) {

        ctx->file_.seekg(0, std::ios::end);
        auto file_size_tmp = ctx->file_.tellg();
        if (file_size_tmp < 0) {
          ctx->file_size_ = 0;
        } else {
          ctx->file_size_ = static_cast<size_t>(file_size_tmp);
        }
        ctx->file_.seekg(0, std::ios::beg);
        ctx->is_sending_file_ = true;
        ctx->file_sent_ = 0;

        response.SetStatus(200, "OK")
            .SetHead("Content-Type", GetMimeType(file_path))
            .SetHead("Content-Length", std::to_string(ctx->file_size_))
            .SetHead("Connection", req.IsKeepAlive() ? "keep-alive" : "close");

        ctx->write_buf_ = response.Serialize();
      }
    } else {
      HttpResponse response;
      response.SetStatus(404, "Not Found")
          .SetHead("Content-Type", "text/html")
          .SetBody("<h1>404 FILE Not Found</h1>");
      ctx->write_buf_ = response.Serialize();
    }
    // 清除数据并发送
    ctx->read_buf_.erase(0, ctx->http_parser_.GetParserOffset());
    ctx->http_parser_.Reset();
    ResetOneShot(ctx, EPOLLIN | EPOLLOUT);
  } else if (state == ParserState::ERROR) {
    HttpResponse response;
    response.SetVersion(HttpVersion::HTTP_1_1)
        .SetStatus(400, "Bad Request")
        .SetBody("400 Bad Request: Invalid HTTP Format")
        .SetHead("Connection", "close");
    ctx->write_buf_ = response.Serialize();
    ResetOneShot(ctx, EPOLLIN | EPOLLOUT);
  } else {
    ResetOneShot(ctx, EPOLLIN);
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
