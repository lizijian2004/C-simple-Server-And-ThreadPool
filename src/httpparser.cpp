#include "httpparser.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <sstream>
#include <string>

std::string HttpRequest::GetHead(const std::string &key,
                                 const std::string &def) const {
  auto it = request_header_.find(key);
  return it != request_header_.end() ? it->second : def;
}

size_t HttpRequest::GetContentLenth() const {
  auto it = request_header_.find("Content-Length");
  return it != request_header_.end() ? std::stoul(it->second) : 0;
}

bool HttpRequest::IsKeepAlive() const {
  auto it = request_header_.find("Connection");
  if (it != request_header_.end()) {
    std::string conn = it->second;
    std::transform(conn.begin(), conn.end(), conn.begin(), ::tolower);
    return conn == "keep-alive";
  }
  // 因为HTTP/1.1默认长链接
  return request_version_ == HttpVersion::HTTP_1_1;
}

// --------------HttpResponse-------------
HttpResponse &HttpResponse::SetVersion(HttpVersion ver) {
  response_version_ = ver;
  return *this;
}

HttpResponse &HttpResponse::SetHead(const std::string &key,
                                    const std::string &value) {
  response_header_[key] = value;
  return *this;
}

HttpResponse &HttpResponse::SetStatus(int code, const std::string &reason) {
  response_code_ = code;
  response_reason_ = reason;
  return *this;
}

HttpResponse &HttpResponse::SetBody(const std::string &body) {
  response_body_ = body;
  SetHead("Content-Length", std::to_string(response_body_.size()));
  return *this;
}

std::string HttpResponse::Serialize() const {
  std::ostringstream oss;
  const char *version = "";
  switch (response_version_) {
  case HttpVersion::HTTP_1_1:
    version = "HTTP/1.1";
    break;
  case HttpVersion::HTTP_1_0:
    version = "HTTP/1.0";
    break;
  default:
    version = "HTTP/1.1";
    break;
  }
  oss << version << ' ' << response_code_ << ' ' << response_reason_ << "\r\n";
  for (auto &h : response_header_) {
    oss << h.first << ": " << h.second << "\r\n";
  }
  oss << "\r\n" << response_body_;
  return oss.str();
}

// -----------HttpParser------------

void HttpParser::Reset() {
  state_ = ParserState::REQUEST_LINE;
  request_ = HttpRequest();
  parse_offset_ = 0;
  chunked_ = false;
  content_remaining_ = 0;
}

ParserState HttpParser::Parser(char *data, size_t len) {
  if (data != nullptr && len > 0) {
    buffer_->append(data);
  }
  // 尽可能让每次增加数据都进行分析状态的递进
  while (state_ != ParserState::COMPLETE && state_ != ParserState::ERROR) {
    switch (state_) {
    case ParserState::REQUEST_LINE:
      state_ = ParserRequestLine();
      break;
    case ParserState::HEADERS:
      state_ = ParserHeader();
      break;
    case ParserState::BODY:
      state_ = ParserBody();
      break;
    default:
      break;
    }
  }
  return state_;
}

ParserState HttpParser::ParserRequestLine() {
  // 因为通常请求行只有一行所以一次处理即可
  std::string line;
  if (!ConsumeLine(line))
    return ParserState::REQUEST_LINE; // 状态是解析请求行
  if (!ParserRequestLine(line))
    return ParserState::ERROR;
  return ParserState::HEADERS;
}

ParserState HttpParser::ParserHeader() {
  // 因为请求头不确定数量, 所以使用循环处理
  while (true) {
    std::string line;
    if (!ConsumeLine(line))
      return ParserState::HEADERS;
    // 如果没有读取到数据说明HTTP请求头部分结束
    if (line.empty()) {
      size_t length = request_.GetContentLenth();
      std::string chunk = request_.GetHead("Transfer-Encoding");
      std::transform(chunk.begin(), chunk.end(), chunk.begin(), ::tolower);
      if (chunk == "chunked") {
        chunked_ = true;
        content_remaining_ = 0;
      } else {
        chunked_ = false;
        content_remaining_ = length;
      }
      if (chunked_ || content_remaining_ > 0)
        return ParserState::BODY;
      else
        return ParserState::COMPLETE;
    }
    if (!ParserHeader(line))
      return ParserState::ERROR;
  }
}

