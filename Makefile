.PHONY: configure-msvc configure-gcc configure-clang msvc gcc clang \
        msvc-debug msvc-dev msvc-ship gcc-debug gcc-dev gcc-ship clang-debug clang-dev clang-ship \
        msvc-asan gcc-asan gcc-tsan clang-asan clang-tsan \
        test-gcc-asan test-gcc-tsan test-clang-asan test-clang-tsan test-msvc-asan \
        md mv ms gd gv gs cd cv cs \
        clean-msvc-debug clean-msvc-dev clean-msvc-ship clean-msvc-asan clean-msvc \
        clean-gcc-debug clean-gcc-dev clean-gcc-ship clean-gcc-asan clean-gcc-tsan clean-gcc \
        clean-clang-debug clean-clang-dev clean-clang-ship clean-clang-asan clean-clang-tsan clean-clang clean

# Configure presets (FetchContent downloads deps on first configure)
configure-msvc:
	cmake --preset msvc-debug
	cmake --preset msvc-dev
	cmake --preset msvc-ship

configure-gcc:
	cmake --preset gcc-debug
	cmake --preset gcc-dev
	cmake --preset gcc-ship

configure-clang:
	cmake --preset clang-debug
	cmake --preset clang-dev
	cmake --preset clang-ship

# Full setup + build (cmake configure -> cmake build)
msvc: configure-msvc
	cmake --build --preset msvc-debug
	cmake --build --preset msvc-dev
	cmake --build --preset msvc-ship

gcc: configure-gcc
	cmake --build --preset gcc-debug
	cmake --build --preset gcc-dev
	cmake --build --preset gcc-ship

clang: configure-clang
	cmake --build --preset clang-debug
	cmake --build --preset clang-dev
	cmake --build --preset clang-ship

# Individual build targets
msvc-debug:
	cmake --build --preset msvc-debug

msvc-dev:
	cmake --build --preset msvc-dev

msvc-ship:
	cmake --build --preset msvc-ship

gcc-debug:
	cmake --build --preset gcc-debug

gcc-dev:
	cmake --build --preset gcc-dev

gcc-ship:
	cmake --build --preset gcc-ship

clang-debug:
	cmake --build --preset clang-debug

clang-dev:
	cmake --build --preset clang-dev

clang-ship:
	cmake --build --preset clang-ship

# Sanitizer builds (occasional-use: configure + build in one step; the
# configure is a cached no-op after the first run). test-* builds then runs
# the whole suite under the sanitizer.
msvc-asan:
	cmake --preset msvc-asan
	cmake --build --preset msvc-asan

gcc-asan:
	cmake --preset gcc-asan
	cmake --build --preset gcc-asan

gcc-tsan:
	cmake --preset gcc-tsan
	cmake --build --preset gcc-tsan

clang-asan:
	cmake --preset clang-asan
	cmake --build --preset clang-asan

clang-tsan:
	cmake --preset clang-tsan
	cmake --build --preset clang-tsan

test-msvc-asan: msvc-asan
	ctest --preset msvc-asan

test-gcc-asan: gcc-asan
	ctest --preset gcc-asan

test-gcc-tsan: gcc-tsan
	ctest --preset gcc-tsan

test-clang-asan: clang-asan
	ctest --preset clang-asan

test-clang-tsan: clang-tsan
	ctest --preset clang-tsan

# Aliases (d=debug, v=dev, s=ship)
md: msvc-debug
mv: msvc-dev
ms: msvc-ship
gd: gcc-debug
gv: gcc-dev
gs: gcc-ship
cd: clang-debug
cv: clang-dev
cs: clang-ship

# Clean build outputs — removes the entire build directory.
# After cleaning, re-run the relevant configure-* target before building.
clean-msvc-debug:
	cmake -E rm -rf "$(CURDIR)/out/build/msvc-debug"

clean-msvc-dev:
	cmake -E rm -rf "$(CURDIR)/out/build/msvc-dev"

clean-msvc-ship:
	cmake -E rm -rf "$(CURDIR)/out/build/msvc-ship"

clean-msvc-asan:
	cmake -E rm -rf "$(CURDIR)/out/build/msvc-asan"

clean-msvc: clean-msvc-debug clean-msvc-dev clean-msvc-ship clean-msvc-asan

clean-gcc-debug:
	cmake -E rm -rf "$(CURDIR)/out/build/gcc-debug"

clean-gcc-dev:
	cmake -E rm -rf "$(CURDIR)/out/build/gcc-dev"

clean-gcc-ship:
	cmake -E rm -rf "$(CURDIR)/out/build/gcc-ship"

clean-gcc-asan:
	cmake -E rm -rf "$(CURDIR)/out/build/gcc-asan"

clean-gcc-tsan:
	cmake -E rm -rf "$(CURDIR)/out/build/gcc-tsan"

clean-gcc: clean-gcc-debug clean-gcc-dev clean-gcc-ship clean-gcc-asan clean-gcc-tsan

clean-clang-debug:
	cmake -E rm -rf "$(CURDIR)/out/build/clang-debug"

clean-clang-dev:
	cmake -E rm -rf "$(CURDIR)/out/build/clang-dev"

clean-clang-ship:
	cmake -E rm -rf "$(CURDIR)/out/build/clang-ship"

clean-clang-asan:
	cmake -E rm -rf "$(CURDIR)/out/build/clang-asan"

clean-clang-tsan:
	cmake -E rm -rf "$(CURDIR)/out/build/clang-tsan"

clean-clang: clean-clang-debug clean-clang-dev clean-clang-ship clean-clang-asan clean-clang-tsan

clean: clean-msvc clean-gcc clean-clang