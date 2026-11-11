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
#include "sql/table_function/file_sql_table_function.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "common/timezone/ob_time_convert.h"
#include "sql/table_function/file_sql_parquet.h"

namespace oceanbase
{
namespace sql
{
using namespace common;

namespace
{

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// 比较两个字段名是否相同
bool same_key(const ObString &a, const ObString &b)
{
  if (a.length() != b.length()) { return false; }
  if (a.length() == 0) { return true; }
  return 0 == memcmp(a.ptr(), b.ptr(), a.length());
}

bool is_all_digits(const ObString &s)
{
  if (s.empty()) { return false; }
  for (int64_t i = 0; i < s.length(); ++i) {
    if (!isdigit(static_cast<unsigned char>(s.ptr()[i]))) { return false; }
  }
  return true;
}

bool is_number(const ObString &s)
{
  if (s.empty()) { return false; }
  bool seen_digit = false;
  bool seen_dot = false;
  bool seen_exp = false;
  for (int64_t i = 0; i < s.length(); ++i) {
    const char c = s.ptr()[i];
    if (isdigit(static_cast<unsigned char>(c))) {
      seen_digit = true;
    } else if (c == '.' && !seen_dot && !seen_exp) {
      seen_dot = true;
    } else if ((c == 'e' || c == 'E') && seen_digit && !seen_exp) {
      seen_exp = true;
      if (i + 1 < s.length() && (s.ptr()[i + 1] == '+' || s.ptr()[i + 1] == '-')) { ++i; }
    } else {
      return false;
    }
  }
  return seen_digit;
}

bool is_bool(const ObString &s)
{
  return 0 == s.case_compare(ObString::make_string("true")) || 0 == s.case_compare(ObString::make_string("false"))
         || 0 == s.case_compare(ObString::make_string("TRUE")) || 0 == s.case_compare(ObString::make_string("FALSE"))
         || 0 == s.case_compare(ObString::make_string("True")) || 0 == s.case_compare(ObString::make_string("False"));
}

bool is_date(const ObString &s)
{
  // YYYY-MM-DD
  if (s.length() != 10) { return false; }
  for (int64_t i = 0; i < s.length(); ++i) {
    const char c = s.ptr()[i];
    if (i == 4 || i == 7) {
      if (c != '-') { return false; }
    } else if (!isdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

bool is_datetime(const ObString &s)
{
  // YYYY-MM-DD[ T]HH:MM:SS[.ffffff]
  if (s.length() < 19) { return false; }
  if (!is_date(ObString(s.length() - 9, s.ptr()))) { return false; }
  const char sep = s.ptr()[10];
  if (sep != ' ' && sep != 'T') { return false; }
  for (int64_t i = 11; i < 19; ++i) {
    const char c = s.ptr()[i];
    // 时间部分从第 11 位开始：HH:MM:SS，两个冒号分别在第 13 位和第 16 位
    if (i == 13 || i == 16) {
      if (c != ':') { return false; }
    } else if (!isdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  // 可选的小数部分
  if (s.length() > 19) {
    if (s.ptr()[19] != '.') { return false; }
    if (s.length() == 20) { return false; }
    for (int64_t i = 20; i < s.length(); ++i) {
      if (!isdigit(static_cast<unsigned char>(s.ptr()[i]))) { return false; }
    }
  }
  return true;
}

// 解析十进制数字文本
int parse_digits(const ObString &value, int64_t &result)
{
  int ret = OB_SUCCESS;
  result = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const char c = value.ptr()[i];
    if (!isdigit(static_cast<unsigned char>(c))) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      result = result * 10 + (c - '0');
    }
  }
  return ret;
}

// 解析 YYYY-MM-DD 中的年月日
int parse_date_parts(const ObString &value, int64_t &year, int64_t &month, int64_t &day)
{
  int ret = OB_SUCCESS;
  if (!is_date(value)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid date text", K(ret), K(value));
  } else if (OB_FAIL(parse_digits(ObString(4, value.ptr()), year))
             || OB_FAIL(parse_digits(ObString(2, value.ptr() + 5), month))
             || OB_FAIL(parse_digits(ObString(2, value.ptr() + 8), day))) {
    LOG_WARN("failed to parse date text", K(ret), K(value));
  }
  return ret;
}

// 解析 YYYY-MM-DD HH:MM:SS[.ffffff] 中的时分秒与微秒
int parse_time_parts(const ObString &value, int64_t &hour, int64_t &minute, int64_t &second, int64_t &usec)
{
  int ret = OB_SUCCESS;
  usec = 0;
  if (OB_FAIL(parse_digits(ObString(2, value.ptr() + 11), hour))
      || OB_FAIL(parse_digits(ObString(2, value.ptr() + 14), minute))
      || OB_FAIL(parse_digits(ObString(2, value.ptr() + 17), second))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("failed to parse datetime text", K(ret), K(value));
  } else if (value.length() > 20) {
    int64_t digits = value.length() - 20;
    if (digits > 6) { digits = 6; }
    if (OB_FAIL(parse_digits(ObString(digits, value.ptr() + 20), usec))) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      for (int64_t i = digits; i < 6; ++i) { usec *= 10; }
    }
  }
  return ret;
}

int copy_string(ObIAllocator &allocator, const ObString &src, ObString &dst)
{
  int ret = OB_SUCCESS;
  char *buf = static_cast<char *>(allocator.alloc(src.length() + 1));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    if (src.length() > 0) { MEMCPY(buf, src.ptr(), src.length()); }
    buf[src.length()] = '\0';
    dst.assign_ptr(buf, src.length());
  }
  return ret;
}

// 解析一行 CSV（支持双引号包裹、转义双引号、字段内换行）
int parse_csv_line(const ObString &line, ObIAllocator &allocator, ObArray<ObString> &fields)
{
  int ret = OB_SUCCESS;
  const char *data = line.ptr();
  const int64_t len = line.length();
  int64_t pos = 0;
  while (pos <= len) {
    ObString field;
    if (pos < len && data[pos] == '"') {
      // 引号字段："" 表示一个引号
      ++pos;
      std::string value;
      bool closed = false;
      while (pos < len) {
        const char c = data[pos];
        if (c == '"') {
          if (pos + 1 < len && data[pos + 1] == '"') {
            value.push_back('"');
            pos += 2;
            continue;
          }
          ++pos;
          closed = true;
          break;
        }
        value.push_back(c);
        ++pos;
      }
      if (!closed) { ret = OB_ERR_UNEXPECTED; }
      if (OB_SUCC(ret)) {
        ret = copy_string(allocator, ObString(value.length(), value.c_str()), field);
      }
      // 跳过引号后的分隔符
      while (pos < len && data[pos] != ',') { ++pos; }
    } else {
      const int64_t start = pos;
      while (pos < len && data[pos] != ',') { ++pos; }
      field.assign_ptr(data + start, pos - start);
    }
    if (OB_SUCC(ret) && OB_FAIL(fields.push_back(field))) { ret = OB_ERR_UNEXPECTED; }
    if (pos >= len) { break; }
    ++pos; // 跳过逗号
  }
  return ret;
}

// 解析一行 JSON（只处理扁平的 key/value 对象）
int parse_jsonl_line(const ObString &line, ObIAllocator &allocator,
                     ObArray<ObString> &keys, ObArray<ObString> &values, ObArray<bool> &nulls)
{
  int ret = OB_SUCCESS;
  const char *data = line.ptr();
  const int64_t len = line.length();
  int64_t pos = 0;
  auto skip_ws = [&]() { while (pos < len && is_space(data[pos])) { ++pos; } };
  skip_ws();
  if (pos >= len || data[pos] != '{') { return OB_ERR_UNEXPECTED; }
  ++pos;
  skip_ws();
  if (pos < len && data[pos] == '}') { return OB_SUCCESS; }
  while (pos < len) {
    skip_ws();
    if (pos >= len || data[pos] != '"') { return OB_ERR_UNEXPECTED; }
    ++pos;
    std::string key;
    while (pos < len && data[pos] != '"') {
      if (data[pos] == '\\' && pos + 1 < len) { ++pos; }
      key.push_back(data[pos]);
      ++pos;
    }
    if (pos >= len) { return OB_ERR_UNEXPECTED; }
    ++pos;
    skip_ws();
    if (pos >= len || data[pos] != ':') { return OB_ERR_UNEXPECTED; }
    ++pos;
    skip_ws();
    if (pos >= len) { return OB_ERR_UNEXPECTED; }
    ObString value;
    bool is_null = false;
    if (data[pos] == '"') {
      ++pos;
      std::string v;
      while (pos < len && data[pos] != '"') {
        if (data[pos] == '\\' && pos + 1 < len) {
          ++pos;
          const char e = data[pos];
          if (e == 'n') { v.push_back('\n'); }
          else if (e == 't') { v.push_back('\t'); }
          else { v.push_back(e); }
        } else {
          v.push_back(data[pos]);
        }
        ++pos;
      }
      if (pos >= len) { return OB_ERR_UNEXPECTED; }
      ++pos;
      ret = copy_string(allocator, ObString(v.length(), v.c_str()), value);
    } else if (data[pos] == 'n') {
      // null
      if (pos + 3 < len && 0 == strncmp(data + pos, "null", 4)) { pos += 4; }
      else { return OB_ERR_UNEXPECTED; }
      is_null = true;
    } else {
      const int64_t start = pos;
      while (pos < len && data[pos] != ',' && data[pos] != '}') { ++pos; }
      value.assign_ptr(data + start, pos - start);
      // 去掉尾部空白
      while (value.length() > 0 && is_space(value.ptr()[value.length() - 1])) {
        value = ObString(value.length() - 1, value.ptr());
      }
    }
    if (OB_SUCC(ret)) {
      // key 是函数内的局部字符串，必须复制到分配器里才能在函数返回后继续使用
      ObString key_str;
      if (OB_FAIL(copy_string(allocator, ObString(key.length(), key.c_str()), key_str))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(keys.push_back(key_str))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(values.push_back(value))) { ret = OB_ERR_UNEXPECTED; }
      else if (OB_FAIL(nulls.push_back(is_null))) { ret = OB_ERR_UNEXPECTED; }
    }
    if (OB_SUCC(ret)) {
      skip_ws();
      if (pos < len && data[pos] == ',') { ++pos; continue; }
      break;
    }
  }
  return ret;
}

int read_whole_file(const ObString &path, ObIAllocator &allocator, ObString &content)
{
  int ret = OB_SUCCESS;
  std::string path_str(path.ptr(), path.length());
  std::ifstream input(path_str.c_str(), std::ios::binary);
  if (!input.is_open()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("failed to open file", K(ret), K(path));
  } else {
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string &text = buffer.str();
    ret = copy_string(allocator, ObString(text.length(), text.c_str()), content);
  }
  return ret;
}

// 把一条记录放进结果里，纯空白记录忽略
int push_record(const ObString &record, ObArray<ObString> &lines)
{
  int ret = OB_SUCCESS;
  bool blank = true;
  for (int64_t k = 0; k < record.length(); ++k) {
    if (!is_space(record.ptr()[k])) { blank = false; break; }
  }
  if (!blank && OB_FAIL(lines.push_back(record))) { ret = OB_ERR_UNEXPECTED; }
  return ret;
}

// 按换行拆分文本。JSONL 每行一个 JSON 对象，字符串里不会出现裸换行，
// 所以这里不做引号处理，避免 JSON 的转义引号把记录边界算错。
int split_lines(const ObString &content, ObArray<ObString> &lines)
{
  int ret = OB_SUCCESS;
  const char *data = content.ptr();
  const int64_t len = content.length();
  int64_t start = 0;
  for (int64_t i = 0; i < len; ++i) {
    if (data[i] != '\n') { continue; }
    int64_t end = i;
    if (end > start && data[end - 1] == '\r') { --end; }
    if (OB_FAIL(push_record(ObString(end - start, data + start), lines))) { return ret; }
    start = i + 1;
  }
  if (start < len) {
    if (OB_FAIL(push_record(ObString(len - start, data + start), lines))) { return ret; }
  }
  return ret;
}

// 按 CSV 记录拆分：引号里的换行属于字段内容，不能当成记录分隔符。
// 只有位于字段起始（上一个逗号之后）的双引号才会开启引号字段，
// 与 parse_csv_line 的取字段规则保持一致。
int split_csv_records(const ObString &content, ObArray<ObString> &lines)
{
  int ret = OB_SUCCESS;
  const char *data = content.ptr();
  const int64_t len = content.length();
  int64_t start = 0;
  bool in_quotes = false;
  bool field_start = true;
  int64_t i = 0;
  while (i < len) {
    const char c = data[i];
    if (in_quotes) {
      if (c == '"') {
        // "" 表示一个引号
        if (i + 1 < len && data[i + 1] == '"') { i += 2; continue; }
        in_quotes = false;
        ++i;
        continue;
      }
      ++i;
      continue;
    }
    if (c == '\n') {
      int64_t end = i;
      if (end > start && data[end - 1] == '\r') { --end; }
      if (OB_FAIL(push_record(ObString(end - start, data + start), lines))) { return ret; }
      start = i + 1;
      field_start = true;
      ++i;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
    if (c == ',') { field_start = true; ++i; continue; }
    if (c == '"' && field_start) { in_quotes = true; field_start = false; ++i; continue; }
    field_start = false;
    ++i;
  }
  if (start < len) {
    if (OB_FAIL(push_record(ObString(len - start, data + start), lines))) { return ret; }
  }
  return ret;
}

// 依据列的原始值推断类型，并记录是否可空
void infer_column_types(FileSqlTable &table)
{
  for (int64_t c = 0; c < table.columns_.count(); ++c) {
    bool all_int = true;
    bool all_num = true;
    bool all_bool = true;
    bool all_date = true;
    bool all_dt = true;
    bool nullable = false;
    bool has_value = false;
    for (int64_t r = 0; r < table.rows_.count(); ++r) {
      const FileSqlRow &row = table.rows_.at(r);
      if (c >= row.values_.count()) { continue; }
      if (c < row.nulls_.count() && row.nulls_.at(c)) { nullable = true; continue; }
      const ObString &v = row.values_.at(c);
      if (v.empty()) { nullable = true; continue; }
      has_value = true;
      if (!is_all_digits(v)) { all_int = false; }
      if (!is_number(v)) { all_num = false; }
      if (!is_bool(v)) { all_bool = false; }
      if (!is_date(v)) { all_date = false; }
      if (!is_datetime(v)) { all_dt = false; }
    }
    ObDataType type;
    if (!has_value) {
      type.set_obj_type(ObVarcharType);
      type.set_collation_level(CS_LEVEL_IMPLICIT);
      type.set_length(262144);
    } else if (all_bool) {
      type.set_obj_type(ObTinyIntType);
      type.set_precision(1);
      type.set_scale(0);
    } else if (all_dt) {
      type.set_obj_type(ObDateTimeType);
      type.set_precision(19);
      type.set_scale(0);
    } else if (all_date) {
      type.set_obj_type(ObDateType);
      type.set_precision(10);
      type.set_scale(0);
    } else if (all_num) {
      if (all_int) { type.set_obj_type(ObIntType); type.set_precision(20); type.set_scale(0); }
      else { type.set_obj_type(ObDoubleType); type.set_precision(22); type.set_scale(0); }
    } else {
      type.set_obj_type(ObVarcharType);
      type.set_collation_level(CS_LEVEL_IMPLICIT);
      type.set_length(262144);
    }
    table.columns_.at(c).type_ = type;
    table.columns_.at(c).nullable_ = nullable;
    if (all_bool) { table.columns_.at(c).kind_ = FILE_SQL_VALUE_BOOL; }
    else if (all_dt) { table.columns_.at(c).kind_ = FILE_SQL_VALUE_DATETIME; }
    else if (all_date) { table.columns_.at(c).kind_ = FILE_SQL_VALUE_DATE; }
    else if (all_num && all_int) { table.columns_.at(c).kind_ = FILE_SQL_VALUE_INT; }
    else if (all_num) { table.columns_.at(c).kind_ = FILE_SQL_VALUE_DOUBLE; }
    else { table.columns_.at(c).kind_ = FILE_SQL_VALUE_VARCHAR; }
  }
}

} // namespace

int file_sql_parse_format(const ObString &format_name, FileSqlFormat &format)
{
  int ret = OB_SUCCESS;
  if (format_name.empty()) {
    // 未显式指定格式时保留调用方按表函数语义选定的格式
  } else if (0 == format_name.case_compare(ObString::make_string("csv"))) {
    format = FILE_SQL_FORMAT_CSV;
  } else if (0 == format_name.case_compare(ObString::make_string("json"))
             || 0 == format_name.case_compare(ObString::make_string("newline_delimited"))
             || 0 == format_name.case_compare(ObString::make_string("jsonl"))) {
    format = FILE_SQL_FORMAT_JSONL;
  } else if (0 == format_name.case_compare(ObString::make_string("parquet"))) {
    format = FILE_SQL_FORMAT_PARQUET;
  } else {
    ret = OB_ERR_PARSE_SQL;
    LOG_USER_ERROR(OB_ERR_PARSE_SQL, "unsupported file format",
                 format_name.length(), format_name.ptr(), 1);
  }
  return ret;
}

int file_sql_parse_date(const ObString &value, int32_t &date_value)
{
  int ret = OB_SUCCESS;
  int64_t year = 0;
  int64_t month = 0;
  int64_t day = 0;
  if (OB_FAIL(parse_date_parts(value, year, month, day))) {
  } else if (month < 1 || month > 12) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid month in date text", K(ret), K(value));
  } else if (day < 1 || day > ObTimeConverter::get_days_of_month(static_cast<int32_t>(year),
                                                                 static_cast<int32_t>(month))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid day in date text", K(ret), K(value));
  } else {
    date_value = ObTimeConverter::calc_date(year, month, day);
  }
  return ret;
}

int file_sql_parse_datetime(const ObString &value, int64_t &datetime_value)
{
  int ret = OB_SUCCESS;
  int32_t date_value = 0;
  int64_t hour = 0;
  int64_t minute = 0;
  int64_t second = 0;
  int64_t usec = 0;
  if (OB_FAIL(file_sql_parse_date(ObString(10, value.ptr()), date_value))) {
  } else if (OB_FAIL(parse_time_parts(value, hour, minute, second, usec))) {
  } else if (hour > 23 || minute > 59 || second > 59) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid time in datetime text", K(ret), K(value));
  } else {
    const int64_t usecs_of_day = 24LL * 60 * 60 * 1000000;
    const int64_t time_value = (hour * 60 * 60 + minute * 60 + second) * 1000000 + usec;
    datetime_value = static_cast<int64_t>(date_value) * usecs_of_day + time_value;
  }
  return ret;
}

int file_sql_list_files(const ObString &pattern, ObIAllocator &allocator, ObArray<ObString> &files)
{
  int ret = OB_SUCCESS;
  // pattern 形如 /path/to/dir/*
  const ObString &p = pattern;
  int64_t slash = -1;
  for (int64_t i = p.length() - 1; i >= 0; --i) {
    if (p.ptr()[i] == '/') { slash = i; break; }
  }
  if (slash < 0) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const ObString dir_part(p.length() - slash - 1, p.ptr());
    std::string dir(dir_part.ptr(), dir_part.length());
    DIR *handle = opendir(dir.c_str());
    if (handle == nullptr) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("failed to open dir", K(ret), K(dir_part));
    } else {
      struct dirent *entry = nullptr;
      while (nullptr != (entry = readdir(handle))) {
        const char *name = entry->d_name;
        if (name[0] == '.') { continue; }
        ObString file_name;
        if (OB_SUCC(ret) && OB_FAIL(copy_string(allocator, ObString(strlen(name), name), file_name))) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        if (OB_SUCC(ret) && OB_FAIL(files.push_back(file_name))) { ret = OB_ERR_UNEXPECTED; break; }
      }
      closedir(handle);
      if (OB_SUCC(ret)) {
        std::sort(files.begin(), files.end(), [](const ObString &a, const ObString &b) {
          return 0 == strcmp(a.ptr(), b.ptr()) ? false : strcmp(a.ptr(), b.ptr()) < 0;
        });
      }
    }
  }
  return ret;
}

