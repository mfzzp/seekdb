/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG
#include "sql/table_function/table_function.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_array_expr_utils.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/pl/ob_pl_user_type.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/session/ob_sql_session_info.h"
#include "lib/string/ob_sql_string.h"
#include "lib/string/ob_string_buffer.h"
#include "common/timezone/ob_time_convert.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <sys/stat.h>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace oceanbase {
namespace sql {
using namespace common;
namespace {

enum class FileInferType : uint8_t
{
  NULL_TYPE = 0,
  BOOL_TYPE,
  BIGINT_TYPE,
  DOUBLE_TYPE,
  DATE_TYPE,
  DATETIME_TYPE,
  VARCHAR_TYPE,
};

struct FileColumnState
{
  FileColumnState() : type_(FileInferType::NULL_TYPE), nullable_(false), max_len_(0) {}
  FileInferType type_;
  bool nullable_;
  int64_t max_len_;
};

inline bool file_str_equal_ci(const ObString &a, const char *b)
{
  const int64_t len = static_cast<int64_t>(strlen(b));
  return a.length() == len && 0 == strncasecmp(a.ptr(), b, len);
}

inline bool file_is_digits(const char *p, int64_t len)
{
  bool ok = len > 0;
  for (int64_t i = 0; ok && i < len; ++i) {
    ok = p[i] >= '0' && p[i] <= '9';
  }
  return ok;
}

inline bool file_parse_int(const ObString &s, int64_t &value)
{
  bool ok = false;
  if (!s.empty()) {
    char buf[64];
    if (s.length() < static_cast<int64_t>(sizeof(buf))) {
      MEMCPY(buf, s.ptr(), s.length());
      buf[s.length()] = '\0';
      char *end = nullptr;
      errno = 0;
      const long long v = strtoll(buf, &end, 10);
      if (0 == errno && end == buf + s.length()) {
        value = static_cast<int64_t>(v);
        ok = true;
      }
    }
  }
  return ok;
}

inline bool file_parse_double(const ObString &s, double &value)
{
  bool ok = false;
  if (!s.empty()) {
    char buf[128];
    if (s.length() < static_cast<int64_t>(sizeof(buf))) {
      MEMCPY(buf, s.ptr(), s.length());
      buf[s.length()] = '\0';
      char *end = nullptr;
      errno = 0;
      const double v = strtod(buf, &end);
      if (0 == errno && end == buf + s.length() && std::isfinite(v)) {
        value = v;
        ok = true;
      }
    }
  }
  return ok;
}

inline bool file_is_bool(const ObString &s, bool &value)
{
  bool ok = true;
  if (file_str_equal_ci(s, "true")) {
    value = true;
  } else if (file_str_equal_ci(s, "false")) {
    value = false;
  } else {
    ok = false;
  }
  return ok;
}

inline bool file_is_date(const ObString &s)
{
  bool ok = s.length() == 10 && s.ptr()[4] == '-' && s.ptr()[7] == '-'
            && file_is_digits(s.ptr(), 4) && file_is_digits(s.ptr() + 5, 2)
            && file_is_digits(s.ptr() + 8, 2);
  if (ok) {
    int32_t date = 0;
    ok = OB_SUCCESS == ObTimeConverter::str_to_date(s, date);
  }
  return ok;
}

inline bool file_is_datetime(const ObString &s)
{
  bool ok = s.length() == 19 && (s.ptr()[10] == ' ' || s.ptr()[10] == 'T')
            && s.ptr()[4] == '-' && s.ptr()[7] == '-'
            && s.ptr()[13] == ':' && s.ptr()[16] == ':'
            && file_is_digits(s.ptr(), 4) && file_is_digits(s.ptr() + 5, 2)
            && file_is_digits(s.ptr() + 8, 2) && file_is_digits(s.ptr() + 11, 2)
            && file_is_digits(s.ptr() + 14, 2) && file_is_digits(s.ptr() + 17, 2);
  if (ok) {
    int64_t value = 0;
    ObTimeConvertCtx ctx(nullptr, false);
    ok = OB_SUCCESS == ObTimeConverter::str_to_datetime(s, ctx, value, nullptr);
  }
  return ok;
}

inline FileInferType file_detect_type(const ObString &s)
{
  FileInferType type = FileInferType::VARCHAR_TYPE;
  bool bool_value = false;
  int64_t int_value = 0;
  double double_value = 0.0;
  if (file_is_bool(s, bool_value)) {
    type = FileInferType::BOOL_TYPE;
  } else if (file_parse_int(s, int_value)) {
    type = FileInferType::BIGINT_TYPE;
  } else if (file_is_date(s)) {
    type = FileInferType::DATE_TYPE;
  } else if (file_is_datetime(s)) {
    type = FileInferType::DATETIME_TYPE;
  } else if (file_parse_double(s, double_value)) {
    type = FileInferType::DOUBLE_TYPE;
  }
  return type;
}

inline FileInferType file_merge_type(FileInferType a, FileInferType b)
{
  if (FileInferType::NULL_TYPE == a) { return b; }
  if (FileInferType::NULL_TYPE == b) { return a; }
  if (a == b) { return a; }
  if ((FileInferType::BIGINT_TYPE == a && FileInferType::DOUBLE_TYPE == b)
      || (FileInferType::DOUBLE_TYPE == a && FileInferType::BIGINT_TYPE == b)) {
    return FileInferType::DOUBLE_TYPE;
  }
  if ((FileInferType::DATE_TYPE == a && FileInferType::DATETIME_TYPE == b)
      || (FileInferType::DATETIME_TYPE == a && FileInferType::DATE_TYPE == b)) {
    return FileInferType::DATETIME_TYPE;
  }
  return FileInferType::VARCHAR_TYPE;
}

class CsvRecordReader
{
public:
  explicit CsvRecordReader(ObIAllocator &allocator)
    : allocator_(allocator), fp_(nullptr), buffer_(&allocator), fields_(), quoted_() {}
  ~CsvRecordReader() { close(); }
  int open(const ObString &path)
  {
    int ret = OB_SUCCESS;
    char name[OB_MAX_FILE_NAME_LENGTH];
    if (path.length() >= static_cast<int64_t>(sizeof(name))) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      MEMCPY(name, path.ptr(), path.length());
      name[path.length()] = '\0';
      fp_ = fopen(name, "rb");
      if (nullptr == fp_) { ret = OB_FILE_NOT_EXIST; }
    }
    return ret;
  }
  void close()
  {
    if (nullptr != fp_) {
      fclose(fp_);
      fp_ = nullptr;
    }
  }
  const ObIArray<ObString> &fields() const { return fields_; }
  const ObIArray<bool> &quoted() const { return quoted_; }
  int next()
  {
    int ret = OB_SUCCESS;
    fields_.reuse();
    quoted_.reuse();
    buffer_.reuse();
    int c = fgetc(fp_);
    if (EOF == c) {
      ret = OB_ITER_END;
    }
    bool done = false;
    while (OB_SUCC(ret) && !done) {
      bool is_quoted = false;
      const uint64_t start = buffer_.length();
      if ('"' == c) {
        is_quoted = true;
        c = fgetc(fp_);
        bool closed = false;
        while (OB_SUCC(ret) && EOF != c) {
          if ('"' == c) {
            const int next = fgetc(fp_);
            if ('"' == next) {
              if (OB_FAIL(buffer_.append("\"", 1))) {
              } else {
                c = fgetc(fp_);
              }
            } else {
              c = next;
              closed = true;
              break;
            }
          } else {
            char ch = static_cast<char>(c);
            if ('\r' == ch) {
              const int next = fgetc(fp_);
              if ('\n' == next) { ch = '\n'; }
              else if (EOF != next) { ungetc(next, fp_); }
            }
            if (OB_FAIL(buffer_.append(&ch, 1))) {
            } else {
              c = fgetc(fp_);
            }
          }
        }
        if (OB_SUCC(ret) && !closed) { ret = OB_INVALID_DATA; }
      } else {
        while (OB_SUCC(ret) && EOF != c && ',' != c && '\n' != c && '\r' != c) {
          char ch = static_cast<char>(c);
          if (OB_FAIL(buffer_.append(&ch, 1))) {
          } else {
            c = fgetc(fp_);
          }
        }
      }
      if (OB_SUCC(ret)) {
        ObString field;
        const char *base = buffer_.ptr();
        field.assign_ptr(nullptr == base ? "" : base + start,
                         static_cast<int32_t>(buffer_.length() - start));
        if (OB_FAIL(fields_.push_back(field)) || OB_FAIL(quoted_.push_back(is_quoted))) {
        } else if (',' == c) {
          c = fgetc(fp_);
        } else if ('\n' == c) {
          done = true;
        } else if ('\r' == c) {
          const int next = fgetc(fp_);
          if ('\n' != next && EOF != next) { ungetc(next, fp_); }
          done = true;
        } else if (EOF == c) {
          done = true;
        } else {
          ret = OB_INVALID_DATA;
        }
      }
    }
    return ret;
  }
private:
  ObIAllocator &allocator_;
  FILE *fp_;
  ObStringBuffer buffer_;
  ObSEArray<ObString, 16> fields_;
  ObSEArray<bool, 16> quoted_;
};

int make_file_column(ObIAllocator &allocator,
                     const ObString &name,
                     FileInferType type,
                     int64_t max_len,
                     bool nullable,
                     FileTableColumn &column)
{
  int ret = OB_SUCCESS;
  ObString copy;
  if (OB_FAIL(ob_write_string(allocator, name, copy))) {
  } else {
    column.name_ = copy;
    column.nullable_ = nullable;
    column.meta_.reset();
    switch (type) {
      case FileInferType::BOOL_TYPE:
        column.meta_.set_tinyint();
        column.accuracy_.set_precision(1);
        break;
      case FileInferType::BIGINT_TYPE:
        column.meta_.set_int();
        column.accuracy_.set_precision(20);
        break;
      case FileInferType::DOUBLE_TYPE:
        column.meta_.set_double();
        break;
      case FileInferType::DATE_TYPE:
        column.meta_.set_date();
        break;
      case FileInferType::DATETIME_TYPE:
        column.meta_.set_datetime();
        break;
      case FileInferType::VARCHAR_TYPE:
      case FileInferType::NULL_TYPE:
      default:
        column.meta_.set_varchar();
        column.meta_.set_collation_level(CS_LEVEL_IMPLICIT);
        column.meta_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
        column.accuracy_.set_length(max_len > 0 ? max_len : 65535);
        break;
    }
  }
  return ret;
}

int infer_csv_schema(ObIAllocator &allocator,
                     const ObString &path,
                     bool header,
                     ObIArray<FileTableColumn> &columns)
{
  int ret = OB_SUCCESS;
  CsvRecordReader reader(allocator);
  ObSEArray<ObString, 16> names;
  ObSEArray<FileColumnState, 16> states;
  if (OB_FAIL(reader.open(path))) {
  } else if (OB_FAIL(reader.next())) {
    if (OB_ITER_END == ret) { ret = OB_INVALID_DATA; }
  } else {
    const ObIArray<ObString> &first = reader.fields();
    int64_t column_count = first.count();
    if (header) {
      for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
        ObString name;
        if (OB_FAIL(ob_write_string(allocator, first.at(i), name))) {
        } else if (name.empty()) {
          ret = OB_INVALID_DATA;
        } else {
          ObSqlString unique;
          if (OB_FAIL(unique.append(name))) {
          } else {
            int64_t suffix = 2;
            bool exists = true;
            while (OB_SUCC(ret) && exists) {
              exists = false;
              for (int64_t j = 0; !exists && j < names.count(); ++j) {
                exists = names.at(j).case_compare(unique.string()) == 0;
              }
              if (exists) {
                unique.reuse();
                if (OB_FAIL(unique.append_fmt("%.*s__%ld", name.length(), name.ptr(), suffix++))) {
                }
              }
            }
            if (OB_SUCC(ret)) {
              ObString final_name;
              if (OB_FAIL(ob_write_string(allocator, unique.string(), final_name))) {
              } else if (OB_FAIL(names.push_back(final_name))) {
              }
            }
          }
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(reader.next())) {
        if (OB_ITER_END == ret) { ret = OB_SUCCESS; }
      }
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
        ObSqlString generated;
        if (OB_FAIL(generated.append_fmt("column%ld", i + 1))) {
        } else {
          ObString name;
          if (OB_FAIL(ob_write_string(allocator, generated.string(), name))) {
          } else if (OB_FAIL(names.push_back(name))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; i < column_count; ++i) {
        FileColumnState state;
        state.nullable_ = false;
        if (OB_FAIL(states.push_back(state))) { break; }
      }
    }
    auto infer_record = [&](const ObIArray<ObString> &fields,
                            const ObIArray<bool> &quoted) -> int {
      int local_ret = OB_SUCCESS;
      if (fields.count() != column_count) {
        local_ret = OB_INVALID_DATA;
      }
      for (int64_t i = 0; OB_SUCC(local_ret) && i < fields.count(); ++i) {
        FileColumnState &state = states.at(i);
        const ObString &value = fields.at(i);
        const bool is_null = value.empty() && !quoted.at(i);
        if (is_null) {
          state.nullable_ = true;
        } else {
          state.type_ = file_merge_type(state.type_, file_detect_type(value));
          state.max_len_ = std::max(state.max_len_, static_cast<int64_t>(value.length()));
        }
      }
      return local_ret;
    };
    if (OB_SUCC(ret) && !header) {
      ret = infer_record(first, reader.quoted());
    }
    while (OB_SUCC(ret) && OB_SUCC(reader.next())) {
      ret = infer_record(reader.fields(), reader.quoted());
    }
    if (OB_ITER_END == ret) { ret = OB_SUCCESS; }
    for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
      FileTableColumn column;
      if (OB_FAIL(make_file_column(allocator, names.at(i), states.at(i).type_,
                                   states.at(i).max_len_, states.at(i).nullable_, column))) {
      } else if (OB_FAIL(columns.push_back(column))) {
      }
    }
  }
  reader.close();
  return ret;
}

enum class JsonValueType : uint8_t
{
  JNULL = 0,
  JBOOL,
  JINT,
  JDOUBLE,
  JSTRING,
};

struct JsonValue
{
  JsonValue() : type_(JsonValueType::JNULL), bool_(false), int_(0), double_(0.0), str_() {}
  JsonValueType type_;
  bool bool_;
  int64_t int_;
  double double_;
  std::string str_;
};

struct JsonField
{
  std::string name_;
  JsonValue value_;
};

struct JsonObject
{
  std::vector<JsonField> fields_;
};

inline void json_skip_ws(const std::string &text, size_t &pos)
{
  while (pos < text.size() && (' ' == text[pos] || '\t' == text[pos]
                               || '\r' == text[pos] || '\n' == text[pos])) {
    ++pos;
  }
}

inline int json_hex_value(char c)
{
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
  if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
  return -1;
}

inline void json_append_utf8(std::string &out, uint32_t cp)
{
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

inline int json_parse_string(const std::string &text, size_t &pos, std::string &out)
{
  int ret = OB_SUCCESS;
  if (pos >= text.size() || '"' != text[pos]) {
    ret = OB_INVALID_DATA;
  } else {
    ++pos;
    while (OB_SUCC(ret) && pos < text.size() && '"' != text[pos]) {
      char c = text[pos++];
      if ('\\' == c) {
        if (pos >= text.size()) {
          ret = OB_INVALID_DATA;
        } else {
          const char esc = text[pos++];
          switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
              if (pos + 4 > text.size()) {
                ret = OB_INVALID_DATA;
                break;
              }
              uint32_t cp = 0;
              for (int i = 0; OB_SUCC(ret) && i < 4; ++i) {
                const int h = json_hex_value(text[pos + i]);
                if (h < 0) { ret = OB_INVALID_DATA; }
                else { cp = (cp << 4) | static_cast<uint32_t>(h); }
              }
              if (OB_SUCC(ret)) {
                pos += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && pos + 6 <= text.size()
                    && '\\' == text[pos] && 'u' == text[pos + 1]) {
                  uint32_t low = 0;
                  bool low_ok = true;
                  for (int i = 0; i < 4; ++i) {
                    const int h = json_hex_value(text[pos + 2 + i]);
                    if (h < 0) { low_ok = false; break; }
                    low = (low << 4) | static_cast<uint32_t>(h);
                  }
                  if (low_ok && low >= 0xDC00 && low <= 0xDFFF) {
                    pos += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                  }
                }
                json_append_utf8(out, cp);
              }
              break;
            }
            default: ret = OB_INVALID_DATA; break;
          }
        }
      } else if (static_cast<unsigned char>(c) < 0x20) {
        ret = OB_INVALID_DATA;
      } else {
        out.push_back(c);
      }
    }
    if (OB_SUCC(ret)) {
      if (pos >= text.size()) { ret = OB_INVALID_DATA; }
      else { ++pos; }
    }
  }
  return ret;
}

