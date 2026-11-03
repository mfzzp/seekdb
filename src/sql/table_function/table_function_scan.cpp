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
#ifndef _WIN32
#include <dirent.h>
#endif
#include <string>
#include <vector>
#include <algorithm>
#ifdef _WIN32
#include <filesystem>
#endif
#include <fstream>
#include <map>

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
        const bool is_null = (value.empty() && !quoted.at(i))
                             || 0 == value.case_compare("NULL");
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

// Minimal Thrift compact protocol reader for Parquet metadata.
struct ThriftNode
{
  enum Kind : uint8_t { NONE = 0, BOOL, I64, DOUBLE, BINARY, LIST, STRUCT };
  ThriftNode() : kind_(NONE), i_(0), d_(0.0), s_(), list_(), fields_() {}
  Kind kind_;
  int64_t i_;
  double d_;
  std::string s_;
  std::vector<ThriftNode> list_;
  std::map<int16_t, ThriftNode> fields_;
};

class CompactReader
{
public:
  CompactReader(const char *data, int64_t size) : p_(data), end_(data + size) {}
  int read_struct(ThriftNode &node)
  {
    int ret = OB_SUCCESS;
    int16_t last_id = 0;
    node.kind_ = ThriftNode::STRUCT;
    while (OB_SUCC(ret)) {
      if (p_ >= end_) { ret = OB_INVALID_DATA; break; }
      const uint8_t header = static_cast<uint8_t>(*p_++);
      if (0 == header) { break; }
      const uint8_t type = header & 0x0F;
      uint8_t delta = header >> 4;
      int16_t field_id = 0;
      if (0 == delta) {
        int64_t zig = 0;
        if (OB_FAIL(read_zigzag(zig))) { break; }
        field_id = static_cast<int16_t>(zig);
      } else {
        field_id = static_cast<int16_t>(last_id + delta);
      }
      last_id = field_id;
      ThriftNode value;
      if (OB_FAIL(read_value(type, value))) { break; }
      node.fields_[field_id] = value;
    }
    return ret;
  }
  const char *pos() const { return p_; }
  int64_t remain() const { return end_ - p_; }
private:
  int read_u8(uint8_t &v)
  {
    int ret = OB_SUCCESS;
    if (p_ >= end_) { ret = OB_INVALID_DATA; }
    else { v = static_cast<uint8_t>(*p_++); }
    return ret;
  }
  int read_varint(uint64_t &v)
  {
    int ret = OB_SUCCESS;
    v = 0;
    int shift = 0;
    bool done = false;
    while (OB_SUCC(ret) && !done) {
      uint8_t b = 0;
      if (OB_FAIL(read_u8(b))) {
      } else if (shift >= 64) {
        ret = OB_INVALID_DATA;
      } else {
        v |= (static_cast<uint64_t>(b & 0x7F) << shift);
        done = 0 == (b & 0x80);
        shift += 7;
      }
    }
    return ret;
  }
  int read_zigzag(int64_t &v)
  {
    uint64_t u = 0;
    int ret = read_varint(u);
    if (OB_SUCC(ret)) {
      v = static_cast<int64_t>((u >> 1) ^ (0 == (u & 1) ? 0 : ~static_cast<uint64_t>(0)));
    }
    return ret;
  }
  int read_bytes(std::string &s)
  {
    int ret = OB_SUCCESS;
    uint64_t len = 0;
    if (OB_FAIL(read_varint(len))) {
    } else if (len > static_cast<uint64_t>(end_ - p_)) {
      ret = OB_INVALID_DATA;
    } else {
      s.assign(p_, static_cast<size_t>(len));
      p_ += len;
    }
    return ret;
  }
  int read_value(uint8_t type, ThriftNode &node)
  {
    int ret = OB_SUCCESS;
    switch (type) {
      case 1: node.kind_ = ThriftNode::BOOL; node.i_ = 1; break;
      case 2: node.kind_ = ThriftNode::BOOL; node.i_ = 0; break;
      case 3: {
        uint8_t v = 0;
        if (OB_FAIL(read_u8(v))) { } else { node.kind_ = ThriftNode::I64; node.i_ = static_cast<int8_t>(v); }
        break;
      }
      case 4:
      case 5:
      case 6: {
        int64_t v = 0;
        if (OB_FAIL(read_zigzag(v))) { } else { node.kind_ = ThriftNode::I64; node.i_ = v; }
        break;
      }
      case 7: {
        if (p_ + 8 > end_) { ret = OB_INVALID_DATA; }
        else {
          uint64_t bits = 0;
          for (int i = 0; i < 8; ++i) {
            bits |= static_cast<uint64_t>(static_cast<uint8_t>(*p_++)) << (8 * i);
          }
          node.kind_ = ThriftNode::DOUBLE;
          MEMCPY(&node.d_, &bits, sizeof(bits));
        }
        break;
      }
      case 8: {
        node.kind_ = ThriftNode::BINARY;
        ret = read_bytes(node.s_);
        break;
      }
      case 9:
      case 10: {
        uint8_t header = 0;
        if (OB_FAIL(read_u8(header))) { break; }
        uint64_t count = header >> 4;
        const uint8_t elem_type = header & 0x0F;
        if (15 == count) {
          if (OB_FAIL(read_varint(count))) { break; }
        }
        node.kind_ = ThriftNode::LIST;
        for (uint64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
          ThriftNode elem;
          if (OB_FAIL(read_value(elem_type, elem))) { break; }
          node.list_.push_back(elem);
        }
        break;
      }
      case 11: {
        uint8_t header = 0;
        if (OB_FAIL(read_u8(header))) { break; }
        uint64_t count = header >> 4;
        if (15 == count && OB_FAIL(read_varint(count))) { break; }
        uint8_t key_type = 0;
        uint8_t value_type = 0;
        if (OB_FAIL(read_u8(key_type)) || OB_FAIL(read_u8(value_type))) { break; }
        node.kind_ = ThriftNode::LIST;
        for (uint64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
          ThriftNode k, v;
          if (OB_FAIL(read_value(key_type, k)) || OB_FAIL(read_value(value_type, v))) { break; }
        }
        break;
      }
      case 12: {
        node.kind_ = ThriftNode::STRUCT;
        ret = read_struct(node);
        break;
      }
      default: ret = OB_INVALID_DATA; break;
    }
    return ret;
  }
private:
  const char *p_;
  const char *end_;
};

