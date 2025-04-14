# create-starpack

`create-starpack` is a command-line tool written in C++ designed to build Starpack packages (`.starpack`) from source code and build instructions defined in a `STARBUILD` file. It automates the process of fetching sources, compiling code, and packaging the results into the Starpack format, intended for use with the **Starpack package manager** for the **sfgos** operating system.

## Features

* **Parses `STARBUILD` files:** Reads package metadata and build steps from a custom `STARBUILD` script.
* **Source Fetching:**
    * Downloads source files from HTTP/HTTPS URLs using `libcurl`, with progress indication.
    * Clones Git repositories using `libgit2`, with progress indication. Supports `git+URL` syntax.
    * Copies local source files referenced in the `STARBUILD`.
    * Supports custom download filenames using `filename::URL` syntax.
* **Archive Extraction:** Automatically extracts downloaded/copied archives (tarballs, zip files, etc.) using `libarchive` and `libmagic` (unless the source contains "NOEXTRACT").
* **Build Script Execution:** Executes standard build phases defined in the `STARBUILD` (`prepare`, `compile`, `verify`, `assemble`) within a bash shell environment.
* **Subpackage Support:** Handles `STARBUILD` files defining multiple output packages from a single build process, with specific dependencies and assembly steps (`dependencies_subpkg`, `assemble_subpkg`).
* **Fakeroot Integration:** Optionally runs build steps (`prepare`, `compile`, `verify`, `assemble`) under `fakeroot` to simulate root privileges for file ownership/permissions (default for non-root users).
* **Post-Processing:**
    * Optionally strips unneeded symbols and debug information from ELF binaries using the system `strip` command.
    * Removes Libtool archive (`.la`) and static library (`.a`) files.
* **Packaging:** Creates the final `.starpack` archive (gzipped tarball) containing the built files under a `files/` prefix and a `metadata.yaml` file.
* **Symlink Creation:** Creates symlinks specified via `symlink: "link:target"` lines in the `STARBUILD` file within the package directory before final archiving.
* **Cleanup:** Optionally removes intermediate source and build directories after a successful build.
* **Root Execution Warning:** Warns the user and requires confirmation if run directly as the root user.

## Dependencies

### Build-Time Dependencies

* A C++17 compliant compiler (like GCC or Clang)
* CMake (Recommended for building)
* `libcurl` development files (e.g., `libcurl4-openssl-dev` on Debian/Ubuntu)
* `libarchive` development files (e.g., `libarchive-dev`)
* `libmagic` development files (e.g., `libmagic-dev`)
* `libyaml-cpp` development files (e.g., `libyaml-cpp-dev`)
* `libgit2` development files (e.g., `libgit2-dev`)

### Run-Time Dependencies

* `libcurl` shared library
* `libarchive` shared library
* `libmagic` shared library
* `libyaml-cpp` shared library
* `libgit2` shared library
* `tar` utility
* `gzip` utility
* `/bin/bash`

### Optional Run-Time Dependencies

* `fakeroot`: Required if running as a non-root user and needing to manage file permissions/ownership during the build (enabled by default for non-root).
* `strip` (from `binutils`): Required for binary stripping (enabled by default unless `--nostrip` is used).

## Building

Assuming you have the build-time dependencies installed:

```bash
# Clone the repository
git clone https://github.com/SFGOS/create-starpack.git
cd create-starpack

# Create a build directory
mkdir build
cd build

# Configure the build with CMake
cmake ..

# Compile the project
make

# The executable 'create-starpack' should now be in the build directory
# Optionally, install it (may require sudo)
# sudo make install
