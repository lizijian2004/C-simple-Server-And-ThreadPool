#include "threadpool.hpp"
#include <functional>
#include <iostream>
#include <mutex>

void ThreadPool::Stop() {
  is_exit_ = true;
  cond_.notify_all();
  for (auto &th : threads_)
    if (th.joinable())
      th.join();
  {
    std::unique_lock<std::mutex> lock(mux_);
    threads_.clear();
  }
}

std::shared_ptr<std::function<void()>> ThreadPool::GetTask() {
  std::unique_lock<std::mutex> lock(mux_);
  if (tasks_.empty())
    cond_.wait(lock, [this] { return !tasks_.empty() || is_exit(); });
  if (tasks_.empty() || is_exit())
    return nullptr;
  auto task = std::move(tasks_.front());
  tasks_.pop_front();
  return task;
}

ThreadPool::ThreadPool(uint16_t thread_num) {
  if (thread_num <= 0)
    thread_num = std::thread::hardware_concurrency();
  thread_num_ = thread_num;
  for (int i = 0; i < thread_num_; ++i) {
    threads_.emplace_back(std::thread(&ThreadPool::Run, this));
  }
}

void ThreadPool::Run() {
  while (!is_exit()) {
    auto task = GetTask();
    if (!task)
      continue;
    try {
      (*task)();
    } catch (std::exception &e) {
      std::lock_guard<std::mutex> lock(mux_);
      std::cerr << e.what();
    } catch (...) {
      std::lock_guard<std::mutex> lock(mux_);
      std::cerr << "Run failed\n";
    }
  }
}