inline const ThriftNode *thrift_field(const ThriftNode &node, int16_t id)
{
  auto it = node.fields_.find(id);
  return it == node.fields_.end() ? nullptr : &it->second;
}

inline int64_t thrift_i64(const ThriftNode *node, int64_t def = 0)
{
  return node != nullptr && ThriftNode::I64 == node->kind_ ? node->i_ : def;
}

inline bool thrift_bool(const ThriftNode *node, bool def = false)
{
  return node != nullptr && ThriftNode::BOOL == node->kind_ ? (0 != node->i_) : def;
}

inline std::string thrift_str(const ThriftNode *node)
{
  return node != nullptr && ThriftNode::BINARY == node->kind_ ? node->s_ : std::string();
}

struct ParquetColumn
{
  std::string name_;
  int32_t parquet_type_ = 0;
  int32_t repetition_ = 0;  // 0 required, 1 optional, 2 repeated
  int32_t max_def_ = 0;
  int32_t converted_type_ = -1;
};

struct ParquetColumnChunk
{
  int32_t type_ = 0;
  int32_t codec_ = 0;
  int64_t num_values_ = 0;
  int64_t data_page_offset_ = 0;
  int64_t dictionary_page_offset_ = 0;
  int64_t total_compressed_size_ = 0;
};

struct ParquetRowGroup
{
  int64_t num_rows_ = 0;
  std::vector<ParquetColumnChunk> columns_;
};

struct ParquetFile
{
  std::vector<ParquetColumn> columns_;
  std::vector<ParquetRowGroup> row_groups_;
  int64_t num_rows_ = 0;
};

inline int parquet_type_to_file_type(int32_t type, int32_t converted, FileInferType &file_type)
{
  int ret = OB_SUCCESS;
  switch (type) {
    case 0: file_type = FileInferType::BOOL_TYPE; break;
    case 1: file_type = FileInferType::BIGINT_TYPE; break;
    case 2: file_type = FileInferType::BIGINT_TYPE; break;
    case 3: ret = OB_NOT_SUPPORTED; break;
    case 4: file_type = FileInferType::DOUBLE_TYPE; break;
    case 5: file_type = FileInferType::DOUBLE_TYPE; break;
    case 6: file_type = FileInferType::VARCHAR_TYPE; break;
    case 7: file_type = FileInferType::VARCHAR_TYPE; break;
    default: ret = OB_NOT_SUPPORTED; break;
  }
  if (OB_SUCC(ret) && 6 == converted && (1 == type || 2 == type)) {
    file_type = FileInferType::DATE_TYPE;
  }
  return ret;
}

inline int read_whole_file(const ObString &path, std::string &data)
{
  int ret = OB_SUCCESS;
  std::string path_text(path.ptr(), path.length());
  std::ifstream input(path_text, std::ios::binary);
  if (!input.is_open()) {
    ret = OB_FILE_NOT_EXIST;
  } else {
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) {
      ret = OB_IO_ERROR;
    } else {
      data.resize(static_cast<size_t>(size));
      input.seekg(0, std::ios::beg);
      if (size > 0) { input.read(&data[0], size); }
      if (!input) { ret = OB_IO_ERROR; }
    }
  }
  return ret;
}

