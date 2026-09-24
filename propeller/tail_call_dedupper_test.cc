// Copyright 2026 The Propeller Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "propeller/tail_call_dedupper.h"

#include <cstdint>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace propeller {
namespace {

// x86-64 epilogue: add $0x18,%rsp; pop %rbx; pop %rbp; ret  (7 bytes, > 5-byte
// jmp patch, so folding saves space).
const std::vector<uint8_t> kEpilogueA = {0x48, 0x83, 0xc4, 0x18,
                                         0x5b, 0x5d, 0xc3};
// A different 7-byte epilogue: add $0x28,%rsp; pop %rbx; pop %rbp; ret.
const std::vector<uint8_t> kEpilogueB = {0x48, 0x83, 0xc4, 0x28,
                                         0x5b, 0x5d, 0xc3};
// pop %rbp; ret  (2 bytes, <= 5-byte patch, never worth folding on x86).
const std::vector<uint8_t> kTinyEpilogue = {0x5d, 0xc3};

TailCallBlock MakeBlock(std::string module, std::string function, uint32_t id,
                        uint64_t address, std::vector<uint8_t> bytes) {
  return TailCallBlock{std::move(module), std::move(function), id, address,
                       std::move(bytes)};
}

// Locks the FNV-1a constants against well-known reference vectors.
TEST(Fnv1aHashTest, MatchesKnownVectors) {
  EXPECT_EQ(Fnv1aHash(std::vector<uint8_t>{}), 0xcbf29ce484222325ULL);
  EXPECT_EQ(Fnv1aHash(std::vector<uint8_t>{'a'}), 0xaf63dc4c8601ec8cULL);
  EXPECT_EQ(Fnv1aHash(std::vector<uint8_t>{'f', 'o', 'o', 'b', 'a', 'r'}),
            0x85944171f73967e8ULL);
}

TEST(Fnv1aHashTest, IdenticalBytesHashEqualDifferentBytesDiffer) {
  EXPECT_EQ(Fnv1aHash(kEpilogueA), Fnv1aHash(kEpilogueA));
  EXPECT_NE(Fnv1aHash(kEpilogueA), Fnv1aHash(kEpilogueB));
}

// Two functions with an identical epilogue -> one master, one fold.
TEST(DeduplicateTailCallBlocksTest, FoldsIdenticalEpilogueAcrossFunctions) {
  std::vector<TailCallBlock> blocks = {
      MakeBlock("bar.cpp", "bar", /*id=*/3, /*address=*/0x1000, kEpilogueA),
      MakeBlock("foo.cpp", "foo", /*id=*/5, /*address=*/0x2000, kEpilogueA),
  };
  TailCallDedupResult result = DeduplicateTailCallBlocks(blocks);

  EXPECT_EQ(result.num_groups, 1);
  EXPECT_EQ(result.num_masters, 1);
  EXPECT_EQ(result.num_folds, 1);
  // (count-1) * (size - patch) = 1 * (7 - 5) = 2 bytes saved.
  EXPECT_EQ(result.bytes_saved, 2);

  ASSERT_EQ(result.directives.size(), 2u);
  // Master is the lowest-address block (bar, 0x1000).
  const Directive& master = result.directives[0];
  EXPECT_EQ(master.kind, DirectiveKind::kMaster);
  EXPECT_EQ(master.function_name, "bar");
  EXPECT_EQ(master.bb_id, 3u);
  const Directive& fold = result.directives[1];
  EXPECT_EQ(fold.kind, DirectiveKind::kFold);
  EXPECT_EQ(fold.function_name, "foo");
  EXPECT_EQ(fold.bb_id, 5u);
  // Master and fold reference the same master id (K).
  EXPECT_EQ(master.master_id, fold.master_id);
}

// Three identical copies -> one master + two folds.
TEST(DeduplicateTailCallBlocksTest, FoldsThreeWay) {
  std::vector<TailCallBlock> blocks = {
      MakeBlock("a.cpp", "a", 1, 0x1000, kEpilogueA),
      MakeBlock("b.cpp", "b", 1, 0x2000, kEpilogueA),
      MakeBlock("c.cpp", "c", 1, 0x3000, kEpilogueA),
  };
  TailCallDedupResult result = DeduplicateTailCallBlocks(blocks);
  EXPECT_EQ(result.num_groups, 1);
  EXPECT_EQ(result.num_folds, 2);
  EXPECT_EQ(result.bytes_saved, 2 * (7 - 5));
}

// Distinct byte sequences are never folded together, even if grouped.
TEST(DeduplicateTailCallBlocksTest, DoesNotFoldDifferentBytes) {
  std::vector<TailCallBlock> blocks = {
      MakeBlock("a.cpp", "a", 1, 0x1000, kEpilogueA),
      MakeBlock("b.cpp", "b", 1, 0x2000, kEpilogueB),
  };
  TailCallDedupResult result = DeduplicateTailCallBlocks(blocks);
  EXPECT_EQ(result.num_groups, 0);
  EXPECT_TRUE(result.directives.empty());
}

// Blocks whose size does not exceed the jump patch are not folded.
TEST(DeduplicateTailCallBlocksTest, DoesNotFoldWhenNoSizeBenefit) {
  std::vector<TailCallBlock> blocks = {
      MakeBlock("a.cpp", "a", 1, 0x1000, kTinyEpilogue),
      MakeBlock("b.cpp", "b", 1, 0x2000, kTinyEpilogue),
  };
  TailCallDedupResult result = DeduplicateTailCallBlocks(blocks);
  EXPECT_EQ(result.num_groups, 0);
}

// A singleton block (no duplicate) is left untouched.
TEST(DeduplicateTailCallBlocksTest, DoesNotFoldSingleton) {
  std::vector<TailCallBlock> blocks = {
      MakeBlock("a.cpp", "a", 1, 0x1000, kEpilogueA),
  };
  TailCallDedupResult result = DeduplicateTailCallBlocks(blocks);
  EXPECT_EQ(result.num_groups, 0);
}

// The emitted text matches the directive format from the paper (Fig. 10).
TEST(FormatDirectivesTest, MatchesPaperFormat) {
  std::vector<TailCallBlock> blocks = {
      MakeBlock("bar.cpp", "bar", /*id=*/0, /*address=*/0x1190, kEpilogueA),
      MakeBlock("foo.cpp", "foo", /*id=*/2, /*address=*/0x2000, kEpilogueA),
  };
  TailCallDedupResult result = DeduplicateTailCallBlocks(blocks);
  EXPECT_EQ(FormatDirectives(result),
            "m bar.cpp\n"
            "f bar\n"
            "bbm 0 (DeduBB.master.0)\n"
            "m foo.cpp\n"
            "f foo\n"
            "bbf 2 (DeduBB.master.0)\n");
}

// AArch64 uses a 4-byte branch patch; an 8-byte epilogue still folds.
TEST(DeduplicateTailCallBlocksTest, HonorsArmPatchSize) {
  // ldp x29, x30, [sp], #32 ; ret  (8 bytes).
  const std::vector<uint8_t> kArmEpilogue = {0xfd, 0x7b, 0xc2, 0xa8,
                                             0xc0, 0x03, 0x5f, 0xd6};
  std::vector<TailCallBlock> blocks = {
      MakeBlock("a.cpp", "a", 1, 0x1000, kArmEpilogue),
      MakeBlock("b.cpp", "b", 1, 0x2000, kArmEpilogue),
  };
  TailCallDedupOptions options;
  options.patch_size = 4;  // AArch64 `b`.
  TailCallDedupResult result = DeduplicateTailCallBlocks(blocks, options);
  EXPECT_EQ(result.num_folds, 1);
  EXPECT_EQ(result.bytes_saved, 8 - 4);
}

}  // namespace
}  // namespace propeller
