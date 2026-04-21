#include "servertask.hpp"
#include <iostream>
#include <thread>

int main() {
  try {
    EpollServer server(8080, std::thread::hardware_concurrency());
    std::cout << "Server started on port 8080\n";
    server.Start();
  } catch (const std::exception &e) {
    std::cerr << "Server error: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
