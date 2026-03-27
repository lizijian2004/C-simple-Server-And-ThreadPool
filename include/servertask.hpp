#pragma once
#ifndef SERVERTASK_HPP
#define SERVERTASK_HPP
#include "threadpool.hpp"
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <unistd.h>
#include <unordered_map>

struct ConnContext {
  int fd_;
  int epoll_;
  sockaddr_in addr_;
  std::string read_buf_;
  std::string write_buf_;
  std::mutex conn_mux_;
  ConnContext(int epoll, int fd, const sockaddr_in &addr)
      : epoll_(epoll), fd_(fd), addr_(addr) {};
  ~ConnContext() {
    if (fd_ > 0) {
      close(fd_);
    }
    fd_ = -1;
  }
};

class EpollServer {
public:
  EpollServer(uint16_t port,
              int thread_num = std::thread::hardware_concurrency());
  ~EpollServer() {
    Stop();
    if (epfd_ > 0)
      close(epfd_);
    if (server_fd_ > 0)
      close(server_fd_);
  }
  bool is_running() { return is_running_; }
  void Start();
  void Stop();

private:
  bool EpollCTL(int op, uint32_t events,
                std::shared_ptr<ConnContext> ctx = nullptr);
  bool ResetOneShot(std::shared_ptr<ConnContext> ctx, uint32_t events);
  void ReleaseConn(int fd);
  void HandleAccept();
  void HandleRead(std::shared_ptr<ConnContext> ctx);
  void HandleWrite(std::shared_ptr<ConnContext> ctx);
  size_t SendData(std::shared_ptr<ConnContext> ctx);

private:
  int epfd_;
  int server_fd_;
  uint16_t port_;
  ThreadPool pool_;
  std::atomic<bool> is_running_;
  std::unordered_map<int, std::shared_ptr<ConnContext>> conn_map_;
  std::mutex conn_map_mux_;
  static constexpr int MAX_EPOLL = 1024;
  static constexpr int BUF_SIZE = 1024 * 1024 * 4;
  static constexpr int LISTEN_NUM = 2048;
};

#endif // !SERVERTASK_HPP
