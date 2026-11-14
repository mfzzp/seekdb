// Focused regression for bulk JSON/SQL string scanning; not runtime code.
#define main lite_program_main
#include "../src/minivectordb_lite.cpp"
#undef main
#include <cassert>

int main() {
  uint32_t state = 17;
  const std::string alphabet = "abcXYZ 0123,()[]\"'\\\t\n\r";
  for (size_t trial = 0; trial < 2000; ++trial) {
    std::string value = trial % 2 ? "中文" : "";
    for (size_t i = 0; i < trial % 257; ++i) {
      state ^= state << 13; state ^= state >> 17; state ^= state << 5;
      value += alphabet[state % alphabet.size()];
    }
    std::string wire = lite::esc(value);
    size_t position = 0;
    assert(lite::jstr(wire, position) == value && position == wire.size());
    std::string sql = "SELECT '";
    for (char c : value) {
      if (c == '\'' || c == '\\') sql += '\\';
      sql += c;
    }
    sql += "' AS text;";
    const auto tokens = lite::tok(sql);
    assert(tokens.size() == 4 && tokens[1] == "'" + value + "'");
    const auto canonical = lite::tok("SELECT " + lite::sqltok(tokens[1]) + " AS text;");
    assert(canonical == tokens);
  }
  std::cout << "quoted_string_cases=2000 PASS\n";
}