inline int json_parse_value(const std::string &text, size_t &pos, JsonValue &value)
{
  int ret = OB_SUCCESS;
  json_skip_ws(text, pos);
  if (pos >= text.size()) {
    ret = OB_INVALID_DATA;
  } else if ('"' == text[pos]) {
    value.type_ = JsonValueType::JSTRING;
    ret = json_parse_string(text, pos, value.str_);
  } else if ('t' == text[pos] && pos + 4 <= text.size()
             && 0 == text.compare(pos, 4, "true")) {
    value.type_ = JsonValueType::JBOOL;
    value.bool_ = true;
    pos += 4;
  } else if ('f' == text[pos] && pos + 5 <= text.size()
             && 0 == text.compare(pos, 5, "false")) {
    value.type_ = JsonValueType::JBOOL;
    value.bool_ = false;
    pos += 5;
  } else if ('n' == text[pos] && pos + 4 <= text.size()
             && 0 == text.compare(pos, 4, "null")) {
    value.type_ = JsonValueType::JNULL;
    pos += 4;
  } else if ('{' == text[pos] || '[' == text[pos]) {
    ret = OB_NOT_SUPPORTED;
  } else {
    const size_t start = pos;
    if ('-' == text[pos]) { ++pos; }
    bool digits = false;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
      digits = true;
      ++pos;
    }
    bool is_double = false;
    if (pos < text.size() && '.' == text[pos]) {
      is_double = true;
      ++pos;
      while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') { ++pos; }
    }
    if (pos < text.size() && ('e' == text[pos] || 'E' == text[pos])) {
      is_double = true;
      ++pos;
      if (pos < text.size() && ('+' == text[pos] || '-' == text[pos])) { ++pos; }
      while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') { ++pos; }
    }
    if (!digits) {
      ret = OB_INVALID_DATA;
    } else {
      const std::string token = text.substr(start, pos - start);
      char *end = nullptr;
      if (is_double) {
        value.type_ = JsonValueType::JDOUBLE;
        value.double_ = strtod(token.c_str(), &end);
      } else {
        value.type_ = JsonValueType::JINT;
        value.int_ = strtoll(token.c_str(), &end, 10);
      }
      if (nullptr == end || '\0' != *end) {
        ret = OB_INVALID_DATA;
      }
    }
  }
  return ret;
}