inline void parquet_add_schema(const std::vector<ThriftNode> &schema,
                               int64_t index,
                               int32_t parent_max_def,
                               std::vector<ParquetColumn> &columns)
{
  if (index < 0 || index >= static_cast<int64_t>(schema.size())) { return; }
  const ThriftNode &element = schema[static_cast<size_t>(index)];
  const ThriftNode *name_node = thrift_field(element, 4);
  const ThriftNode *children_node = thrift_field(element, 5);
  const int32_t repetition = static_cast<int32_t>(thrift_i64(thrift_field(element, 3), 0));
  const int32_t max_def = parent_max_def + (0 == repetition ? 0 : 1);
  const int32_t num_children = static_cast<int32_t>(thrift_i64(children_node, 0));
  if (num_children <= 0) {
    ParquetColumn column;
    column.name_ = thrift_str(name_node);
    column.parquet_type_ = static_cast<int32_t>(thrift_i64(thrift_field(element, 1), -1));
    column.repetition_ = repetition;
    column.max_def_ = max_def;
    column.converted_type_ = static_cast<int32_t>(thrift_i64(thrift_field(element, 6), -1));
    columns.push_back(column);
  } else {
    int64_t child = index + 1;
    for (int32_t i = 0; i < num_children; ++i) {
      parquet_add_schema(schema, child, max_def, columns);
      const ThriftNode &child_element = schema[static_cast<size_t>(child)];
      const int32_t grandchildren = static_cast<int32_t>(
          thrift_i64(thrift_field(child_element, 5), 0));
      child += 1 + (grandchildren > 0 ? grandchildren : 0);
    }
  }
}

inline int parse_parquet_metadata(const std::string &data, ParquetFile &file)
{
  int ret = OB_SUCCESS;
  if (data.size() < 12 || 0 != data.compare(0, 4, "PAR1")
      || 0 != data.compare(data.size() - 4, 4, "PAR1")) {
    ret = OB_INVALID_DATA;
  } else {
    uint32_t footer_len = 0;
    for (int i = 0; i < 4; ++i) {
      footer_len |= static_cast<uint32_t>(static_cast<uint8_t>(data[data.size() - 8 + i]))
                    << (8 * i);
    }
    if (footer_len > data.size() - 8) {
      ret = OB_INVALID_DATA;
    } else {
      const int64_t footer_start = static_cast<int64_t>(data.size() - 8 - footer_len);
      CompactReader reader(data.data() + footer_start, footer_len);
      ThriftNode meta;
      if (OB_FAIL(reader.read_struct(meta))) {
      } else {
        file.num_rows_ = thrift_i64(thrift_field(meta, 3), 0);
        const ThriftNode *schema = thrift_field(meta, 2);
        if (nullptr == schema || ThriftNode::LIST != schema->kind_ || schema->list_.empty()) {
          ret = OB_INVALID_DATA;
        } else {
          parquet_add_schema(schema->list_, 0, 0, file.columns_);
        }
        const ThriftNode *row_groups = thrift_field(meta, 4);
        if (OB_SUCC(ret) && nullptr != row_groups && ThriftNode::LIST == row_groups->kind_) {
          for (size_t i = 0; OB_SUCC(ret) && i < row_groups->list_.size(); ++i) {
            const ThriftNode &rg_node = row_groups->list_[i];
            ParquetRowGroup rg;
            rg.num_rows_ = thrift_i64(thrift_field(rg_node, 3), 0);
            const ThriftNode *cols = thrift_field(rg_node, 1);
            if (nullptr != cols && ThriftNode::LIST == cols->kind_) {
              for (size_t j = 0; j < cols->list_.size(); ++j) {
                const ThriftNode &cc = cols->list_[j];
                const ThriftNode *meta_node = thrift_field(cc, 3);
                if (nullptr == meta_node) { continue; }
                ParquetColumnChunk chunk;
                chunk.type_ = static_cast<int32_t>(thrift_i64(thrift_field(*meta_node, 1), 0));
                chunk.codec_ = static_cast<int32_t>(thrift_i64(thrift_field(*meta_node, 4), 0));
                chunk.num_values_ = thrift_i64(thrift_field(*meta_node, 5), 0);
                chunk.data_page_offset_ = thrift_i64(thrift_field(*meta_node, 9), 0);
                chunk.dictionary_page_offset_ = thrift_i64(thrift_field(*meta_node, 11), 0);
                chunk.total_compressed_size_ = thrift_i64(thrift_field(*meta_node, 7), 0);
                rg.columns_.push_back(chunk);
              }
            }
            file.row_groups_.push_back(rg);
          }
        }
      }
    }
  }
  return ret;
}

inline int infer_parquet_schema(ObIAllocator &allocator,
                                const ObString &path,
                                ObIArray<FileTableColumn> &columns)
{
  int ret = OB_SUCCESS;
  std::string data;
  ParquetFile file;
  if (OB_FAIL(read_whole_file(path, data))) {
  } else if (OB_FAIL(parse_parquet_metadata(data, file))) {
  } else {
    for (size_t i = 0; OB_SUCC(ret) && i < file.columns_.size(); ++i) {
      const ParquetColumn &source = file.columns_[i];
      FileInferType type = FileInferType::VARCHAR_TYPE;
      if (OB_FAIL(parquet_type_to_file_type(source.parquet_type_, source.converted_type_, type))) {
      } else {
        FileTableColumn column;
        if (OB_FAIL(make_file_column(allocator,
                                     ObString(static_cast<int32_t>(source.name_.size()),
                                              source.name_.data()),
                                     type, 65535, 1 == source.repetition_, column))) {
        } else if (OB_FAIL(columns.push_back(column))) {
        }
      }
    }
  }
  return ret;
}

