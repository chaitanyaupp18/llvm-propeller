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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace propeller {

uint64_t Fnv1aHash(const uint8_t* data, size_t size) {
  // 64-bit FNV-1a constants.
  constexpr uint64_t kOffsetBasis = 0xcbf29ce484222325ULL;
  constexpr uint64_t kPrime = 0x100000001b3ULL;
  uint64_t hash = kOffsetBasis;
  for (size_t i = 0; i < size; ++i) {
    hash ^= static_cast<uint64_t>(data[i]);
    hash *= kPrime;
  }
  return hash;
}

uint64_t Fnv1aHash(const std::vector<uint8_t>& bytes) {
  return Fnv1aHash(bytes.data(), bytes.size());
}

namespace {

// A set of blocks (referenced by index into the caller's `blocks` vector) whose
// machine bytes are all identical. The first element (lowest address) is the
// master copy.
struct IdenticalGroup {
  std::vector<int> members;  // indices into `blocks`, in ascending address order
};

// Orders blocks deterministically; the hottest is promoted to the master.
// We prioritize functions that do not have a ThinLTO ".llvm." suffix,
// so that the master block is placed in a stable symbol name.
bool BlockOrderLess(const TailCallBlock& a, const TailCallBlock& b) {
  if (a.frequency != b.frequency) {
    return a.frequency > b.frequency;
  }
  bool a_has_llvm = a.function_name.find(".llvm.") != std::string::npos;
  bool b_has_llvm = b.function_name.find(".llvm.") != std::string::npos;
  if (a_has_llvm != b_has_llvm) {
    return !a_has_llvm;  // prefer a if it does NOT have .llvm.
  }
  return std::tie(a.address, a.module_name, a.function_name, a.bb_id) <
         std::tie(b.address, b.module_name, b.function_name, b.bb_id);
}

// Orders directives for output: grouped by module, then function, then bb id.
bool DirectiveOrderLess(const Directive& a, const Directive& b) {
  return std::tie(a.module_name, a.function_name, a.bb_id) <
         std::tie(b.module_name, b.function_name, b.bb_id);
}

}  // namespace

TailCallDedupResult DeduplicateTailCallBlocks(
    const std::vector<TailCallBlock>& blocks,
    const TailCallDedupOptions& options) {
  TailCallDedupResult result;

  // Process blocks in a deterministic order so master selection (first in a
  // group) and master-id assignment are reproducible.
  std::vector<int> order(blocks.size());
  for (int i = 0; i < static_cast<int>(blocks.size()); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](int x, int y) {
    return BlockOrderLess(blocks[x], blocks[y]);
  });

  // Bucket by FNV-1a hash, then split each bucket into byte-identical groups.
  // The hash is only a fast filter; group membership is decided by an exact
  // byte-by-byte comparison, which rules out hash collisions.
  std::unordered_map<uint64_t, std::vector<IdenticalGroup>> buckets;
  std::vector<IdenticalGroup*> all_groups;  // stable pointers into `buckets`
  for (int idx : order) {
    const TailCallBlock& block = blocks[idx];
    if (block.bytes.empty()) continue;
    uint64_t hash = Fnv1aHash(block.bytes);
    std::vector<IdenticalGroup>& bucket = buckets[hash];

    IdenticalGroup* match = nullptr;
    for (IdenticalGroup& group : bucket) {
      // Verify against the group's representative (master) byte-for-byte.
      if (blocks[group.members.front()].bytes == block.bytes) {
        if (!options.intra_module_only ||
            blocks[group.members.front()].module_name == block.module_name) {
          match = &group;
          break;
        }
      }
    }
    if (match == nullptr) {
      bucket.push_back(IdenticalGroup{{idx}});
      // NOTE: pointers into a vector are invalidated on growth; we collect the
      // final pointers in a second pass below instead of here.
    } else {
      match->members.push_back(idx);
    }
  }

  // Collect groups that are worth folding into a flat, deterministically ordered
  // list (sorted by the master block) so master ids are stable.
  for (auto& [hash, bucket] : buckets) {
    for (IdenticalGroup& group : bucket) {
      const int count = static_cast<int>(group.members.size());
      if (count < options.min_occurrences) continue;
      const int size = static_cast<int>(blocks[group.members.front()].bytes.size());
      // Size benefit of folding `count` identical blocks of `size` bytes each by
      // keeping one master in place and replacing the other `count - 1` copies
      // with a `patch_size`-byte jump.
      const int64_t benefit =
          static_cast<int64_t>(count - 1) * (size - options.patch_size);
      if (size <= options.patch_size || benefit <= 0) continue;
      all_groups.push_back(&group);
    }
  }
  std::sort(all_groups.begin(), all_groups.end(),
            [&](const IdenticalGroup* x, const IdenticalGroup* y) {
              return BlockOrderLess(blocks[x->members.front()],
                                    blocks[y->members.front()]);
            });

  // Emit directives and tally stats.
  uint64_t master_id = options.first_master_id;
  for (const IdenticalGroup* group : all_groups) {
    const int count = static_cast<int>(group->members.size());
    const int size = static_cast<int>(blocks[group->members.front()].bytes.size());

    const TailCallBlock& master = blocks[group->members.front()];
    result.directives.push_back(Directive{DirectiveKind::kMaster,
                                          master.module_name,
                                          master.function_name, master.bb_id,
                                          master_id});
    for (size_t i = 1; i < group->members.size(); ++i) {
      const TailCallBlock& fold = blocks[group->members[i]];
      result.directives.push_back(Directive{DirectiveKind::kFold,
                                            fold.module_name,
                                            fold.function_name, fold.bb_id,
                                            master_id});
    }

    ++result.num_groups;
    ++result.num_masters;
    result.num_folds += count - 1;
    result.bytes_saved += static_cast<int64_t>(count - 1) * (size - options.patch_size);
    ++master_id;
  }

  std::sort(result.directives.begin(), result.directives.end(),
            DirectiveOrderLess);
  return result;
}

std::string FormatDirectives(const TailCallDedupResult& result) {
  std::string out;
  const std::string* cur_module = nullptr;
  const std::string* cur_function = nullptr;
  for (const Directive& d : result.directives) {
    if (cur_module == nullptr || *cur_module != d.module_name) {
      out += "m ";
      out += d.module_name;
      out += "\n";
      cur_module = &d.module_name;
      cur_function = nullptr;  // force re-emitting the function header
    }
    if (cur_function == nullptr || *cur_function != d.function_name) {
      out += "f ";
      out += d.function_name;
      out += "\n";
      cur_function = &d.function_name;
    }
    out += (d.kind == DirectiveKind::kMaster) ? "bbm " : "bbf ";
    out += std::to_string(d.bb_id);
    out += " (DeduBB.master.";
    out += std::to_string(d.master_id);
    out += ")\n";
  }
  return out;
}

}  // namespace propeller
