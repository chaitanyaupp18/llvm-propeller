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

#ifndef PROPELLER_TAIL_CALL_PROFILE_WRITER_H_
#define PROPELLER_TAIL_CALL_PROFILE_WRITER_H_

// Binary-facing glue for DeduBB tail-call deduplication ("Step 1").
//
// `WriteTailCallDedupProfile` reads the basic blocks of `opts.binary_name()`
// from its `SHT_LLVM_BB_ADDR_MAP` section, extracts the raw machine bytes of
// each block that ends in a tail call or return, runs the FNV-1a + byte-by-byte
// deduplication (see `tail_call_dedupper.h`), and writes the resulting
// `m`/`f`/`bbm`/`bbf` directives to `opts.tail_call_profile_out_name()`.
//
// This is a static analysis of the binary: it does not require a perf profile.
// A profile is only consulted when `opts.tail_call_dedup_cold_only()` is set, to
// restrict folding to cold blocks.

#include <vector>

#include "absl/status/status.h"
#include "propeller/bb_addr_map.pb.h"
#include "propeller/binary_content.h"
#include "propeller/propeller_options.pb.h"
#include "propeller/tail_call_dedupper.h"

namespace propeller {

// Builds the deduplication candidate blocks (those ending in a tail call or
// return) from a decoded `BbAddrMapPb`, slicing their raw bytes out of
// `binary_content`. Exposed for testing. If `cold_only` is true, blocks with a
// non-zero post-link frequency are skipped.
std::vector<TailCallBlock> BuildTailCallBlocks(
    const BbAddrMapPb& bb_addr_map, const BinaryContent& binary_content,
    bool cold_only);

// Returns the size in bytes of the jump used to patch a folded block on the
// binary's architecture (x86: 5, AArch64/ARM: 4).
int TailCallPatchSize(const BinaryContent& binary_content);

// Runs DeduBB tail-call deduplication on `opts.binary_name()` and writes the
// directive file to `opts.tail_call_profile_out_name()`. A no-op (returns OK) if
// `tail_call_profile_out_name` is empty.
absl::Status WriteTailCallDedupProfile(const PropellerOptions& opts);

}  // namespace propeller

#endif  // PROPELLER_TAIL_CALL_PROFILE_WRITER_H_
