# MiniVectorDB Lite

This directory is the clean-room C++17 implementation for the competition JSONL
contract. The parent branch supplies the official protocol corpus only; the
runtime entrypoint is competition_lite/src/minivectordb_lite.cpp.

The implementation uses a single writer, private transaction write sets,
binary WAL, MemTable/Immutable/SSTable metadata, a separate float32 vector
file, exact Top-K, and a safe ANN path with exact fallback. Runtime code is
checked against the 2000 physical-line budget.