inline int snappy_decompress(const char *src, int64_t src_len, std::string &out)
{
  int ret = OB_SUCCESS;
  const uint8_t *p = reinterpret_cast<const uint8_t *>(src);
  const uint8_t *end = p + src_len;
  uint64_t expected = 0;
  int shift = 0;
  while (p < end && 0 != (*p & 0x80)) {
    expected |= static_cast<uint64_t>(*p & 0x7F) << shift;
    shift += 7;
    ++p;
  }
  if (p >= end) {
    ret = OB_INVALID_DATA;
  } else {
    expected |= static_cast<uint64_t>(*p++) << shift;
    out.clear();
    out.reserve(static_cast<size_t>(expected));
    while (OB_SUCC(ret) && p < end && out.size() < expected) {
      const uint8_t tag = *p++;
      const uint8_t type = tag & 0x03;
      if (0 == type) {
        uint64_t len = tag >> 2;
        if (len < 60) {
          len += 1;
        } else {
          const uint32_t extra = static_cast<uint32_t>(len - 59);
          if (p + extra > end) { ret = OB_INVALID_DATA; break; }
          len = 0;
          for (uint32_t i = 0; i < extra; ++i) {
            len |= static_cast<uint64_t>(*p++) << (8 * i);
          }
          len += 1;
        }
        if (p + len > end || out.size() + len > expected) {
          ret = OB_INVALID_DATA;
        } else {
          out.append(reinterpret_cast<const char *>(p), static_cast<size_t>(len));
          p += len;
        }
      } else {
        uint64_t len = 0;
        uint64_t offset = 0;
        if (1 == type) {
          if (p >= end) { ret = OB_INVALID_DATA; break; }
          len = 4 + ((tag >> 2) & 0x07);
          offset = (static_cast<uint64_t>(tag & 0xE0) << 3) | *p++;
        } else if (2 == type) {
          if (p + 2 > end) { ret = OB_INVALID_DATA; break; }
          len = 1 + (tag >> 2);
          offset = static_cast<uint64_t>(p[0]) | (static_cast<uint64_t>(p[1]) << 8);
          p += 2;
        } else {
          if (p + 4 > end) { ret = OB_INVALID_DATA; break; }
          len = 1 + (tag >> 2);
          offset = static_cast<uint64_t>(p[0]) | (static_cast<uint64_t>(p[1]) << 8)
                   | (static_cast<uint64_t>(p[2]) << 16)
                   | (static_cast<uint64_t>(p[3]) << 24);
          p += 4;
        }
        if (0 == offset || offset > out.size() || out.size() + len > expected) {
          ret = OB_INVALID_DATA;
        } else {
          for (uint64_t i = 0; i < len; ++i) {
            out.push_back(out[out.size() - offset]);
          }
        }
      }
    }
    if (OB_SUCC(ret) && out.size() != expected) { ret = OB_INVALID_DATA; }
  }
  return ret;
}

inline int parquet_decompress(int32_t codec,
                              const char *src,
                              int64_t src_len,
                              int64_t uncompressed_len,
                              std::string &out)
{
  int ret = OB_SUCCESS;
  if (0 == codec) {
    out.assign(src, static_cast<size_t>(src_len));
  } else if (1 == codec) {
    ret = snappy_decompress(src, src_len, out);
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  if (OB_SUCC(ret) && static_cast<int64_t>(out.size()) != uncompressed_len) {
    ret = OB_INVALID_DATA;
  }
  return ret;
}

inline int parquet_read_varint(const uint8_t *&p, const uint8_t *end, uint64_t &v)
{
  int ret = OB_SUCCESS;
  v = 0;
  int shift = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    if (p >= end || shift >= 64) {
      ret = OB_INVALID_DATA;
    } else {
      const uint8_t b = *p++;
      v |= static_cast<uint64_t>(b & 0x7F) << shift;
      done = 0 == (b & 0x80);
      shift += 7;
    }
  }
  return ret;
}

