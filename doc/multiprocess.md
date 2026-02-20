# Multiprocess Aix

_This document describes usage of the multiprocess feature. For design information, see the [design/multiprocess.md](design/multiprocess.md) file._

## Build Option

The `-DENABLE_IPC=ON` build option, supported and enabled by default on Unix systems, can be passed to build the supplemental `aix-node` and `aix-gui` multiprocess executables.

## Debugging

The `-debug=ipc` command line option can be used to see requests and responses between processes.

## Installation

Specifying `-DENABLE_IPC=ON` requires [Cap'n Proto](https://capnproto.org/) to be installed. See [build-unix.md](build-unix.md) and [build-osx.md](build-osx.md) for information about installing dependencies.

### Depends installation

Alternatively the [depends system](../depends) can be used to avoid needing to install local dependencies:

```
cd <BITCOIN_SOURCE_DIRECTORY>
make -C depends NO_QT=1
# Set host platform to output of gcc -dumpmachine or clang -dumpmachine or check the depends/ directory for the generated subdirectory name
HOST_PLATFORM="x86_64-pc-linux-gnu"
cmake -B build --toolchain=depends/$HOST_PLATFORM/toolchain.cmake
cmake --build build
build/bin/aix -m node -regtest -printtoconsole -debug=ipc
BITCOIN_CMD="aix -m" build/test/functional/test_runner.py
```

The `cmake` build will pick up settings and library locations from the depends directory, so there is no need to pass `-DENABLE_IPC=ON` as a separate flag when using the depends system (it's controlled by the `NO_IPC=1` option).

### Cross-compiling

When cross-compiling and not using depends, native code generation tools from [libmultiprocess](https://github.com/aix-core/libmultiprocess) and [Cap'n Proto](https://capnproto.org/) are required. They can be passed to the cmake build by specifying `-DMPGEN_EXECUTABLE=/path/to/mpgen -DCAPNP_EXECUTABLE=/path/to/capnp -DCAPNPC_CXX_EXECUTABLE=/path/to/capnpc-c++` options.

### External libmultiprocess installation

By default when `-DENABLE_IPC=ON` is enabled, the libmultiprocess sources at [../src/ipc/libmultiprocess/](../src/ipc/libmultiprocess/) are built as part of the aix cmake build, but alternately an external [libmultiprocess](https://github.com/aix-core/libmultiprocess/) cmake package can be used instead by following its [installation instructions](https://github.com/aix-core/libmultiprocess/blob/master/doc/install.md) and specifying `-DWITH_EXTERNAL_LIBMULTIPROCESS=ON` to the aix build, so it will use the external package instead of the sources. This can be useful when making changes to the upstream project. If libmultiprocess is not installed in a default system location it is possible to specify the [`CMAKE_PREFIX_PATH`](https://cmake.org/cmake/help/latest/envvar/CMAKE_PREFIX_PATH.html) environment variable to point to the installation prefix where libmultiprocess is installed.

## Usage

Recommended way to use multiprocess binaries is to invoke `aix` CLI like `aix -m node -debug=ipc` or `aix -m gui -printtoconsole -debug=ipc`.

When the `-m` (`--multiprocess`) option is used the `aix` command will execute multiprocess binaries instead of monolithic ones (`aix-node` instead of `aixd`, and `aix-gui` instead of `aix-qt`). The multiprocess binaries can also be invoked directly, but this is not recommended as they may change or be renamed in the future, and they are not installed in the PATH.

The multiprocess binaries currently function the same as the monolithic binaries, except they support an `-ipcbind` option.

In the future, after [#10102](https://github.com/aix/aix/pull/10102) they will have other differences. Specifically `aix-gui` will spawn a `aix-node` process to run P2P and RPC code, communicating with it across a socket pair, and `aix-node` will spawn `aix-wallet` to run wallet code, also communicating over a socket pair. This will let node, wallet, and GUI code run in separate address spaces for better isolation, and allow future improvements like being able to start and stop components independently on different machines and environments. [#19460](https://github.com/aix/aix/pull/19460) also adds a new `aix-wallet -ipcconnect` option to allow new wallet processes to connect to an existing node process.
And [#19461](https://github.com/aix/aix/pull/19461) adds a new `aix-gui -ipcconnect` option to allow new GUI processes to connect to an existing node process.