inline int json_parse_object(const std::string &text, JsonObject &object)
{
  int ret = OB_SUCCESS;
  size_t pos = 0;
  json_skip_ws(text, pos);
  if (pos >= text.size() || '{' != text[pos]) {
    ret = OB_INVALID_DATA;
  } else {
    ++pos;
    json_skip_ws(text, pos);
    if (pos < text.size() && '}' == text[pos]) {
      ++pos;
    } else {
      while (OB_SUCC(ret)) {
        json_skip_ws(text, pos);
        JsonField field;
        if (OB_FAIL(json_parse_string(text, pos, field.name_))) {
          ret = OB_INVALID_DATA;
        } else {
          json_skip_ws(text, pos);
          if (pos >= text.size() || ':' != text[pos]) {
            ret = OB_INVALID_DATA;
          } else {
            ++pos;
            if (OB_FAIL(json_parse_value(text, pos, field.value_))) {
            } else {
              bool replaced = false;
              for (size_t i = 0; !replaced && i < object.fields_.size(); ++i) {
                if (object.fields_[i].name_ == field.name_) {
                  object.fields_[i].value_ = field.value_;
                  replaced = true;
                }
              }
              if (!replaced) {
                object.fields_.push_back(field);
              }
              json_skip_ws(text, pos);
              if (pos >= text.size()) {
                ret = OB_INVALID_DATA;
              } else if ('}' == text[pos]) {
                ++pos;
                break;
              } else if (',' == text[pos]) {
                ++pos;
              } else {
                ret = OB_INVALID_DATA;
              }
            }
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    json_skip_ws(text, pos);
    if (pos != text.size()) { ret = OB_INVALID_DATA; }
  }
  return ret;
}

inline FileInferType json_value_type(const JsonValue &value, int64_t &max_len)
{
  FileInferType type = FileInferType::NULL_TYPE;
  switch (value.type_) {
    case JsonValueType::JBOOL: type = FileInferType::BOOL_TYPE; break;
    case JsonValueType::JINT: type = FileInferType::BIGINT_TYPE; break;
    case JsonValueType::JDOUBLE: type = FileInferType::DOUBLE_TYPE; break;
    case JsonValueType::JSTRING:
      max_len = std::max(max_len, static_cast<int64_t>(value.str_.size()));
      if (file_is_date(ObString(static_cast<int32_t>(value.str_.size()), value.str_.data()))) {
        type = FileInferType::DATE_TYPE;
      } else if (file_is_datetime(ObString(static_cast<int32_t>(value.str_.size()),
                                           value.str_.data()))) {
        type = FileInferType::DATETIME_TYPE;
      } else {
        type = FileInferType::VARCHAR_TYPE;
      }
      break;
    default: type = FileInferType::NULL_TYPE; break;
  }
  return type;
}

int infer_json_schema(ObIAllocator &allocator,
                      const ObString &path,
                      ObIArray<FileTableColumn> &columns)
{
  int ret = OB_SUCCESS;
  std::string path_text(path.ptr(), path.length());
  std::ifstream input(path_text, std::ios::binary);
  std::string line;
  std::vector<std::string> names;
  std::vector<FileColumnState> states;
  int64_t row_count = 0;
  if (!input.is_open()) {
    ret = OB_FILE_NOT_EXIST;
  }
  while (OB_SUCC(ret) && std::getline(input, line)) {
    if (!line.empty() && '\r' == line.back()) { line.pop_back(); }
    if (line.empty()) { continue; }
    JsonObject object;
    if (OB_FAIL(json_parse_object(line, object))) {
      ret = OB_INVALID_DATA;
    } else {
      std::vector<bool> present(names.size(), false);
      for (size_t i = 0; OB_SUCC(ret) && i < object.fields_.size(); ++i) {
        const JsonField &field = object.fields_[i];
        size_t index = names.size();
        for (size_t j = 0; j < names.size(); ++j) {
          if (names[j] == field.name_) { index = j; break; }
        }
        if (index == names.size()) {
          names.push_back(field.name_);
          FileColumnState state;
          if (row_count > 0) { state.nullable_ = true; }
          states.push_back(state);
          present.resize(names.size(), false);
        }
        present[index] = true;
        FileColumnState &state = states[index];
        if (JsonValueType::JNULL == field.value_.type_) {
          state.nullable_ = true;
        } else {
          const FileInferType type = json_value_type(field.value_, state.max_len_);
          state.type_ = file_merge_type(state.type_, type);
        }
      }
      for (size_t i = 0; OB_SUCC(ret) && i < present.size(); ++i) {
        if (!present[i]) { states[i].nullable_ = true; }
      }
      ++row_count;
    }
  }
  if (OB_SUCC(ret) && names.empty()) {
    ret = OB_INVALID_DATA;
  }
  for (size_t i = 0; OB_SUCC(ret) && i < names.size(); ++i) {
    FileTableColumn column;
    if (OB_FAIL(make_file_column(allocator,
                                 ObString(static_cast<int32_t>(names[i].size()), names[i].data()),
                                 states[i].type_, states[i].max_len_,
                                 states[i].nullable_, column))) {
    } else if (OB_FAIL(columns.push_back(column))) {
    }
  }
  return ret;
}

} // anonymous namespace

int infer_file_table_schema(ObIAllocator &allocator,
                            const ObString &path,
                            FileTableFormat format,
                            bool header,
                            ObIArray<FileTableColumn> &columns)
{
  int ret = OB_SUCCESS;
  if (FileTableFormat::CSV == format) {
    ret = infer_csv_schema(allocator, path, header, columns);
  } else if (FileTableFormat::JSONL == format) {
    ret = infer_json_schema(allocator, path, columns);
  } else if (FileTableFormat::GLOB == format) {
    FileTableColumn column;
    if (OB_FAIL(make_file_column(allocator, ObString::make_string("file"),
                                 FileInferType::VARCHAR_TYPE, 1024, false, column))) {
    } else if (OB_FAIL(columns.push_back(column))) {
    }
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "file format");
  }
  return ret;
}

int serialize_file_table_options(ObIAllocator &allocator,
                                 const ObString &path,
                                 FileTableFormat format,
                                 bool header,
                                 const ObIArray<FileTableColumn> &columns,
                                 ObString &options)
{
  int ret = OB_SUCCESS;
  ObSqlString buf;
  if (OB_FAIL(buf.append_fmt("FTS1\n%d\n%d\n%lld\n", static_cast<int>(format),
                             header ? 1 : 0, static_cast<long long>(columns.count())))) {
  } else if (OB_FAIL(buf.append_fmt("%.*s\n", path.length(), path.ptr()))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
      const FileTableColumn &column = columns.at(i);
      if (OB_FAIL(buf.append_fmt("%d\t%lld\t%lld\t%d\t%.*s\n",
                                 static_cast<int>(column.meta_.get_type()),
                                 static_cast<long long>(column.accuracy_.get_precision()),
                                 static_cast<long long>(column.accuracy_.get_scale()),
                                 column.nullable_ ? 1 : 0,
                                 column.name_.length(), column.name_.ptr()))) {
      }
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(ob_write_string(allocator, buf.string(), options))) {
  }
  return ret;
}

int deserialize_file_table_options(ObIAllocator &allocator,
                                   const ObString &options,
                                   ObString &path,
                                   FileTableFormat &format,
                                   bool &header,
                                   ObIArray<FileTableColumn> &columns)
{
  int ret = OB_SUCCESS;
  std::string text(options.ptr(), options.length());
  std::vector<std::string> lines;
  size_t begin = 0;
  while (begin <= text.size()) {
    const size_t end = text.find('\n', begin);
    lines.push_back(text.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
    if (end == std::string::npos) { break; }
    begin = end + 1;
  }
  if (lines.size() < 5 || lines[0] != "FTS1") {
    ret = OB_INVALID_ARGUMENT;
  } else {
    format = static_cast<FileTableFormat>(atoi(lines[1].c_str()));
    header = atoi(lines[2].c_str()) != 0;
    const int64_t count = atoll(lines[3].c_str());
    const std::string &path_text = lines[4];
    if (OB_FAIL(ob_write_string(allocator, ObString(path_text.size(), path_text.data()), path))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
      const size_t index = 5 + i;
      if (index >= lines.size()) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        const std::string &line = lines[index];
        const size_t p1 = line.find('\t');
        const size_t p2 = p1 == std::string::npos ? p1 : line.find('\t', p1 + 1);
        const size_t p3 = p2 == std::string::npos ? p2 : line.find('\t', p2 + 1);
        const size_t p4 = p3 == std::string::npos ? p3 : line.find('\t', p3 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos
            || p3 == std::string::npos || p4 == std::string::npos) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          const int type = atoi(line.substr(0, p1).c_str());
          const int64_t precision = atoll(line.substr(p1 + 1, p2 - p1 - 1).c_str());
          const int64_t scale = atoll(line.substr(p2 + 1, p3 - p2 - 1).c_str());
          const bool nullable = atoi(line.substr(p3 + 1, p4 - p3 - 1).c_str()) != 0;
          const std::string name = line.substr(p4 + 1);
          FileTableColumn column;
          ObString name_copy;
          if (OB_FAIL(ob_write_string(allocator,
                                      ObString(name.size(), name.data()), name_copy))) {
          } else {
            column.name_ = name_copy;
            column.nullable_ = nullable;
            column.meta_.reset();
            column.meta_.set_type(static_cast<ObObjType>(type));
            if (ObVarcharType == type) {
              column.meta_.set_collation_level(CS_LEVEL_IMPLICIT);
              column.meta_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
            }
            column.accuracy_.set_precision(static_cast<ObPrecision>(precision));
            column.accuracy_.set_scale(static_cast<ObScale>(scale));
            if (ObVarcharType == type) {
              column.accuracy_.set_length(static_cast<ObLength>(precision > 0 ? precision : 65535));
            }
            if (OB_FAIL(columns.push_back(column))) {
            }
          }
        }
      }
    }
  }
  return ret;
}

namespace {
class GeneratorTableScan final : public ITableFunctionScan
{
public:
  explicit GeneratorTableScan(const TableFunctionScanContext &context)
    : arguments_(context.arguments_), projection_(context.column_ids_), limit_(0), position_(0) {}
  int open() override { return rescan(arguments_); }
  int rescan(const ObIArray<ObObj> &arguments) override
  {
    int ret = OB_SUCCESS;
    position_ = 0;
    if (arguments.count() != 1 || arguments.at(0).is_null()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "generator requires a non-NULL integer");
    } else {
      limit_ = arguments.at(0).get_int();
      row_.cells_ = &value_;
      row_.count_ = projection_.count();
    }
    return ret;
  }
  int get_next_row(ObNewRow *&row) override
  {
    int ret = OB_SUCCESS;
    if (position_ >= limit_) {
      ret = OB_ITER_END;
    } else {
      value_.set_int(++position_);
      row = &row_;
    }
    return ret;
  }
  int close() override { position_ = 0; return OB_SUCCESS; }
private:
  const ObIArray<ObObj> &arguments_;
  const ObIArray<uint64_t> &projection_;
  int64_t limit_;
  int64_t position_;
  ObObj value_;
  ObNewRow row_;
};

class CollectionTableScan final : public ITableFunctionScan
{
  struct Input {
    Input() : array_(nullptr), collection_(nullptr), position_(0), columns_(0) {}
    ObIArrayType *array_;
    pl::ObPLCollection *collection_;
    int64_t position_;
    int64_t columns_;
  };
public:
  explicit CollectionTableScan(const TableFunctionScanContext &context)
    : exec_ctx_(context.exec_ctx_), arguments_(context.arguments_), projection_(context.column_ids_),
      column_counts_(context.argument_column_counts_), input_allocator_("TableFuncInput"),
      row_allocator_("TableFuncRow") {}
  int open() override { return rescan(arguments_); }
  int rescan(const ObIArray<ObObj> &arguments) override
  {
    int ret = OB_SUCCESS;
    inputs_.reuse();
    values_.reuse();
    cells_.reuse();
    input_allocator_.reuse();
    row_allocator_.reuse();
    if (arguments.count() != column_counts_.count()) { ret = OB_ERR_UNEXPECTED; }
    int64_t total_columns = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < arguments.count(); ++i) {
      Input input;
      input.columns_ = column_counts_.at(i);
      total_columns += input.columns_;
      const ObObj &argument = arguments.at(i);
      if (argument.is_null()) {
      } else if (argument.is_collection_sql_type()) {
        ObCollectionTypeBase *type = nullptr;
        ObString bytes = argument.get_string();
        OZ (ObArrayExprUtils::get_coll_type_by_subschema_id(&exec_ctx_, argument.get_udt_subschema_id(), type));
        OZ (ObArrayTypeObjFactory::construct(input_allocator_, *type, input.array_));
        OZ (ObTextStringHelper::read_real_string_data(exec_ctx_, &input_allocator_,
            ObCollectionSQLType, CS_TYPE_BINARY, true, bytes));
        OZ (input.array_->init(bytes));
      } else if (argument.is_pl_extend()) {
        auto *composite = reinterpret_cast<pl::ObPLComposite *>(argument.get_ext());
        if (composite == nullptr) { // typed NULL composite
        } else if (!composite->is_collection()) { ret = OB_ERR_INVALID_TYPE_FOR_ARGUMENT; }
        else if (!composite->is_null()) { input.collection_ = static_cast<pl::ObPLCollection *>(composite); }
      } else { ret = OB_ERR_INVALID_TYPE_FOR_ARGUMENT; }
      OZ (inputs_.push_back(input));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < total_columns; ++i) { OZ (values_.push_back(ObObj())); }
    for (int64_t i = 0; OB_SUCC(ret) && i < projection_.count(); ++i) { OZ (cells_.push_back(ObObj())); }
    return ret;
  }
  int get_next_row(ObNewRow *&row) override
  {
    int ret = OB_SUCCESS;
    bool any = false;
    row_allocator_.reuse();
    for (int64_t i = 0; i < values_.count(); ++i) { values_.at(i).set_null(); }
    int64_t offset = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < inputs_.count(); ++i) {
      Input &input = inputs_.at(i);
      if (input.array_ != nullptr && input.position_ < input.array_->size()) {
        any = true;
        const int64_t position = input.position_++;
        if (!input.array_->is_null(position)) {
          if (input.array_->get_format() == Nested_Array) {
            const ObCollectionArrayType *type = static_cast<const ObCollectionArrayType *>(input.array_->get_array_type());
            ObIArrayType *child = nullptr;
            ObString bytes;
            OZ (ObArrayTypeObjFactory::construct(row_allocator_, *type->element_type_, child));
            OZ (input.array_->at(position, *child));
            OZ (ObArrayExprUtils::set_array_res(child, child->get_raw_binary_len(), row_allocator_, bytes));
            if (OB_SUCC(ret)) {
              values_.at(offset).set_varchar(bytes);
              values_.at(offset).set_type(ObCollectionSQLType);
              values_.at(offset).set_has_lob_header();
            }
          } else { OZ (input.array_->elem_at(position, values_.at(offset))); }
        }
      } else if (input.collection_ != nullptr && input.collection_->is_inited()) {
        pl::ObPLCollection &collection = *input.collection_;
        ObObj *elements = static_cast<ObObj *>(collection.get_data());
        // Deleted PL collection elements are holes, not NULL elements.
        while (input.position_ < collection.get_count() &&
               elements[input.position_].get_type() == ObMaxType) { ++input.position_; }
        if (input.position_ < collection.get_count()) {
          any = true;
          const ObObj &element = elements[input.position_++];
          if (element.is_null() || (element.is_pl_extend() && element.get_ext() == 0)) {
          } else if (element.is_pl_extend() && element.get_ext() != 0 &&
                     reinterpret_cast<pl::ObPLComposite *>(element.get_ext())->is_record()) {
            auto *record = reinterpret_cast<pl::ObPLRecord *>(element.get_ext());
            if (!record->is_null() && record->get_count() != input.columns_) { ret = OB_ERR_UNEXPECTED; }
            for (int64_t j = 0; OB_SUCC(ret) && !record->is_null() && record->is_inited() && j < input.columns_; ++j) {
              OZ (record->get_element(j, values_.at(offset + j)));
            }
          } else if (input.columns_ == 1) {
            values_.at(offset) = element;
          } else { ret = OB_ERR_UNEXPECTED; }
        }
      }
      offset += input.columns_;
    }
    if (OB_SUCC(ret) && !any) { ret = OB_ITER_END; }
    for (int64_t i = 0; OB_SUCC(ret) && i < projection_.count(); ++i) {
      const int64_t position = projection_.at(i) - OB_APP_MIN_COLUMN_ID;
      if (position < 0 || position >= values_.count()) { ret = OB_ERR_UNEXPECTED; }
      else { cells_.at(i) = values_.at(position); }
    }
    if (OB_SUCC(ret)) {
      row_.cells_ = cells_.get_data();
      row_.count_ = cells_.count();
      row = &row_;
    }
    return ret;
  }
  int close() override
  {
    inputs_.reuse();
    input_allocator_.reuse();
    row_allocator_.reuse();
    return OB_SUCCESS;
  }
