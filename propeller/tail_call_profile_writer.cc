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

#include "propeller/tail_call_profile_writer.h"

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/TargetParser/Triple.h"
#include "propeller/bb_addr_map.h"
#include "propeller/bb_addr_map.pb.h"
#include "propeller/binary_content.h"
#include "propeller/propeller_options.pb.h"
#include "propeller/status_macros.h"  // Included for macros.
#include "propeller/tail_call_dedupper.h"

namespace propeller {
namespace {

// Slices `size` raw bytes at binary address `addr` out of the text section that
// contains it. Mirrors the section lookup used by `MiniDisassembler`.
absl::StatusOr<std::vector<uint8_t>> GetBlockBytes(
    const llvm::object::ObjectFile& object_file, uint64_t addr, uint64_t size) {
  for (const llvm::object::SectionRef& section : object_file.sections()) {
    if (!section.isText() || section.isVirtual()) continue;
    const uint64_t section_addr = section.getAddress();
    const uint64_t section_size = section.getSize();
    if (addr < section_addr || addr + size > section_addr + section_size)
      continue;
    llvm::Expected<llvm::StringRef> content = section.getContents();
    if (!content) {
      return absl::FailedPreconditionError("section has no content");
    }
    const uint64_t offset = addr - section_addr;
    const uint8_t* data =
        reinterpret_cast<const uint8_t*>(content->data()) + offset;
    return std::vector<uint8_t>(data, data + size);
  }
  return absl::NotFoundError(
      absl::StrFormat("no text section contains address 0x%lx", addr));
}

}  // namespace

int TailCallPatchSize(const BinaryContent& binary_content) {
  switch (llvm::Triple::ArchType(binary_content.object_file->getArch())) {
    case llvm::Triple::x86:
    case llvm::Triple::x86_64:
      return 5;  // `jmp rel32`
    case llvm::Triple::aarch64:
    case llvm::Triple::aarch64_be:
    case llvm::Triple::arm:
    case llvm::Triple::armeb:
    case llvm::Triple::thumb:
    case llvm::Triple::thumbeb:
      return 4;  // `b`
    default:
      return 5;
  }
}

std::vector<TailCallBlock> BuildTailCallBlocks(
    const BbAddrMapPb& bb_addr_map, const BinaryContent& binary_content,
    bool cold_only) {
  std::vector<TailCallBlock> blocks;
  for (const ModuleBbAddrMapPb& module : bb_addr_map.module_bb_addr_maps()) {
    for (const FunctionBbAddrMapPb& function :
         module.function_bb_addr_maps()) {
      const std::string function_name =
          function.function_names().empty()
              ? std::string(absl::StrFormat("0x%lx", function.function_address()))
              : std::string(function.function_names(0));
      for (const BbRangePb& bb_range : function.bb_ranges()) {
        for (const BbEntryPb& bb_entry : bb_range.bb_entries()) {
          // Only blocks that end in a tail call or a return are foldable with a
          // single jump (DeduBB Case I).
          if (!bb_entry.has_tail_call() && !bb_entry.has_return()) continue;
          if (bb_entry.size() == 0) continue;
          // Exception-handling pads have special unwinder requirements; leave
          // them alone.
          if (bb_entry.is_eh_pad()) continue;
          // Optionally restrict to cold blocks to preserve hot-path
          // performance (DeduBB profile-guided mode).
          if (cold_only && (bb_entry.post_link_frequency() > 0 || bb_entry.frequency() > 0)) continue;

          const uint64_t address = bb_range.base_address() + bb_entry.offset();
          absl::StatusOr<std::vector<uint8_t>> bytes = GetBlockBytes(
              *binary_content.object_file, address, bb_entry.size());
          if (!bytes.ok()) {
            LOG(WARNING) << "Skipping block " << function_name << "#"
                         << bb_entry.id() << ": " << bytes.status().message();
            continue;
          }
          std::string mod_name = std::string(module.module_name());
          if (mod_name.empty()) {
            size_t uniq_pos = function_name.find(".__uniq.");
            if (uniq_pos != std::string::npos) {
              mod_name = function_name.substr(uniq_pos + 8);
            }
          }
          blocks.push_back(TailCallBlock{mod_name, function_name,
                                         bb_entry.id(), address,
                                         std::max(static_cast<uint64_t>(bb_entry.post_link_frequency()), static_cast<uint64_t>(bb_entry.frequency())),
                                         *std::move(bytes)});
        }
      }
    }
  }
  return blocks;
}

absl::Status WriteTailCallDedupProfile(const PropellerOptions& opts) {
  if (opts.tail_call_profile_out_name().empty()) return absl::OkStatus();

  ASSIGN_OR_RETURN(std::unique_ptr<BinaryContent> binary_content,
                   GetBinaryContent(opts.binary_name()));

  // `GetBbAddrMap` decodes the `SHT_LLVM_BB_ADDR_MAP` section into a proto with
  // module names (via DWARF), function names (via the symbol table), and the
  // per-block tail-call/return flags we need.
  const BbAddrMapPb bb_addr_map = GetBbAddrMap(opts.binary_name());

  std::vector<TailCallBlock> blocks = BuildTailCallBlocks(
      bb_addr_map, *binary_content, opts.tail_call_dedup_cold_only());

  TailCallDedupOptions dedup_options;
  dedup_options.patch_size = TailCallPatchSize(*binary_content);
  dedup_options.intra_module_only = opts.tail_call_dedup_intra_module_only();
  const TailCallDedupResult result =
      DeduplicateTailCallBlocks(blocks, dedup_options);

  const std::string out_path(opts.tail_call_profile_out_name());
  std::ofstream out(out_path, std::ofstream::out | std::ofstream::trunc);
  out << FormatDirectives(result);
  if (!out) {
    return absl::UnavailableError(
        absl::StrFormat("Failed to write DeduBB directives to '%s'", out_path));
  }

  LOG(INFO) << "DeduBB tail-call dedup: " << blocks.size()
            << " candidate blocks, " << result.num_groups << " master group(s), "
            << result.num_folds << " fold(s), ~" << result.bytes_saved
            << " bytes saved; wrote " << opts.tail_call_profile_out_name();
  return absl::OkStatus();
}

}  // namespace propeller
