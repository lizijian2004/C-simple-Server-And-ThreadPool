#include "httpparser.hpp"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>

// 如果找到key则返回对应键值对的值, 否则返回空
std::string HttpRequest::GetHeader(const std::string &key,
                                   const std::string &def) const {
  auto it = headers.find(key);
  return it != headers.end() ? it->second : def;
}
// 类似同上
size_t HttpRequest::GetContentLength() const {
  auto it = headers.find("Content-Length");
  if (it != headers.end()) {
    return std::stoul(it->second);
  }
  return 0;
}

bool HttpRequest::IsKeepAlive() const {
  auto it = headers.find("Connection");
  if (it != headers.end()) {
    std::string conn = it->second;
    std::transform(conn.begin(), conn.end(), conn.begin(), ::tolower);
    return conn == "keep-alive";
  }
  // HTTP/1.1 默认持久连接，HTTP/1.0 默认关闭
  return version == HttpVersion::HTTP_1_1;
}

// ---------- HttpResponse 实现 ----------
HttpResponse &HttpResponse::SetVersion(HttpVersion ver) {
  version_ = ver;
  return *this;
}

// 设置状态码
HttpResponse &HttpResponse::SetStatus(int code, const std::string &reason) {
  status_code_ = code;
  reason_ = reason;
  return *this;
}

HttpResponse &HttpResponse::SetHeader(const std::string &key,
                                      const std::string &value) {
  headers_[key] = value;
  return *this;
}

// 由于Http是文本协议,所以需要将数字的大小转换为字符
HttpResponse &HttpResponse::SetBody(const std::string &body) {
  body_ = body;
  SetHeader("Content-Length", std::to_string(body.size()));
  return *this;
}

std::string HttpResponse::Serialize() const {
  std::ostringstream oss;
  // 状态行
  const char *version_str = "";
  switch (version_) {
  case HttpVersion::HTTP_1_0:
    version_str = "HTTP/1.0";
    break;
  case HttpVersion::HTTP_1_1:
    version_str = "HTTP/1.1";
    break;
  default:
    version_str = "HTTP/1.1";
  }
  oss << version_str << " " << status_code_ << " " << reason_ << "\r\n";
  // 头部
  for (const auto &h : headers_) {
    oss << h.first << ": " << h.second << "\r\n";
  }
  // 空行 + 主体
  oss << "\r\n" << body_;
  return oss.str();
}

// ---------- HttpParser 实现 ----------
void HttpParser::Reset() {
  state_ = ParseState::REQUEST_LINE;
  request_ = HttpRequest();
  content_remaining_ = 0;
  chunked_ = false;
  parse_offset_ = 0;
}

ParseState HttpParser::Parse(const char *data, size_t len) {
  // 将新数据追加到缓冲区
  buffer_->append(data, len);
  // 主循环，每次尽量推进状态
  while (state_ != ParseState::COMPLETE && state_ != ParseState::ERROR) {
    switch (state_) {
    case ParseState::REQUEST_LINE:
      state_ = ParseRequestLine();
      break;
    case ParseState::HEADERS:
      state_ = ParseHeaders();
      break;
    case ParseState::BODY:
      state_ = ParseBody();
      break;
    default:
      break;
    }
  }
  return state_;
}

ParseState HttpParser::ParseRequestLine() {
  std::string line;
  if (!ConsumeLine(line)) {
    return ParseState::REQUEST_LINE; // 需要更多数据
  }
  if (!ParseRequestLine(line)) {
    return ParseState::ERROR;
  }
  return ParseState::HEADERS;
}

ParseState HttpParser::ParseHeaders() {
  while (true) {
    std::string line;
    if (!ConsumeLine(line)) {
      return ParseState::HEADERS; // 需要更多数据
    }
    if (line.empty()) {
      // 空行，头部结束，准备处理主体
      size_t clen = request_.GetContentLength();
      std::string te = request_.GetHeader("Transfer-Encoding");
      std::transform(te.begin(), te.end(), te.begin(), ::tolower);
      if (te == "chunked") {
        chunked_ = true;
        content_remaining_ = 0;
      } else {
        chunked_ = false;
        content_remaining_ = clen;
      }
      if (content_remaining_ > 0 || chunked_) {
        return ParseState::BODY;
      } else {
        return ParseState::COMPLETE;
      }
    }
    if (!ParseHeaderLine(line)) {
      return ParseState::ERROR;
    }
  }
}

