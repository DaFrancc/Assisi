.PHONY: configure-msvc configure-gcc configure-clang msvc gcc clang \
        msvc-debug msvc-dev msvc-ship gcc-debug gcc-dev gcc-ship clang-debug clang-dev clang-ship \
        msvc-asan gcc-asan gcc-tsan clang-asan clang-tsan \
        test-gcc-asan test-gcc-tsan test-clang-asan test-clang-tsan test-msvc-asan \
        md mv ms gd gv gs cd cv cs \
        msvc-debug-chiara msvc-dev-chiara msvc-ship-chiara \
        gcc-debug-chiara gcc-dev-chiara gcc-ship-chiara \
        clang-debug-chiara clang-dev-chiara clang-ship-chiara \
        gcc-tsan-chiara test-gcc-tsan-chiara \
        md-c mv-c ms-c gd-c gv-c gs-c cd-c cv-c cs-c \
        clean-msvc-debug clean-msvc-dev clean-msvc-ship clean-msvc-asan clean-msvc-chiara clean-msvc \
        clean-gcc-debug clean-gcc-dev clean-gcc-ship clean-gcc-asan clean-gcc-tsan clean-gcc-chiara clean-gcc \
        clean-clang-debug clean-clang-dev clean-clang-ship clean-clang-asan clean-clang-tsan clean-clang-chiara \
        clean-clang clean \
        format format-check

# Source formatting (.uncrustify.cfg). The reflectgen fixtures are excluded:
# the golden is compared byte-for-byte against generator output and the fixture
# is that generator's input, so formatting either breaks the reflectgen test.
FORMAT_FILES = $(shell git ls-files '*.cpp' '*.hpp' '*.h' '*.cc' '*.hxx' \
                       | grep -v '^tools/reflectgen/tests/golden/' \
                       | grep -v '^tools/reflectgen/tests/fixtures/')

# Twice: a first pass over unformatted source can leave a couple of files one
# pass short of a fixed point, which then fails format-check.
format:
	uncrustify -c .uncrustify.cfg -l CPP --no-backup $(FORMAT_FILES)
	uncrustify -c .uncrustify.cfg -l CPP --no-backup $(FORMAT_FILES)

# Reports what `make format` would change, and fails if anything would.
format-check:
	uncrustify -c .uncrustify.cfg -l CPP --check $(FORMAT_FILES)

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

# Chiara builds: the same configs with the capture system compiled in
# (docs/chiara-design-notes.md). Off by default everywhere, so these are the
# only way to get it. Configure + build in one step like the sanitizer targets;
# the configure is a cached no-op after the first run.
#
# They get their own build directories rather than flipping a cache variable in
# place: ASSISI_CHIARA_ENABLED is a PUBLIC define, so toggling it rebuilds
# everything that includes a Chiara header. Separate dirs mean switching back
# and forth costs nothing, and the FetchContent deps stay shared.
#
# `make gs-c` is the one to reach for: an optimized build with capture. Profiling
# a debug build mostly measures the debug build.
msvc-debug-chiara:
	cmake --preset msvc-debug-chiara
	cmake --build --preset msvc-debug-chiara

msvc-dev-chiara:
	cmake --preset msvc-dev-chiara
	cmake --build --preset msvc-dev-chiara

msvc-ship-chiara:
	cmake --preset msvc-ship-chiara
	cmake --build --preset msvc-ship-chiara

gcc-debug-chiara:
	cmake --preset gcc-debug-chiara
	cmake --build --preset gcc-debug-chiara

gcc-dev-chiara:
	cmake --preset gcc-dev-chiara
	cmake --build --preset gcc-dev-chiara

gcc-ship-chiara:
	cmake --preset gcc-ship-chiara
	cmake --build --preset gcc-ship-chiara

clang-debug-chiara:
	cmake --preset clang-debug-chiara
	cmake --build --preset clang-debug-chiara

clang-dev-chiara:
	cmake --preset clang-dev-chiara
	cmake --build --preset clang-dev-chiara

clang-ship-chiara:
	cmake --preset clang-ship-chiara
	cmake --build --preset clang-ship-chiara

# Chiara's own race tests need tsan *and* the capture compiled in; gcc-tsan
# inherits gcc-debug, where Chiara is off, so it cannot cover them.
gcc-tsan-chiara:
	cmake --preset gcc-tsan-chiara
	cmake --build --preset gcc-tsan-chiara

test-gcc-tsan-chiara: gcc-tsan-chiara
	ctest --preset gcc-tsan-chiara

# Aliases (d=debug, v=dev, s=ship; -c=with Chiara)
md: msvc-debug
mv: msvc-dev
ms: msvc-ship
gd: gcc-debug
gv: gcc-dev
gs: gcc-ship
cd: clang-debug
cv: clang-dev
cs: clang-ship

md-c: msvc-debug-chiara
mv-c: msvc-dev-chiara
ms-c: msvc-ship-chiara
gd-c: gcc-debug-chiara
gv-c: gcc-dev-chiara
gs-c: gcc-ship-chiara
cd-c: clang-debug-chiara
cv-c: clang-dev-chiara
cs-c: clang-ship-chiara

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

clean-msvc-chiara:
	cmake -E rm -rf "$(CURDIR)/out/build/msvc-debug-chiara"
	cmake -E rm -rf "$(CURDIR)/out/build/msvc-dev-chiara"
	cmake -E rm -rf "$(CURDIR)/out/build/msvc-ship-chiara"

clean-msvc: clean-msvc-debug clean-msvc-dev clean-msvc-ship clean-msvc-asan clean-msvc-chiara

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

clean-gcc-chiara:
	cmake -E rm -rf "$(CURDIR)/out/build/gcc-debug-chiara"
	cmake -E rm -rf "$(CURDIR)/out/build/gcc-dev-chiara"
	cmake -E rm -rf "$(CURDIR)/out/build/gcc-ship-chiara"
	cmake -E rm -rf "$(CURDIR)/out/build/gcc-tsan-chiara"

clean-gcc: clean-gcc-debug clean-gcc-dev clean-gcc-ship clean-gcc-asan clean-gcc-tsan clean-gcc-chiara

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

clean-clang-chiara:
	cmake -E rm -rf "$(CURDIR)/out/build/clang-debug-chiara"
	cmake -E rm -rf "$(CURDIR)/out/build/clang-dev-chiara"
	cmake -E rm -rf "$(CURDIR)/out/build/clang-ship-chiara"

clean-clang: clean-clang-debug clean-clang-dev clean-clang-ship clean-clang-asan clean-clang-tsan clean-clang-chiara

clean: clean-msvc clean-gcc clean-clang