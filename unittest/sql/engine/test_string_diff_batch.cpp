// Standalone differential check: batch decoding versus scalar decoding.
#include "storage/blocksstable/encoding/ob_string_diff_decoder.h"
#include "storage/blocksstable/encoding/ob_bit_stream.h"
#include <cstdio>
#include <vector>

using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::blocksstable;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d mask %u: %s\n", __LINE__, mask, #x); return false; } } while (0)

static bool check(uint32_t mask, int length, bool nullable)
{
  ObArenaAllocator arena("DiffBatchTest");
  const int rows = 11;
  int diff_count = 0;
  for (int j = 0; j < length; ++j) { diff_count += (mask >> (j % 16)) & 1; }
  const int meta_size = sizeof(ObStringDiffHeader) + length + length - diff_count;
  const int null_size = nullable ? (rows + 7) / 8 : 0;
  std::vector<char> data(meta_size + null_size + rows * diff_count + 16, 0);
  auto *header = new (data.data()) ObStringDiffHeader();
  header->string_size_ = length;
  header->diff_desc_cnt_ = length;
  header->length_ = diff_count;
  int common = 0;
  for (int j = 0; j < length; ++j) {
    header->diff_descs_[j].count_ = 1;
    header->diff_descs_[j].diff_ = (mask >> (j % 16)) & 1;
    if (!header->diff_descs_[j].diff_) { header->common_data()[common++] = char(j + 1); }
  }
  ObBitStream bits(reinterpret_cast<unsigned char *>(data.data() + meta_size), null_size);
  for (int row = 0; row < rows; ++row) {
    if (nullable && row % 4 == 0) { CHECK(OB_SUCCESS == bits.set(row, 1, int64_t(1))); }
    int offset = 0;
    for (int j = 0; j < length; ++j) {
      if (header->diff_descs_[j].diff_) {
        data[meta_size + null_size + row * diff_count + offset++] = char(j + 31 * row);
      }
    }
  }
  ObColumnHeader column;
  column.offset_ = 0;
  column.length_ = meta_size;
  ObMicroBlockHeader micro;
  micro.row_count_ = rows;
  micro.extend_value_bit_ = 1;
  ObColumnDecoderCtx ctx;
  ctx.reset();
  ctx.allocator_ = &arena;
  ctx.col_header_ = &column;
  ctx.micro_block_header_ = &micro;
  ctx.cache_attributes_[ObColumnHeader::FIX_LENGTH] = true;
  ctx.cache_attributes_[ObColumnHeader::HAS_EXTEND_VALUE] = nullable;
  ObStringDiffDecoder decoder;
  CHECK(OB_SUCCESS == decoder.init(micro, column, data.data()));
  const int32_t ids[] = {10, 1, 5, 1, 0, 8, 3, 4, 7};
  ObDatum decoded[9];
  CHECK(OB_SUCCESS == decoder.batch_decode(ctx, nullptr, ids, nullptr, 9, decoded));
  for (int i = 0; i < 9; ++i) {
    ObDatum scalar;
    CHECK(OB_SUCCESS == decoder.decode(ctx, scalar, ids[i], bits, data.data(), data.size()));
    CHECK(scalar.is_null() == decoded[i].is_null());
    if (!scalar.is_null()) {
      CHECK(scalar.len_ == length && decoded[i].len_ == length);
      CHECK(0 == MEMCMP(scalar.ptr_, decoded[i].ptr_, length));
      for (int j = 0; j < length; ++j) {
        CHECK(decoded[i].ptr_[j] == (header->diff_descs_[j].diff_ ? char(j + 31 * ids[i]) : char(j + 1)));
      }
    }
  }
  return true;
}

int main()
{
  for (uint32_t mask = 0; mask < 65536; ++mask) {
    if (!check(mask, 16, mask % 2 != 0)) { return 1; }
  }
  for (int length : {1, 15, 17, 31}) {
    for (uint32_t mask : {0U, 1U, 0x5555U, 0xaaaaU, 0xffffU}) {
      if (!check(mask, length, false) || !check(mask, length, true)) { return 1; }
    }
  }
  std::puts("65576 batches: scalar equality, independent bytes, NULLs and repeated/reordered IDs passed");
}