ParserState HttpParser::ParserBody() {
  if (chunked_) {
    while (true) {
      if (content_remaining_ == 0) {
        std::string line;
        if (!ConsumeLine(line))
          return ParserState::BODY;
        // 去除注释部分
        size_t pos = line.find(';');
        if (pos != std::string::npos)
          line = line.substr(0, pos);
        line = Trim(line);
        if (line.empty())
          return ParserState::ERROR;
        // 存储结果
        char *endptr = nullptr;
        long size = strtol(line.c_str(), &endptr, 16);
        if (endptr == line.c_str() || size < 0)
          return ParserState::ERROR;
        content_remaining_ = static_cast<size_t>(size);
        // 因为HTTP数据体部分的结束标志是 0
        if (content_remaining_ == 0) {
          ConsumeLine(line);
          return ParserState::COMPLETE;
        }
      }
      // 读取数据块, 数据来源是服务器本地的文件或其他信息
      if (buffer_->size() - parse_offset_ < content_remaining_)
        return ParserState::BODY; // 数据不足
      request_.request_body_.append(buffer_->data() + parse_offset_,
                                    content_remaining_);
      parse_offset_ += content_remaining_;
      content_remaining_ = 0;
      if (buffer_->size() - parse_offset_ < 2)
        return ParserState::BODY;
      if ((*buffer_)[parse_offset_] != '\r' ||
          (*buffer_)[parse_offset_ + 1] != '\n')
        return ParserState::ERROR;
      parse_offset_ += 2;
    }
  } else {
    size_t avail = buffer_->size() - parse_offset_;
    if (avail < content_remaining_)
      return ParserState::BODY;
    request_.request_body_.assign(buffer_->data() + parse_offset_,
                                  content_remaining_);
    parse_offset_ += content_remaining_;
    return ParserState::COMPLETE;
  }
}

// 每次提取一行HTTP信息, 将其存入buffer_中, 不包括\r\n
bool HttpParser::ConsumeLine(std::string &line) {
  size_t pos = buffer_->find("\r\n", parse_offset_);
  if (pos == std::string::npos)
    return false;
  // 这里要注意substr的设计
  line = buffer_->substr(parse_offset_, pos - parse_offset_);
  parse_offset_ = pos + 2;
  return true;
}

bool HttpParser::ParserRequestLine(const std::string &line) {
  std::istringstream is{line};
  std::string method, uri, version;
  if (!(is >> method >> uri >> version))
    return false;
  request_.request_method_ = StrToMethod(method);
  request_.request_version_ = StrToVersion(version);
  request_.request_uri_ = uri;
  size_t pos = uri.find('?');
  if (pos == std::string::npos)
    request_.request_path_ = uri;
  else {
    request_.request_path_ = uri.substr(0, pos);
    ParserQueryString(uri.substr(pos + 1));
  }
  return request_.request_method_ != HttpMethod::UNKNOW &&
         request_.request_version_ != HttpVersion::UNKNOW;
}

bool HttpParser::ParserHeader(const std::string &str) {
  size_t pos = str.find(':');
  if (pos == std::string::npos)
    return false;
  std::string key = str.substr(0, pos);
  std::string val = str.substr(pos + 1);
  key = Trim(key);
  val = Trim(val);
  request_.request_header_[key] = val;
  return true;
}

void HttpParser::ParserQueryString(const std::string &str) {
  size_t start = 0;
  while (start < str.size()) {
    size_t eq = str.find('=', start);
    size_t amp = str.find('&', start);
    if (amp == std::string::npos)
      amp = str.size();
    if (eq != std::string::npos && eq < amp) {
      std::string key = str.substr(start, eq - start);
      // 这里不要忘记-1, 为的是value对应的开头处
      std::string val = str.substr(eq + 1, amp - eq - 1);
      request_.request_query_params_[key] = val;
    } else {
      std::string key = str.substr(start, amp - start);
      request_.request_query_params_[key] = "";
    }
    start = amp + 1;
  }
}

HttpMethod HttpParser::StrToMethod(const std::string &str) {
  if (str == "GET")
    return HttpMethod::GET;
  if (str == "POST")
    return HttpMethod::POST;
  return HttpMethod::UNKNOW;
}

HttpVersion HttpParser::StrToVersion(const std::string &str) {
  if (str == "HTTP/1.0")
    return HttpVersion::HTTP_1_0;
  if (str == "HTTP/1.1")
    return HttpVersion::HTTP_1_1;
  return HttpVersion::UNKNOW;
}

std::string HttpParser::Trim(std::string &str) {
  // 找第一个不是后面字符集里的字符,\r\n在这里不是换行,是不可以出现在前导空白里的字符
  size_t start = str.find_first_not_of(" \t\r\n");
  if (start == std::string::npos)
    return "";
  size_t end = str.find_last_not_of(" \t\r\n");
  return str.substr(start, end - start + 1);
}
