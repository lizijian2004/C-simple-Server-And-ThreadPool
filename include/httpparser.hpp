#pragma once
#include <memory>
#ifndef HTTPPARSER_HPP
#define HTTPPARSER_HPP
#include "servertask.hpp"
#include "threadpool.hpp"

class HttpParser {
public:
  // 仅仅持有使用权(外部的引用), 生命周期由外部管理, 这就是注入
  explicit HttpParser(ThreadPool &pool) : pool_(pool) {};
  void ParserRequest(std::shared_ptr<ConnContext> ctx) {
    pool_.AddTask([this, ctx]() { Parser(ctx); });
  }

private:
  void Parser(std::shared_ptr<ConnContext> ctx);
  void GetSuffixByType();

private:
  ThreadPool &pool_;
  std::string mode;    // 请求方法
  std::string path;    // 请求的文件或路径
  std::string version; // HTTP 版本
};

#endif // !HTTPPARSER_HPP