int file_sql_load(const ObString &path, FileSqlFormat format, bool sample_only,
                  ObIAllocator &allocator, FileSqlTable &table)
{
  int ret = OB_SUCCESS;
  bool parquet_loaded = false;
  ObString content;
  if (FILE_SQL_FORMAT_GLOB == format) {
    // glob 只返回一列文件名，路径是通配符，不需要读取文件
    FileSqlColumn column;
    column.name_ = ObString::make_string("file");
    column.type_.set_obj_type(ObVarcharType);
    column.type_.set_collation_level(CS_LEVEL_IMPLICIT);
    column.nullable_ = false;
    if (OB_FAIL(table.columns_.push_back(column))) { ret = OB_ERR_UNEXPECTED; }
  } else if (OB_FAIL(read_whole_file(path, allocator, content))) {
  } else if (FILE_SQL_FORMAT_PARQUET == format || file_sql_is_parquet_content(content)) {
    // parquet 文件：列名与列类型来自文件自身的 schema，不需要再从内容推断
    parquet_loaded = true;
    if (OB_FAIL(file_sql_load_parquet(content, sample_only, allocator, table))) {
      LOG_WARN("failed to load parquet file", K(ret), K(path));
    }
  } else if (format == FILE_SQL_FORMAT_CSV) {
    ObArray<ObString> lines;
    if (OB_FAIL(split_csv_records(content, lines)) || lines.count() == 0) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObArray<ObString> header;
      if (OB_FAIL(parse_csv_line(lines.at(0), allocator, header))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        // 列名去重：重复列名追加 __2、__3 ...
        for (int64_t i = 0; i < header.count(); ++i) {
          ObString name = header.at(i);
          int64_t dup = 1;
          while (true) {
            bool conflict = false;
            for (int64_t j = 0; j < table.columns_.count(); ++j) {
              if (0 == table.columns_.at(j).name_.case_compare(name)) { conflict = true; break; }
            }
            if (!conflict) { break; }
            ++dup;
            char buf[64];
            const int64_t len = snprintf(buf, sizeof(buf), "%.*s__%ld", name.length(), name.ptr(), dup);
            // 重命名后的列名要复制到分配器里，buf 是栈上的临时空间
            ObString renamed;
            if (OB_FAIL(copy_string(allocator, ObString(len, buf), renamed))) {
              ret = OB_ERR_UNEXPECTED;
              break;
            }
            name = renamed;
          }
          if (OB_FAIL(ret)) { break; }
          FileSqlColumn column;
          column.name_ = name;
          column.nullable_ = true;
          if (OB_FAIL(table.columns_.push_back(column))) { ret = OB_ERR_UNEXPECTED; }
        }
        if (OB_SUCC(ret)) {
          // 数据行始终参与类型推断，保证绑定期与扫描期推断出的列类型一致
          for (int64_t r = 1; OB_SUCC(ret) && r < lines.count(); ++r) {
            ObArray<ObString> values;
            if (OB_FAIL(parse_csv_line(lines.at(r), allocator, values))) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("file sql: failed to parse csv record", K(ret), K(r));
              break;
            }
            FileSqlRow row;
            for (int64_t c = 0; OB_SUCC(ret) && c < table.columns_.count(); ++c) {
              ObString value;
              bool is_null = true;
              if (c < values.count()) {
                value = values.at(c);
                is_null = value.empty();
              }
              if (OB_FAIL(row.values_.push_back(value))) { ret = OB_ERR_UNEXPECTED; }
              else if (OB_FAIL(row.nulls_.push_back(is_null))) { ret = OB_ERR_UNEXPECTED; }
            }
            if (OB_SUCC(ret) && OB_FAIL(table.rows_.push_back(row))) { ret = OB_ERR_UNEXPECTED; }
          }
        }
      }
    }
  } else if (format == FILE_SQL_FORMAT_JSONL) {    ObArray<ObString> lines;
    if (OB_FAIL(split_lines(content, lines)) || lines.count() == 0) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObArray<ObString> keys;
      // 第一遍：收集所有出现过的 key，列顺序按首次出现
      for (int64_t r = 0; OB_SUCC(ret) && r < lines.count(); ++r) {
        ObArray<ObString> row_keys;
        ObArray<ObString> row_values;
        ObArray<bool> row_nulls;
        if (OB_FAIL(parse_jsonl_line(lines.at(r), allocator, row_keys, row_values, row_nulls))) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        for (int64_t k = 0; OB_SUCC(ret) && k < row_keys.count(); ++k) {
          bool found = false;
          for (int64_t j = 0; j < keys.count(); ++j) {
            if (same_key(keys.at(j), row_keys.at(k))) { found = true; break; }
          }
          if (!found && OB_FAIL(keys.push_back(row_keys.at(k)))) { ret = OB_ERR_UNEXPECTED; }
        }
      }
      // 第二遍：按统一的列顺序生成数据行，缺失的字段为 NULL
      if (OB_SUCC(ret)) {
        for (int64_t r = 0; OB_SUCC(ret) && r < lines.count(); ++r) {
          ObArray<ObString> row_keys;
          ObArray<ObString> row_values;
          ObArray<bool> row_nulls;
          if (OB_FAIL(parse_jsonl_line(lines.at(r), allocator, row_keys, row_values, row_nulls))) {
            ret = OB_ERR_UNEXPECTED;
            break;
          }
          FileSqlRow row;
          for (int64_t c = 0; OB_SUCC(ret) && c < keys.count(); ++c) {
            ObString value;
            bool is_null = true;
            for (int64_t k = 0; k < row_keys.count(); ++k) {
              if (same_key(keys.at(c), row_keys.at(k))) {
                value = row_values.at(k);
                is_null = row_nulls.at(k);
                break;
              }
            }
            if (OB_FAIL(row.values_.push_back(value))) { ret = OB_ERR_UNEXPECTED; }
            else if (OB_FAIL(row.nulls_.push_back(is_null))) { ret = OB_ERR_UNEXPECTED; }
          }
          if (OB_SUCC(ret) && OB_FAIL(table.rows_.push_back(row))) { ret = OB_ERR_UNEXPECTED; }
        }
      }
      if (OB_SUCC(ret)) {
        for (int64_t k = 0; OB_SUCC(ret) && k < keys.count(); ++k) {
          FileSqlColumn column;
          column.name_ = keys.at(k);
          column.nullable_ = true;
          if (OB_FAIL(table.columns_.push_back(column))) { ret = OB_ERR_UNEXPECTED; }
        }
      }
    }
  } else if (format == FILE_SQL_FORMAT_GLOB) {
    // glob 的列已在前面构造
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  if (OB_SUCC(ret) && format != FILE_SQL_FORMAT_GLOB && !parquet_loaded) {
    infer_column_types(table);
    if (sample_only) { table.rows_.reuse(); }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
