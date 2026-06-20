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
INC  := -Isrc -Isrc/vendor/sqlite

# High-signal warnings as errors. Conversion warnings stay visible but non-fatal
# (they're advisory, not bugs). Zero-variadic-args is intentional in our macros.
WARN := -Wall -Wextra -Wpedantic -Werror \
        -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
        -Wmissing-variable-declarations \
        -Wno-gnu-zero-variadic-macro-arguments \
        -Wconversion -Wsign-conversion \
        -Wno-error=conversion -Wno-error=sign-conversion

SAN     := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
DEBUG   := -O0 -g3 $(SAN)
RELEASE := -O2 -g -DNDEBUG -flto

BUILD := build

# SQLite is vendored (src/vendor/sqlite) and compiled via the unity wrapper
# src/sqlite/sqlite_amalgamation.c — no system libsqlite3 dependency anywhere (required
# for Windows; pins the version everywhere; compile-time options baked into the wrapper).
# Built once into its own object: no sanitizers, warnings off, -O2. Linked into every target.
SQLITE_OBJ := $(BUILD)/sqlite3.o
# Extra link libs SQLite needs per platform (none on macOS; Linux adds -lpthread -ldl -lm).
LDLIBS :=

# Engine = all library sources (no test runner, no app entry, no raw vendor, and not the
# SQLite unity wrapper — that is compiled separately into $(SQLITE_OBJ); the rest of vendor
# is compiled via a warning-silenced unity wrapper in src/json/).
ENGINE_SRC := $(shell find src -name '*.c' -not -path 'src/test/*' -not -path 'src/vendor/*' -not -path 'src/app/*' -not -path 'src/mcpd/*' -not -path 'src/sharedemo/*' -not -path 'src/sqlite/*')
# Test build = engine + integration tests + the runner, all with -DMB_TEST.
TEST_SRC   := $(ENGINE_SRC) $(wildcard tests/*.c) $(wildcard src/test/*.c)

.DEFAULT_GOAL := test

# ---- test (debug + sanitizers; the default) ----
.PHONY: test
test: $(BUILD)/test_runner
	$(BUILD)/test_runner

TESTDEFS := -DMB_TEST

# SQLite, vendored: compiled once, no sanitizers (it's third-party and ASan would only
# slow it down) and warnings off. Linked into every binary below in place of -lsqlite3.
$(SQLITE_OBJ): src/sqlite/sqlite_amalgamation.c src/vendor/sqlite/sqlite3.c | $(BUILD)
	$(CC) $(STD) -Isrc/vendor/sqlite -O2 -w -c src/sqlite/sqlite_amalgamation.c -o $@

$(BUILD)/test_runner: $(TEST_SRC) $(SQLITE_OBJ) | $(BUILD)
	$(CC) $(STD) $(INC) $(WARN) $(DEBUG) $(TESTDEFS) $(TEST_SRC) $(SQLITE_OBJ) $(LDLIBS) -o $@

# ---- release (optimized static lib) ----
.PHONY: release
release: $(SQLITE_OBJ) | $(BUILD)
	$(CC) $(STD) $(INC) $(WARN) $(RELEASE) -c $(ENGINE_SRC)
	ar rcs $(BUILD)/libmoneybooks.a *.o $(SQLITE_OBJ)
	@rm -f *.o
	@echo "built $(BUILD)/libmoneybooks.a"

# ---- leaks (Apple) ----
# Built WITHOUT sanitizers: ASan replaces malloc, which `leaks` can't introspect.
.PHONY: leaks
leaks: $(SQLITE_OBJ) | $(BUILD)
	$(CC) $(STD) $(INC) $(WARN) -O1 -g $(TESTDEFS) $(TEST_SRC) $(SQLITE_OBJ) $(LDLIBS) -o $(BUILD)/leak_runner
	MallocStackLogging=1 leaks --atExit -- $(BUILD)/leak_runner

# ---- static analysis ----
.PHONY: analyze
analyze:
	@for f in $(ENGINE_SRC); do \
	  echo "analyze $$f"; \
	  $(CC) $(STD) $(INC) -DMB_TEST --analyze -Xclang -analyzer-output=text $$f || exit 1; \
	done
	@echo "static analysis clean"

# ---- dead-code report (coverage: 0-hit functions are candidates) ----
.PHONY: deadcode
deadcode: $(SQLITE_OBJ) | $(BUILD)
	$(CC) $(STD) $(INC) -DMB_TEST -fprofile-instr-generate -fcoverage-mapping \
	  $(TEST_SRC) $(SQLITE_OBJ) $(LDLIBS) -o $(BUILD)/cov_runner
	LLVM_PROFILE_FILE=$(BUILD)/cov.profraw $(BUILD)/cov_runner >/dev/null || true
	xcrun llvm-profdata merge -sparse $(BUILD)/cov.profraw -o $(BUILD)/cov.profdata
	@echo "=== per-function execution (look for 0 / 0.00% = dead-code candidates) ==="
	xcrun llvm-cov report $(BUILD)/cov_runner -instr-profile=$(BUILD)/cov.profdata \
	  -show-functions $(ENGINE_SRC)
	@echo "(review each 0-hit function: genuinely dead -> remove; missing test -> add one)"

# ---- stdio MCP server (pure C, for Claude Desktop & other MCP clients) ----
.PHONY: mcp
mcp: $(SQLITE_OBJ) | $(BUILD)
	$(CC) $(STD) $(INC) -O2 $(ENGINE_SRC) src/mcpd/main.c $(SQLITE_OBJ) $(LDLIBS) -o $(BUILD)/money-books-mcp
	@echo "built $(BUILD)/money-books-mcp  —  Claude Desktop command: $(CURDIR)/$(BUILD)/money-books-mcp <book.sqlite>"

# ---- native app (macOS GUI: WKWebView via webview/webview) ----
# Run scripts/fetch_webview.sh once to vendor webview. Builds the React UI, compiles the
# webview C++ impl, then links our C engine + the shell. Displays on a logged-in session.
WV := src/vendor/webview
# main.c is C; savepanel.m is Objective-C (NSSavePanel). clang compiles each by extension.
APP_C := $(ENGINE_SRC) src/app/main.c src/app/savepanel.m

.PHONY: ui
ui:
	cd ui && npm install --silent && npm run build

# The shipped app includes live read-only sharing, so it compiles with -DMB_WITH_SHARE and
# links the iroh staticlib (built by share-lib). This is the app's lone Rust dependency.
.PHONY: app
app: ui share-lib $(SQLITE_OBJ) | $(BUILD)
	@test -f $(WV)/core/src/webview.cc || { echo ">> run scripts/fetch_webview.sh first"; exit 1; }
	clang++ -std=c++17 -DWEBVIEW_STATIC -I$(WV)/core/include -O2 -c $(WV)/core/src/webview.cc -o $(BUILD)/webview.o
	clang -std=c11 -DWEBVIEW_STATIC -DMB_WITH_SHARE $(INC) -I$(WV)/core/include -I$(IROH_DIR) -O2 $(APP_C) $(BUILD)/webview.o \
	  $(SQLITE_OBJ) $(IROH_LIB) $(LDLIBS) -lc++ -framework WebKit -framework Cocoa $(IROH_NATIVE) -o $(BUILD)/MoneyBooks
	@echo "built $(BUILD)/MoneyBooks  —  run: ./$(BUILD)/MoneyBooks book.sqlite"

# ---- iroh share transport (Phase 7b-2 — the lone Rust dependency) ----
# Vendor with scripts/fetch_iroh.sh; build the staticlib with `make share-lib`. Only the
# share build defines MB_WITH_SHARE, so transport_iroh.c is inert in test/mcp/app builds and
# those stay Rust-free. Native libs come from `cargo rustc -- --print native-static-libs`.
IROH_DIR    := src/vendor/iroh-c-ffi
IROH_LIB    := $(IROH_DIR)/target/release/libiroh_c_ffi.a
IROH_NATIVE := -framework Security -framework SystemConfiguration -framework CoreWLAN \
               -framework SecurityFoundation -framework Foundation -framework CoreFoundation \
               -lobjc -liconv

.PHONY: share-lib
share-lib:
	@test -d $(IROH_DIR) || { echo ">> run scripts/fetch_iroh.sh first"; exit 1; }
	cd $(IROH_DIR) && cargo build --release
	@echo "built $(IROH_LIB)"

# Two-process demo proving real host<->guest QUIC (no UI). See src/sharedemo/main.c.
.PHONY: share-demo
share-demo: share-lib $(SQLITE_OBJ) | $(BUILD)
	$(CC) $(STD) $(INC) -I$(IROH_DIR) -O2 -DMB_WITH_SHARE $(ENGINE_SRC) src/sharedemo/main.c \
	  $(SQLITE_OBJ) $(IROH_LIB) $(LDLIBS) $(IROH_NATIVE) -o $(BUILD)/share-demo
	@echo "built $(BUILD)/share-demo"
	@echo "  host:  ./$(BUILD)/share-demo host book.sqlite"
	@echo "  guest: ./$(BUILD)/share-demo guest '<address from host>'"

$(BUILD):
	@mkdir -p $(BUILD)

.PHONY: clean
clean:
	rm -rf $(BUILD) *.o *.a *.gcda *.gcno *.profraw *.profdata *.plist *.dSYM