inline int parquet_rle_hybrid(const uint8_t *data,
                              int64_t len,
                              int32_t bit_width,
                              int64_t count,
                              std::vector<int64_t> &values)
{
  int ret = OB_SUCCESS;
  const uint8_t *p = data;
  const uint8_t *end = data + len;
  values.clear();
  values.reserve(static_cast<size_t>(count));
  const int32_t byte_width = (bit_width + 7) / 8;
  while (OB_SUCC(ret) && p < end && static_cast<int64_t>(values.size()) < count) {
    uint64_t header = 0;
    if (OB_FAIL(parquet_read_varint(p, end, header))) {
    } else if (0 == (header & 1)) {
      const uint64_t run = header >> 1;
      if (p + byte_width > end) {
        ret = OB_INVALID_DATA;
      } else {
        uint64_t value = 0;
        for (int32_t i = 0; i < byte_width; ++i) {
          value |= static_cast<uint64_t>(p[i]) << (8 * i);
        }
        p += byte_width;
        for (uint64_t i = 0; i < run && static_cast<int64_t>(values.size()) < count; ++i) {
          values.push_back(static_cast<int64_t>(value));
        }
      }
    } else {
      const uint64_t groups = header >> 1;
      const int64_t total = static_cast<int64_t>(groups) * 8;
      const int64_t bytes = (total * bit_width + 7) / 8;
      if (p + bytes > end) {
        ret = OB_INVALID_DATA;
      } else {
        uint64_t bit_pos = 0;
        for (int64_t i = 0; i < total && static_cast<int64_t>(values.size()) < count; ++i) {
          uint64_t value = 0;
          for (int32_t b = 0; b < bit_width; ++b) {
            const uint64_t byte_index = bit_pos >> 3;
            const uint64_t bit_index = bit_pos & 7;
            if (byte_index < static_cast<uint64_t>(bytes)
                && (p[byte_index] & (1 << bit_index))) {
              value |= (static_cast<uint64_t>(1) << b);
            }
            ++bit_pos;
          }
          values.push_back(static_cast<int64_t>(value));
        }
        p += bytes;
      }
    }
  }
  if (OB_SUCC(ret) && static_cast<int64_t>(values.size()) < count) {
    ret = OB_INVALID_DATA;
  }
  return ret;
}

inline int parquet_plain_values(const uint8_t *data,
                                int64_t len,
                                int32_t type,
                                int64_t count,
                                std::vector<JsonValue> &values)
{
  int ret = OB_SUCCESS;
  const uint8_t *p = data;
  const uint8_t *end = data + len;
  values.clear();
  values.reserve(static_cast<size_t>(count));
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    JsonValue value;
    if (0 == type) {
      const int64_t byte_index = i >> 3;
      if (byte_index >= (len + 0)) { ret = OB_INVALID_DATA; break; }
      value.type_ = JsonValueType::JBOOL;
      value.bool_ = 0 != (data[byte_index] & (1 << (i & 7)));
      values.push_back(value);
    } else if (1 == type) {
      if (p + 4 > end) { ret = OB_INVALID_DATA; break; }
      uint32_t raw = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
                     | (static_cast<uint32_t>(p[2]) << 16)
                     | (static_cast<uint32_t>(p[3]) << 24);
      p += 4;
      value.type_ = JsonValueType::JINT;
      value.int_ = static_cast<int64_t>(static_cast<int32_t>(raw));
      values.push_back(value);
    } else if (2 == type) {
      if (p + 8 > end) { ret = OB_INVALID_DATA; break; }
      uint64_t raw = 0;
      for (int b = 0; b < 8; ++b) {
        raw |= static_cast<uint64_t>(p[b]) << (8 * b);
      }
      p += 8;
      value.type_ = JsonValueType::JINT;
      value.int_ = static_cast<int64_t>(raw);
      values.push_back(value);
    } else if (5 == type) {
      if (p + 8 > end) { ret = OB_INVALID_DATA; break; }
      uint64_t raw = 0;
      for (int b = 0; b < 8; ++b) {
        raw |= static_cast<uint64_t>(p[b]) << (8 * b);
      }
      p += 8;
      value.type_ = JsonValueType::JDOUBLE;
      MEMCPY(&value.double_, &raw, sizeof(raw));
      values.push_back(value);
    } else if (4 == type) {
      if (p + 4 > end) { ret = OB_INVALID_DATA; break; }
      uint32_t raw = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
                     | (static_cast<uint32_t>(p[2]) << 16)
                     | (static_cast<uint32_t>(p[3]) << 24);
      p += 4;
      value.type_ = JsonValueType::JDOUBLE;
      value.double_ = static_cast<double>(static_cast<float>(raw));
      values.push_back(value);
    } else if (6 == type || 7 == type) {
      if (p + 4 > end) { ret = OB_INVALID_DATA; break; }
      const uint32_t size = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
                            | (static_cast<uint32_t>(p[2]) << 16)
                            | (static_cast<uint32_t>(p[3]) << 24);
      p += 4;
      if (p + size > end) { ret = OB_INVALID_DATA; break; }
      value.type_ = JsonValueType::JSTRING;
      value.str_.assign(reinterpret_cast<const char *>(p), size);
      p += size;
      values.push_back(value);
    } else {
      ret = OB_NOT_SUPPORTED;
    }
  }
  return ret;
}

