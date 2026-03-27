#pragma once
#ifndef THREADPOOL_HPP
#define THREADPOOL_HPP
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

class ThreadPool {
public:
  ThreadPool(ThreadPool &&) = delete;
  ThreadPool &operator=(ThreadPool &&) = delete;
  ThreadPool(const ThreadPool &) = delete;
  ThreadPool &operator=(const ThreadPool &) = delete;
  ThreadPool(uint16_t thread_num);
  ~ThreadPool() { Stop(); }
  static ThreadPool &GetDefault() {
    static ThreadPool pool(std::thread::hardware_concurrency());
    return pool;
  }

  bool is_exit() { return is_exit_; }
  template <class Func, class... Arg>
  auto AddTask(Func &&func, Arg &&...arg)
      -> std::future<std::invoke_result_t<Func, Arg...>> {
    using RetType = std::invoke_result_t<Func, Arg...>;
    auto add_task = std::make_shared<std::packaged_task<RetType()>>(
        [func = std::forward<Func>(func),
         arg_tuple = std::make_tuple(std::forward<Arg>(arg)...)]() -> RetType {
          return std::apply(func, arg_tuple);
        });
    auto ret = add_task->get_future();
    {
      std::unique_lock<std::mutex> lock(mux_);
      if (is_exit())
        throw std::runtime_error("the thread pool stopped add task");
      auto task = std::make_shared<std::function<void()>>(
          [add_task] { (*add_task)(); });
      tasks_.emplace_back(task);
    }
    cond_.notify_one();
    return ret;
  }
  void Run();
  void Stop();

private:
  std::shared_ptr<std::function<void()>> GetTask();
  int thread_num_;
  std::atomic<bool> is_exit_{false};
  std::mutex mux_;
  std::condition_variable cond_;
  std::deque<std::shared_ptr<std::function<void()>>> tasks_;
  std::vector<std::thread> threads_;
};

#endif
