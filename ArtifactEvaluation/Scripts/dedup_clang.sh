#!/bin/bash

## This script does the following:
## 1. It checks out and builds trunk LLVM.
## 2. It builds a Clang binary with basic-block-address-map enabled.
## 3. It runs Propeller to generate cross-module tail-call deduplication directives.
## 4. It builds a final DeduBB-optimized Clang binary.
## 5. It compares the binary sizes to measure deduplication savings.

set -eux

CWD="$(pwd)"
BASE_DIR=${CWD}/clang_dedubb_binaries
if [[ -d "${BASE_DIR}" ]]; then
    mv ${BASE_DIR} "${CWD}/clang_dedubb_binaries.old"
fi
mkdir -p "${BASE_DIR}"

PATH_TO_LLVM_SOURCES=${BASE_DIR}/sources
PATH_TO_TRUNK_LLVM_BUILD=${BASE_DIR}/trunk_llvm_build
PATH_TO_TRUNK_LLVM_INSTALL=${BASE_DIR}/trunk_llvm_install
PATH_TO_ALL_BINARIES=${BASE_DIR}/PreBuiltBinaries
PATH_TO_PROFILES=${BASE_DIR}/Profiles
PATH_TO_ALL_RESULTS=${BASE_DIR}/Results
mkdir -p ${PATH_TO_ALL_RESULTS}
mkdir -p ${PATH_TO_ALL_BINARIES}
mkdir -p ${PATH_TO_PROFILES}

date > ${PATH_TO_ALL_RESULTS}/script_start_time.txt

# 1. Build Trunk LLVM
mkdir -p ${PATH_TO_LLVM_SOURCES} && cd ${PATH_TO_LLVM_SOURCES}
git clone https://github.com/llvm/llvm-project.git
cd ${PATH_TO_LLVM_SOURCES}/llvm-project && git reset --hard acbd822
mkdir -p ${PATH_TO_TRUNK_LLVM_BUILD} && cd ${PATH_TO_TRUNK_LLVM_BUILD}
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_ENABLE_PROJECTS="clang;lld;compiler-rt" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DLLVM_USE_LINKER=lld -DCMAKE_INSTALL_PREFIX="${PATH_TO_TRUNK_LLVM_INSTALL}" -DLLVM_ENABLE_RTTI=On -DLLVM_INCLUDE_TESTS=Off ${PATH_TO_LLVM_SOURCES}/llvm-project/llvm
ninja install
CLANG_VERSION=$(sed -Ene 's!^CLANG_EXECUTABLE_VERSION:STRING=(.*)$!\1!p' ${PATH_TO_TRUNK_LLVM_BUILD}/CMakeCache.txt)

# 2. Build BBAddrMap Baseline
COMMON_CMAKE_FLAGS=(
  "-DLLVM_OPTIMIZED_TABLEGEN=On"
  "-DCMAKE_BUILD_TYPE=Release"
  "-DLLVM_TARGETS_TO_BUILD=X86"
  "-DLLVM_ENABLE_PROJECTS=clang"
  "-DCMAKE_C_COMPILER=${PATH_TO_TRUNK_LLVM_INSTALL}/bin/clang"
  "-DCMAKE_CXX_COMPILER=${PATH_TO_TRUNK_LLVM_INSTALL}/bin/clang++"
  "-DLLVM_USE_LINKER=lld"
  "-DLLVM_ENABLE_LTO=Thin" )

INSTRUMENTED_PROPELLER_CC_LD_CMAKE_FLAGS=(
  "-DCMAKE_C_FLAGS=-funique-internal-linkage-names -fbasic-block-address-map"
  "-DCMAKE_CXX_FLAGS=-funique-internal-linkage-names -fbasic-block-address-map"
  "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -Wl,--lto-basic-block-address-map"
  "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld -Wl,--lto-basic-block-address-map"
  "-DCMAKE_MODULE_LINKER_FLAGS=-fuse-ld=lld -Wl,--lto-basic-block-address-map" )

PATH_TO_BBADDRMAP_CLANG_BUILD=${BASE_DIR}/bbaddrmap_clang_build
mkdir -p ${PATH_TO_BBADDRMAP_CLANG_BUILD} && cd ${PATH_TO_BBADDRMAP_CLANG_BUILD}
cmake -G Ninja "${COMMON_CMAKE_FLAGS[@]}" "${INSTRUMENTED_PROPELLER_CC_LD_CMAKE_FLAGS[@]}" ${PATH_TO_LLVM_SOURCES}/llvm-project/llvm
ninja clang

# 3. Generate DeduBB Directives
cd ${CWD}
# Ensure generate_propeller_profiles is built
ninja -C ../../build generate_propeller_profiles
PATH_TO_GENERATE_PROFILES=${CWD}/../../build/propeller/generate_propeller_profiles

/usr/bin/time -v ${PATH_TO_GENERATE_PROFILES} --binary=${PATH_TO_BBADDRMAP_CLANG_BUILD}/bin/clang-${CLANG_VERSION} --tail_call_profile=${PATH_TO_PROFILES}/dedubb_directives.txt 2> ${PATH_TO_ALL_RESULTS}/mem_propeller_dedup_conversion.txt

# 4. Build DeduBB Optimized Clang
OPTIMIZED_DEDUBB_CC_LD_CMAKE_FLAGS=(
  "-DCMAKE_C_FLAGS=-funique-internal-linkage-names -fbasic-block-address-map"
  "-DCMAKE_CXX_FLAGS=-funique-internal-linkage-names -fbasic-block-address-map"
  "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -Wl,--lto-basic-block-address-map -Wl,-mllvm,-dedubb-directives=${PATH_TO_PROFILES}/dedubb_directives.txt"
  "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld -Wl,--lto-basic-block-address-map -Wl,-mllvm,-dedubb-directives=${PATH_TO_PROFILES}/dedubb_directives.txt"
  "-DCMAKE_MODULE_LINKER_FLAGS=-fuse-ld=lld -Wl,--lto-basic-block-address-map -Wl,-mllvm,-dedubb-directives=${PATH_TO_PROFILES}/dedubb_directives.txt" )

PATH_TO_OPTIMIZED_DEDUBB_BUILD=${BASE_DIR}/optimized_dedubb_build
mkdir -p ${PATH_TO_OPTIMIZED_DEDUBB_BUILD} && cd ${PATH_TO_OPTIMIZED_DEDUBB_BUILD}
cmake -G Ninja "${COMMON_CMAKE_FLAGS[@]}" "${OPTIMIZED_DEDUBB_CC_LD_CMAKE_FLAGS[@]}" ${PATH_TO_LLVM_SOURCES}/llvm-project/llvm
ninja clang

# 5. Measure Sizes
printf "Baseline BBAddrMap Stats\n" > ${BASE_DIR}/Results/sizes_clang_dedup.txt
ls -l ${PATH_TO_BBADDRMAP_CLANG_BUILD}/bin/clang-${CLANG_VERSION} | awk '{print $5}' >> ${BASE_DIR}/Results/sizes_clang_dedup.txt

printf "\nDeduBB Optimized Stats\n" >> ${BASE_DIR}/Results/sizes_clang_dedup.txt
ls -l ${PATH_TO_OPTIMIZED_DEDUBB_BUILD}/bin/clang-${CLANG_VERSION} | awk '{print $5}' >> ${BASE_DIR}/Results/sizes_clang_dedup.txt

date > ${PATH_TO_ALL_RESULTS}/script_end_time.txt
cat ${BASE_DIR}/Results/sizes_clang_dedup.txt
