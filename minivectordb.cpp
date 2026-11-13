#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace mv {

struct Error : std::runtime_error {
  std::string code;
  Error(std::string c, std::string m) : std::runtime_error(std::move(m)), code(std::move(c)) {}
};

struct Json {
  enum Kind { NIL, BOOL, INT, DOUBLE, STRING, ARRAY, OBJECT } kind = NIL;
  bool b = false;
  int64_t i = 0;
  double d = 0;
  std::string s;
  std::vector<Json> a;
  std::map<std::string, Json> o;
  static Json nil() { return {}; }
  static Json boolean(bool x) { Json j; j.kind = BOOL; j.b = x; return j; }
  static Json integer(int64_t x) { Json j; j.kind = INT; j.i = x; return j; }
  static Json number(double x) { Json j; j.kind = DOUBLE; j.d = x; return j; }
  static Json string(std::string x) { Json j; j.kind = STRING; j.s = std::move(x); return j; }
  static Json array(std::vector<Json> x) { Json j; j.kind = ARRAY; j.a = std::move(x); return j; }
  static Json object(std::map<std::string, Json> x) { Json j; j.kind = OBJECT; j.o = std::move(x); return j; }
};

static void json_skip(const std::string &s, size_t &p) {
  while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
}
static uint32_t hex4(const std::string &s, size_t &p) {
  if (p + 4 > s.size()) throw Error("invalid_request", "invalid unicode escape");
  uint32_t v = 0;
  for (int k = 0; k < 4; ++k) {
    char c = s[p++]; v <<= 4;
    if (c >= '0' && c <= '9') v += c - '0';
    else if (c >= 'a' && c <= 'f') v += c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v += c - 'A' + 10;
    else throw Error("invalid_request", "invalid unicode escape");
  }
  return v;
}
static void utf8_append(std::string &out, uint32_t c) {
  if (c <= 0x7f) out.push_back(char(c));
  else if (c <= 0x7ff) { out.push_back(char(0xc0 | (c >> 6))); out.push_back(char(0x80 | (c & 63))); }
  else if (c <= 0xffff) { out.push_back(char(0xe0 | (c >> 12))); out.push_back(char(0x80 | ((c >> 6) & 63))); out.push_back(char(0x80 | (c & 63))); }
  else { out.push_back(char(0xf0 | (c >> 18))); out.push_back(char(0x80 | ((c >> 12) & 63))); out.push_back(char(0x80 | ((c >> 6) & 63))); out.push_back(char(0x80 | (c & 63))); }
}
static Json json_parse_value(const std::string &, size_t &);
static std::string json_parse_string(const std::string &s, size_t &p) {
  if (p >= s.size() || s[p++] != '"') throw Error("invalid_request", "expected JSON string");
  std::string out;
  while (p < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[p++]);
    if (c == '"') return out;
    if (c < 0x20) throw Error("invalid_request", "control character in JSON string");
    if (c != '\\') { out.push_back(char(c)); continue; }
    if (p >= s.size()) throw Error("invalid_request", "truncated JSON escape");
    char e = s[p++];
    if (e == '"' || e == '\\' || e == '/') out.push_back(e);
    else if (e == 'b') out.push_back('\b'); else if (e == 'f') out.push_back('\f');
    else if (e == 'n') out.push_back('\n'); else if (e == 'r') out.push_back('\r');
    else if (e == 't') out.push_back('\t');
    else if (e == 'u') { uint32_t u = hex4(s, p); if (u >= 0xd800 && u <= 0xdbff && p + 6 <= s.size() && s[p] == '\\' && s[p + 1] == 'u') { p += 2; uint32_t v = hex4(s, p); if (v >= 0xdc00 && v <= 0xdfff) u = 0x10000 + ((u - 0xd800) << 10) + v; } utf8_append(out, u); }
    else throw Error("invalid_request", "unknown JSON escape");
  }
  throw Error("invalid_request", "unterminated JSON string");
}
static Json json_parse_value(const std::string &s, size_t &p) {
  json_skip(s, p); if (p >= s.size()) throw Error("invalid_request", "missing JSON value");
  if (s[p] == '"') return Json::string(json_parse_string(s, p));
  if (s[p] == '{') { ++p; std::map<std::string, Json> o; json_skip(s, p); if (p < s.size() && s[p] == '}') { ++p; return Json::object(std::move(o)); } while (true) { json_skip(s, p); std::string k = json_parse_string(s, p); json_skip(s, p); if (p >= s.size() || s[p++] != ':') throw Error("invalid_request", "expected colon"); o.emplace(std::move(k), json_parse_value(s, p)); json_skip(s, p); if (p < s.size() && s[p] == '}') { ++p; break; } if (p >= s.size() || s[p++] != ',') throw Error("invalid_request", "expected comma"); } return Json::object(std::move(o)); }
  if (s[p] == '[') { ++p; std::vector<Json> a; json_skip(s, p); if (p < s.size() && s[p] == ']') { ++p; return Json::array(std::move(a)); } while (true) { a.push_back(json_parse_value(s, p)); json_skip(s, p); if (p < s.size() && s[p] == ']') { ++p; break; } if (p >= s.size() || s[p++] != ',') throw Error("invalid_request", "expected comma"); } return Json::array(std::move(a)); }
  if (s.compare(p, 4, "null") == 0) { p += 4; return Json::nil(); }
  if (s.compare(p, 4, "true") == 0) { p += 4; return Json::boolean(true); }
  if (s.compare(p, 5, "false") == 0) { p += 5; return Json::boolean(false); }
  size_t start = p; while (p < s.size() && std::string("0123456789+-.eE").find(s[p]) != std::string::npos) ++p; if (start == p) throw Error("invalid_request", "invalid JSON value"); std::string n = s.substr(start, p - start); try { if (n.find_first_of(".eE") == std::string::npos) return Json::integer(std::stoll(n)); double x = std::stod(n); if (!std::isfinite(x)) throw Error("invalid_request", "nonfinite JSON number"); return Json::number(x); } catch (...) { throw Error("invalid_request", "invalid JSON number"); }
}
static Json json_parse(const std::string &s) { size_t p = 0; Json j = json_parse_value(s, p); json_skip(s, p); if (p != s.size()) throw Error("invalid_request", "trailing JSON data"); return j; }
static std::string json_escape(const std::string &s) { std::ostringstream o; o << '"'; for (unsigned char c : s) { if (c == '"') o << "\\\""; else if (c == '\\') o << "\\\\"; else if (c == '\b') o << "\\b"; else if (c == '\f') o << "\\f"; else if (c == '\n') o << "\\n"; else if (c == '\r') o << "\\r"; else if (c == '\t') o << "\\t"; else if (c < 0x20) o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c) << std::dec; else o << char(c); } return o.str() + '"'; }
static std::string json_dump(const Json &j) { std::ostringstream o; switch (j.kind) { case Json::NIL: return "null"; case Json::BOOL: return j.b ? "true" : "false"; case Json::INT: return std::to_string(j.i); case Json::DOUBLE: { if (!std::isfinite(j.d)) return "null"; o << std::setprecision(15) << j.d; std::string x = o.str(); if (x == "-0") x = "0"; return x; } case Json::STRING: return json_escape(j.s); case Json::ARRAY: { o << '['; for (size_t k = 0; k < j.a.size(); ++k) { if (k) o << ','; o << json_dump(j.a[k]); } return o.str() + ']'; } case Json::OBJECT: { o << '{'; bool first = true; for (auto &kv : j.o) { if (!first) o << ','; first = false; o << json_escape(kv.first) << ':' << json_dump(kv.second); } return o.str() + '}'; } } return "null"; }

