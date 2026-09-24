# littlefs-cpp

A C++ wrapper for littlefs: every buffer inside the object, no allocation, and a file that closes itself.

Part of [hwlib](https://github.com/integra-lib) — architecture-independent C++20
components shared between firmware projects. Header-only,
no exceptions, no RTTI.

## Use it

littlefs itself is not part of this component, and has no CMake build of its own: the
project provides a target for it, and names it if it is not called `littlefs`.

```bash
git submodule add git@github.com:integra-lib/littlefs-cpp.git external/hwlib/littlefs-cpp
```

```cmake
add_library(littlefs STATIC external/littlefs/lfs.c external/littlefs/lfs_util.c)
target_include_directories(littlefs PUBLIC external/littlefs)

add_subdirectory(external/hwlib/littlefs-cpp)   # -DHWLIB_LITTLEFS_TARGET=<name> if yours differs
target_link_libraries(app PRIVATE Hwlib::littlefs_cpp)
```

```cpp
#include <hwlib/persistence/littlefs.hpp>
```

Verified against littlefs v2.10.1. Each component carries its own include directory,
so this header stays unreachable until the component is linked.

## What it does

```cpp
struct Flash   // anything with these three; Sync() too, if it has one
{
    int Read(std::uint32_t block, std::uint32_t offset, std::uint32_t size, void* out);
    int Write(std::uint32_t block, std::uint32_t offset, const void* in, std::uint32_t size);
    int Erase(std::uint32_t block);
};

hwlib::persistence::Littlefs<Flash> fs{flash, {.blockSize = 4096U, .blockCount = 32U}};
if (const int err = fs.MountOrFormat(); err != LFS_ERR_OK)
{
    LOG_ERR("filesystem: %s", hwlib::persistence::LittlefsErrorName(err).data());
}

hwlib::persistence::Littlefs<Flash>::File file{fs};
if (file.Open("/ring/0001", LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND) == LFS_ERR_OK)
{
    std::ignore = file.Write(record);
}   // closed — and committed — here

const auto files = fs.FileCount("/ring");
std::ignore = fs.ForEach("/ring", hwlib::persistence::LittlefsEntry::eFile, [](const hwlib::persistence::LittlefsEntryInfo& e) {
    Print(e.name, e.size);
});
```

Every call returns littlefs's own result — `LFS_ERR_OK`, a negative `LFS_ERR_*`, or a
size — rather than throwing. `Raw()` hands out the `lfs_t` for anything not wrapped.

## What it is careful about

**Mounting never erases on its own initiative.** `Mount()` only mounts.
`MountOrFormat()` formats only when littlefs reports the flash as corrupt — which is
what blank flash reports on first boot. Any other failure, an I/O error included, is
returned and nothing is erased: a transient read error at boot must not cost the data.

**Nothing is allocated.** The filesystem's read, program and lookahead buffers are
members, and a `File` carries its own cache and opens with `lfs_file_opencfg`. The
component builds and is tested with `LFS_NO_MALLOC`, where the allocating
`lfs_file_open` does not exist.

**A file stays where it was opened.** littlefs keeps a pointer to every open file and
updates it on commits, so `File` can be neither copied nor moved; it closes itself when
it goes out of scope, and closing twice is an error, not a second close.

**Paths are copied, not borrowed.** littlefs takes C strings and a `std::string_view` need
not be terminated, so every path is copied into a terminated buffer of
`LITTLEFS_PATH_MAX` characters first; a longer one is `LFS_ERR_NAMETOOLONG`.

**Directories are never left open.** `ForEach` closes the directory on every way out.
littlefs keeps a pointer to an open directory too, and a directory left open on a
finished stack frame is written through on the next commit.

The device is borrowed and must outlive the filesystem; the filesystem must outlive
every `File` opened on it.

## Geometry

`LittlefsGeometry` defaults to the values a163-cgm-firmware ran with — 128-byte read
and program sizes, 5 block cycles — so a filesystem it formatted mounts unchanged; the
cache and lookahead sizes are template parameters, 128 and 16 by default. `blockCycles`
is the one to revisit: littlefs suggests 100 to 1000, and 5 relocates metadata far
more often than wear levelling needs. It is not stored on flash, so changing it is
safe.

## Coming from a163-cgm-firmware

The wrapper is `utils::Lfs`, `LfsFile`, `ScopedLfsFile` and `LittlefsIterator` from
a163's `lib/littlefs`. Four defects, each confirmed by running the original against
littlefs v2.10.1 on a RAM block device:

* **It formatted on any mount failure.** One transient read error at boot, and every
  file on the device was gone.
* **`FileCount` left its directory open.** It returned with the `lfs_dir_t` on its own
  stack frame still on littlefs's list of open directories; the next file open read
  that dead frame — AddressSanitizer: stack-use-after-return.
* **A moved `ScopedLfsFile` closed a null handle.** The moved-from object kept
  `m_isOpen == true`, so its destructor closed the file a second time and littlefs's
  own assertion stopped it. With `LFS_NO_ASSERT`, as a163's release build has, that is
  a null dereference instead.
* **`LfsFile` passed a `std::string_view` as a C string.** A view of `"/a"` inside
  `"/ab"` opened `"/ab"`.

What changed besides:

* the geometry and tuning, hard-coded in the constructor, became `LittlefsGeometry`;
* `LfsFile` allocated its `lfs_file_t` and opened with the allocating `lfs_file_open`;
  `File` needs neither;
* `LittlefsIterator` became `ForEach`: copying the iterator — which a range-for does —
  copied an open `lfs_dir_t`, and the directory's lifetime was the iterator's to get
  right on every copy;
* the method names follow the library's style (`Mkdir`, `Remove`, `FileCount`), and
  `ErrorToString` is `LittlefsErrorName`.

Not carried over: the `LFS_THREADSAFE` locking, which a163 never enabled and which
locked one static mutex for every filesystem in the process; and `lfs_api.hpp`, the
ring-of-files layer on top, which is that product's.

## Versioning

Every component is released on its own, tagged `vX.Y.Z`. Pre-1.0, a minor release may
break the API, which is why dependants accept a single minor.

```bash
git -C external/hwlib/littlefs-cpp fetch --tags
git -C external/hwlib/littlefs-cpp checkout v0.2.0
git add external/hwlib/littlefs-cpp && git commit -m "build: bump littlefs-cpp to v0.2.0"
```

## In a consumer's CI

The component is an ordinary submodule, so the build needs it checked out. On GitLab
that means `GIT_SUBMODULE_STRATEGY: normal` (or `recursive`) on every job that builds —
not only on the ones that run unit tests.

## Develop it

```bash
git submodule update --init          # ci-shared, needed by pre-commit
cmake -S . -B build && cmake --build build -j && ctest --test-dir build
```

Built on its own, the component fetches littlefs v2.10.1 and GoogleTest, builds littlefs
with `LFS_NO_MALLOC`, and runs the tests under AddressSanitizer and UndefinedBehavior-
Sanitizer. A consumer never builds the tests and never fetches either.

The style configs are symlinks into the `ci-shared` submodule, and the pipeline comes
from the same place. On GitHub this repository carries a self-contained build-and-test
workflow instead; the shared setup is what GitLab will use.