private:
  ObExecContext &exec_ctx_;
  const ObIArray<ObObj> &arguments_;
  const ObIArray<uint64_t> &projection_;
  const ObIArray<int64_t> &column_counts_;
  ObArenaAllocator input_allocator_;
  ObArenaAllocator row_allocator_;
  ObSEArray<Input, 4> inputs_;
  ObSEArray<ObObj, 8> values_;
  ObSEArray<ObObj, 8> cells_;
  ObNewRow row_;
};

inline bool file_glob_match(const char *name, const char *pattern)
{
  while ('\0' != *pattern) {
    if ('*' == *pattern) {
      while ('*' == *pattern) { ++pattern; }
      if ('\0' == *pattern) { return true; }
      for (const char *p = name; '\0' != *p; ++p) {
        if (file_glob_match(p, pattern)) { return true; }
      }
      return false;
    } else if ('?' == *pattern) {
      if ('\0' == *name) { return false; }
      ++name;
      ++pattern;
    } else {
      if (*name != *pattern) { return false; }
      ++name;
      ++pattern;
    }
  }
  return '\0' == *name;
}

class FileTableScan final : public ITableFunctionScan
{
public:
  explicit FileTableScan(const TableFunctionScanContext &context)
    : exec_ctx_(context.exec_ctx_), allocator_(context.allocator_),
      column_ids_(context.column_ids_), options_(context.options_),
      path_(), format_(FileTableFormat::CSV), header_(true),
      reader_(context.allocator_), json_in_(), json_path_(),
      columns_(), values_(), cells_(), row_(),
      glob_files_(), glob_pos_(0), opened_(false) {}
  int open() override
  {
    int ret = OB_SUCCESS;
    if (OB_FAIL(load_options())) {
      LOG_WARN("failed to load file table options", K(ret));
    } else if (OB_FAIL(check_access())) {
      LOG_WARN("failed to check file table access", K(ret));
    } else if (OB_FAIL(FileTableFormat::GLOB == format_ ? open_glob() : open_reader())) {
      LOG_WARN("failed to open file table source", K(ret), K(format_));
    } else {
      opened_ = true;
    }
    return ret;
  }
  int rescan(const ObIArray<ObObj> &) override
  {
    int ret = OB_SUCCESS;
    close();
    if (OB_FAIL(open())) {
    }
    return ret;
  }
  int get_next_row(ObNewRow *&row) override
  {
    int ret = OB_SUCCESS;
    row = nullptr;
    if (!opened_) {
      ret = OB_NOT_INIT;
    } else if (FileTableFormat::GLOB == format_) {
      if (glob_pos_ >= glob_files_.count()) {
        ret = OB_ITER_END;
      } else if (values_.count() != 1) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        values_.at(0).set_varchar(glob_files_.at(glob_pos_++));
        for (int64_t i = 0; OB_SUCC(ret) && i < cells_.count(); ++i) {
          cells_.at(i) = values_.at(0);
        }
        if (OB_SUCC(ret)) {
          row_.cells_ = cells_.get_data();
          row_.count_ = cells_.count();
          row = &row_;
        }
      }
    } else if (FileTableFormat::JSONL == format_) {
      JsonObject &object = json_object_;
      if (OB_FAIL(next_json_object(object))) {
        if (OB_ITER_END != ret) {
          LOG_WARN("failed to read json record", K(ret));
        }
      } else if (values_.count() != columns_.count()) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
          const JsonField *found = nullptr;
          for (size_t j = 0; nullptr == found && j < object.fields_.size(); ++j) {
            if (object.fields_[j].name_ == std::string(columns_.at(i).name_.ptr(),
                                                       columns_.at(i).name_.length())) {
              found = &object.fields_[j];
            }
          }
          if (nullptr == found) {
            values_.at(i).set_null();
          } else if (OB_FAIL(make_json_obj(columns_.at(i), found->value_, values_.at(i)))) {
          }
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < cells_.count(); ++i) {
          const int64_t position = static_cast<int64_t>(column_ids_.at(i)) - OB_APP_MIN_COLUMN_ID;
          if (position < 0 || position >= values_.count()) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            cells_.at(i) = values_.at(position);
          }
        }
        if (OB_SUCC(ret)) {
          row_.cells_ = cells_.get_data();
          row_.count_ = cells_.count();
          row = &row_;
        }
      }
    } else if (FileTableFormat::CSV != format_) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "file format");
    } else if (OB_FAIL(reader_.next())) {
      if (OB_ITER_END == ret) {
        // end of file
      } else {
        LOG_WARN("failed to read csv record", K(ret));
      }
    } else {
      const ObIArray<ObString> &fields = reader_.fields();
      const ObIArray<bool> &quoted = reader_.quoted();
      if (fields.count() != columns_.count() || fields.count() != values_.count()) {
        ret = OB_INVALID_DATA;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); ++i) {
        if (OB_FAIL(make_obj(columns_.at(i), fields.at(i), quoted.at(i), values_.at(i)))) {
        }
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < cells_.count(); ++i) {
        const int64_t position = static_cast<int64_t>(column_ids_.at(i)) - OB_APP_MIN_COLUMN_ID;
        if (position < 0 || position >= values_.count()) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          cells_.at(i) = values_.at(position);
        }
      }
      if (OB_SUCC(ret)) {
        row_.cells_ = cells_.get_data();
        row_.count_ = cells_.count();
        row = &row_;
      }
    }
    return ret;
  }
  int close() override
  {
    reader_.close();
    if (json_in_.is_open()) { json_in_.close(); }
    glob_files_.reuse();
    glob_pos_ = 0;
    opened_ = false;
    return OB_SUCCESS;
  }
