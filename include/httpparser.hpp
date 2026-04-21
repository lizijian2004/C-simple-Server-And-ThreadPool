#pragma once
#include <cstddef>
#ifndef HTTPPARSER
#define HTTPPARSER
#include <string>
#include <unordered_map>

enum class HttpVersion { HTTP_1_0, HTTP_1_1, UNKNOW };
enum class HttpMethod { GET, POST, UNKNOW };
enum class ParserState { REQUEST_LINE, HEADERS, BODY, ERROR, COMPLETE };

struct HttpRequest {
  HttpMethod request_method_;
  std::string request_uri_;
  std::string request_path_;
  std::unordered_map<std::string, std::string> request_query_params_; // 参数
  HttpVersion request_version_;
  std::unordered_map<std::string, std::string> request_header_;
  std::string request_body_;

  std::string GetHead(const std::string &key,
                      const std::string &def = "") const;
  size_t GetContentLenth() const;
  bool IsKeepAlive() const;
};

class HttpResponse {
public:
  // 返回值类型不是void是为了方便链式调用, 即tmp.SetBody().SetVersion();
  HttpResponse &SetVersion(HttpVersion ver);
  HttpResponse &SetHead(const std::string &key, const std::string &value);
  HttpResponse &SetStatus(int code, const std::string &reason);
  HttpResponse &SetBody(const std::string &body);
  std::string Serialize() const;

private:
  HttpVersion response_version_ = HttpVersion::HTTP_1_1;
  int response_code_ = 200;
  std::string response_reason_ = "OK";
  std::unordered_map<std::string, std::string> response_header_;
  std::string response_body_;
};

class HttpParser {
public:
  HttpParser() { Reset(); }
  void Reset();
  void AttachReadBuffer(std::string *str) { buffer_ = str; }
  // 增量解析数据, 返回当前状态, 也就是增加数据到缓存区
  ParserState Parser(char *data, size_t len);
  ParserState GetState() { return state_; }
  const size_t GetParserOffset() { return parse_offset_; }
  const HttpRequest &GetHttpRequest() const { return request_; }

private:
  ParserState state_;
  HttpRequest request_;
  std::string *buffer_;
  // 解析到那里了
  size_t parse_offset_ = 0;
  // 剩余多少
  size_t content_remaining_ = 0;
  // 在不知道全部长度的情况下应该如何传输
  bool chunked_ = false;

private:
  ParserState ParserRequestLine();
  bool ParserRequestLine(const std::string &line);
  ParserState ParserHeader();
  bool ParserHeader(const std::string &line);
  ParserState ParserBody();

  // 辅助函数区域
  bool ConsumeLine(std::string &line); // 获取一行数据
  void ParserQueryString(const std::string &str);
  HttpMethod StrToMethod(const std::string &str);
  HttpVersion StrToVersion(const std::string &str);
  // 去除前后导空白
  std::string Trim(std::string &str);
};

#endif // !HTTPPARSER