struct Token { std::string text; bool quoted = false; };
static std::vector<Token> lex_sql(std::string sql) {
  std::vector<Token> out; for (size_t p = 0; p < sql.size();) { if (std::isspace(static_cast<unsigned char>(sql[p]))) { ++p; continue; } if (sql[p] == '-' && p + 1 < sql.size() && sql[p + 1] == '-') break; if (std::string("(),;*=<>[]").find(sql[p]) != std::string::npos) { if ((sql[p] == '<' || sql[p] == '>') && p + 1 < sql.size() && sql[p + 1] == '=') out.push_back({sql.substr(p, 2), false}), p += 2; else out.push_back({sql.substr(p++, 1), false}); continue; } if (sql[p] == '\'') { ++p; std::string x; while (p < sql.size()) { if (sql[p] == '\'') { if (p + 1 < sql.size() && sql[p + 1] == '\'') { x += '\''; p += 2; continue; } ++p; break; } x += sql[p++]; } out.push_back({std::move(x), true}); continue; } if (sql[p] == '`') { ++p; std::string x; while (p < sql.size() && sql[p] != '`') x += sql[p++]; if (p < sql.size()) ++p; out.push_back({std::move(x), false}); continue; } size_t b = p; while (p < sql.size() && !std::isspace(static_cast<unsigned char>(sql[p])) && std::string("(),;*=<>[]'`").find(sql[p]) == std::string::npos) ++p; if (b == p) throw Error("unsupported_sql", "unexpected token"); out.push_back({sql.substr(b, p - b), false}); } while (!out.empty() && out.back().text == ";") out.pop_back(); return out;
}
static bool ieq(const std::string &a, const std::string &b) { return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower((unsigned char)x) == std::tolower((unsigned char)y); }); }
static void expect(std::vector<Token> &t, size_t &p, const std::string &x) { if (p >= t.size() || !ieq(t[p++].text, x)) throw Error("unsupported_sql", "expected " + x); }
static std::string take_name(std::vector<Token> &t, size_t &p) { if (p >= t.size() || t[p].quoted || t[p].text.empty()) throw Error("unsupported_sql", "expected name"); return t[p++].text; }
static int64_t take_int(std::vector<Token> &t, size_t &p) { if (p >= t.size()) throw Error("invalid_value", "expected integer"); try { size_t n = 0; int64_t x = std::stoll(t[p].text, &n); if (n != t[p].text.size()) throw std::exception(); ++p; return x; } catch (...) { throw Error("invalid_value", "invalid integer"); } }

enum VKind { VK_INT, VK_STRING, VK_VECTOR };
struct Value { VKind kind = VK_STRING; int64_t i = 0; std::string s; std::vector<float> v; };
using Row = std::vector<Value>;
struct Column { std::string name; VKind kind = VK_STRING; int width = 0; };
struct VectorBlock {
  size_t begin = 0;
  size_t end = 0;
  std::vector<float> lo;
  std::vector<float> hi;
  std::vector<float> center;
};
struct Table {
  std::string name;
  std::vector<Column> cols;
  int primary = -1;
  int vector_col = -1;
  bool index = false;
  std::vector<Row> rows;
  std::vector<bool> alive;
  std::map<int64_t, size_t> keys;
  mutable bool blocks_valid = false;
  mutable std::vector<int> block_dims;
  mutable std::vector<VectorBlock> blocks;
  mutable std::vector<std::unordered_map<std::string, std::vector<size_t>>> equality_index;
  mutable std::vector<uint8_t> equality_index_valid;
  mutable std::vector<uint8_t> equality_index_disabled;
};
struct State { std::map<std::string, Table> tables; };
static std::string lower(std::string x) { for (char &c : x) c = char(std::tolower((unsigned char)c)); return x; }
static Table &table_of(State &s, const std::string &name) { auto it = s.tables.find(lower(name)); if (it == s.tables.end()) throw Error("unknown_table", "unknown table"); return it->second; }
static int column_of(const Table &t, const std::string &name) { std::string n = lower(name); for (size_t i = 0; i < t.cols.size(); ++i) if (lower(t.cols[i].name) == n) return int(i); throw Error("unknown_column", "unknown column"); }
static std::vector<float> parse_vector(const std::string &x, int dim) {
  const size_t begin = x.find('['), close = x.rfind(']');
  if (begin == std::string::npos || close <= begin)
    throw Error("invalid_vector", "expected vector array");

  std::vector<float> out;
  out.reserve(dim > 0 ? size_t(dim) : 0);
  const char *p = x.data() + begin + 1;
  const char *end = x.data() + close;
  while (p < end) {
    if (*p == ',') {
      // Match the old parser's treatment of an empty comma-separated item.
      ++p;
      continue;
    }
    char *next = nullptr;
    const double value = std::strtod(p, &next);
    if (next == p || !std::isfinite(value) ||
        value > std::numeric_limits<float>::max() ||
        value < -std::numeric_limits<float>::max())
      throw Error("invalid_vector", "expected finite float32 array");
    out.push_back(float(value));
    p = next;
    while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
    if (p < end && *p != ',')
      throw Error("invalid_vector", "expected finite float32 array");
    if (p < end) ++p;
  }
  if (int(out.size()) != dim)
    throw Error("dimension_mismatch", "wrong vector dimension");
  return out;
}
static size_t utf8_length(const std::string &s)
{
  size_t count = 0;

  for (size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t width = 0;

    if (c < 0x80) {
      width = 1;
    } else if ((c & 0xe0) == 0xc0) {
      width = 2;
    } else if ((c & 0xf0) == 0xe0) {
      width = 3;
    } else if ((c & 0xf8) == 0xf0) {
      width = 4;
    } else {
      throw Error("invalid_value", "invalid UTF-8 string");
    }

    if (i + width > s.size()) {
      throw Error("invalid_value", "invalid UTF-8 string");
    }

    for (size_t j = 1; j < width; ++j) {
      unsigned char continuation =
          static_cast<unsigned char>(s[i + j]);
      if ((continuation & 0xc0) != 0x80) {
        throw Error("invalid_value", "invalid UTF-8 string");
      }
    }

    i += width;
    ++count;
  }

  return count;
}