inline void parquet_days_to_date(int32_t days, std::string &out)
{
  int64_t z = static_cast<int64_t>(days) + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = static_cast<uint32_t>(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = static_cast<int64_t>(yoe) + era * 400;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  const uint32_t d = doy - (153 * mp + 2) / 5 + 1;
  const uint32_t m = mp < 10 ? mp + 3 : mp - 9;
  if (m <= 2) { ++y; }
  char buf[32];
  snprintf(buf, sizeof(buf), "%04lld-%02u-%02u", static_cast<long long>(y), m, d);
  out.assign(buf);
}

inline int parquet_dictionary_values(const uint8_t *data,
                                     int64_t len,
                                     int32_t type,
                                     int64_t count,
                                     const std::vector<JsonValue> &dictionary,
                                     std::vector<JsonValue> &values)
{
  int ret = OB_SUCCESS;
  if (len < 1) {
    ret = OB_INVALID_DATA;
  } else {
    const int32_t bit_width = data[0];
    std::vector<int64_t> indices;
    if (OB_FAIL(parquet_rle_hybrid(data + 1, len - 1, bit_width, count, indices))) {
    } else {
      values.clear();
      for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
        if (indices[i] < 0 || indices[i] >= static_cast<int64_t>(dictionary.size())) {
          ret = OB_INVALID_DATA;
        } else {
          values.push_back(dictionary[static_cast<size_t>(indices[i])]);
        }
      }
    }
  }
  return ret;
}

inline int parquet_decode_column(const std::string &file,
                                 const ParquetColumn &column,
                                 const ParquetColumnChunk &chunk,
                                 std::vector<JsonValue> &values)
{
  int ret = OB_SUCCESS;
  int64_t pos = chunk.dictionary_page_offset_ > 0
                ? chunk.dictionary_page_offset_ : chunk.data_page_offset_;
  std::vector<JsonValue> dictionary;
  values.clear();
  values.reserve(static_cast<size_t>(chunk.num_values_));
  while (OB_SUCC(ret) && static_cast<int64_t>(values.size()) < chunk.num_values_) {
    if (pos < 0 || pos >= static_cast<int64_t>(file.size())) {
      ret = OB_INVALID_DATA;
      break;
    }
    CompactReader header_reader(file.data() + pos, file.size() - pos);
    ThriftNode page_header;
    if (OB_FAIL(header_reader.read_struct(page_header))) {
      break;
    }
    const int64_t header_size = header_reader.pos() - (file.data() + pos);
    const int64_t page_type = thrift_i64(thrift_field(page_header, 1), -1);
    const int64_t uncompressed_size = thrift_i64(thrift_field(page_header, 2), 0);
    const int64_t compressed_size = thrift_i64(thrift_field(page_header, 3), 0);
    if (header_size <= 0 || compressed_size < 0
        || pos + header_size + compressed_size > static_cast<int64_t>(file.size())) {
      ret = OB_INVALID_DATA;
      break;
    }
    const char *page_data = file.data() + pos + header_size;
    std::string page;
    if (3 == page_type) {
      const ThriftNode *v2_header = thrift_field(page_header, 8);
      const int64_t def_len = thrift_i64(thrift_field(*v2_header, 5), 0);
      const int64_t rep_len = thrift_i64(thrift_field(*v2_header, 6), 0);
      const bool is_compressed = thrift_bool(thrift_field(*v2_header, 7), true);
      const int64_t levels_len = def_len + rep_len;
      if (levels_len < 0 || levels_len > compressed_size
          || levels_len > uncompressed_size) {
        ret = OB_INVALID_DATA;
        break;
      }
      if (is_compressed && 0 != chunk.codec_) {
        std::string values;
        if (OB_FAIL(parquet_decompress(chunk.codec_, page_data + levels_len,
                                       compressed_size - levels_len,
                                       uncompressed_size - levels_len, values))) {
          break;
        }
        page.assign(page_data, static_cast<size_t>(levels_len));
        page.append(values);
      } else {
        page.assign(page_data, static_cast<size_t>(compressed_size));
      }
    } else if (OB_FAIL(parquet_decompress(chunk.codec_, page_data, compressed_size,
                                          uncompressed_size, page))) {
      LOG_WARN("failed to decompress parquet page", K(ret), K(page_type),
               K(compressed_size), K(uncompressed_size));
      break;
    }
    const uint8_t *data = reinterpret_cast<const uint8_t *>(page.data());
    const int64_t data_len = static_cast<int64_t>(page.size());
    if (2 == page_type) {
      const ThriftNode *dict_header = thrift_field(page_header, 7);
      const int64_t dict_count = thrift_i64(thrift_field(*dict_header, 1), 0);
      if (OB_FAIL(parquet_plain_values(data, data_len, chunk.type_, dict_count, dictionary))) {
        break;
      }
    } else if (0 == page_type || 3 == page_type) {
      const ThriftNode *data_header = thrift_field(page_header, 0 == page_type ? 5 : 8);
      const int64_t num_values = thrift_i64(thrift_field(*data_header, 1), 0);
      const int64_t encoding = thrift_i64(thrift_field(*data_header, 0 == page_type ? 2 : 4), 0);
      int64_t def_len = 0;
      int64_t rep_len = 0;
      const uint8_t *value_data = data;
      int64_t value_len = data_len;
      std::vector<int64_t> def_levels;
      if (0 == page_type) {
        if (column.max_def_ > 0) {
          if (data_len < 4) { ret = OB_INVALID_DATA; break; }
          const uint32_t len = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8)
                               | (static_cast<uint32_t>(data[2]) << 16)
                               | (static_cast<uint32_t>(data[3]) << 24);
          if (4 + static_cast<int64_t>(len) > data_len) { ret = OB_INVALID_DATA; break; }
          const int32_t bit_width = column.max_def_ > 1 ? 2 : 1;
          if (OB_FAIL(parquet_rle_hybrid(data + 4, len, bit_width, num_values, def_levels))) { break; }
          def_len = 4 + len;
        }
      } else {
        def_len = thrift_i64(thrift_field(*data_header, 5), 0);
        rep_len = thrift_i64(thrift_field(*data_header, 6), 0);
        if (rep_len + def_len > data_len) { ret = OB_INVALID_DATA; break; }
        if (column.max_def_ > 0) {
          const int32_t bit_width = column.max_def_ > 1 ? 2 : 1;
          if (OB_FAIL(parquet_rle_hybrid(data + rep_len, def_len, bit_width, num_values, def_levels))) { break; }
        }
      }
      int64_t defined = num_values;
      if (column.max_def_ > 0) {
        defined = 0;
        for (int64_t i = 0; i < static_cast<int64_t>(def_levels.size()); ++i) {
          if (def_levels[i] == column.max_def_) { ++defined; }
        }
      }
      value_data = data + (0 == page_type ? def_len : rep_len + def_len);
      value_len = data_len - (0 == page_type ? def_len : rep_len + def_len);
      std::vector<JsonValue> non_null;
      if (encoding == 0) {
        if (OB_FAIL(parquet_plain_values(value_data, value_len, chunk.type_, defined, non_null))) { break; }
      } else if (encoding == 2 || encoding == 8) {
        if (OB_FAIL(parquet_dictionary_values(value_data, value_len, chunk.type_, defined,
                                              dictionary, non_null))) { break; }
      } else {
        ret = OB_NOT_SUPPORTED;
        break;
      }
      if (6 == column.converted_type_) {
        for (size_t i = 0; i < non_null.size(); ++i) {
          if (JsonValueType::JINT == non_null[i].type_) {
            std::string date;
            parquet_days_to_date(static_cast<int32_t>(non_null[i].int_), date);
            non_null[i].type_ = JsonValueType::JSTRING;
            non_null[i].str_ = date;
          }
        }
      }
      int64_t value_index = 0;
      for (int64_t i = 0; OB_SUCC(ret) && i < num_values; ++i) {
        if (column.max_def_ > 0 && def_levels[i] != column.max_def_) {
          values.push_back(JsonValue());
        } else if (value_index >= static_cast<int64_t>(non_null.size())) {
          ret = OB_INVALID_DATA;
        } else {
          values.push_back(non_null[static_cast<size_t>(value_index++)]);
        }
      }
    } else {
      // Index page or unknown page: skip.
    }
    pos += header_size + compressed_size;
  }
  return ret;
}

