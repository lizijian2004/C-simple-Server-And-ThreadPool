#include "servertask.hpp"
#include "threadpool.hpp"
#include <exception>
#include <iostream>
#include <thread>

int main(int argc, char *argv[]) {
  try {
    EpollServer server(8080, std::thread::hardware_concurrency());
    server.Start();
  } catch (std::exception &e) {
    std::cerr << e.what() << std::endl;
  }
  return 0;
}