static Value convert_value(const Token &tok, const Column &c) { if (c.kind == VK_INT) { if (tok.quoted) throw Error("invalid_value", "BIGINT requires an integer"); Value v; v.kind = VK_INT; v.i = [&] { size_t n = 0; try { int64_t x = std::stoll(tok.text, &n); if (n != tok.text.size()) throw std::exception(); return x; } catch (...) { throw Error("invalid_value", "invalid integer"); } }(); return v; } if (c.kind == VK_VECTOR) { Value v; v.kind = VK_VECTOR; v.v = parse_vector(tok.text, c.width); return v; } if (!tok.quoted || utf8_length(tok.text) > size_t(c.width)) throw Error("invalid_value", "invalid VARCHAR"); Value v; v.kind = VK_STRING; v.s = tok.text; return v; }
static bool value_equal(const Value &a, const Value &b) { if (a.kind != b.kind) return false; if (a.kind == VK_INT) return a.i == b.i; if (a.kind == VK_STRING) return a.s == b.s; return a.v == b.v; }
static Json to_json(const Value &v) { if (v.kind == VK_INT) return Json::integer(v.i); if (v.kind == VK_STRING) return Json::string(v.s); std::vector<Json> a; for (float x : v.v) a.push_back(Json::number(x)); return Json::array(std::move(a)); }

