.PHONY: build test test-asan test-tsan test-all clean format check-format lint setup

BUILD_DIR       := build
BUILD_ASAN_DIR  := build-asan
BUILD_TSAN_DIR  := build-tsan
NPROC           := $(shell nproc)

# --- Default build (Release) ---
build:
	cmake -B $(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON -DBUILD_EXAMPLES=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	ninja -C $(BUILD_DIR) -j$(NPROC)

# --- Test (default build) ---
test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

# --- ASan + UBSan build & test ---
test-asan:
	cmake -B $(BUILD_ASAN_DIR) -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DENABLE_ASAN=ON -DENABLE_UBSAN=ON
	ninja -C $(BUILD_ASAN_DIR) -j$(NPROC)
	ctest --test-dir $(BUILD_ASAN_DIR) --output-on-failure

# --- TSan build & test ---
test-tsan:
	cmake -B $(BUILD_TSAN_DIR) -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DENABLE_TSAN=ON
	ninja -C $(BUILD_TSAN_DIR) -j$(NPROC)
	ctest --test-dir $(BUILD_TSAN_DIR) --output-on-failure

# --- Run all test suites sequentially ---
test-all: test test-asan test-tsan

# --- Clean all build directories ---
clean:
	rm -rf $(BUILD_DIR) $(BUILD_ASAN_DIR) $(BUILD_TSAN_DIR)

# --- Format all source files in-place ---
format:
	find include src tests examples -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i

# --- Check formatting (for CI, exits non-zero on diff) ---
check-format:
	find include src tests examples -name '*.cpp' -o -name '*.hpp' | xargs clang-format --dry-run --Werror

# --- Run clang-tidy on all source files ---
lint: build
	run-clang-tidy -p $(BUILD_DIR) -header-filter='include/hotcoco/.*'

# --- Set up local git hooks ---
setup:
	git config core.hooksPath .githooks
