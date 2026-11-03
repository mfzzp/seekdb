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

class FileTableScan final : public ITableFunctionScan
{
public:
  explicit FileTableScan(const TableFunctionScanContext &context)
    : exec_ctx_(context.exec_ctx_), allocator_(context.allocator_),
      column_ids_(context.column_ids_), options_(context.options_),
      path_(), format_(FileTableFormat::CSV), header_(true),
      reader_(context.allocator_), columns_(), values_(), cells_(), row_(), opened_(false) {}
  int open() override
  {
    int ret = OB_SUCCESS;
    if (OB_FAIL(load_options())) {
      LOG_WARN("failed to load file table options", K(ret));
    } else if (OB_FAIL(check_access())) {
      LOG_WARN("failed to check file table access", K(ret));
    } else if (OB_FAIL(open_reader())) {
      LOG_WARN("failed to open file table reader", K(ret));
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
    if (FileTableFormat::CSV != format_) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "file format");
    } else if (OB_FAIL(reader_.open(path_))) {
    } else if (header_ && OB_FAIL(reader_.next())) {
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
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
  ObSEArray<FileTableColumn, 16> columns_;
  ObSEArray<ObObj, 16> values_;
  ObSEArray<ObObj, 16> cells_;
  ObNewRow row_;
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
