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

#ifndef PROPELLER_TAIL_CALL_DEDUPPER_H_
#define PROPELLER_TAIL_CALL_DEDUPPER_H_

// DeduBB tail-call basic-block deduplication core (LCTES'26).
//
// This is the dependency-free heart of "Step 1": given the candidate basic
// blocks that end in a tail call or a return, it identifies byte-identical
// duplicates with the FNV-1a hash (a fast O(1) average filter) followed by a
// byte-by-byte verification (to rule out hash collisions), then folds the
// duplicates that yield a net size reduction. The result is a list of
// directives in the `bbm`/`bbf` form consumed by the LLVM CodeGen side.
//
// The matching is intentionally *literal byte equality*: two blocks are folded
// only when their machine bytes are identical. This is always safe -- a folded
// block ending in a tail call or return is replaced by a single jump to the
// master copy, and jumping (rather than calling) preserves the current stack
// frame, so executing the master's tail/return transfers control exactly as the
// original block would have. Position-dependent encodings (e.g. a tail `jmp`
// with a PC-relative displacement that differs by location) simply will not
// match, so no unsafe fold is ever emitted.
//
// This header deliberately depends only on the C++ standard library so the
// algorithm can be unit-tested and demonstrated in isolation, without pulling in
// LLVM/abseil/protobuf. The binary-facing glue lives in
// `tail_call_profile_writer.h`.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace propeller {

// 64-bit FNV-1a hash (Fowler-Noll-Vo, Noll). Used as the fast bucket key before
// the byte-by-byte verification.
uint64_t Fnv1aHash(const uint8_t* data, size_t size);
uint64_t Fnv1aHash(const std::vector<uint8_t>& bytes);

// A basic block that is a candidate for tail-call deduplication, i.e. it ends
// with a tail call or a return so it can be folded with a single jump to a
// master copy.
struct TailCallBlock {
  std::string module_name;     // Source module ("" if unknown).
  std::string function_name;   // Function symbol the block belongs to.
  uint32_t bb_id = 0;          // BB id within the function (matches BBAddrMap).
  uint64_t address = 0;        // Binary address (used for deterministic order).
  uint64_t frequency = 0;      // Execution frequency (used for master selection).
  std::vector<uint8_t> bytes;  // Raw machine bytes of the block.
};

// Kind of an emitted directive: `bbm` (master copy) or `bbf` (fold to master).
enum class DirectiveKind { kMaster, kFold };

// One emitted directive line. A group of identical blocks produces exactly one
// `kMaster` directive (the retained copy, labelled `DeduBB.master.<master_id>`)
// and one `kFold` directive per duplicate (replaced by a jump to the master).
struct Directive {
  DirectiveKind kind = DirectiveKind::kFold;
  std::string module_name;
  std::string function_name;
  uint32_t bb_id = 0;
  uint64_t master_id = 0;  // K in `DeduBB.master.K`; shared across the group.
};

struct TailCallDedupOptions {
  // Bytes needed to patch a folded block with a single jump to the master.
  // x86-64 `jmp rel32` is 5 bytes; AArch64 `b` is 4 bytes.
  int patch_size = 5;
  // Minimum number of identical copies in a group before folding is considered.
  int min_occurrences = 2;
  // First value of K used for `DeduBB.master.K` symbol names. Any unique
  // numbering works; the CodeGen side only relies on master/fold lines of the
  // same group sharing the same K.
  uint64_t first_master_id = 0;
  // If true, only deduplicate blocks that belong to the same module.
  bool intra_module_only = false;
};

struct TailCallDedupResult {
  // Directives to emit, in deterministic order (by module, function, bb id).
  std::vector<Directive> directives;
  int num_groups = 0;       // Number of master groups (folded).
  int num_masters = 0;      // Number of `bbm` directives (== num_groups).
  int num_folds = 0;        // Number of `bbf` directives.
  int64_t bytes_saved = 0;  // Estimated `.text` reduction in bytes.
};

// Runs the DeduBB tail-call dedup algorithm over `blocks`:
//   1. bucket blocks by FNV-1a hash of their bytes,
//   2. within a bucket, group blocks with *identical* bytes (byte-by-byte),
//   3. fold each group whose size benefit `(count-1) * (size - patch_size)` is
//      positive, promoting the first block (in address order) to the master.
// The input is not mutated; ordering of the output is deterministic.
TailCallDedupResult DeduplicateTailCallBlocks(
    const std::vector<TailCallBlock>& blocks,
    const TailCallDedupOptions& options = {});

// Formats `result` as the textual directive file consumed by the LLVM CodeGen
// side:
//
//   m <module>
//   f <function>
//   bbm <id> (DeduBB.master.<K>)
//   bbf <id> (DeduBB.master.<K>)
//
// `m`/`f` headers are emitted whenever the module/function changes.
std::string FormatDirectives(const TailCallDedupResult& result);

}  // namespace propeller

#endif  // PROPELLER_TAIL_CALL_DEDUPPER_H_