ParseState HttpParser::ParseBody() {
  if (chunked_) {
    // 简化版 chunked 解析：仅支持最基本的十六进制长度行 + 数据块
    while (true) {
      if (content_remaining_ == 0) {
        // 需要读取新的块大小行
        std::string line;
        if (!ConsumeLine(line))
          return ParseState::BODY;
        // 去除可能的注释和空格
        size_t pos = line.find(';');
        if (pos != std::string::npos)
          line = line.substr(0, pos);
        line = Trim(line);
        if (line.empty())
          return ParseState::ERROR;
        char *endptr = nullptr;
        long size = strtol(line.c_str(), &endptr, 16);
        if (endptr == line.c_str() || size < 0)
          return ParseState::ERROR;
        content_remaining_ = static_cast<size_t>(size);
        if (content_remaining_ == 0) {
          // 最后一块，需要读取结尾空行（根据 RFC 应再跟一个空行）
          ConsumeLine(line); // 忽略最后的空行
          return ParseState::COMPLETE;
        }
      }
      // 读取数据块
      if (buffer_->size() - parse_offset_ < content_remaining_) {
        return ParseState::BODY; // 数据不够
      }
      request_.body.append(buffer_->data() + parse_offset_, content_remaining_);
      parse_offset_ += content_remaining_;
      content_remaining_ = 0;
      // 读取块后的 CRLF
      if (buffer_->size() - parse_offset_ < 2)
        return ParseState::BODY;
      if (buffer_->at(parse_offset_) != '\r' ||
          buffer_->at(parse_offset_ + 1) != '\n')
        return ParseState::ERROR;
      parse_offset_ += 2;
    }
  } else {
    // 普通 Content-Length 主体
    size_t avail = buffer_->size() - parse_offset_;
    if (avail < content_remaining_) {
      return ParseState::BODY; // 需要更多数据
    }
    request_.body.assign(buffer_->data() + parse_offset_, content_remaining_);
    parse_offset_ += content_remaining_;
    return ParseState::COMPLETE;
  }
}

// 从 buffer_ 中提取一行（以 \r\n 结尾），成功返回 true 并将行存入 line（不含\r\n）
bool HttpParser::ConsumeLine(std::string &line) {
  size_t pos = buffer_->find("\r\n", parse_offset_);
  if (pos == std::string::npos)
    return false;
  line = buffer_->substr(parse_offset_, pos - parse_offset_);
  parse_offset_ = pos + 2;
  return true;
}

bool HttpParser::ParseRequestLine(const std::string &line) {
  // 通过使用istringstream将原本的字符串转化为流, 进行简单拆解
  std::istringstream iss(line);
  std::string method_str, uri, version_str;
  if (!(iss >> method_str >> uri >> version_str))
    return false;
  request_.method = StringToMethod(method_str);
  request_.uri = uri;
  request_.version = StringToVersion(version_str);
  // 分离路径和查询参数
  size_t qpos = uri.find('?');
  if (qpos != std::string::npos) {
    request_.path = uri.substr(0, qpos);
    ParseQueryString(uri.substr(qpos + 1));
  } else {
    request_.path = uri;
  }
  return request_.method != HttpMethod::UNKNOWN &&
         request_.version != HttpVersion::UNKNOWN;
}

void HttpParser::ParseQueryString(const std::string &query) {
  size_t start = 0;
  while (start < query.size()) {
    size_t eq = query.find('=', start);
    size_t amp = query.find('&', start);
    if (amp == std::string::npos)
      amp = query.size();
    if (eq != std::string::npos && eq < amp) {
      std::string key = query.substr(start, eq - start);
      std::string val = query.substr(eq + 1, amp - eq - 1);
      request_.query_params[key] = val;
    } else {
      std::string key = query.substr(start, amp - start);
      request_.query_params[key] = "";
    }
    start = amp + 1;
  }
}

bool HttpParser::ParseHeaderLine(const std::string &line) {
  size_t colon = line.find(':');
  if (colon == std::string::npos)
    return false;
  std::string key = line.substr(0, colon);
  std::string value = line.substr(colon + 1);
  // 去除前导空白
  key = Trim(key);
  value = Trim(value);
  request_.headers[key] = value;
  return true;
}

HttpMethod HttpParser::StringToMethod(const std::string &str) {
  if (str == "GET")
    return HttpMethod::GET;
  if (str == "POST")
    return HttpMethod::POST;
  return HttpMethod::UNKNOWN;
}

HttpVersion HttpParser::StringToVersion(const std::string &str) {
  if (str == "HTTP/1.0")
    return HttpVersion::HTTP_1_0;
  if (str == "HTTP/1.1")
    return HttpVersion::HTTP_1_1;
  return HttpVersion::UNKNOWN;
}

std::string HttpParser::Trim(const std::string &str) {
  size_t start = str.find_first_not_of(" \t\r\n");
  if (start == std::string::npos)
    return "";
  size_t end = str.find_last_not_of(" \t\r\n");
  return str.substr(start, end - start + 1);
}
