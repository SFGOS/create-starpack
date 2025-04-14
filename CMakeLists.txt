cmake_minimum_required(VERSION 3.15)

#
# Project Definition
#
set(CMAKE_C_COMPILER /usr/bin/clang)
set(CMAKE_CXX_COMPILER /usr/bin/clang++)
project(create-starpack VERSION 1.0 LANGUAGES CXX)

# Use C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optionally display full compiler/linker commands
set(CMAKE_VERBOSE_MAKEFILE ON)

#
# Basic Dependencies
#
# Threads (pthreads)
find_package(Threads REQUIRED)

# YAML-CPP
# We'll link it statically below using -Wl,-Bstatic.
find_package(yaml-cpp REQUIRED)

# cURL (Dynamically Linked)
find_path(CURL_INCLUDE_DIR curl/curl.h PATHS /usr/local/include /usr/include)
if(NOT CURL_INCLUDE_DIR)
    message(FATAL_ERROR "cURL include directory not found!")
endif()

find_library(CURL_LIBRARY NAMES curl PATHS /usr/local/lib /usr/lib)
if(NOT CURL_LIBRARY)
    message(FATAL_ERROR "Dynamic cURL library (libcurl.so) not found!")
endif()
include_directories(${CURL_INCLUDE_DIR})

# OpenSSL (Dynamically Linked)
find_package(OpenSSL REQUIRED)

# ZLIB (Dynamically Linked)
find_package(ZLIB REQUIRED)

# Zstd (Dynamically Linked)
find_library(ZSTD_LIBRARY zstd REQUIRED)

# nghttp2 (Dynamically Linked)
find_library(NGHTTP2_LIBRARY nghttp2 REQUIRED)

# PCRE (Dynamically Linked)
find_library(PCRE_LIBRARY pcre REQUIRED)

# Brotli (Dynamically Linked)
find_library(BROTLIDEC_LIBRARY brotlidec REQUIRED)
find_library(BROTLIENC_LIBRARY brotlienc REQUIRED)

# libmagic (Dynamically Linked or via pkg-config)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBMAGIC REQUIRED libmagic)
include_directories(${LIBMAGIC_INCLUDE_DIRS})

# libarchive (Likely Dynamically Linked)
# We'll rely on pkg-config. If you wanted it static, you'd set PKG_CONFIG_USE_STATIC_LIBS=ON.
set(PKG_CONFIG_USE_STATIC_LIBS OFF)
pkg_check_modules(LIBARCHIVE REQUIRED libarchive)
include_directories(${LIBARCHIVE_INCLUDE_DIRS})

#
# libgit2 (Statically Linked)
# We'll discover it via pkg-config, then forcibly link it statically.
#
pkg_check_modules(LIBGIT2 REQUIRED libgit2)
include_directories(${LIBGIT2_INCLUDE_DIRS})

#
# libunistring (Dynamically Linked)
#
find_library(UNISTRING_LIBRARY unistring REQUIRED)
if(NOT UNISTRING_LIBRARY)
    message(FATAL_ERROR "Dynamic libunistring not found!")
endif()

#
# Additional dynamic libraries  OpenLDAP, libidn2, libpsl
#
find_library(OPENLDAP_LIBRARY ldap REQUIRED)
find_library(LBER_LIBRARY lber REQUIRED)
find_library(LIBIDN2_LIBRARY idn2 REQUIRED)
find_library(LIBPSL_LIBRARY psl REQUIRED)

#
# Source Files & Includes
#
add_executable(create-starpack
    src/create-starpack.cpp
)
include_directories(include)

#
# Linking Logic
#
# We only want to static-link yaml-cpp and libgit2. Everything else is dynamic.
# We'll use -Wl,-Bstatic before them, then -Wl,-Bdynamic for the rest.
#
target_link_libraries(create-starpack PRIVATE

    # 1) Dynamically Linked Libraries
    ${CURL_LIBRARY}
    ${OPENSSL_LIBRARIES}
    ${ZLIB_LIBRARIES}
    ${ZSTD_LIBRARY}
    ${NGHTTP2_LIBRARY}
    ${PCRE_LIBRARY}
    ${BROTLIDEC_LIBRARY}
    ${BROTLIENC_LIBRARY}
    ${LIBMAGIC_LIBRARIES}
    ${LIBARCHIVE_LIBRARIES}  # from pkg-config
    ${UNISTRING_LIBRARY}
    ${OPENLDAP_LIBRARY}
    ${LBER_LIBRARY}
    ${LIBIDN2_LIBRARY}
    ${LIBPSL_LIBRARY}

    # Pthreads, plus dl and rt if needed for typical Linux systems
    Threads::Threads
    dl
    rt

    # 2) Statically Linked Libraries
    # Switch to static mode with -Wl,-Bstatic
    -Wl,-Bstatic
        yaml-cpp
        ${LIBGIT2_LIBRARIES}
    # Then revert to dynamic with -Wl,-Bdynamic
    -Wl,-Bdynamic
)

#
# Additional Linker Flags
#
# -O2 for some optimization, -ffunction-sections and -fdata-sections plus
# --gc-sections to remove unused code, if you want to minimize the binary size.
#
set(CMAKE_EXE_LINKER_FLAGS
    "${CMAKE_EXE_LINKER_FLAGS} -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections"
)

#
# Install Rules
#
# This step copies the final 'create-starpack' binary to the system.
install(TARGETS create-starpack DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)