private:
  int load_options()
  {
    int ret = OB_SUCCESS;
    FileTableFormat format = FileTableFormat::CSV;
    bool header = true;
    ObSEArray<FileTableColumn, 16> columns;
    if (OB_FAIL(deserialize_file_table_options(allocator_, options_,
                                               path_, format, header, columns))) {
      LOG_WARN("failed to deserialize file table options", K(ret));
    } else if (OB_FAIL(columns_.assign(columns))) {
    } else if (OB_FAIL(values_.prepare_allocate(columns_.count()))) {
    } else if (OB_FAIL(cells_.prepare_allocate(column_ids_.count()))) {
    } else {
      format_ = format;
      header_ = header;
    }
    return ret;
  }
  int check_access()
  {
    int ret = OB_SUCCESS;
    ObSQLSessionInfo *session = exec_ctx_.get_my_session();
    bool enabled = false;
    ObString secure_priv;
    if (OB_ISNULL(session)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(session->get_enable_file_sql(enabled))) {
    } else if (!enabled) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "file sql is disabled, set enable_file_sql = ON first");
    } else if (OB_FAIL(session->get_secure_file_priv(secure_priv))) {
    } else if (OB_FAIL(ObResolverUtils::check_secure_path(secure_priv, path_))) {
    }
    return ret;
  }
  int open_reader()
  {
    int ret = OB_SUCCESS;
    if (FileTableFormat::CSV == format_) {
      if (OB_FAIL(reader_.open(path_))) {
      } else if (header_ && OB_FAIL(reader_.next())) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    } else if (FileTableFormat::JSONL == format_) {
      json_path_.assign(path_.ptr(), path_.length());
      json_in_.open(json_path_, std::ios::binary);
      if (!json_in_.is_open()) {
        ret = OB_FILE_NOT_EXIST;
      }
    } else {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "file format");
    }
    return ret;
  }
  int open_glob()
  {
    int ret = OB_SUCCESS;
    glob_files_.reuse();
    glob_pos_ = 0;
    std::string pattern(path_.ptr(), path_.length());
    std::filesystem::path pattern_path(pattern);
    std::string dir = pattern_path.parent_path().string();
    std::string filter = pattern_path.filename().string();
    if (dir.empty()) { dir = "."; }
    std::error_code ec;
    std::vector<std::string> files;
    if (!std::filesystem::exists(dir, ec) || ec) {
      ret = OB_FILE_NOT_EXIST;
    } else {
      if (std::string::npos == filter.find('*') && std::string::npos == filter.find('?')) {
        if (std::filesystem::is_regular_file(pattern_path, ec) && !ec) {
          std::string full = pattern_path.string();
          for (size_t j = 0; j < full.size(); ++j) {
            if ('\\' == full[j]) { full[j] = '/'; }
          }
          files.push_back(full);
        }
      } else {
        for (std::filesystem::directory_iterator it(dir, ec), end;
             OB_SUCC(ret) && !ec && it != end; it.increment(ec)) {
          std::error_code file_ec;
          if (!it->is_regular_file(file_ec) || file_ec) {
            continue;
          }
          const std::string name = it->path().filename().string();
          if (file_glob_match(name.c_str(), filter.c_str())) {
            std::string full = it->path().string();
            for (size_t j = 0; j < full.size(); ++j) {
              if ('\\' == full[j]) { full[j] = '/'; }
            }
            files.push_back(full);
          }
        }
        if (ec) {
          ret = OB_IO_ERROR;
        }
      }
    }
    if (OB_SUCC(ret)) {
      std::sort(files.begin(), files.end());
      for (size_t i = 0; OB_SUCC(ret) && i < files.size(); ++i) {
        ObString file;
        if (OB_FAIL(ob_write_string(allocator_,
                                    ObString(static_cast<int32_t>(files[i].size()),
                                             files[i].data()),
                                    file))) {
        } else if (OB_FAIL(glob_files_.push_back(file))) {
        }
      }
    }
    return ret;
  }
  int next_json_object(JsonObject &object)
  {
    int ret = OB_SUCCESS;
    object.fields_.clear();
    std::string line;
    bool got = false;
    while (!got && std::getline(json_in_, line)) {
      if (!line.empty() && '\r' == line.back()) { line.pop_back(); }
      if (line.empty()) { continue; }
      if (OB_FAIL(json_parse_object(line, object))) {
        LOG_WARN("failed to parse JSON line", K(ret));
      } else {
        got = true;
      }
    }
    if (OB_SUCC(ret) && !got) {
      ret = OB_ITER_END;
    }
    return ret;
  }
  int make_json_obj(const FileTableColumn &column, const JsonValue &value, ObObj &obj)
  {
    int ret = OB_SUCCESS;
    if (JsonValueType::JNULL == value.type_) {
      obj.set_null();
    } else if (JsonValueType::JBOOL == value.type_) {
      obj.set_bool(value.bool_);
    } else if (JsonValueType::JINT == value.type_) {
      if (ObDoubleType == column.meta_.get_type()) {
        obj.set_double(static_cast<double>(value.int_));
      } else {
        obj.set_int(value.int_);
      }
    } else if (JsonValueType::JDOUBLE == value.type_) {
      obj.set_double(value.double_);
    } else if (JsonValueType::JSTRING == value.type_) {
      ret = make_obj(column,
                     ObString(static_cast<int32_t>(value.str_.size()), value.str_.data()),
                     true, obj);
    } else {
      ret = OB_INVALID_DATA;
    }
    return ret;
  }
  int make_obj(const FileTableColumn &column,
               const ObString &field,
               bool quoted,
               ObObj &obj)
  {
    int ret = OB_SUCCESS;
    const ObObjType type = column.meta_.get_type();
    obj.set_null();
    if (field.empty() && !quoted) {
      // NULL
    } else if (ObTinyIntType == type) {
      bool value = false;
      if (!file_is_bool(field, value)) {
        ret = OB_INVALID_DATA;
      } else {
        obj.set_bool(value);
      }
    } else if (ObIntType == type) {
      int64_t value = 0;
      if (!file_parse_int(field, value)) {
        ret = OB_INVALID_DATA;
      } else {
        obj.set_int(value);
      }
    } else if (ObDoubleType == type) {
      double value = 0.0;
      if (!file_parse_double(field, value)) {
        ret = OB_INVALID_DATA;
      } else {
        obj.set_double(value);
      }
    } else if (ObDateType == type) {
      int32_t value = 0;
      if (OB_FAIL(ObTimeConverter::str_to_date(field, value))) {
      } else {
        obj.set_date(value);
      }
    } else if (ObDateTimeType == type) {
      int64_t value = 0;
      ObTimeConvertCtx ctx(nullptr, false);
      if (OB_FAIL(ObTimeConverter::str_to_datetime(field, ctx, value, nullptr))) {
      } else {
        obj.set_datetime(value);
      }
    } else {
      obj.set_varchar(field);
      obj.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    if (OB_FAIL(ret)) {
      LOG_WARN("failed to convert file column", K(ret), K(type), K(field));
    }
    return ret;
  }
private:
  ObExecContext &exec_ctx_;
  ObIAllocator &allocator_;
  const ObIArray<uint64_t> &column_ids_;
  const ObString &options_;
  ObString path_;
  FileTableFormat format_;
  bool header_;
  CsvRecordReader reader_;
  std::ifstream json_in_;
  std::string json_path_;
  JsonObject json_object_;
  ObSEArray<FileTableColumn, 16> columns_;
  ObSEArray<ObObj, 16> values_;
  ObSEArray<ObObj, 16> cells_;
  ObNewRow row_;
  ObSEArray<ObString, 32> glob_files_;
  int64_t glob_pos_;
  bool opened_;
};
}

int create_file_table_scan(const TableFunctionScanContext &context, ITableFunctionScan *&scan)
{
  void *memory = context.allocator_.alloc(sizeof(FileTableScan));
  if (memory == nullptr) { return OB_ALLOCATE_MEMORY_FAILED; }
  scan = new (memory) FileTableScan(context);
  return OB_SUCCESS;
}

int create_generator_table_scan(const TableFunctionScanContext &context, ITableFunctionScan *&scan)
{
  void *memory = context.allocator_.alloc(sizeof(GeneratorTableScan));
  if (memory == nullptr) { return OB_ALLOCATE_MEMORY_FAILED; }
  scan = new (memory) GeneratorTableScan(context);
  return OB_SUCCESS;
}

int create_collection_table_scan(const TableFunctionScanContext &context, ITableFunctionScan *&scan)
{
  void *memory = context.allocator_.alloc(sizeof(CollectionTableScan));
  if (memory == nullptr) { return OB_ALLOCATE_MEMORY_FAILED; }
  scan = new (memory) CollectionTableScan(context);
  return OB_SUCCESS;
}
} // namespace sql
} // namespace oceanbase
