# LLVM Propeller

Propeller is a profile-guided, relinking optimizer for warehouse-scale
applications. It is built on top of LLVM and provides a framework for computing
whole-program optimizations for applications built with LLVM.

_For artifact evaluation, see [ArtifactEvaluation/README.md](ArtifactEvaluation/README.md)_

## Quickstart

### Prerequisites and dependencies

| Operating System | Version  |
| --- | --- |
| Ubuntu | 22.04 or newer |

While the Propeller build system automatically pulls in most of its dependencies,
you will need to install a few packages manually:

```
# Common dependencies
sudo apt install -y wget lsb-release software-properties-common gnupg

# Propeller builds with Clang 19.0.0 or newer.
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 19
export CC=clang-19
export CXX=clang++-19

# For the CMake build
sudo apt-get update && sudo apt-get install -y \
    libelf-dev \
    libssl-dev \
    libzstd-dev
```

### Building Propeller from source

The Propeller repository provides both CMake and Bazel build configurations.
The CMake build requires CMake 3.24 or newer, and the Bazel build requires
Bazel 5.0 or newer.

#### CMake
```
cmake -G Ninja -B build
ninja -C build generate_propeller_profiles

# Build and run tests (optional)
ninja -C build
ninja -C build test
```

#### Bazel
```
bazel build //propeller:generate_propeller_profiles

# Build and run tests (optional)
bazel test //propeller/...:all
```

### Generating a Propeller profile
```
./generate_propeller_profiles \
    --binary=/path/to/profiled/binary \
    --profile=/path/to/input/profile.txt \
    --cc_profile=/path/to/out/cc_profile.txt \
    --ld_profile=/path/to/out/ld_profile.txt
```

Propeller generates compiler profiles (`cc_profile.txt`) and linker profiles
(`ld_profile.txt`). The compiler profile is used by LLVM to guide optimizations
and is described in [Propeller Profile Format](propeller_profile_format.md).

### Generating a Tail-Call Deduplication Profile (DeduBB)
This fork of Propeller adds support for cross-module basic block deduplication (DeduBB). To generate a deduplication profile, run Propeller against a binary compiled with `-fbasic-block-address-map`:

```bash
./generate_propeller_profiles \
    --binary=/path/to/binary_with_bbaddrmap \
    --tail_call_profile=dedubb_directives.txt
```

You can then pass these directives to the LLVM CodeGen backend to fold identical blocks across translation units:
```bash
clang++ -O2 -fbasic-block-address-map -mllvm -dedubb-directives=dedubb_directives.txt ...
```

If you are compiling with ThinLTO (`-flto=thin`), you must explicitly instruct the ThinLTO linker backend to retain the address map using `-Wl,--lto-basic-block-address-map`:
```bash
clang++ -O2 -flto=thin -fuse-ld=lld \
    -Wl,--lto-basic-block-address-map \
    -Wl,-mllvm,-dedubb-directives=dedubb_directives.txt ...
```
