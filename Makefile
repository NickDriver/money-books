# Money Books — Phase 0 build.
# clang, C11, sanitizers in debug/test, dead-code tooling. macOS / Apple Silicon.
#
# Targets:
#   make            # = make test  (build + run unit+integration tests, sanitized)
#   make test       # build & run the auto-registering test runner (ASan+UBSan)
#   make release    # optimized static lib, no sanitizers, NDEBUG (MB_INVARIANT stays on)
#   make leaks      # run tests under Apple's `leaks` (Valgrind/LSan weak on arm64)
#   make analyze    # Clang Static Analyzer over engine sources
#   make deadcode   # coverage-based report: functions executed 0 times = candidates
#   make clean

CC   := clang
STD  := -std=c11
INC  := -Isrc

# High-signal warnings as errors. Conversion warnings stay visible but non-fatal
# (they're advisory, not bugs). Zero-variadic-args is intentional in our macros.
WARN := -Wall -Wextra -Wpedantic -Werror \
        -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
        -Wmissing-variable-declarations \
        -Wno-gnu-zero-variadic-macro-arguments \
        -Wconversion -Wsign-conversion \
        -Wno-error=conversion -Wno-error=sign-conversion

# SQLite compile flags (used once the store module lands; harmless now).
DEFS := -DSQLITE_ENABLE_FTS5 -DSQLITE_DEFAULT_FOREIGN_KEYS=1 -DSQLITE_THREADSAFE=1

SAN     := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
DEBUG   := -O0 -g3 $(SAN)
RELEASE := -O2 -g -DNDEBUG -flto

BUILD := build

# Link the system SQLite for now (macOS ships 3.46 + FTS5). Before release we vendor
# the amalgamation (add src/vendor/sqlite3.c, drop -lsqlite3) for portability/FTS5 control.
LDLIBS := -lsqlite3 -lcurl

# Engine = all library sources (no test runner, no app entry, no raw vendor — vendor is
# compiled via a warning-silenced unity wrapper in src/json/).
ENGINE_SRC := $(shell find src -name '*.c' -not -path 'src/test/*' -not -path 'src/vendor/*' -not -path 'src/app/*' -not -path 'src/mcpd/*')
# Test build = engine + integration tests + the runner, all with -DMB_TEST.
TEST_SRC   := $(ENGINE_SRC) $(wildcard tests/*.c) $(wildcard src/test/*.c)

.DEFAULT_GOAL := test

# ---- test (debug + sanitizers; the default) ----
.PHONY: test
test: $(BUILD)/test_runner
	$(BUILD)/test_runner

# Test/cov builds use the in-memory secret backend (no Keychain prompts).
TESTDEFS := -DMB_TEST -DMB_SECRET_MEMORY

$(BUILD)/test_runner: $(TEST_SRC) | $(BUILD)
	$(CC) $(STD) $(INC) $(WARN) $(DEBUG) $(TESTDEFS) $(TEST_SRC) $(LDLIBS) -o $@

# ---- release (optimized static lib) ----
.PHONY: release
release: | $(BUILD)
	$(CC) $(STD) $(INC) $(WARN) $(RELEASE) $(DEFS) -c $(ENGINE_SRC)
	ar rcs $(BUILD)/libmoneybooks.a *.o
	@rm -f *.o
	@echo "built $(BUILD)/libmoneybooks.a"

# ---- leaks (Apple) ----
# Built WITHOUT sanitizers: ASan replaces malloc, which `leaks` can't introspect.
.PHONY: leaks
leaks: | $(BUILD)
	$(CC) $(STD) $(INC) $(WARN) -O1 -g $(TESTDEFS) $(TEST_SRC) $(LDLIBS) -o $(BUILD)/leak_runner
	MallocStackLogging=1 leaks --atExit -- $(BUILD)/leak_runner

# ---- static analysis ----
.PHONY: analyze
analyze:
	@for f in $(ENGINE_SRC); do \
	  echo "analyze $$f"; \
	  $(CC) $(STD) $(INC) -DMB_TEST --analyze -DMB_SECRET_MEMORY -Xclang -analyzer-output=text $$f || exit 1; \
	done
	@echo "static analysis clean"

# ---- dead-code report (coverage: 0-hit functions are candidates) ----
.PHONY: deadcode
deadcode: | $(BUILD)
	$(CC) $(STD) $(INC) -DMB_TEST -DMB_SECRET_MEMORY -fprofile-instr-generate -fcoverage-mapping \
	  $(TEST_SRC) $(LDLIBS) -o $(BUILD)/cov_runner
	LLVM_PROFILE_FILE=$(BUILD)/cov.profraw $(BUILD)/cov_runner >/dev/null || true
	xcrun llvm-profdata merge -sparse $(BUILD)/cov.profraw -o $(BUILD)/cov.profdata
	@echo "=== per-function execution (look for 0 / 0.00% = dead-code candidates) ==="
	xcrun llvm-cov report $(BUILD)/cov_runner -instr-profile=$(BUILD)/cov.profdata \
	  -show-functions $(ENGINE_SRC)
	@echo "(review each 0-hit function: genuinely dead -> remove; missing test -> add one)"

# ---- stdio MCP server (pure C, for Claude Desktop & other MCP clients) ----
.PHONY: mcp
mcp: | $(BUILD)
	$(CC) $(STD) $(INC) -O2 $(ENGINE_SRC) src/mcpd/main.c $(LDLIBS) -framework Security -framework CoreFoundation -o $(BUILD)/money-books-mcp
	@echo "built $(BUILD)/money-books-mcp  —  Claude Desktop command: $(CURDIR)/$(BUILD)/money-books-mcp <book.sqlite>"

# ---- native app (macOS GUI: WKWebView via webview/webview) ----
# Run scripts/fetch_webview.sh once to vendor webview. Builds the React UI, compiles the
# webview C++ impl, then links our C engine + the shell. Displays on a logged-in session.
WV := src/vendor/webview
APP_C := $(ENGINE_SRC) src/app/main.c

.PHONY: ui
ui:
	cd ui && npm install --silent && npm run build

.PHONY: app
app: ui | $(BUILD)
	@test -f $(WV)/core/src/webview.cc || { echo ">> run scripts/fetch_webview.sh first"; exit 1; }
	clang++ -std=c++17 -DWEBVIEW_STATIC -I$(WV)/core/include -O2 -c $(WV)/core/src/webview.cc -o $(BUILD)/webview.o
	clang -std=c11 -DWEBVIEW_STATIC $(INC) -I$(WV)/core/include -O2 $(APP_C) $(BUILD)/webview.o \
	  $(LDLIBS) -lc++ -framework WebKit -framework Cocoa -framework Security -framework CoreFoundation -o $(BUILD)/MoneyBooks
	@echo "built $(BUILD)/MoneyBooks  —  run: ./$(BUILD)/MoneyBooks book.sqlite"

$(BUILD):
	@mkdir -p $(BUILD)

.PHONY: clean
clean:
	rm -rf $(BUILD) *.o *.a *.gcda *.gcno *.profraw *.profdata *.plist *.dSYM