inline int read_parquet_rows(const ObString &path, std::vector<std::vector<JsonValue>> &rows)
{
  int ret = OB_SUCCESS;
  std::string data;
  ParquetFile file;
  if (OB_FAIL(read_whole_file(path, data))) {
  } else if (OB_FAIL(parse_parquet_metadata(data, file))) {
  } else {
    rows.clear();
    rows.resize(static_cast<size_t>(file.num_rows_));
    int64_t row_offset = 0;
    for (size_t rg_index = 0; OB_SUCC(ret) && rg_index < file.row_groups_.size(); ++rg_index) {
      const ParquetRowGroup &rg = file.row_groups_[rg_index];
      for (size_t col_index = 0; OB_SUCC(ret) && col_index < rg.columns_.size()
           && col_index < file.columns_.size(); ++col_index) {
        std::vector<JsonValue> values;
        if (OB_FAIL(parquet_decode_column(data, file.columns_[col_index],
                                          rg.columns_[col_index], values))) {
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < rg.num_rows_; ++i) {
            const int64_t row = row_offset + i;
            if (row < 0 || row >= static_cast<int64_t>(rows.size())) {
              ret = OB_INVALID_DATA;
            } else if (static_cast<size_t>(col_index) >= rows[static_cast<size_t>(row)].size()) {
              rows[static_cast<size_t>(row)].resize(file.columns_.size());
            }
            if (OB_SUCC(ret) && i < static_cast<int64_t>(values.size())) {
              rows[static_cast<size_t>(row)][col_index] = values[static_cast<size_t>(i)];
            }
          }
        }
      }
      row_offset += rg.num_rows_;
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
  } else if (FileTableFormat::PARQUET == format) {
    ret = infer_parquet_schema(allocator, path, columns);
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
      reader_(context.allocator_), json_in_(), json_path_(), json_object_(),
      parquet_rows_(), parquet_pos_(0), file_size_(-1), file_mtime_(-1),
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
    } else if (FileTableFormat::PARQUET == format_) {
      if (parquet_pos_ >= static_cast<int64_t>(parquet_rows_.size())) {
        ret = OB_ITER_END;
      } else {
        const std::vector<JsonValue> &row_values = parquet_rows_[static_cast<size_t>(parquet_pos_++)];
        for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
          if (i < static_cast<int64_t>(row_values.size())) {
            if (OB_FAIL(make_json_obj(columns_.at(i), row_values[static_cast<size_t>(i)],
                                      values_.at(i)))) {
            }
          } else {
            values_.at(i).set_null();
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
    parquet_rows_.clear();
    parquet_pos_ = 0;
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
      std::string text(options_.ptr(), options_.length());
      const size_t stat_pos = text.rfind("\nFSTAT\t");
      if (std::string::npos != stat_pos) {
        long long size = -1;
        long long mtime = -1;
        if (2 == sscanf(text.c_str() + stat_pos + 1, "FSTAT\t%lld\t%lld", &size, &mtime)) {
          file_size_ = static_cast<int64_t>(size);
          file_mtime_ = static_cast<int64_t>(mtime);
        }
      }
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
    } else if (file_size_ >= 0 && FileTableFormat::GLOB != format_) {
      struct stat file_stat;
      std::string path_text(path_.ptr(), path_.length());
      if (0 != ::stat(path_text.c_str(), &file_stat)
          || static_cast<int64_t>(file_stat.st_size) != file_size_
          || static_cast<int64_t>(file_stat.st_mtime) != file_mtime_) {
        ret = OB_INVALID_DATA;
        LOG_WARN("file changed after schema inference", K(ret), K(path_));
      }
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
    } else if (FileTableFormat::PARQUET == format_) {
      parquet_pos_ = 0;
      ret = read_parquet_rows(path_, parquet_rows_);
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
    const std::string pattern(path_.ptr(), path_.length());
    std::string dir;
    std::string filter;
    const size_t slash = pattern.find_last_of("/\\");
    if (std::string::npos == slash) {
      dir = ".";
      filter = pattern;
    } else {
      dir = pattern.substr(0, slash);
      filter = pattern.substr(slash + 1);
      if (dir.empty()) { dir = "/"; }
    }
    std::vector<std::string> files;
    const bool has_wildcard = std::string::npos != filter.find('*')
                              || std::string::npos != filter.find('?');
#ifdef _WIN32
    std::filesystem::path pattern_path(pattern);
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || ec) {
      ret = OB_FILE_NOT_EXIST;
    } else if (!has_wildcard) {
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
      if (ec) { ret = OB_IO_ERROR; }
    }
#else
    if (!has_wildcard) {
      struct stat st;
      if (0 == ::stat(pattern.c_str(), &st) && S_ISREG(st.st_mode)) {
        files.push_back(pattern);
      } else {
        ret = OB_FILE_NOT_EXIST;
      }
    } else {
      DIR *dirp = ::opendir(dir.c_str());
      if (NULL == dirp) {
        ret = OB_FILE_NOT_EXIST;
      } else {
        struct dirent *entry = NULL;
        while (OB_SUCC(ret) && NULL != (entry = ::readdir(dirp))) {
          const char *name = entry->d_name;
          if (0 == strcmp(name, ".") || 0 == strcmp(name, "..")) { continue; }
          if (!file_glob_match(name, filter.c_str())) { continue; }
          std::string full = dir;
          if (full.empty() || '/' != full[full.size() - 1]) { full.push_back('/'); }
          full.append(name);
          struct stat st;
          if (0 == ::stat(full.c_str(), &st) && S_ISREG(st.st_mode)) {
            files.push_back(full);
          }
        }
        ::closedir(dirp);
      }
    }
#endif
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
      if (ObTinyIntType == column.meta_.get_type()) {
        obj.set_bool(value.bool_);
      } else {
        const char *text = value.bool_ ? "true" : "false";
        ObString copy;
        if (OB_FAIL(ob_write_string(allocator_, ObString(static_cast<int32_t>(strlen(text)), text),
                                    copy))) {
        } else {
          ret = make_obj(column, copy, true, obj);
        }
      }
    } else if (JsonValueType::JINT == value.type_) {
      if (ObDoubleType == column.meta_.get_type()) {
        obj.set_double(static_cast<double>(value.int_));
      } else if (ObIntType == column.meta_.get_type()) {
        obj.set_int(value.int_);
      } else {
        const std::string text = std::to_string(value.int_);
        ObString copy;
        if (OB_FAIL(ob_write_string(allocator_, ObString(static_cast<int32_t>(text.size()),
                                                         text.data()), copy))) {
        } else {
          ret = make_obj(column, copy, true, obj);
        }
      }
    } else if (JsonValueType::JDOUBLE == value.type_) {
      if (ObDoubleType == column.meta_.get_type()) {
        obj.set_double(value.double_);
      } else {
        const std::string text = std::to_string(value.double_);
        ObString copy;
        if (OB_FAIL(ob_write_string(allocator_, ObString(static_cast<int32_t>(text.size()),
                                                         text.data()), copy))) {
        } else {
          ret = make_obj(column, copy, true, obj);
        }
      }
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
    if ((field.empty() && !quoted) || 0 == field.case_compare("NULL")) {
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
  std::vector<std::vector<JsonValue>> parquet_rows_;
  int64_t parquet_pos_;
  int64_t file_size_;
  int64_t file_mtime_;
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
