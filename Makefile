.PHONY: configure-msvc configure-gcc configure-clang msvc gcc clang \
        msvc-debug msvc-dev msvc-ship gcc-debug gcc-dev gcc-ship clang-debug clang-dev clang-ship \
        md mv ms gd gv gs cd cv cs \
        clean-msvc-debug clean-msvc-dev clean-msvc-ship clean-msvc \
        clean-gcc-debug clean-gcc-dev clean-gcc-ship clean-gcc \
        clean-clang-debug clean-clang-dev clean-clang-ship clean-clang clean

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

# Clean build outputs
# Use --build-dir instead of --preset to avoid CMake 4.x scanning build
# subdirectories for CMakePresets.json and failing on stale files.
clean-msvc-debug:
	cmake --build "$(CURDIR)/out/build/msvc-debug" --target clean

clean-msvc-dev:
	cmake --build "$(CURDIR)/out/build/msvc-dev" --target clean

clean-msvc-ship:
	cmake --build "$(CURDIR)/out/build/msvc-ship" --target clean

clean-msvc: clean-msvc-debug clean-msvc-dev clean-msvc-ship

clean-gcc-debug:
	cmake --build "$(CURDIR)/out/build/gcc-debug" --target clean

clean-gcc-dev:
	cmake --build "$(CURDIR)/out/build/gcc-dev" --target clean

clean-gcc-ship:
	cmake --build "$(CURDIR)/out/build/gcc-ship" --target clean

clean-gcc: clean-gcc-debug clean-gcc-dev clean-gcc-ship

clean-clang-debug:
	cmake --build "$(CURDIR)/out/build/clang-debug" --target clean

clean-clang-dev:
	cmake --build "$(CURDIR)/out/build/clang-dev" --target clean

clean-clang-ship:
	cmake --build "$(CURDIR)/out/build/clang-ship" --target clean

clean-clang: clean-clang-debug clean-clang-dev clean-clang-ship

clean: clean-msvc clean-gcc clean-clang