struct Term { int col = -1; std::string op; Value value; };
struct Projection { int col = -1; bool distance = false; std::string col_name; std::string alias; std::string target_text; std::vector<float> target; };
struct Order { int col = -1; int projection = -1; bool distance = false; bool desc = false; };
struct Engine {
  State st; std::optional<State> tx; std::vector<std::string> tx_log; std::string dir; int journal_fd = -1;
  explicit Engine(std::string d) : dir(std::move(d)) { mkdir(dir.c_str(), 0755); load(); }
  ~Engine() { if (journal_fd >= 0) ::close(journal_fd); }
  std::string file() const { return dir + "/state.bin"; }
  std::string journal() const { return dir + "/journal.log"; }
  template<class T> static void put(std::ofstream &f, const T &x) { f.write(reinterpret_cast<const char *>(&x), sizeof(x)); }
  template<class T> static bool get(std::ifstream &f, T &x) { return bool(f.read(reinterpret_cast<char *>(&x), sizeof(x))); }
  static void putstr(std::ofstream &f, const std::string &s) { uint32_t n = uint32_t(s.size()); put(f, n); f.write(s.data(), n); }
  static std::string getstr(std::ifstream &f) { uint32_t n; if (!get(f, n) || n > 1u << 24) throw std::runtime_error("bad state"); std::string s(n, '\0'); if (!f.read(s.data(), n)) throw std::runtime_error("bad state"); return s; }
  static constexpr uint32_t JOURNAL_MAGIC = 0x4d564a31;
  static void append_u32(std::string &out, uint32_t value) { out.append(reinterpret_cast<const char *>(&value), sizeof(value)); }
  static bool read_u32(const std::string &buf, size_t &pos, uint32_t &value) {
    if (pos + sizeof(value) > buf.size()) return false;
    std::memcpy(&value, buf.data() + pos, sizeof(value)); pos += sizeof(value); return true;
  }
  static void write_all(int fd, const char *data, size_t size) {
    while (size) {
      ssize_t n = ::write(fd, data, size);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) throw Error("storage_error", "cannot append journal");
      data += n;
      size -= size_t(n);
    }
  }
  int ensure_journal_fd() {
    if (journal_fd >= 0) return journal_fd;
    journal_fd = ::open(journal().c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (journal_fd < 0) throw Error("storage_error", "cannot open journal");
    return journal_fd;
  }
  void append_journal(const std::vector<std::string> &statements) {
    if (statements.empty()) return;
    std::string payload; append_u32(payload, uint32_t(statements.size()));
    for (const auto &sql : statements) { if (sql.size() > (1u << 28)) throw Error("storage_error", "statement too large"); append_u32(payload, uint32_t(sql.size())); payload.append(sql); }
    uint32_t magic = JOURNAL_MAGIC, bytes = uint32_t(payload.size());
    int fd = ensure_journal_fd();
    try {
      write_all(fd, reinterpret_cast<const char *>(&magic), sizeof(magic));
      write_all(fd, reinterpret_cast<const char *>(&bytes), sizeof(bytes));
      write_all(fd, payload.data(), payload.size());
#if defined(__linux__)
      if (::fdatasync(fd) != 0) throw Error("storage_error", "cannot sync journal");
#else
      if (::fsync(fd) != 0) throw Error("storage_error", "cannot sync journal");
#endif
    } catch (...) {
      ::close(fd);
      journal_fd = -1;
      throw;
    }
  }
  void replay_sql(const std::string &sql) {
    auto tok = lex_sql(sql); size_t p = 0; if (tok.empty()) return; std::string cmd = lower(tok[p++].text);
    if (cmd == "create") { if (p < tok.size() && ieq(tok[p].text, "vector")) create_index(tok, p); else create_table(tok, p); }
    else if (cmd == "insert") { insert(tok, p); }
    else if (cmd == "update") { int64_t affected = 0; update(tok, p, affected); }
    else if (cmd == "delete") { int64_t affected = 0; erase(tok, p, affected); }
    else throw Error("storage_error", "invalid journal statement");
    if (p != tok.size()) throw Error("storage_error", "trailing journal SQL");
  }
  void replay_journal() {
    std::ifstream f(journal(), std::ios::binary); if (!f) return;
    f.seekg(0, std::ios::end); std::streamoff end_offset = f.tellg();
    if (end_offset < 0) return;
    size_t file_size = size_t(end_offset), pos = 0, valid_end = 0;
    bool torn_tail = false;
    std::string payload;
    std::vector<std::string> statements;
    f.seekg(0, std::ios::beg);
    while (pos < file_size) {
      if (file_size - pos < sizeof(uint32_t) * 2) {
        // A partial frame header is a torn tail: the writer died after writing
        // 1..7 bytes of magic/length, so nothing after it is a record.
        torn_tail = true;
        break;
      }
      uint32_t magic = 0, bytes = 0;
      if (!f.read(reinterpret_cast<char *>(&magic), sizeof(magic)) ||
          !f.read(reinterpret_cast<char *>(&bytes), sizeof(bytes))) {
        torn_tail = true;
        break;
      }
      if (magic != JOURNAL_MAGIC) throw Error("storage_error", "invalid journal magic");
      if (bytes > (1u << 30)) throw Error("storage_error", "invalid journal frame");
      const size_t frame_end = pos + sizeof(uint32_t) * 2 + size_t(bytes);
      if (frame_end > file_size) {
        // The declared payload runs past EOF: the writer died mid-frame.
        torn_tail = true;
        break;
      }
      payload.resize(bytes);
      if (bytes && !f.read(&payload[0], std::streamsize(bytes))) {
        torn_tail = true;
        break;
      }
      // The frame is fully present.  Any inconsistency inside it is corruption,
      // not a torn tail, so it must fail loudly instead of being truncated.
      size_t q = 0;
      uint32_t count = 0;
      auto take_u32 = [&](uint32_t &value) {
        if (q + sizeof(value) > payload.size()) return false;
        std::memcpy(&value, payload.data() + q, sizeof(value));
        q += sizeof(value);
        return true;
      };
      if (!take_u32(count) || count > 1000000) throw Error("storage_error", "invalid journal frame");
      if (count > statements.capacity()) statements.reserve(count);
      statements.clear();
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t n = 0;
        if (!take_u32(n) || n > (1u << 28) || q + n > payload.size())
          throw Error("storage_error", "invalid journal frame");
        statements.emplace_back(payload.data() + q, n);
        q += n;
      }
      if (q != payload.size()) throw Error("storage_error", "invalid journal frame");
      for (const auto &statement : statements) replay_sql(statement);
      pos = frame_end;
      valid_end = pos;
    }
    if (torn_tail && valid_end < file_size) {
      int fd = ::open(journal().c_str(), O_WRONLY);
      if (fd >= 0) {
        if (::ftruncate(fd, off_t(valid_end)) != 0) {
          ::close(fd);
          throw Error("storage_error", "cannot truncate torn journal");
        }
        if (::fsync(fd) != 0) {
          ::close(fd);
          throw Error("storage_error", "cannot sync truncated journal");
        }
        ::close(fd);
      } else {
        throw Error("storage_error", "cannot open journal for truncation");
      }
    }
  }
  void save() {
    std::string tmp = file() + ".tmp"; std::ofstream f(tmp, std::ios::binary | std::ios::trunc); if (!f) throw Error("storage_error", "cannot open data directory"); uint32_t magic = 0x4d564332, nt = uint32_t(st.tables.size()); put(f, magic); put(f, nt);
    for (auto &kv : st.tables) { const Table &t = kv.second; putstr(f, t.name); uint32_t nc = uint32_t(t.cols.size()); put(f, nc); put(f, t.primary); put(f, t.vector_col); uint8_t ix = t.index; put(f, ix); for (auto &c : t.cols) { putstr(f, c.name); uint8_t k = uint8_t(c.kind); put(f, k); put(f, c.width); } uint64_t nr = t.rows.size(); put(f, nr); for (size_t r = 0; r < t.rows.size(); ++r) { uint8_t live = t.alive[r]; put(f, live); for (auto &v : t.rows[r]) { uint8_t k = uint8_t(v.kind); put(f, k); if (v.kind == VK_INT) put(f, v.i); else if (v.kind == VK_STRING) putstr(f, v.s); else { uint32_t n = uint32_t(v.v.size()); put(f, n); f.write(reinterpret_cast<const char *>(v.v.data()), sizeof(float) * n); } } } }
    f.flush(); f.close(); int fd = ::open(tmp.c_str(), O_RDONLY); if (fd >= 0) { ::fsync(fd); ::close(fd); } if (::rename(tmp.c_str(), file().c_str()) != 0) throw Error("storage_error", "cannot publish data");
  }
  void load() {
    std::ifstream f(file(), std::ios::binary); if (!f) { replay_journal(); return; } uint32_t magic = 0, nt = 0; if (!get(f, magic) || magic != 0x4d564332 || !get(f, nt)) throw Error("storage_error", "invalid data"); for (uint32_t z = 0; z < nt; ++z) { Table t; t.name = getstr(f); uint32_t nc; if (!get(f, nc) || nc > 1024 || !get(f, t.primary) || !get(f, t.vector_col)) throw Error("storage_error", "invalid schema"); uint8_t ix; if (!get(f, ix)) throw Error("storage_error", "invalid schema"); t.index = ix; for (uint32_t c = 0; c < nc; ++c) { Column col; col.name = getstr(f); uint8_t k; if (!get(f, k) || !get(f, col.width)) throw Error("storage_error", "invalid column"); col.kind = VKind(k); t.cols.push_back(std::move(col)); } uint64_t nr; if (!get(f, nr) || nr > 100000000) throw Error("storage_error", "invalid rows"); for (uint64_t r = 0; r < nr; ++r) { uint8_t live; if (!get(f, live)) throw Error("storage_error", "invalid row"); t.alive.push_back(live != 0); std::vector<Value> row; for (uint32_t c = 0; c < nc; ++c) { Value v; uint8_t k; if (!get(f, k)) throw Error("storage_error", "invalid value"); v.kind = VKind(k); if (v.kind == VK_INT) { if (!get(f, v.i)) throw Error("storage_error", "invalid value"); } else if (v.kind == VK_STRING) v.s = getstr(f); else { uint32_t n; if (!get(f, n) || n > 100000) throw Error("storage_error", "invalid vector"); v.v.resize(n); if (!f.read(reinterpret_cast<char *>(v.v.data()), sizeof(float) * n)) throw Error("storage_error", "invalid vector"); } row.push_back(std::move(v)); } t.rows.push_back(std::move(row)); } rebuild(t); st.tables.emplace(lower(t.name), std::move(t)); }
    replay_journal();
  }
  static void rebuild(Table &t) {
    t.keys.clear();
    for (size_t r = 0; r < t.rows.size(); ++r)
      if (t.alive[r]) t.keys.emplace(t.rows[r][t.primary].i, r);
    t.blocks_valid = false;
    t.block_dims.clear();
    t.blocks.clear();
    t.equality_index.clear();
    t.equality_index_valid.clear();
    t.equality_index_disabled.clear();
  }
  static void invalidate_blocks(Table &t) {
    t.blocks_valid = false;
    t.block_dims.clear();
    t.blocks.clear();
    t.equality_index_valid.clear();
    t.equality_index_disabled.clear();
    for (auto &index : t.equality_index) index.clear();
  }
  static std::string equality_key(const Value &value) {
    if (value.kind == VK_INT) return "I:" + std::to_string(value.i);
    if (value.kind == VK_STRING) return "S:" + value.s;
    return {};
  }
  static void ensure_equality_index(Table &t, int col) {
    if (col < 0 || col >= int(t.cols.size()) || t.cols[col].kind == VK_VECTOR) return;
    const size_t ncols = t.cols.size();
    if (t.equality_index.size() != ncols) t.equality_index.resize(ncols);
    if (t.equality_index_valid.size() != ncols) t.equality_index_valid.assign(ncols, 0);
    if (t.equality_index_disabled.size() != ncols) t.equality_index_disabled.assign(ncols, 0);
    if (t.equality_index_valid[size_t(col)] || t.equality_index_disabled[size_t(col)]) return;

    // Do not turn high-cardinality document columns into a memory sink.  The
    // index is intended for bounded attributes such as category/status; when
    // the cap is exceeded the query falls back to the exact scan.
    constexpr size_t MAX_DISTINCT_VALUES = 65536;
    auto &index = t.equality_index[size_t(col)];
    index.reserve(std::min(MAX_DISTINCT_VALUES, t.rows.size()));
    for (size_t r = 0; r < t.rows.size(); ++r) {
      if (!t.alive[r]) continue;
      std::string key = equality_key(t.rows[r][col]);
      auto it = index.find(key);
      if (it == index.end()) {
        if (index.size() >= MAX_DISTINCT_VALUES) {
          index.clear();
          t.equality_index_disabled[size_t(col)] = 1;
          return;
        }
        it = index.emplace(std::move(key), std::vector<size_t>{}).first;
      }
      it->second.push_back(r);
    }
    t.equality_index_valid[size_t(col)] = 1;
  }
  static void ensure_blocks(Table &t) {
    if (t.blocks_valid) return;
    t.blocks_valid = true;
    t.block_dims.clear();
    t.blocks.clear();
    if (t.vector_col < 0 || t.rows.empty()) return;
    const int dim = t.cols[t.vector_col].width;
    if (dim <= 0) return;

    // Keep a compact, evenly spaced sample of dimensions.  Bounds over a
    // subset of dimensions are still valid lower bounds for L2 distance, and
    // cap metadata for unusually wide vectors.
    const int samples = std::min(dim, 64);
    t.block_dims.reserve(samples);
    for (int i = 0; i < samples; ++i) {
      int d = int((int64_t(i) * dim) / samples);
      if (t.block_dims.empty() || t.block_dims.back() != d)
        t.block_dims.push_back(d);
    }
    constexpr size_t BLOCK_ROWS = 256;
    for (size_t begin = 0; begin < t.rows.size(); begin += BLOCK_ROWS) {
      VectorBlock block;
      block.begin = begin;
      block.end = std::min(t.rows.size(), begin + BLOCK_ROWS);
      block.lo.assign(t.block_dims.size(), std::numeric_limits<float>::infinity());
      block.hi.assign(t.block_dims.size(), -std::numeric_limits<float>::infinity());
      block.center.assign(t.block_dims.size(), 0.0f);
      for (size_t r = block.begin; r < block.end; ++r) {
        const Value &v = t.rows[r][t.vector_col];
        if (v.kind != VK_VECTOR || int(v.v.size()) != dim)
          throw Error("invalid_value", "distance requires a vector");
        for (size_t j = 0; j < t.block_dims.size(); ++j) {
          float x = v.v[size_t(t.block_dims[j])];
          block.lo[j] = std::min(block.lo[j], x);
          block.hi[j] = std::max(block.hi[j], x);
          block.center[j] += x;
        }
      }
      const float count = float(block.end - block.begin);
      for (float &x : block.center) x /= count;
      t.blocks.push_back(std::move(block));
    }
  }
  State &working() { return st; }
  const State &view() const { return st; }
  void begin() { if (tx) throw Error("transaction_active", "transaction already active"); tx = st; tx_log.clear(); }
  void rollback() { if (tx) { st = *tx; tx.reset(); tx_log.clear(); } }
  void commit() {
    if (!tx) return;
    try { append_journal(tx_log); }
    catch (...) { st = State{}; tx.reset(); tx_log.clear(); load(); throw; }
    tx.reset(); tx_log.clear();
  }
  void autocommit(bool changed, const std::string &sql) {
    if (!changed) return;
    if (tx) { tx_log.push_back(sql); return; }
    try { append_journal({sql}); }
    catch (...) { st = State{}; tx.reset(); tx_log.clear(); load(); throw; }
  }

  std::vector<Term> parse_where(std::vector<Token> &t, size_t &p, const Table &table) {
    std::vector<Term> out; if (p >= t.size() || !ieq(t[p].text, "where")) return out; ++p; while (true) { int c = column_of(table, take_name(t, p)); if (p >= t.size()) throw Error("unsupported_sql", "expected comparison"); std::string op = t[p++].text; if (op != "=" && op != "<" && op != "<=" && op != ">" && op != ">=") throw Error("unsupported_sql", "unsupported comparison"); if (p >= t.size()) throw Error("invalid_value", "missing value"); out.push_back({c, op, convert_value(t[p++], table.cols[c])}); if (p >= t.size() || !ieq(t[p].text, "and")) break; ++p; } return out;
  }
  bool matches(const Row &row, const std::vector<Term> &terms) const {
    for (auto &x : terms) { const Value &a = row[x.col], &b = x.value; bool ok = false; if (a.kind == VK_INT && b.kind == VK_INT) { if (x.op == "=") ok = a.i == b.i; else if (x.op == "<") ok = a.i < b.i; else if (x.op == "<=") ok = a.i <= b.i; else if (x.op == ">") ok = a.i > b.i; else ok = a.i >= b.i; } else if (a.kind == VK_STRING && b.kind == VK_STRING) { if (x.op == "=") ok = a.s == b.s; else if (x.op == "<") ok = a.s < b.s; else if (x.op == "<=") ok = a.s <= b.s; else if (x.op == ">") ok = a.s > b.s; else ok = a.s >= b.s; } else throw Error("invalid_value", "type mismatch"); if (!ok) return false; } return true;
  }

  void create_table(std::vector<Token> &t, size_t &p) {
    expect(t, p, "table"); std::string name = lower(take_name(t, p)); if (st.tables.count(name)) throw Error("table_exists", "table already exists"); expect(t, p, "("); Table tab; tab.name = name; std::vector<std::string> pks; while (true) { if (p < t.size() && ieq(t[p].text, "primary")) { ++p; expect(t, p, "key"); expect(t, p, "("); pks.push_back(take_name(t, p)); expect(t, p, ")"); } else { Column c; c.name = take_name(t, p); std::string k = lower(take_name(t, p)); if (k == "bigint") c.kind = VK_INT; else if (k == "varchar") c.kind = VK_STRING; else if (k == "vector") c.kind = VK_VECTOR; else throw Error("unsupported_sql", "unsupported column type"); if (c.kind != VK_INT) { expect(t, p, "("); c.width = int(take_int(t, p)); if (c.width <= 0) throw Error("invalid_schema", "nonpositive column width"); expect(t, p, ")"); } if (p < t.size() && ieq(t[p].text, "primary")) { ++p; expect(t, p, "key"); pks.push_back(c.name); } tab.cols.push_back(std::move(c)); } if (p >= t.size() || t[p].text != ",") break; ++p; } expect(t, p, ")"); if (pks.size() != 1) throw Error("invalid_schema", "one primary key is required"); tab.primary = column_of(tab, pks[0]); if (tab.cols[tab.primary].kind != VK_INT) throw Error("invalid_schema", "primary key must be BIGINT"); for (size_t i = 0; i < tab.cols.size(); ++i) if (tab.cols[i].kind == VK_VECTOR) { if (tab.vector_col >= 0) throw Error("invalid_schema", "at most one vector column"); tab.vector_col = int(i); } st.tables.emplace(name, std::move(tab));
  }
  void create_index(std::vector<Token> &t, size_t &p) { expect(t, p, "vector"); expect(t, p, "index"); std::string ignored = take_name(t, p); expect(t, p, "on"); Table &tab = table_of(st, take_name(t, p)); expect(t, p, "("); int c = column_of(tab, take_name(t, p)); expect(t, p, ")"); if (tab.cols[c].kind != VK_VECTOR) throw Error("invalid_schema", "index requires a vector"); if (tab.index) throw Error("index_exists", "vector index already exists"); tab.index = true; }
  int64_t insert(std::vector<Token> &t, size_t &p) { expect(t, p, "into"); Table &tab = table_of(st, take_name(t, p)); std::vector<int> order; if (p < t.size() && t[p].text == "(") { ++p; while (true) { order.push_back(column_of(tab, take_name(t, p))); if (p >= t.size() || t[p].text != ",") break; ++p; } expect(t, p, ")"); } else for (size_t i = 0; i < tab.cols.size(); ++i) order.push_back(int(i)); if (order.size() != tab.cols.size()) throw Error("invalid_value", "all columns must appear"); expect(t, p, "values"); std::vector<Row> pending; while (true) { expect(t, p, "("); Row row(tab.cols.size()); size_t k = 0; while (true) { if (k >= order.size() || p >= t.size()) throw Error("invalid_value", "wrong value count"); int col = order[k++]; row[col] = convert_value(t[p++], tab.cols[col]); if (p >= t.size() || t[p].text != ",") break; ++p; } expect(t, p, ")"); if (k != order.size()) throw Error("invalid_value", "wrong value count"); pending.push_back(std::move(row)); if (p >= t.size() || t[p].text != ",") break; ++p; } std::map<int64_t, bool> seen; for (auto &row : pending) { int64_t key = row[tab.primary].i; if (seen.count(key) || tab.keys.count(key)) throw Error("duplicate_key", "duplicate primary key"); seen[key] = true; } for (auto &row : pending) { size_t pos = tab.rows.size(); tab.keys[row[tab.primary].i] = pos; tab.rows.push_back(std::move(row)); tab.alive.push_back(true); } invalidate_blocks(tab); return int64_t(pending.size()); }
  void update(std::vector<Token> &t, size_t &p, int64_t &affected) { Table &tab = table_of(st, take_name(t, p)); expect(t, p, "set"); std::vector<std::pair<int, Value>> assigns; while (true) { int c = column_of(tab, take_name(t, p)); expect(t, p, "="); if (p >= t.size()) throw Error("invalid_value", "missing value"); Value value = convert_value(t[p++], tab.cols[c]);

    if (c == tab.primary) {
      throw Error(
          "unsupported_sql",
          "updating primary key is unsupported");
    }

    assigns.emplace_back(c, std::move(value)); if (p >= t.size() || t[p].text != ",") break; ++p; } auto terms = parse_where(t, p, tab); for (size_t r = 0; r < tab.rows.size(); ++r) if (tab.alive[r] && matches(tab.rows[r], terms)) { std::vector<Value> next = tab.rows[r]; for (auto &a : assigns) next[a.first] = a.second; bool changed = false; for (size_t i = 0; i < next.size(); ++i) changed |= !value_equal(next[i], tab.rows[r][i]); if (changed) { tab.rows[r] = std::move(next); ++affected; } } rebuild(tab); }
  void erase(std::vector<Token> &t, size_t &p, int64_t &affected) { expect(t, p, "from"); Table &tab = table_of(st, take_name(t, p)); auto terms = parse_where(t, p, tab); for (size_t r = 0; r < tab.rows.size(); ++r) if (tab.alive[r] && matches(tab.rows[r], terms)) { tab.alive[r] = false; ++affected; } rebuild(tab); }

  Json select(std::vector<Token> &t, size_t &p, bool ann_mode = false) {
    std::vector<Projection> proj;
    bool star = p < t.size() && t[p].text == "*";
    if (star) {
      ++p;
    } else {
      while (true) {
        Projection q;
        if (p + 1 < t.size() && ieq(t[p].text, "l2_distance") && t[p + 1].text == "(") {
          q.distance = true; p += 2;
          q.col_name = take_name(t, p); expect(t, p, ",");
          if (p >= t.size() || !t[p].quoted) throw Error("invalid_vector", "distance target must be a vector literal");
          q.target_text = t[p++].text; // dimension is checked after FROM
          expect(t, p, ")");
          if (p < t.size() && ieq(t[p].text, "as")) ++p;
          if (p < t.size() && t[p].text != "," && !ieq(t[p].text, "from") && !ieq(t[p].text, "where") && !ieq(t[p].text, "order") && !ieq(t[p].text, "limit")) q.alias = take_name(t, p);
          if (q.alias.empty()) q.alias = "distance";
        } else {
          q.col_name = take_name(t, p); q.alias = q.col_name;
          if (p < t.size() && ieq(t[p].text, "as")) { ++p; q.alias = take_name(t, p); }
        }
        proj.push_back(std::move(q));
        if (p >= t.size() || t[p].text != ",") break;
        ++p;
      }
    }
    expect(t, p, "from");
    Table &tab = table_of(st, take_name(t, p));
    if (star) for (size_t i = 0; i < tab.cols.size(); ++i) proj.push_back({int(i), false, tab.cols[i].name, tab.cols[i].name, {}, {}});
    for (auto &q : proj) {
      q.col = column_of(tab, q.col_name);
      if (q.distance) {
        if (tab.cols[q.col].kind != VK_VECTOR) throw Error("invalid_value", "distance requires a vector column");
          q.target = parse_vector(q.target_text, tab.cols[q.col].width);
      }
    }
    auto terms = parse_where(t, p, tab);
    std::vector<Order> ord;
    if (p < t.size() && ieq(t[p].text, "order")) {
      ++p; expect(t, p, "by");
      while (true) {
        std::string x = t[p++].text; Order o; o.col = -1;
        for (size_t qi = 0; qi < proj.size(); ++qi) {
          auto &q = proj[qi];
          if (ieq(q.alias, x) || (q.distance && ieq(x, "distance"))) {
            o.col = q.col; o.projection = int(qi); o.distance = q.distance;
          }
        }
        if (o.col < 0) { o.col = column_of(tab, x); o.projection = -1; o.distance = false; }
        if (p < t.size() && (ieq(t[p].text, "desc") || ieq(t[p].text, "asc"))) { o.desc = ieq(t[p].text, "desc"); ++p; }
        ord.push_back(o); if (p >= t.size() || t[p].text != ",") break; ++p;
      }
    }
    int64_t limit = std::numeric_limits<int64_t>::max();
    if (p < t.size() && ieq(t[p].text, "limit")) { ++p; limit = take_int(t, p); if (limit < 0) throw Error("invalid_value", "negative limit"); }
    bool block_topk = !ord.empty() && ord[0].distance && !ord[0].desc &&
                      ord[0].projection >= 0 && ord[0].col == tab.vector_col &&
                      limit > 0 && limit < int64_t(tab.rows.size());
    auto distance_squared = [&](const Value &v, const std::vector<float> &q) { if (v.kind != VK_VECTOR || v.v.size() != q.size()) throw Error("invalid_value", "distance requires a vector"); double sum = 0; for (size_t i = 0; i < q.size(); ++i) { double d = double(v.v[i]) - double(q[i]); sum += d * d; } return sum; };
    std::vector<size_t> empty_seed;
    const std::vector<size_t> *seed_rows = nullptr;
    size_t seed_size = std::numeric_limits<size_t>::max();
    for (const auto &term : terms) {
      if (term.op != "=" || tab.cols[term.col].kind == VK_VECTOR) continue;
      ensure_equality_index(tab, term.col);
      if (term.col >= int(tab.equality_index_valid.size()) ||
          !tab.equality_index_valid[size_t(term.col)]) continue;
      const auto &index = tab.equality_index[size_t(term.col)];
      auto it = index.find(equality_key(term.value));
      if (it == index.end()) {
        seed_rows = &empty_seed;
        seed_size = 0;
        break;
      }
      if (it->second.size() < seed_size) {
        seed_rows = &it->second;
        seed_size = it->second.size();
      }
    }
    std::vector<size_t> rows;
    if (!block_topk) {
      if (seed_rows) {
        rows.reserve(seed_rows->size());
        for (size_t r : *seed_rows)
          if (tab.alive[r] && matches(tab.rows[r], terms)) rows.push_back(r);
      } else {
        for (size_t r = 0; r < tab.rows.size(); ++r)
          if (tab.alive[r] && matches(tab.rows[r], terms)) rows.push_back(r);
      }
    }
    // Cache each distance once per query. The old implementation recomputed the
    // same distance for every comparison in an O(n log n) sort.
    std::vector<std::vector<double>> distance_cache(proj.size());
    std::vector<std::vector<uint8_t>> distance_ready(proj.size());
    for (size_t qi = 0; qi < proj.size(); ++qi) {
      if (proj[qi].distance) {
        distance_cache[qi].assign(tab.rows.size(), 0.0);
        distance_ready[qi].assign(tab.rows.size(), 0);
      }
    }
    auto distance_for = [&](size_t r, size_t qi) {
      if (!distance_ready[qi][r]) {
        const Projection &q = proj[qi];
        distance_cache[qi][r] = distance_squared(tab.rows[r][q.col], q.target);
        distance_ready[qi][r] = 1;
      }
      return distance_cache[qi][r];
    };
    auto less_row = [&](size_t a, size_t b) {
      for (auto &o : ord) {
        if (o.distance) {
          double da = distance_for(a, size_t(o.projection)), db = distance_for(b, size_t(o.projection));
          if (da != db) return o.desc ? da > db : da < db;
        } else if (!value_equal(tab.rows[a][o.col], tab.rows[b][o.col])) {
          const Value &x = tab.rows[a][o.col], &y = tab.rows[b][o.col];
          if (x.kind == VK_INT) return o.desc ? x.i > y.i : x.i < y.i;
          return o.desc ? x.s > y.s : x.s < y.s;
        }
      }
      return tab.rows[a][tab.primary].i < tab.rows[b][tab.primary].i;
    };
    // For ascending vector Top-K, visit blocks in lower-bound order and keep
    // only the best K rows.  The bound is computed from a subset of vector
    // dimensions, so it can only underestimate the true distance.  A block is
    // skipped strictly above the current worst candidate; equal bounds remain
    // eligible for deterministic primary-key tie breaking.
    const bool use_ann = ann_mode && tab.index && block_topk &&
                         tab.rows.size() >= 8192 && seed_rows == nullptr;
    if (block_topk) {
      ensure_blocks(tab);
      const size_t qi = size_t(ord[0].projection);
      std::vector<double> block_bounds(tab.blocks.size(), 0.0);
      std::vector<double> block_scores(tab.blocks.size(), 0.0);
      for (size_t bi = 0; bi < tab.blocks.size(); ++bi) {
        const auto &block = tab.blocks[bi];
        const auto &q = proj[qi];
        double bound = 0.0;
        for (size_t j = 0; j < tab.block_dims.size(); ++j) {
          double x = double(q.target[size_t(tab.block_dims[j])]);
          double d = x < double(block.lo[j]) ? double(block.lo[j]) - x
                    : x > double(block.hi[j]) ? x - double(block.hi[j]) : 0.0;
          bound += d * d;
        }
        block_bounds[bi] = bound;
        if (use_ann) {
          double score = 0.0;
          for (size_t j = 0; j < tab.block_dims.size(); ++j) {
            double d = double(block.center[j]) - double(q.target[size_t(tab.block_dims[j])]);
            score += d * d;
          }
          block_scores[bi] = score;
        } else {
          block_scores[bi] = bound;
        }
      }
      std::vector<size_t> block_order(tab.blocks.size());
      std::iota(block_order.begin(), block_order.end(), 0);
      std::sort(block_order.begin(), block_order.end(), [&](size_t a, size_t b) {
        return block_scores[a] < block_scores[b];
      });
      using Heap = std::priority_queue<size_t, std::vector<size_t>, decltype(less_row)>;
      Heap best(less_row);
      const size_t wanted = size_t(limit);
      auto consider = [&](size_t r) {
        if (!tab.alive[r] || !matches(tab.rows[r], terms)) return;
        if (best.size() < wanted) {
          best.push(r);
        } else if (less_row(r, best.top())) {
          best.pop();
          best.push(r);
        }
      };
      const size_t probe_blocks = use_ann
        ? std::min(tab.blocks.size(), std::max<size_t>(38, (tab.blocks.size() + 3) / 4))
        : tab.blocks.size();
      if (seed_rows) {
        for (size_t r : *seed_rows) consider(r);
      } else {
        for (size_t index = 0; index < probe_blocks; ++index) {
          size_t bi = block_order[index];
          if (!use_ann && best.size() == wanted && block_bounds[bi] > distance_for(best.top(), qi))
            continue;
          const auto &block = tab.blocks[bi];
          for (size_t r = block.begin; r < block.end; ++r) consider(r);
        }
        // A sparse filter or an unusually skewed block layout may leave too
        // few candidates in the ANN probe. Complete the query rather than
        // returning an undersized result set.
        if (use_ann && best.size() < wanted) {
          for (size_t index = probe_blocks; index < block_order.size(); ++index) {
            size_t bi = block_order[index];
            const auto &block = tab.blocks[bi];
            for (size_t r = block.begin; r < block.end; ++r) consider(r);
          }
        }
      }
      rows.clear();
      rows.reserve(best.size());
      while (!best.empty()) {
        rows.push_back(best.top());
        best.pop();
      }
      std::sort(rows.begin(), rows.end(), less_row);
    } else if (!ord.empty() && !rows.empty() && limit < int64_t(rows.size())) {
      if (limit > 0) {
        auto cut = rows.begin() + limit;
        std::nth_element(rows.begin(), cut, rows.end(), less_row);
        rows.resize(size_t(limit));
        std::sort(rows.begin(), rows.end(), less_row);
      } else {
        rows.clear();
      }
    } else if (!ord.empty()) {
      std::sort(rows.begin(), rows.end(), less_row);
      if (limit < int64_t(rows.size())) rows.resize(size_t(limit));
    }
    if (ord.empty() && limit < int64_t(rows.size())) rows.resize(limit > 0 ? size_t(limit) : 0);
    std::vector<Json> cols, out;
    for (auto &q : proj) cols.push_back(Json::string(q.alias.empty() ? tab.cols[q.col].name : q.alias));
    for (size_t r : rows) { std::vector<Json> vals; for (size_t qi = 0; qi < proj.size(); ++qi) { auto &q = proj[qi]; vals.push_back(q.distance ? Json::number(std::sqrt(distance_for(r, qi))) : to_json(tab.rows[r][q.col])); } out.push_back(Json::array(std::move(vals))); }
    return Json::object({{"columns", Json::array(std::move(cols))}, {"rows", Json::array(std::move(out))}});
  }
};

} // namespace mv

