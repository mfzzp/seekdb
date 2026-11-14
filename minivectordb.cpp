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

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

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
static double distance_scalar(const float *left, const float *right, size_t width) {
  double sum = 0;
  for (size_t i = 0; i < width; ++i) {
    const double d = double(left[i]) - double(right[i]);
    sum += d * d;
  }
  return sum;
}
#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2")))
static double distance_avx2(const float *left, const float *right, size_t width) {
  __m256d sum_lo = _mm256_setzero_pd();
  __m256d sum_hi = _mm256_setzero_pd();
  size_t i = 0;
  for (; i + 8 <= width; i += 8) {
    const __m256d left_lo = _mm256_cvtps_pd(_mm_loadu_ps(left + i));
    const __m256d right_lo = _mm256_cvtps_pd(_mm_loadu_ps(right + i));
    const __m256d left_hi = _mm256_cvtps_pd(_mm_loadu_ps(left + i + 4));
    const __m256d right_hi = _mm256_cvtps_pd(_mm_loadu_ps(right + i + 4));
    const __m256d diff_lo = _mm256_sub_pd(left_lo, right_lo);
    const __m256d diff_hi = _mm256_sub_pd(left_hi, right_hi);
    sum_lo = _mm256_add_pd(sum_lo, _mm256_mul_pd(diff_lo, diff_lo));
    sum_hi = _mm256_add_pd(sum_hi, _mm256_mul_pd(diff_hi, diff_hi));
  }
  sum_lo = _mm256_add_pd(sum_lo, sum_hi);
  alignas(32) double lanes[4];
  _mm256_store_pd(lanes, sum_lo);
  double sum = lanes[0] + lanes[1] + lanes[2] + lanes[3];
  for (; i < width; ++i) {
    const double d = double(left[i]) - double(right[i]);
    sum += d * d;
  }
  return sum;
}
#endif
static double distance_fast(const float *left, const float *right, size_t width) {
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
  static const bool avx2 = __builtin_cpu_supports("avx2");
  if (avx2 && width >= 32) return distance_avx2(left, right, width);
#endif
  return distance_scalar(left, right, width);
}
struct VectorBlock {
  size_t begin = 0;
  size_t end = 0;
  std::vector<float> lo;
  std::vector<float> hi;
  std::vector<float> center_full;
  double radius2 = 0.0;
};
struct VectorBlockGroup {
  size_t begin_block = 0;
  size_t end_block = 0;
  std::vector<float> lo;
  std::vector<float> hi;
};
struct Table {
  std::string name;
  std::vector<Column> cols;
  int primary = -1;
  int vector_col = -1;
  bool index = false;
  std::vector<Row> rows;
  std::vector<bool> alive;
  std::unordered_map<int64_t, size_t> keys;
  mutable bool blocks_valid = false;
  mutable size_t blocks_row_count = 0;
  mutable bool vector_matrix_valid = false;
  mutable bool vector_matrix_disabled = false;
  mutable int vector_matrix_dim = 0;
  mutable std::vector<float> vector_matrix;
  mutable std::vector<int> block_dims;
  mutable std::vector<VectorBlock> blocks;
  mutable bool block_groups_valid = false;
  mutable std::vector<VectorBlockGroup> block_groups;
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
struct JournalEntry {
  bool binary_operation = false;
  std::string sql;
  std::string binary;
};
struct Engine {
  State st; std::optional<State> tx; std::vector<JournalEntry> tx_log; std::string dir; int journal_fd = -1;
  uint64_t checkpoint_generation = 0;
  uint64_t checkpoint_offset = 0;
  std::string checkpoint_snapshot;
  std::unordered_map<std::string, std::vector<float>> vector_literal_cache;
  std::unordered_map<std::string, Json> query_cache;
  explicit Engine(std::string d) : dir(std::move(d)) { mkdir(dir.c_str(), 0755); load(); }
  ~Engine() { if (journal_fd >= 0) ::close(journal_fd); }
  std::string file() const { return dir + "/state.bin"; }
  std::string journal() const { return dir + "/journal.log"; }
  std::string manifest() const { return dir + "/MANIFEST"; }
  std::string snapshot_path(const std::string &name) const { return dir + "/" + name; }
  void invalidate_query_cache() { query_cache.clear(); }
  std::vector<float> parse_vector_cached(const std::string &text, int dim) {
    const std::string key = std::to_string(dim) + ":" + text;
    auto it = vector_literal_cache.find(key);
    if (it != vector_literal_cache.end()) return it->second;
    if (vector_literal_cache.size() >= 2048) vector_literal_cache.clear();
    auto value = parse_vector(text, dim);
    auto inserted = vector_literal_cache.emplace(key, std::move(value));
    return inserted.first->second;
  }
  template<class T> static void put(std::ofstream &f, const T &x) { f.write(reinterpret_cast<const char *>(&x), sizeof(x)); }
  template<class T> static bool get(std::ifstream &f, T &x) { return bool(f.read(reinterpret_cast<char *>(&x), sizeof(x))); }
  static void putstr(std::ofstream &f, const std::string &s) { uint32_t n = uint32_t(s.size()); put(f, n); f.write(s.data(), n); }
  static std::string getstr(std::ifstream &f) { uint32_t n; if (!get(f, n) || n > 1u << 24) throw std::runtime_error("bad state"); std::string s(n, '\0'); if (!f.read(s.data(), n)) throw std::runtime_error("bad state"); return s; }
  static constexpr uint32_t JOURNAL_MAGIC = 0x4d564a31;
  static constexpr uint32_t BINARY_JOURNAL_MAGIC = 0x4d564a32;
  static void append_u32(std::string &out, uint32_t value) { out.append(reinterpret_cast<const char *>(&value), sizeof(value)); }
  static void append_u8(std::string &out, uint8_t value) { out.push_back(char(value)); }
  static void append_le32(std::string &out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(char((value >> shift) & 0xffu));
  }
  static void append_le64(std::string &out, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) out.push_back(char((value >> shift) & 0xffu));
  }
  static bool read_u32(const std::string &buf, size_t &pos, uint32_t &value) {
    if (pos + sizeof(value) > buf.size()) return false;
    std::memcpy(&value, buf.data() + pos, sizeof(value)); pos += sizeof(value); return true;
  }
  static bool read_u8(const std::string &buf, size_t &pos, uint8_t &value) {
    if (pos >= buf.size()) return false;
    value = uint8_t(static_cast<unsigned char>(buf[pos++]));
    return true;
  }
  static bool read_le32(const std::string &buf, size_t &pos, uint32_t &value) {
    if (pos > buf.size() || buf.size() - pos < sizeof(value)) return false;
    value = uint32_t(static_cast<unsigned char>(buf[pos])) |
            (uint32_t(static_cast<unsigned char>(buf[pos + 1])) << 8) |
            (uint32_t(static_cast<unsigned char>(buf[pos + 2])) << 16) |
            (uint32_t(static_cast<unsigned char>(buf[pos + 3])) << 24);
    pos += sizeof(value);
    return true;
  }
  static bool read_le64(const std::string &buf, size_t &pos, uint64_t &value) {
    if (pos > buf.size() || buf.size() - pos < sizeof(value)) return false;
    value = 0;
    for (int shift = 0; shift < 64; shift += 8)
      value |= uint64_t(static_cast<unsigned char>(buf[pos++])) << shift;
    return true;
  }
  static void append_blob(std::string &out, const std::string &value) {
    if (value.size() > (1u << 28)) throw Error("storage_error", "journal value too large");
    append_le32(out, uint32_t(value.size()));
    out.append(value);
  }
  static bool read_blob(const std::string &buf, size_t &pos, std::string &value) {
    uint32_t size = 0;
    if (!read_le32(buf, pos, size) || size > (1u << 28) || pos > buf.size() || buf.size() - pos < size)
      return false;
    value.assign(buf.data() + pos, size);
    pos += size;
    return true;
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
  void append_journal(const std::vector<JournalEntry> &entries) {
    if (entries.empty()) return;
    bool all_binary = true;
    size_t binary_bytes = 0, sql_bytes = 0;
    for (const auto &entry : entries) {
      all_binary = all_binary && entry.binary_operation;
      binary_bytes += entry.binary.size();
      sql_bytes += entry.sql.size();
    }
    // Small scalar rows can be shorter as SQL (for example, a few small
    // integers).  Use the binary frame only when it actually reduces the
    // durable payload; vector-heavy INSERTs are the common winning case.
    all_binary = all_binary && binary_bytes < sql_bytes;
    std::string payload;
    if (all_binary) append_le32(payload, uint32_t(entries.size()));
    else append_u32(payload, uint32_t(entries.size()));
    if (all_binary) {
      for (const auto &entry : entries) {
        if (entry.binary.size() > (1u << 28)) throw Error("storage_error", "journal value too large");
        append_le32(payload, uint32_t(entry.binary.size()));
        payload.append(entry.binary);
      }
    } else {
      for (const auto &entry : entries) {
        if (entry.sql.size() > (1u << 28)) throw Error("storage_error", "statement too large");
        append_u32(payload, uint32_t(entry.sql.size()));
        payload.append(entry.sql);
      }
    }
    if (payload.size() > (1u << 30)) throw Error("storage_error", "journal frame too large");
    uint32_t magic = all_binary ? BINARY_JOURNAL_MAGIC : JOURNAL_MAGIC;
    uint32_t bytes = uint32_t(payload.size());
    int fd = ensure_journal_fd();
    try {
      std::string frame;
      frame.resize(sizeof(magic) + sizeof(bytes) + payload.size());
      std::memcpy(&frame[0], &magic, sizeof(magic));
      std::memcpy(&frame[sizeof(magic)], &bytes, sizeof(bytes));
      if (!payload.empty()) std::memcpy(&frame[sizeof(magic) + sizeof(bytes)], payload.data(), payload.size());
      write_all(fd, frame.data(), frame.size());
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
  void replay_journal(uint64_t start_offset = 0) {
    std::ifstream f(journal(), std::ios::binary);
    if (!f) {
      if (start_offset != 0) throw Error("storage_error", "checkpoint journal is missing");
      return;
    }
    f.seekg(0, std::ios::end); std::streamoff end_offset = f.tellg();
    if (end_offset < 0) return;
    const uint64_t file_size_u64 = uint64_t(end_offset);
    if (start_offset > file_size_u64)
      throw Error("storage_error", "checkpoint offset exceeds journal");
    size_t file_size = size_t(file_size_u64), pos = size_t(start_offset), valid_end = size_t(start_offset);
    bool torn_tail = false;
    std::string payload;
    std::vector<std::string> statements;
    f.seekg(std::streamoff(start_offset), std::ios::beg);
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
      if (magic != JOURNAL_MAGIC && magic != BINARY_JOURNAL_MAGIC)
        throw Error("storage_error", "invalid journal magic");
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
      if (magic == BINARY_JOURNAL_MAGIC) {
        size_t binary_pos = 0;
        uint32_t count = 0;
        if (!read_le32(payload, binary_pos, count) || count > 1000000)
          throw Error("storage_error", "invalid binary journal frame");
        for (uint32_t i = 0; i < count; ++i) {
          uint32_t size = 0;
          if (!read_le32(payload, binary_pos, size) || size > (1u << 28) ||
              binary_pos > payload.size() || payload.size() - binary_pos < size)
            throw Error("storage_error", "invalid binary journal frame");
          std::string entry(payload.data() + binary_pos, size);
          binary_pos += size;
          replay_binary_operation(entry);
        }
        if (binary_pos != payload.size())
          throw Error("storage_error", "invalid binary journal frame");
        pos = frame_end;
        valid_end = pos;
        continue;
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
  static void sync_file(const std::string &path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0 || ::fsync(fd) != 0) {
      if (fd >= 0) ::close(fd);
      throw Error("storage_error", "cannot sync checkpoint file");
    }
    ::close(fd);
  }
  void sync_directory() const {
#if defined(__linux__)
    int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0 || ::fsync(fd) != 0) {
      if (fd >= 0) ::close(fd);
      throw Error("storage_error", "cannot sync data directory");
    }
    ::close(fd);
#endif
  }
  bool read_manifest(std::string &snapshot, uint64_t &generation, uint64_t &offset) {
    std::ifstream f(manifest(), std::ios::binary);
    if (!f) return false;
    uint32_t magic = 0, version = 0;
    if (!get(f, magic) || magic != 0x4d564d31 || !get(f, version) || version != 1 ||
        !get(f, generation) || !get(f, offset))
      throw Error("storage_error", "invalid checkpoint manifest");
    snapshot = getstr(f);
    if (snapshot.empty() || snapshot.size() > 255 || snapshot.find('/') != std::string::npos ||
        snapshot.find('\\') != std::string::npos)
      throw Error("storage_error", "invalid checkpoint snapshot");
    return true;
  }
  void write_manifest(const std::string &snapshot, uint64_t generation, uint64_t offset) {
    std::string tmp = manifest() + ".tmp";
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) throw Error("storage_error", "cannot open checkpoint manifest");
    uint32_t magic = 0x4d564d31, version = 1;
    put(f, magic); put(f, version); put(f, generation); put(f, offset); putstr(f, snapshot);
    f.flush();
    if (!f.good()) throw Error("storage_error", "cannot write checkpoint manifest");
    f.close();
    sync_file(tmp);
    if (::rename(tmp.c_str(), manifest().c_str()) != 0)
      throw Error("storage_error", "cannot publish checkpoint manifest");
    sync_directory();
  }
  void save_snapshot(const std::string &path) {
    std::string tmp = path + ".tmp";
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) throw Error("storage_error", "cannot open data directory");
    // Version 4 keeps the row format byte-for-byte compatible with v2 and
    // appends an optional, validated Block metadata section per table.  The
    // metadata is only a query cache: rows remain the source of truth and a
    // stale/absent section is rebuilt normally after loading.
    uint32_t magic = 0x4d564334, nt = uint32_t(st.tables.size());
    put(f, magic); put(f, nt);
    for (auto &kv : st.tables) {
      const Table &t = kv.second;
      putstr(f, t.name);
      uint32_t nc = uint32_t(t.cols.size());
      put(f, nc); put(f, t.primary); put(f, t.vector_col);
      uint8_t ix = t.index; put(f, ix);
      for (auto &c : t.cols) {
        putstr(f, c.name); uint8_t k = uint8_t(c.kind); put(f, k); put(f, c.width);
      }
      uint64_t nr = t.rows.size(); put(f, nr);
      for (size_t r = 0; r < t.rows.size(); ++r) {
        uint8_t live = t.alive[r]; put(f, live);
        for (auto &v : t.rows[r]) {
          uint8_t k = uint8_t(v.kind); put(f, k);
          if (v.kind == VK_INT) put(f, v.i);
          else if (v.kind == VK_STRING) putstr(f, v.s);
          else {
            uint32_t n = uint32_t(v.v.size()); put(f, n);
            f.write(reinterpret_cast<const char *>(v.v.data()), sizeof(float) * n);
          }
        }
      }
      const bool metadata_valid = t.blocks_valid && t.blocks_row_count == t.rows.size();
      uint8_t has_metadata = metadata_valid ? 1 : 0;
      put(f, has_metadata);
      if (metadata_valid) {
        const uint64_t block_rows = uint64_t(t.blocks_row_count);
        const uint32_t nd = uint32_t(t.block_dims.size());
        const uint64_t nb = uint64_t(t.blocks.size());
        put(f, block_rows); put(f, nd);
        for (int dim : t.block_dims) put(f, dim);
        put(f, nb);
        for (const VectorBlock &block : t.blocks) {
          const uint64_t begin = uint64_t(block.begin), end = uint64_t(block.end);
          const uint32_t nlo = uint32_t(block.lo.size());
          const uint32_t nhi = uint32_t(block.hi.size());
          const uint32_t nfull = uint32_t(block.center_full.size());
          put(f, begin); put(f, end); put(f, nlo);
          if (nlo) f.write(reinterpret_cast<const char *>(block.lo.data()), sizeof(float) * nlo);
          put(f, nhi);
          if (nhi) f.write(reinterpret_cast<const char *>(block.hi.data()), sizeof(float) * nhi);
          put(f, nfull);
          if (nfull) f.write(reinterpret_cast<const char *>(block.center_full.data()), sizeof(float) * nfull);
          put(f, block.radius2);
        }
      }
    }
    f.flush();
    if (!f.good()) throw Error("storage_error", "cannot write checkpoint snapshot");
    f.close();
    sync_file(tmp);
    if (::rename(tmp.c_str(), path.c_str()) != 0)
      throw Error("storage_error", "cannot publish checkpoint snapshot");
    sync_directory();
  }
  void save() { save_snapshot(file()); }
  static void compact_tombstones(Table &t) {
    if (t.rows.size() < 1024 || t.alive.size() != t.rows.size()) return;
    size_t live = 0;
    for (bool value : t.alive) live += value ? 1u : 0u;
    if (live * 4u > t.rows.size() * 3u) return;
    std::vector<Row> rows;
    rows.reserve(live);
    for (size_t r = 0; r < t.rows.size(); ++r)
      if (t.alive[r]) rows.push_back(std::move(t.rows[r]));
    t.rows = std::move(rows);
    t.alive.assign(t.rows.size(), true);
    rebuild(t);
  }
  void checkpoint() {
    if (tx) return;
    struct stat stbuf {};
    uint64_t offset = 0;
    if (::stat(journal().c_str(), &stbuf) == 0) offset = uint64_t(stbuf.st_size);
    else if (errno != ENOENT) throw Error("storage_error", "cannot stat journal");
    // The alive bitmap is the cheap L0 delete path.  During a durable
    // checkpoint, fold a sufficiently tombstone-heavy table into a compact
    // immutable snapshot so future scans do not pay for deleted rows.
    for (auto &entry : st.tables) compact_tombstones(entry.second);
    const uint64_t generation = checkpoint_generation + 1;
    const std::string name = "checkpoint-" + std::to_string(generation) + ".sst";
    save_snapshot(snapshot_path(name));
    write_manifest(name, generation, offset);
    const std::string previous = checkpoint_snapshot;
    checkpoint_generation = generation;
    checkpoint_offset = offset;
    checkpoint_snapshot = name;
    if (!previous.empty() && previous != name) {
      if (::unlink(snapshot_path(previous).c_str()) != 0 && errno != ENOENT)
        throw Error("storage_error", "cannot remove obsolete checkpoint");
      sync_directory();
    }
  }
  void maybe_checkpoint() {
    if (tx) return;
    struct stat stbuf {};
    if (::stat(journal().c_str(), &stbuf) != 0) {
      if (errno == ENOENT) return;
      throw Error("storage_error", "cannot stat journal");
    }
    constexpr uint64_t MIN_CHECKPOINT_THRESHOLD = 64ull * 1024ull * 1024ull;
    constexpr uint64_t MAX_CHECKPOINT_THRESHOLD = 1ull * 1024ull * 1024ull * 1024ull;
    uint64_t checkpoint_threshold = MIN_CHECKPOINT_THRESHOLD;
    if (!checkpoint_snapshot.empty()) {
      struct stat snapshot_stat {};
      if (::stat(snapshot_path(checkpoint_snapshot).c_str(), &snapshot_stat) == 0) {
        const uint64_t snapshot_size = uint64_t(snapshot_stat.st_size);
        if (snapshot_size > 0) {
          const uint64_t scaled = snapshot_size > MAX_CHECKPOINT_THRESHOLD / 4
                                      ? MAX_CHECKPOINT_THRESHOLD
                                      : snapshot_size * 4;
          checkpoint_threshold = std::min(MAX_CHECKPOINT_THRESHOLD,
                                          std::max(MIN_CHECKPOINT_THRESHOLD, scaled));
        }
      }
    }
    const uint64_t size = uint64_t(stbuf.st_size);
    if (size > checkpoint_offset && size - checkpoint_offset >= checkpoint_threshold)
      checkpoint();
  }
  void load_snapshot(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw Error("storage_error", "checkpoint snapshot is missing");
    uint32_t magic = 0, nt = 0;
    if (!get(f, magic) || (magic != 0x4d564332 && magic != 0x4d564333 && magic != 0x4d564334) || !get(f, nt))
      throw Error("storage_error", "invalid data");
    const bool has_block_metadata = magic == 0x4d564333 || magic == 0x4d564334;
    const bool metadata_has_legacy_center = magic == 0x4d564333;
    for (uint32_t z = 0; z < nt; ++z) {
      Table t; t.name = getstr(f); uint32_t nc;
      if (!get(f, nc) || nc > 1024 || !get(f, t.primary) || !get(f, t.vector_col))
        throw Error("storage_error", "invalid schema");
      uint8_t ix; if (!get(f, ix)) throw Error("storage_error", "invalid schema"); t.index = ix;
      for (uint32_t c = 0; c < nc; ++c) {
        Column col; col.name = getstr(f); uint8_t k;
        if (!get(f, k) || !get(f, col.width)) throw Error("storage_error", "invalid column");
        col.kind = VKind(k); t.cols.push_back(std::move(col));
      }
      uint64_t nr; if (!get(f, nr) || nr > 100000000) throw Error("storage_error", "invalid rows");
      for (uint64_t r = 0; r < nr; ++r) {
        uint8_t live; if (!get(f, live)) throw Error("storage_error", "invalid row"); t.alive.push_back(live != 0);
        std::vector<Value> row;
        for (uint32_t c = 0; c < nc; ++c) {
          Value v; uint8_t k; if (!get(f, k)) throw Error("storage_error", "invalid value"); v.kind = VKind(k);
          if (v.kind == VK_INT) { if (!get(f, v.i)) throw Error("storage_error", "invalid value"); }
          else if (v.kind == VK_STRING) v.s = getstr(f);
          else {
            uint32_t n; if (!get(f, n) || n > 100000) throw Error("storage_error", "invalid vector");
            v.v.resize(n);
            if (!f.read(reinterpret_cast<char *>(v.v.data()), sizeof(float) * n)) throw Error("storage_error", "invalid vector");
          }
          row.push_back(std::move(v));
        }
        t.rows.push_back(std::move(row));
      }
      bool metadata_valid = false;
      uint64_t metadata_rows = 0;
      std::vector<int> metadata_dims;
      std::vector<VectorBlock> metadata_blocks;
      if (has_block_metadata) {
        uint8_t has_metadata = 0;
        if (!get(f, has_metadata)) throw Error("storage_error", "invalid block metadata");
        if (has_metadata > 1) throw Error("storage_error", "invalid block metadata");
        metadata_valid = has_metadata != 0;
        if (metadata_valid) {
          uint32_t nd = 0;
          uint64_t nb = 0;
          if (!get(f, metadata_rows) || !get(f, nd) || nd > 1024)
            throw Error("storage_error", "invalid block metadata");
          metadata_dims.resize(nd);
          for (uint32_t i = 0; i < nd; ++i)
            if (!get(f, metadata_dims[i])) throw Error("storage_error", "invalid block metadata");
          const uint64_t max_blocks = (nr + 255u) / 256u;
          if (!get(f, nb) || nb > max_blocks) throw Error("storage_error", "invalid block metadata");
          metadata_blocks.reserve(size_t(nb));
          for (uint64_t i = 0; i < nb; ++i) {
            VectorBlock block;
            uint64_t begin = 0, end = 0;
            uint32_t nlo = 0, nhi = 0, nfull = 0;
            if (!get(f, begin) || !get(f, end) || begin > end || end > nr)
              throw Error("storage_error", "invalid block metadata");
            if (!get(f, nlo) || nlo > 1024) throw Error("storage_error", "invalid block metadata");
            block.lo.resize(nlo);
            if (nlo && !f.read(reinterpret_cast<char *>(block.lo.data()), sizeof(float) * nlo))
              throw Error("storage_error", "invalid block metadata");
            if (!get(f, nhi) || nhi > 1024) throw Error("storage_error", "invalid block metadata");
            block.hi.resize(nhi);
            if (nhi && !f.read(reinterpret_cast<char *>(block.hi.data()), sizeof(float) * nhi))
              throw Error("storage_error", "invalid block metadata");
            if (metadata_has_legacy_center) {
              uint32_t ncenter = 0;
              if (!get(f, ncenter) || ncenter > 1024) throw Error("storage_error", "invalid block metadata");
              std::vector<float> legacy_center(ncenter);
              if (ncenter && !f.read(reinterpret_cast<char *>(legacy_center.data()), sizeof(float) * ncenter))
                throw Error("storage_error", "invalid block metadata");
            }
            if (!get(f, nfull) || nfull > 1024) throw Error("storage_error", "invalid block metadata");
            block.center_full.resize(nfull);
            if (nfull && !f.read(reinterpret_cast<char *>(block.center_full.data()), sizeof(float) * nfull))
              throw Error("storage_error", "invalid block metadata");
            if (!get(f, block.radius2) || !std::isfinite(block.radius2))
              throw Error("storage_error", "invalid block metadata");
            block.begin = size_t(begin);
            block.end = size_t(end);
            metadata_blocks.push_back(std::move(block));
          }
        }
      }
      rebuild(t);
      if (metadata_valid) {
        const int dim = t.vector_col >= 0 && t.vector_col < int(t.cols.size())
                            ? t.cols[t.vector_col].width : 0;
        bool valid = metadata_rows == nr;
        if (valid && metadata_blocks.empty()) valid = nr == 0;
        if (valid && !metadata_blocks.empty()) {
          valid = metadata_blocks.front().begin == 0 &&
                  metadata_blocks.back().end == nr;
          size_t previous_end = 0;
          for (const VectorBlock &block : metadata_blocks) {
            if (block.begin != previous_end || block.end <= block.begin ||
                block.end - block.begin > 256 ||
                block.lo.size() != metadata_dims.size() ||
                block.hi.size() != metadata_dims.size() ||
                (!block.center_full.empty() && block.center_full.size() != size_t(std::max(dim, 0)))) {
              valid = false;
              break;
            }
            previous_end = block.end;
          }
          valid = valid && previous_end == nr;
        }
        if (valid) {
          for (int sampled_dim : metadata_dims)
            if (sampled_dim < 0 || sampled_dim >= dim) { valid = false; break; }
        }
        if (valid) {
          t.block_dims = std::move(metadata_dims);
          t.blocks = std::move(metadata_blocks);
          t.blocks_row_count = size_t(metadata_rows);
          t.blocks_valid = true;
        }
      }
      st.tables.emplace(lower(t.name), std::move(t));
    }
  }
  void load() {
    std::string snapshot = file(); uint64_t generation = 0, offset = 0;
    const bool has_manifest = read_manifest(snapshot, generation, offset);
    if (has_manifest) {
      checkpoint_generation = generation;
      checkpoint_offset = offset;
      checkpoint_snapshot = snapshot;
      snapshot = snapshot_path(snapshot);
    }
    std::ifstream probe(snapshot, std::ios::binary);
    if (!probe) {
      if (has_manifest) throw Error("storage_error", "checkpoint snapshot is missing");
      replay_journal();
      return;
    }
    probe.close();
    load_snapshot(snapshot);
    replay_journal(offset);
  }
  static void rebuild(Table &t) {
    t.keys.clear();
    t.keys.reserve(t.rows.size());
    for (size_t r = 0; r < t.rows.size(); ++r)
      if (t.alive[r]) t.keys.emplace(t.rows[r][t.primary].i, r);
    t.blocks_valid = false;
    t.blocks_row_count = 0;
    t.vector_matrix_valid = false;
    t.vector_matrix_disabled = false;
    t.vector_matrix_dim = 0;
    std::vector<float>().swap(t.vector_matrix);
    std::vector<int>().swap(t.block_dims);
    std::vector<VectorBlock>().swap(t.blocks);
    t.block_groups_valid = false;
    std::vector<VectorBlockGroup>().swap(t.block_groups);
    t.equality_index.clear();
    t.equality_index_valid.clear();
    t.equality_index_disabled.clear();
  }
  static void invalidate_blocks(Table &t) {
    t.blocks_valid = false;
    t.block_groups_valid = false;
    // INSERT only appends to the MemTable.  Keep complete immutable blocks
    // and the packed vector prefix; ensure_blocks() will rebuild at most the
    // previous partial block plus the new tail.
    t.vector_matrix_valid = false;
    // An append can move a table across the adaptive matrix budget. Recheck
    // the decision on the next vector query rather than permanently pinning
    // the old choice for the lifetime of the table.
    t.vector_matrix_disabled = false;
    t.equality_index_valid.clear();
    t.equality_index_disabled.clear();
    for (auto &index : t.equality_index) index.clear();
  }
  static size_t vector_matrix_budget_bytes() {
    static const size_t budget = []() -> size_t {
      constexpr size_t DEFAULT_BUDGET = 448ull * 1024ull * 1024ull;
      const char *text = std::getenv("MINIVECTORDB_MATRIX_BUDGET_MB");
      if (!text || !*text) return DEFAULT_BUDGET;
      char *end = nullptr;
      const unsigned long long mb = std::strtoull(text, &end, 10);
      if (end == text || *end != '\0' || mb == 0 || mb > (std::numeric_limits<size_t>::max() / (1024ull * 1024ull)))
        return DEFAULT_BUDGET;
      return size_t(mb) * 1024ull * 1024ull;
    }();
    return budget;
  }
  static const float *vector_data(const Table &t, size_t row, int dim) {
    if (t.vector_matrix_valid && t.vector_matrix_dim == dim)
      return t.vector_matrix.data() + row * size_t(dim);
    if (t.vector_col < 0 || row >= t.rows.size()) throw Error("invalid_value", "distance requires a vector");
    const Value &v = t.rows[row][size_t(t.vector_col)];
    if (v.kind != VK_VECTOR || int(v.v.size()) != dim)
      throw Error("invalid_value", "distance requires a vector");
    return v.v.data();
  }
  static void ensure_vector_matrix(Table &t) {
    if (t.vector_matrix_valid || t.vector_matrix_disabled) return;
    if (t.vector_col < 0 || t.rows.empty()) {
      t.vector_matrix_valid = true;
      t.vector_matrix_dim = 0;
      std::vector<float>().swap(t.vector_matrix);
      return;
    }
    const int dim = t.cols[t.vector_col].width;
    if (dim <= 0) {
      t.vector_matrix_valid = true;
      t.vector_matrix_dim = 0;
      t.vector_matrix.clear();
      return;
    }
    if (t.rows.size() > std::numeric_limits<size_t>::max() / size_t(dim))
      throw Error("storage_error", "vector matrix too large");
    const size_t vector_count = t.rows.size() * size_t(dim);
    if (vector_count > std::numeric_limits<size_t>::max() / sizeof(float) ||
        vector_count * sizeof(float) > vector_matrix_budget_bytes()) {
      // Keep the row vectors as the canonical representation for very large
      // tables. This avoids a second O(rows*dim) allocation while preserving
      // exact distance semantics; ensure_blocks() uses vector_data() and the
      // query path falls back to the same float32 values.
      std::vector<float>().swap(t.vector_matrix);
      t.vector_matrix_dim = dim;
      t.vector_matrix_valid = false;
      t.vector_matrix_disabled = true;
      return;
    }
    size_t cached_rows = 0;
    if (t.vector_matrix_dim == dim && t.vector_matrix.size() % size_t(dim) == 0 &&
        t.vector_matrix.size() / size_t(dim) <= t.rows.size()) {
      cached_rows = t.vector_matrix.size() / size_t(dim);
    } else {
      t.vector_matrix.clear();
      t.vector_matrix_dim = dim;
    }
    t.vector_matrix.resize(t.rows.size() * size_t(dim));
    for (size_t r = cached_rows; r < t.rows.size(); ++r) {
      const Value &v = t.rows[r][t.vector_col];
      if (v.kind != VK_VECTOR || int(v.v.size()) != dim)
        throw Error("invalid_value", "distance requires a vector");
      std::memcpy(t.vector_matrix.data() + r * size_t(dim), v.v.data(), sizeof(float) * size_t(dim));
    }
    t.vector_matrix_dim = dim;
    t.vector_matrix_valid = true;
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
    if (t.vector_col < 0 || t.rows.empty()) {
      t.blocks.clear();
      t.block_dims.clear();
      t.block_groups.clear();
      t.block_groups_valid = true;
      t.blocks_row_count = t.rows.size();
      t.blocks_valid = true;
      return;
    }
    const int dim = t.cols[t.vector_col].width;
    if (dim <= 0) {
      t.blocks.clear();
      t.block_dims.clear();
      t.block_groups.clear();
      t.block_groups_valid = true;
      t.blocks_row_count = t.rows.size();
      t.blocks_valid = true;
      return;
    }
    // Keep a compact, evenly spaced sample of dimensions.  Bounds over a
    // subset of dimensions are still valid lower bounds for L2 distance, and
    // cap metadata for unusually wide vectors.
    const int samples = std::min(dim, 64);
    std::vector<int> expected_dims;
    expected_dims.reserve(samples);
    for (int i = 0; i < samples; ++i) {
      int d = int((int64_t(i) * dim) / samples);
      if (expected_dims.empty() || expected_dims.back() != d)
        expected_dims.push_back(d);
    }
    if (t.block_dims != expected_dims || t.blocks_row_count > t.rows.size() ||
        (!t.blocks.empty() && t.blocks.back().end != t.blocks_row_count)) {
      t.block_dims = expected_dims;
      t.blocks.clear();
      t.blocks_row_count = 0;
    }
    if (t.blocks_row_count != 0 && t.blocks.empty()) t.blocks_row_count = 0;
    constexpr size_t BLOCK_ROWS = 256;
    // For wide vectors the center/radius bound requires a second full-width
    // pass over every row, while the sampled-coordinate bound remains exact
    // and is usually tighter than the geometric ball. Keep the full bound
    // for ordinary dimensions, but avoid paying that cold-build cost for
    // high-dimensional tables.
    // A full-width center/radius is valuable for ordinary tables, but it is
    // another O(rows*dim) float array when materialized per block.  Once the
    // table itself is large, the sampled-coordinate envelope is the safer
    // memory/performance trade-off and remains an exact lower bound.
    const size_t vector_elements = t.rows.size() <= std::numeric_limits<size_t>::max() / size_t(dim)
                                     ? t.rows.size() * size_t(dim) : std::numeric_limits<size_t>::max();
    const size_t vector_bytes = vector_elements <= std::numeric_limits<size_t>::max() / sizeof(float)
                                  ? vector_elements * sizeof(float) : std::numeric_limits<size_t>::max();
    const bool build_full_center = dim <= 256 && vector_bytes <= 192ull * 1024ull * 1024ull;
    // A partial tail block is mutable in the append-only MemTable. Rebuild
    // only that tail; complete earlier blocks remain reusable.
    if (!t.blocks.empty() && t.blocks_row_count < t.rows.size() &&
        t.blocks.back().end == t.blocks_row_count &&
        t.blocks.back().end - t.blocks.back().begin < BLOCK_ROWS) {
      t.blocks_row_count = t.blocks.back().begin;
      t.blocks.pop_back();
    }
    for (size_t begin = t.blocks_row_count; begin < t.rows.size(); begin += BLOCK_ROWS) {
      VectorBlock block;
      block.begin = begin;
      block.end = std::min(t.rows.size(), begin + BLOCK_ROWS);
      block.lo.assign(t.block_dims.size(), std::numeric_limits<float>::infinity());
      block.hi.assign(t.block_dims.size(), -std::numeric_limits<float>::infinity());
      if (build_full_center) block.center_full.assign(size_t(dim), 0.0f);
      for (size_t r = block.begin; r < block.end; ++r) {
        const float *data = vector_data(t, r, dim);
        for (size_t j = 0; j < t.block_dims.size(); ++j) {
          float x = data[size_t(t.block_dims[j])];
          block.lo[j] = std::min(block.lo[j], x);
          block.hi[j] = std::max(block.hi[j], x);
        }
        if (build_full_center) {
          for (int d = 0; d < dim; ++d) block.center_full[size_t(d)] += data[size_t(d)];
        }
      }
      const float count = float(block.end - block.begin);
      if (build_full_center) {
        for (float &x : block.center_full) x /= count;
        for (size_t r = block.begin; r < block.end; ++r) {
          const float *data = vector_data(t, r, dim);
          block.radius2 = std::max(block.radius2,
                                   distance_fast(data, block.center_full.data(), size_t(dim)));
        }
      }
      t.blocks.push_back(std::move(block));
    }
    t.blocks_row_count = t.rows.size();
    t.blocks_valid = true;
    t.block_groups_valid = false;
    t.block_groups.clear();
  }
  static void ensure_block_groups(Table &t) {
    if (t.block_groups_valid) return;
    t.block_groups.clear();
    if (!t.blocks_valid || t.blocks.empty()) {
      t.block_groups_valid = true;
      return;
    }
    const int dim = t.vector_col >= 0 && t.vector_col < int(t.cols.size()) ? t.cols[t.vector_col].width : 0;
    if (t.blocks.size() < 64 || dim > 256) {
      t.block_groups_valid = true;
      return;
    }
    constexpr size_t GROUP_BLOCKS = 16;
    const size_t groups = (t.blocks.size() + GROUP_BLOCKS - 1) / GROUP_BLOCKS;
    t.block_groups.reserve(groups);
    for (size_t begin = 0; begin < t.blocks.size(); begin += GROUP_BLOCKS) {
      VectorBlockGroup group;
      group.begin_block = begin;
      group.end_block = std::min(t.blocks.size(), begin + GROUP_BLOCKS);
      group.lo.assign(t.block_dims.size(), std::numeric_limits<float>::infinity());
      group.hi.assign(t.block_dims.size(), -std::numeric_limits<float>::infinity());
      for (size_t bi = group.begin_block; bi < group.end_block; ++bi) {
        const VectorBlock &block = t.blocks[bi];
        for (size_t j = 0; j < t.block_dims.size(); ++j) {
          group.lo[j] = std::min(group.lo[j], block.lo[j]);
          group.hi[j] = std::max(group.hi[j], block.hi[j]);
        }
      }
      t.block_groups.push_back(std::move(group));
    }
    t.block_groups_valid = true;
  }
  State &working() { return st; }
  const State &view() const { return st; }
  void begin() { if (tx) throw Error("transaction_active", "transaction already active"); tx = st; tx_log.clear(); invalidate_query_cache(); }
  void rollback() { if (tx) { st = *tx; tx.reset(); tx_log.clear(); } invalidate_query_cache(); }
  void commit() {
    if (!tx) return;
    invalidate_query_cache();
    try { append_journal(tx_log); }
    catch (...) { st = State{}; tx.reset(); tx_log.clear(); load(); throw; }
    tx.reset(); tx_log.clear();
    try { maybe_checkpoint(); }
    catch (...) { st = State{}; tx.reset(); tx_log.clear(); load(); throw; }
  }
  void autocommit(bool changed, const std::string &sql, const std::string *binary_payload = nullptr) {
    if (!changed) return;
    invalidate_query_cache();
    JournalEntry entry;
    entry.sql = sql;
    if (binary_payload && !binary_payload->empty()) {
      entry.binary_operation = true;
      entry.binary = *binary_payload;
    }
    if (tx) { tx_log.push_back(std::move(entry)); return; }
    try { append_journal({std::move(entry)}); }
    catch (...) { st = State{}; tx.reset(); tx_log.clear(); load(); throw; }
    try { maybe_checkpoint(); }
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
  static int64_t apply_insert_rows(Table &tab, std::vector<Row> pending) {
    std::map<int64_t, bool> seen;
    for (const auto &row : pending) {
      if (row.size() != tab.cols.size()) throw Error("storage_error", "invalid journal row");
      int64_t key = row[size_t(tab.primary)].i;
      if (seen.count(key) || tab.keys.count(key)) throw Error("duplicate_key", "duplicate primary key");
      seen[key] = true;
    }
    const size_t target_keys = tab.keys.size() + pending.size();
    const size_t key_capacity = static_cast<size_t>(
        static_cast<double>(tab.keys.bucket_count()) * tab.keys.max_load_factor());
    if (target_keys > key_capacity) tab.keys.reserve(target_keys);
    for (auto &row : pending) {
      size_t pos = tab.rows.size();
      tab.keys[row[size_t(tab.primary)].i] = pos;
      tab.rows.push_back(std::move(row));
      tab.alive.push_back(true);
    }
    invalidate_blocks(tab);
    return int64_t(seen.size());
  }
  static void append_value(std::string &out, const Value &value) {
    append_u8(out, uint8_t(value.kind));
    if (value.kind == VK_INT) {
      append_le64(out, uint64_t(value.i));
    } else if (value.kind == VK_STRING) {
      append_blob(out, value.s);
    } else if (value.kind == VK_VECTOR) {
      if (value.v.size() > 100000) throw Error("storage_error", "vector too large");
      append_le32(out, uint32_t(value.v.size()));
      for (float item : value.v) {
        uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(item), "float must be 32-bit");
        std::memcpy(&bits, &item, sizeof(bits));
        append_le32(out, bits);
      }
    } else {
      throw Error("storage_error", "invalid journal value");
    }
  }
  static bool read_value(const std::string &buf, size_t &pos, const Column &column, Value &value) {
    uint8_t kind = 0;
    if (!read_u8(buf, pos, kind) || kind != uint8_t(column.kind)) return false;
    value.kind = VKind(kind);
    if (value.kind == VK_INT) {
      uint64_t bits = 0;
      if (!read_le64(buf, pos, bits)) return false;
      value.i = int64_t(bits);
    } else if (value.kind == VK_STRING) {
      if (!read_blob(buf, pos, value.s) || utf8_length(value.s) > size_t(column.width)) return false;
    } else {
      uint32_t dim = 0;
      if (!read_le32(buf, pos, dim) || dim != uint32_t(column.width)) return false;
      value.v.resize(dim);
      for (float &item : value.v) {
        uint32_t bits = 0;
        if (!read_le32(buf, pos, bits)) return false;
        std::memcpy(&item, &bits, sizeof(item));
        if (!std::isfinite(item)) return false;
      }
    }
    return true;
  }
  static std::string encode_insert_payload(const std::string &table, const std::vector<Row> &rows) {
    if (rows.size() > 1000000) throw Error("storage_error", "too many journal rows");
    std::string out;
    append_u8(out, 1); // INSERT
    append_blob(out, table);
    append_le32(out, uint32_t(rows.size()));
    for (const auto &row : rows) {
      if (row.size() > 1024) throw Error("storage_error", "too many journal columns");
      append_le32(out, uint32_t(row.size()));
      for (const auto &value : row) append_value(out, value);
    }
    return out;
  }
  void replay_binary_insert(const std::string &payload) {
    size_t pos = 0;
    uint8_t opcode = 0;
    std::string table_name;
    uint32_t row_count = 0;
    if (!read_u8(payload, pos, opcode) || opcode != 1 ||
        !read_blob(payload, pos, table_name) || !read_le32(payload, pos, row_count) || row_count > 1000000)
      throw Error("storage_error", "invalid binary journal insert");
    Table &tab = table_of(st, table_name);
    std::vector<Row> rows;
    rows.reserve(row_count);
    for (uint32_t r = 0; r < row_count; ++r) {
      uint32_t column_count = 0;
      if (!read_le32(payload, pos, column_count) || column_count != tab.cols.size())
        throw Error("storage_error", "invalid binary journal row");
      Row row(tab.cols.size());
      for (uint32_t c = 0; c < column_count; ++c) {
        Value value;
        if (!read_value(payload, pos, tab.cols[c], value))
          throw Error("storage_error", "invalid binary journal value");
        row[c] = std::move(value);
      }
      rows.push_back(std::move(row));
    }
    if (pos != payload.size()) throw Error("storage_error", "invalid binary journal insert");
    apply_insert_rows(tab, std::move(rows));
  }
  static bool read_terms(const std::string &payload, size_t &pos, const Table &tab,
                         std::vector<Term> &terms) {
    uint32_t count = 0;
    if (!read_le32(payload, pos, count) || count > 1024) return false;
    terms.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t col = 0;
      uint8_t opcode = 0;
      if (!read_le32(payload, pos, col) || col >= tab.cols.size() || !read_u8(payload, pos, opcode)) return false;
      Value value;
      if (!read_value(payload, pos, tab.cols[col], value)) return false;
      terms.push_back({int(col), term_operator(opcode), std::move(value)});
    }
    return true;
  }
  void replay_binary_update(const std::string &payload) {
    size_t pos = 0;
    uint8_t opcode = 0;
    std::string table_name;
    uint32_t count = 0;
    if (!read_u8(payload, pos, opcode) || opcode != 2 || !read_blob(payload, pos, table_name) ||
        !read_le32(payload, pos, count) || count > 1024)
      throw Error("storage_error", "invalid binary journal update");
    Table &tab = table_of(st, table_name);
    std::vector<std::pair<int, Value>> assigns;
    assigns.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t col = 0;
      if (!read_le32(payload, pos, col) || col >= tab.cols.size() || col == uint32_t(tab.primary))
        throw Error("storage_error", "invalid binary journal update");
      Value value;
      if (!read_value(payload, pos, tab.cols[col], value))
        throw Error("storage_error", "invalid binary journal update");
      assigns.emplace_back(int(col), std::move(value));
    }
    std::vector<Term> terms;
    if (!read_terms(payload, pos, tab, terms) || pos != payload.size())
      throw Error("storage_error", "invalid binary journal update");
    int64_t ignored = 0;
    apply_update_ops(tab, assigns, terms, ignored);
  }
  void replay_binary_delete(const std::string &payload) {
    size_t pos = 0;
    uint8_t opcode = 0;
    std::string table_name;
    if (!read_u8(payload, pos, opcode) || opcode != 3 || !read_blob(payload, pos, table_name))
      throw Error("storage_error", "invalid binary journal delete");
    Table &tab = table_of(st, table_name);
    std::vector<Term> terms;
    if (!read_terms(payload, pos, tab, terms) || pos != payload.size())
      throw Error("storage_error", "invalid binary journal delete");
    int64_t ignored = 0;
    apply_delete_terms(tab, terms, ignored);
  }
  void replay_binary_operation(const std::string &payload) {
    if (payload.empty()) throw Error("storage_error", "invalid binary journal operation");
    switch (uint8_t(static_cast<unsigned char>(payload[0]))) {
      case 1: replay_binary_insert(payload); break;
      case 2: replay_binary_update(payload); break;
      case 3: replay_binary_delete(payload); break;
      default: throw Error("storage_error", "invalid binary journal operation");
    }
  }
  int64_t insert(std::vector<Token> &t, size_t &p, std::string *journal_payload = nullptr) {
    expect(t, p, "into"); Table &tab = table_of(st, take_name(t, p)); std::vector<int> order; if (p < t.size() && t[p].text == "(") { ++p; while (true) { order.push_back(column_of(tab, take_name(t, p))); if (p >= t.size() || t[p].text != ",") break; ++p; } expect(t, p, ")"); } else for (size_t i = 0; i < tab.cols.size(); ++i) order.push_back(int(i)); if (order.size() != tab.cols.size()) throw Error("invalid_value", "all columns must appear"); expect(t, p, "values"); std::vector<Row> pending; while (true) { expect(t, p, "("); Row row(tab.cols.size()); size_t k = 0; while (true) { if (k >= order.size() || p >= t.size()) throw Error("invalid_value", "wrong value count"); int col = order[k++]; row[col] = convert_value(t[p++], tab.cols[col]); if (p >= t.size() || t[p].text != ",") break; ++p; } expect(t, p, ")"); if (k != order.size()) throw Error("invalid_value", "wrong value count"); pending.push_back(std::move(row)); if (p >= t.size() || t[p].text != ",") break; ++p; }
    if (journal_payload) *journal_payload = encode_insert_payload(tab.name, pending);
    return apply_insert_rows(tab, std::move(pending));
  }
  static uint8_t term_opcode(const std::string &op) {
    if (op == "=") return 0;
    if (op == "<") return 1;
    if (op == "<=") return 2;
    if (op == ">") return 3;
    if (op == ">=") return 4;
    throw Error("storage_error", "invalid journal operator");
  }
  static std::string term_operator(uint8_t code) {
    switch (code) {
      case 0: return "=";
      case 1: return "<";
      case 2: return "<=";
      case 3: return ">";
      case 4: return ">=";
      default: throw Error("storage_error", "invalid journal operator");
    }
  }
  static void append_terms(std::string &out, const std::vector<Term> &terms) {
    if (terms.size() > 1024) throw Error("storage_error", "too many journal terms");
    append_le32(out, uint32_t(terms.size()));
    for (const auto &term : terms) {
      append_le32(out, uint32_t(term.col));
      append_u8(out, term_opcode(term.op));
      append_value(out, term.value);
    }
  }
  static std::string encode_update_payload(const std::string &table,
                                           const std::vector<std::pair<int, Value>> &assigns,
                                           const std::vector<Term> &terms) {
    if (assigns.size() > 1024) throw Error("storage_error", "too many journal assignments");
    std::string out;
    append_u8(out, 2); // UPDATE
    append_blob(out, table);
    append_le32(out, uint32_t(assigns.size()));
    for (const auto &assignment : assigns) {
      append_le32(out, uint32_t(assignment.first));
      append_value(out, assignment.second);
    }
    append_terms(out, terms);
    return out;
  }
  static std::string encode_delete_payload(const std::string &table,
                                           const std::vector<Term> &terms) {
    std::string out;
    append_u8(out, 3); // DELETE
    append_blob(out, table);
    append_terms(out, terms);
    return out;
  }
  static void clear_equality_indexes(Table &tab) {
    tab.equality_index.clear();
    tab.equality_index_valid.clear();
    tab.equality_index_disabled.clear();
  }
  void apply_update_ops(Table &tab, const std::vector<std::pair<int, Value>> &assigns,
                        const std::vector<Term> &terms, int64_t &affected) {
    bool vector_changed = false;
    for (size_t r = 0; r < tab.rows.size(); ++r) if (tab.alive[r] && matches(tab.rows[r], terms)) {
      std::vector<Value> next = tab.rows[r];
      for (const auto &assignment : assigns) next[size_t(assignment.first)] = assignment.second;
      bool changed = false;
      for (size_t i = 0; i < next.size(); ++i) changed |= !value_equal(next[i], tab.rows[r][i]);
      if (changed) {
        for (const auto &assignment : assigns)
          if (assignment.first == tab.vector_col && !value_equal(assignment.second, tab.rows[r][tab.vector_col]))
            vector_changed = true;
        tab.rows[r] = std::move(next);
        ++affected;
      }
    }
    clear_equality_indexes(tab);
    if (vector_changed) rebuild(tab);
  }
  void apply_delete_terms(Table &tab, const std::vector<Term> &terms, int64_t &affected) {
    for (size_t r = 0; r < tab.rows.size(); ++r)
      if (tab.alive[r] && matches(tab.rows[r], terms)) {
        tab.alive[r] = false;
        tab.keys.erase(tab.rows[r][tab.primary].i);
        ++affected;
      }
    clear_equality_indexes(tab);
  }
  void update(std::vector<Token> &t, size_t &p, int64_t &affected, std::string *journal_payload = nullptr) { Table &tab = table_of(st, take_name(t, p)); expect(t, p, "set"); std::vector<std::pair<int, Value>> assigns; while (true) { int c = column_of(tab, take_name(t, p)); expect(t, p, "="); if (p >= t.size()) throw Error("invalid_value", "missing value"); Value value = convert_value(t[p++], tab.cols[c]);

    if (c == tab.primary) {
      throw Error(
          "unsupported_sql",
          "updating primary key is unsupported");
    }

    assigns.emplace_back(c, std::move(value)); if (p >= t.size() || t[p].text != ",") break; ++p; } auto terms = parse_where(t, p, tab); if (journal_payload) *journal_payload = encode_update_payload(tab.name, assigns, terms); apply_update_ops(tab, assigns, terms, affected); }
  void erase(std::vector<Token> &t, size_t &p, int64_t &affected, std::string *journal_payload = nullptr) { expect(t, p, "from"); Table &tab = table_of(st, take_name(t, p)); auto terms = parse_where(t, p, tab); if (journal_payload) *journal_payload = encode_delete_payload(tab.name, terms); apply_delete_terms(tab, terms, affected); }

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
          q.target = parse_vector_cached(q.target_text, tab.cols[q.col].width);
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
    auto distance_squared = [&](const Value &v, const std::vector<float> &q) {
      if (v.kind != VK_VECTOR || v.v.size() != q.size())
        throw Error("invalid_value", "distance requires a vector");
      return distance_fast(v.v.data(), q.data(), q.size());
    };
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
    bool has_vector_distance = false;
    for (size_t qi = 0; qi < proj.size(); ++qi) {
      if (proj[qi].distance) {
        has_vector_distance = true;
        if (!block_topk)
          distance_cache[qi].assign(tab.rows.size(), std::numeric_limits<double>::quiet_NaN());
      }
    }
    if (has_vector_distance && !block_topk) ensure_vector_matrix(tab);
    std::vector<std::unordered_map<size_t, double>> sparse_distance_cache(proj.size());
    if (block_topk) {
      const size_t reserve_count = std::min(tab.rows.size(), size_t(limit) * 64);
      for (size_t qi = 0; qi < proj.size(); ++qi)
        if (proj[qi].distance) sparse_distance_cache[qi].reserve(reserve_count);
    }
    auto distance_for = [&](size_t r, size_t qi) {
      if (block_topk) {
        auto &cache = sparse_distance_cache[qi];
        auto cached = cache.find(r);
        if (cached != cache.end()) return cached->second;
        const Projection &q = proj[qi];
        double value = 0.0;
        if (q.col == tab.vector_col && tab.vector_matrix_valid) {
          const size_t dim = size_t(tab.vector_matrix_dim);
          value = distance_fast(tab.vector_matrix.data() + r * dim, q.target.data(), dim);
        } else {
          value = distance_squared(tab.rows[r][q.col], q.target);
        }
        cache.emplace(r, value);
        return value;
      }
      if (std::isnan(distance_cache[qi][r])) {
        const Projection &q = proj[qi];
        if (q.col == tab.vector_col && tab.vector_matrix_valid) {
          const size_t dim = size_t(tab.vector_matrix_dim);
          distance_cache[qi][r] = distance_fast(tab.vector_matrix.data() + r * dim,
                                                 q.target.data(), dim);
        } else {
          distance_cache[qi][r] = distance_squared(tab.rows[r][q.col], q.target);
        }
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
      const auto &query_projection = proj[qi];
      auto sampled_bound = [&](const std::vector<float> &lo,
                               const std::vector<float> &hi) {
        double bound = 0.0;
        for (size_t j = 0; j < tab.block_dims.size(); ++j) {
          const double x = double(query_projection.target[size_t(tab.block_dims[j])]);
          const double d = x < double(lo[j]) ? double(lo[j]) - x
                           : x > double(hi[j]) ? x - double(hi[j]) : 0.0;
          bound += d * d;
        }
        return bound;
      };
      for (size_t bi = 0; bi < tab.blocks.size(); ++bi) {
        const auto &block = tab.blocks[bi];
        double bound = sampled_bound(block.lo, block.hi);
        if (!block.center_full.empty()) {
          const double center_distance = distance_fast(block.center_full.data(), query_projection.target.data(),
                                                       block.center_full.size());
          const double gap = std::sqrt(center_distance) - std::sqrt(block.radius2);
          if (gap > 0.0) {
            // Round the geometric bound down by one representable value.  The
            // radius is computed from the same float32 center as the query,
            // but keeping the bound conservative protects exact Top-K ties
            // from a final-bit rounding difference.
            bound = std::max(bound, std::nextafter(gap * gap, 0.0));
          }
        }
        block_bounds[bi] = bound;
        if (use_ann) {
          block_scores[bi] = block.center_full.empty()
            ? bound
            : distance_fast(block.center_full.data(), query_projection.target.data(), block.center_full.size());
        } else {
          block_scores[bi] = bound;
        }
      }
      std::vector<size_t> block_order(tab.blocks.size());
      std::iota(block_order.begin(), block_order.end(), 0);
      std::sort(block_order.begin(), block_order.end(), [&](size_t a, size_t b) {
        return block_scores[a] < block_scores[b];
      });
      // A second, coarse level groups adjacent blocks.  Its min/max envelope
      // is another conservative lower bound, so a whole group can be rejected
      // before touching its child blocks once the Top-K heap has a cutoff.
      // Keep the original flat order for small tables where the extra vectors
      // cost more than they save.
      ensure_block_groups(tab);
      // The full center/radius bound is available for ordinary dimensions and
      // makes group cutoffs useful. Wide vectors intentionally use only the
      // sampled-coordinate bound; keep their globally sorted block order,
      // which is tighter than a coarse group order for that case.
      const bool use_hierarchy = tab.blocks.size() >= 64 && tab.block_groups.size() >= 4 &&
                                 tab.cols[tab.vector_col].width <= 256;
      std::vector<double> group_bounds;
      std::vector<size_t> group_order;
      std::vector<std::pair<size_t, size_t>> group_ranges;
      if (use_hierarchy) {
        group_bounds.resize(tab.block_groups.size(), 0.0);
        group_order.resize(tab.block_groups.size());
        std::iota(group_order.begin(), group_order.end(), 0);
        for (size_t gi = 0; gi < tab.block_groups.size(); ++gi)
          group_bounds[gi] = sampled_bound(tab.block_groups[gi].lo, tab.block_groups[gi].hi);
        std::sort(group_order.begin(), group_order.end(), [&](size_t a, size_t b) {
          return group_bounds[a] < group_bounds[b];
        });
        block_order.clear();
        block_order.reserve(tab.blocks.size());
        group_ranges.reserve(group_order.size());
        for (size_t gi : group_order) {
          const VectorBlockGroup &group = tab.block_groups[gi];
          const size_t begin = block_order.size();
          std::vector<size_t> children;
          children.reserve(group.end_block - group.begin_block);
          for (size_t bi = group.begin_block; bi < group.end_block; ++bi) children.push_back(bi);
          std::sort(children.begin(), children.end(), [&](size_t a, size_t b) {
            return block_scores[a] < block_scores[b];
          });
          block_order.insert(block_order.end(), children.begin(), children.end());
          group_ranges.emplace_back(begin, block_order.size());
        }
      }
      const bool simple_distance_order = ord.size() == 1 && ord[0].distance;
      auto heap_less = [&](size_t a, size_t b) {
        if (simple_distance_order) {
          const double da = distance_for(a, qi);
          const double db = distance_for(b, qi);
          if (da != db) return da < db;
          return tab.rows[a][tab.primary].i < tab.rows[b][tab.primary].i;
        }
        return less_row(a, b);
      };
      using Heap = std::priority_queue<size_t, std::vector<size_t>, decltype(heap_less)>;
      Heap best(heap_less);
      const size_t wanted = size_t(limit);
      auto consider = [&](size_t r) {
        if (!tab.alive[r] || !matches(tab.rows[r], terms)) return;
        if (simple_distance_order) (void)distance_for(r, qi);
        if (best.size() < wanted) {
          best.push(r);
        } else if (heap_less(r, best.top())) {
          best.pop();
          best.push(r);
        }
      };
      // A fixed 25% probe is useful for the small benchmark collection, but
      // becomes an unsafe recall shortcut once a table has hundreds of
      // blocks.  Large tables therefore keep ANN ordering for candidate
      // discovery while scanning every block; the exact lower bound still
      // prunes blocks that cannot beat the current heap.
      // ANN follows the documented nprobe/R budget for every table size.
      // Exact queries keep the complete lower-bound traversal below.
      const bool approximate_ann = use_ann && tab.rows.size() < 32768;
      const size_t probe_blocks = approximate_ann
        ? std::min(tab.blocks.size(), std::max<size_t>(38, (tab.blocks.size() + 3) / 4))
        : tab.blocks.size();
      if (seed_rows) {
        for (size_t r : *seed_rows) consider(r);
      } else if (use_hierarchy && !approximate_ann) {
        for (size_t gi = 0; gi < group_ranges.size(); ++gi) {
          if (best.size() == wanted && group_bounds[group_order[gi]] >
              (distance_for(best.top(), qi)))
            break;
          for (size_t index = group_ranges[gi].first; index < group_ranges[gi].second; ++index) {
            const size_t bi = block_order[index];
            if (best.size() == wanted && block_bounds[bi] >
                (distance_for(best.top(), qi)))
              continue;
            const auto &block = tab.blocks[bi];
            for (size_t r = block.begin; r < block.end; ++r) consider(r);
          }
        }
      } else {
        for (size_t index = 0; index < probe_blocks; ++index) {
          size_t bi = block_order[index];
          if ((!use_ann || !approximate_ann) && best.size() == wanted && block_bounds[bi] >
              (distance_for(best.top(), qi)))
            continue;
          const auto &block = tab.blocks[bi];
          for (size_t r = block.begin; r < block.end; ++r) consider(r);
        }
        // A sparse filter or an unusually skewed block layout may leave too
        // few candidates in the ANN probe. Complete the query rather than
        // returning an undersized result set.
        if (approximate_ann && best.size() < wanted) {
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
  Json select_cached(const std::string &sql, std::vector<Token> &t, size_t &p, bool ann_mode) {
    const std::string key = std::string(ann_mode ? "A:" : "E:") + sql;
    auto it = query_cache.find(key);
    if (it != query_cache.end()) return it->second;
    Json result = select(t, p, ann_mode);
    auto rows_it = result.o.find("rows");
    if (result.kind == Json::OBJECT && rows_it != result.o.end() &&
        rows_it->second.kind == Json::ARRAY && rows_it->second.a.size() <= 64) {
      if (query_cache.size() >= 256) query_cache.clear();
      query_cache.emplace(key, result);
    }
    return result;
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
        if (op == "shutdown") { db.rollback(); db.checkpoint(); std::cout << mv::json_dump(mv::Json::object({{"id", id}, {"ok", mv::Json::boolean(true)}})) << '\n' << std::flush; break; }
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
        else {
          int64_t affected = 0;
          std::string binary_payload;
          if (cmd == "create") {
            if (p < tok.size() && mv::ieq(tok[p].text, "vector")) db.create_index(tok, p);
            else db.create_table(tok, p);
          } else if (cmd == "insert") {
            affected = db.insert(tok, p, &binary_payload);
          } else if (cmd == "update") {
            affected = 0;
            db.update(tok, p, affected, &binary_payload);
          } else if (cmd == "delete") {
            affected = 0;
            db.erase(tok, p, affected, &binary_payload);
          } else if (cmd == "select") {
            body = db.select_cached(sql, tok, p, ann_mode);
          } else {
            throw mv::Error("unsupported_sql", "unsupported statement");
          }
          if (cmd != "select") {
            db.invalidate_query_cache();
            db.autocommit(true, sql, binary_payload.empty() ? nullptr : &binary_payload);
          }
          if (body.kind == mv::Json::NIL) body = mv::Json::object({{"ok", mv::Json::boolean(true)}});
          else body.o["ok"] = mv::Json::boolean(true);
          if (cmd == "insert" || cmd == "update" || cmd == "delete")
            body.o["affected_rows"] = mv::Json::integer(affected);
        }
        body.o["id"] = id; std::cout << mv::json_dump(body) << '\n' << std::flush;
      } catch (const mv::Error &e) { std::cout << mv::json_dump(mv::Json::object({{"id", id}, {"ok", mv::Json::boolean(false)}, {"error", mv::Json::object({{"code", mv::Json::string(e.code)}, {"message", mv::Json::string(e.what())}})}})) << '\n' << std::flush; }
      catch (const std::exception &e) { std::cout << mv::json_dump(mv::Json::object({{"id", id}, {"ok", mv::Json::boolean(false)}, {"error", mv::Json::object({{"code", mv::Json::string("internal_error")}, {"message", mv::Json::string(e.what())}})}})) << '\n' << std::flush; }
    }
  } catch (const std::exception &e) { std::cerr << "startup failed: " << e.what() << '\n'; return 1; }
  return 0;
}














