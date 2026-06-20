# Money Books — cross-platform build (macOS / Windows / Linux).
#
# This is the portable successor to the Makefile (which stays for macOS dev tooling:
# `make leaks`, `make analyze`, `make deadcode`). CMake is what builds Windows.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Debug      # configure (Debug ⇒ sanitized tests)
#   cmake --build build --target test_runner     # build the test runner
#   ctest --test-dir build --output-on-failure   # run the suite
#   cmake --build build --target money-books-mcp  # the stdio MCP server
#   cmake --build build --target MoneyBooks       # the GUI app (needs webview + iroh vendored)
#
# Engine sources are compiled per-target (not as one shared library) on purpose: the
# in-tree unit tests compile only under -DMB_TEST, and the GUI app compiles the iroh
# transport only under -DMB_WITH_SHARE. A shared object library can't express both.
cmake_minimum_required(VERSION 3.20)
project(MoneyBooks VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Sanitizers mirror `make test` (ASan+UBSan). Applied to the test runner only — the
# Makefile builds mcp/app/share-demo without them. Off under MSVC (no UBSan there).
set(_mb_san_default ON)
if(MSVC)
  set(_mb_san_default OFF)
endif()
option(MB_SANITIZE "Build the test runner with ASan+UBSan" ${_mb_san_default})

# Strict warnings (our C only). Enforced via the test runner, which compiles every
# engine source — matching the Makefile, where only the test build carries -Werror.
# MSVC wasn't a target when the engine was written, so start lenient there.
if(MSVC)
  set(MB_WARNINGS /W3)
else()
  set(MB_WARNINGS
    -Wall -Wextra -Wpedantic -Werror
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wmissing-variable-declarations
    -Wno-gnu-zero-variadic-macro-arguments
    -Wconversion -Wsign-conversion -Wno-error=conversion -Wno-error=sign-conversion)
endif()

set(MB_SAN_FLAGS "")
if(MB_SANITIZE)
  set(MB_SAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
endif()

# ---------------------------------------------------------------------------
# Vendored SQLite — its own translation unit, warnings off, never sanitized
# (third-party; ASan would only slow it). Compile-time options live in the wrapper.
# ---------------------------------------------------------------------------
add_library(mb_sqlite STATIC src/sqlite/sqlite_amalgamation.c)
target_include_directories(mb_sqlite PUBLIC src/vendor/sqlite)
if(NOT MSVC)
  target_compile_options(mb_sqlite PRIVATE -w)
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(mb_sqlite PUBLIC pthread dl m)   # SQLite's link deps on Linux
endif()

# ---------------------------------------------------------------------------
# Engine source list — mirrors the Makefile's find-glob exclusions.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE MB_ENGINE_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.c)
list(FILTER MB_ENGINE_SRC EXCLUDE REGEX "/src/(test|vendor|app|mcpd|sharedemo|sqlite)/")

# Common wiring shared by every binary (includes + the vendored SQLite).
function(mb_target_common tgt)
  target_include_directories(${tgt} PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/vendor/sqlite)
  target_link_libraries(${tgt} PRIVATE mb_sqlite)
endfunction()

# ---------------------------------------------------------------------------
# Test runner — engine + integration tests + in-tree unit tests, -DMB_TEST.
# This target carries the strict warnings and the sanitizers.
# ---------------------------------------------------------------------------
file(GLOB MB_TEST_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*.c ${CMAKE_SOURCE_DIR}/src/test/*.c)
add_executable(test_runner ${MB_ENGINE_SRC} ${MB_TEST_SRC})
mb_target_common(test_runner)
target_compile_definitions(test_runner PRIVATE MB_TEST)
target_compile_options(test_runner PRIVATE ${MB_WARNINGS} ${MB_SAN_FLAGS})
target_link_options(test_runner PRIVATE ${MB_SAN_FLAGS})

enable_testing()
add_test(NAME suite COMMAND test_runner)

# ---------------------------------------------------------------------------
# Stdio MCP server — engine + the mcpd entry point. Rust-free, GUI-free; the
# first thing expected to build on a fresh Windows toolchain.
# ---------------------------------------------------------------------------
add_executable(money-books-mcp ${MB_ENGINE_SRC} src/mcpd/main.c)
mb_target_common(money-books-mcp)
target_compile_options(money-books-mcp PRIVATE $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-O2>)

# ---------------------------------------------------------------------------
# GUI app (MoneyBooks): webview shell + live read-only sharing over iroh.
# Built only when the vendored, gitignored deps are present (fetch scripts run).
# ---------------------------------------------------------------------------
set(WV ${CMAKE_SOURCE_DIR}/src/vendor/webview)
set(IROH_DIR ${CMAKE_SOURCE_DIR}/src/vendor/iroh-c-ffi)

if(WIN32)
  set(IROH_LIB ${IROH_DIR}/target/release/iroh_c_ffi.lib)
else()
  set(IROH_LIB ${IROH_DIR}/target/release/libiroh_c_ffi.a)
endif()

if(EXISTS ${WV}/core/src/webview.cc AND EXISTS ${IROH_DIR})
  enable_language(CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)

  # webview backend, compiled as C++ in its own warnings-relaxed lib.
  add_library(mb_webview STATIC ${WV}/core/src/webview.cc)
  target_compile_definitions(mb_webview PUBLIC WEBVIEW_STATIC)
  target_include_directories(mb_webview PUBLIC ${WV}/core/include)

  # iroh staticlib (the lone Rust dependency), built by cargo. The custom command has no
  # source deps listed, so CMake (re)builds it only when the artifact is missing — run the
  # `share_lib` target explicitly to force a rebuild, mirroring `make share-lib`.
  add_custom_command(OUTPUT ${IROH_LIB}
    COMMAND cargo build --release
    WORKING_DIRECTORY ${IROH_DIR}
    COMMENT "Building iroh-c-ffi (Rust staticlib)"
    VERBATIM)
  add_custom_target(share_lib DEPENDS ${IROH_LIB})

  # Platform-specific shell sources + native libraries.
  set(MB_APP_PLATFORM "")
  if(APPLE)
    enable_language(OBJC)
    set(MB_APP_PLATFORM src/app/savepanel.m)
  elseif(WIN32)
    # platform_win.c (file dialogs, %APPDATA%, GetModuleFileName) lands in the platform step.
    # set(MB_APP_PLATFORM src/app/platform_win.c)
  endif()

  add_executable(MoneyBooks WIN32 ${MB_ENGINE_SRC} src/app/main.c ${MB_APP_PLATFORM})
  add_dependencies(MoneyBooks share_lib)
  mb_target_common(MoneyBooks)
  target_compile_definitions(MoneyBooks PRIVATE MB_WITH_SHARE WEBVIEW_STATIC)
  target_include_directories(MoneyBooks PRIVATE ${IROH_DIR} ${WV}/core/include)
  target_link_libraries(MoneyBooks PRIVATE mb_webview ${IROH_LIB})

  if(APPLE)
    # iroh's macOS native-static-libs + the WKWebView frameworks.
    target_link_libraries(MoneyBooks PRIVATE
      "-framework WebKit" "-framework Cocoa"
      "-framework Security" "-framework SystemConfiguration" "-framework CoreWLAN"
      "-framework SecurityFoundation" "-framework Foundation" "-framework CoreFoundation"
      objc iconv)
  elseif(WIN32)
    # iroh's Windows native-static-libs + WebView2/COM. Finalized in the platform step.
    target_link_libraries(MoneyBooks PRIVATE
      ws2_32 userenv bcrypt ntdll secur32 crypt32 ncrypt advapi32 ole32 oleaut32 shell32 version)
  elseif(UNIX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK REQUIRED gtk+-3.0)
    pkg_check_modules(WEBKIT2 REQUIRED webkit2gtk-4.1)
    target_include_directories(MoneyBooks PRIVATE ${GTK_INCLUDE_DIRS} ${WEBKIT2_INCLUDE_DIRS})
    target_link_libraries(MoneyBooks PRIVATE ${GTK_LIBRARIES} ${WEBKIT2_LIBRARIES})
  endif()
endif()