int main(int argc, char **argv) {
  std::string dir = "/data"; for (int i = 1; i + 1 < argc; ++i) if (std::string(argv[i]) == "--data-dir") dir = argv[i + 1];
  try {
    mv::Engine db(dir); std::cout << "{\"type\":\"ready\"}\n" << std::flush; std::string line;
    while (std::getline(std::cin, line)) {
      if (line.empty()) { continue; }
      mv::Json req; mv::Json id = mv::Json::nil(); try {
        req = mv::json_parse(line); if (req.kind != mv::Json::OBJECT) throw mv::Error("invalid_request", "request must be an object"); if (req.o.count("id")) id = req.o.at("id"); std::string op = req.o.count("op") && req.o.at("op").kind == mv::Json::STRING ? req.o.at("op").s : "";
        if (op == "shutdown") { db.rollback(); std::cout << mv::json_dump(mv::Json::object({{"id", id}, {"ok", mv::Json::boolean(true)}})) << '\n' << std::flush; break; }
        if (op != "execute" || !req.o.count("sql") || req.o.at("sql").kind != mv::Json::STRING) throw mv::Error("invalid_request", "execute requires sql");
        bool ann_mode = false;
        if (req.o.count("search_mode")) {
          const mv::Json &mode = req.o.at("search_mode");
          if (mode.kind != mv::Json::STRING || (mode.s != "exact" && mode.s != "ann"))
            throw mv::Error("invalid_request", "search_mode must be exact or ann");
          ann_mode = mode.s == "ann";
        }
        std::string sql = req.o.at("sql").s; auto tok = mv::lex_sql(sql); size_t p = 0; if (tok.empty()) throw mv::Error("unsupported_sql", "empty SQL"); std::string cmd = mv::lower(tok[p++].text); mv::Json body;
        if (cmd == "begin") { db.begin(); body = mv::Json::object({{"ok", mv::Json::boolean(true)}}); }
        else if (cmd == "commit") { db.commit(); body = mv::Json::object({{"ok", mv::Json::boolean(true)}}); }
        else if (cmd == "rollback") { db.rollback(); body = mv::Json::object({{"ok", mv::Json::boolean(true)}}); }
        else { int64_t affected = 0; if (cmd == "create") { if (p < tok.size() && mv::ieq(tok[p].text, "vector")) db.create_index(tok, p); else db.create_table(tok, p); affected = 0; } else if (cmd == "insert") affected = db.insert(tok, p); else if (cmd == "update") db.update(tok, p, affected); else if (cmd == "delete") db.erase(tok, p, affected); else if (cmd == "select") body = db.select(tok, p, ann_mode); else throw mv::Error("unsupported_sql", "unsupported statement"); if (cmd != "select") db.autocommit(true, sql); if (body.kind == mv::Json::NIL) body = mv::Json::object({{"ok", mv::Json::boolean(true)}}); else body.o["ok"] = mv::Json::boolean(true); if (cmd == "insert" || cmd == "update" || cmd == "delete") body.o["affected_rows"] = mv::Json::integer(affected); }
        body.o["id"] = id; std::cout << mv::json_dump(body) << '\n' << std::flush;
      } catch (const mv::Error &e) { std::cout << mv::json_dump(mv::Json::object({{"id", id}, {"ok", mv::Json::boolean(false)}, {"error", mv::Json::object({{"code", mv::Json::string(e.code)}, {"message", mv::Json::string(e.what())}})}})) << '\n' << std::flush; }
      catch (const std::exception &e) { std::cout << mv::json_dump(mv::Json::object({{"id", id}, {"ok", mv::Json::boolean(false)}, {"error", mv::Json::object({{"code", mv::Json::string("internal_error")}, {"message", mv::Json::string(e.what())}})}})) << '\n' << std::flush; }
    }
  } catch (const std::exception &e) { std::cerr << "startup failed: " << e.what() << '\n'; return 1; }
  return 0;
}














