// Standalone differential test against the generic JSON tree serializer.
#include "storage/fts/ob_fts_parser_helper.h"
#include "common/json_type/ob_json_tree.h"
#include "common/json_type/ob_json_bin.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::storage;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return false; } } while (0)

static bool check_array(int count, int length)
{
  ObArenaAllocator arena("TokenJsonTest");
  ObFTParseHelper helper;
  CHECK(OB_SUCCESS == helper.init(&arena, ObString::make_string("space.1"), ObString()));
  ObFTWordMap words;
  CHECK(OB_SUCCESS == words.create(997, ObMemAttr("TokenJsonTest")));
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  std::vector<std::string> strings;
  strings.reserve(count);
  for (int i = 0; i < count; ++i) {
    strings.push_back(std::to_string(i) + std::string(length, 'a' + i % 26));
    if (i % 3 == 0) { strings.back() += "\"\\\n\t"; }
    if (i % 5 == 0) { strings.back() += "\xe4\xb8\xad\xe6\x96\x87"; }
    if (i % 7 == 0) { strings.back().push_back('\0'); }
    const std::string &s = strings.back();
    CHECK(OB_SUCCESS == words.set_refactored(ObFTWord(s.size(), s.data(), meta), i + 1));
  }
  ObJsonArray old_array(&arena);
  for (auto it = words.begin(); it != words.end(); ++it) {
    ObJsonString *value = OB_NEWx(ObJsonString, &arena, it->first.get_word().get_string());
    CHECK(nullptr != value);
    CHECK(OB_SUCCESS == old_array.append(value));
  }
  ObString expected, actual;
  ObJsonBinSerializer serializer(&arena);
  CHECK(OB_SUCCESS == serializer.serialize(&old_array, expected));
  CHECK(OB_SUCCESS == helper.make_token_array_binary(words, actual));
  ObJsonBin decoded(actual.ptr(), actual.length(), &arena);
  CHECK(OB_SUCCESS == decoded.reset_iter());
  ObIJsonBase *root = &decoded;
  CHECK(root->element_count() == count);
  if (expected.length() != actual.length()
      || 0 != MEMCMP(expected.ptr(), actual.ptr(), expected.length())) {
    std::fprintf(stderr, "count=%d length=%d old_bytes=%d new_bytes=%d\n", count, length, expected.length(), actual.length());
    return false;
  }
  int i = 0;
  for (auto it = words.begin(); it != words.end(); ++it, ++i) {
    ObIJsonBase *value = nullptr;
    CHECK(OB_SUCCESS == root->get_array_element(i, value));
    CHECK(value->json_type() == ObJsonNodeType::J_STRING);
    const ObString word = it->first.get_word().get_string();
    CHECK(value->get_data_length() == word.length());
    CHECK(0 == MEMCMP(value->get_data(), word.ptr(), word.length()));
  }
  return true;
}

int main()
{
  int cases = 0;
  for (int count : {0, 1, 2, 3, 15, 31, 63, 127, 128, 255, 256, 511, 1024}) {
    for (int length : {0, 1, 7, 63, 64, 126, 127, 128, 254, 255, 256}) {
      if (!check_array(count, length)) { return 1; }
      ++cases;
    }
  }
  for (int count : {65535, 65536}) {
    if (!check_array(count, 0)) { return 1; }
    ++cases;
  }
  for (int length : {65520, 65535, 65536}) {
    if (!check_array(1, length)) { return 1; }
    ++cases;
  }
  std::printf("%d arrays: exact binary equality and every decoded word passed\n", cases);
  return 0;
}
