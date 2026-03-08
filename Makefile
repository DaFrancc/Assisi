.PHONY: conan-gcc conan-clang conan-msvc init clean-debug clean-release clean-sanitize clean

# Linux — GCC presets
conan-gcc:
	conan install . -of=out/build/gcc-debug     -pr:h=./profiles/gcc-debug     -pr:b=./profiles/gcc-debug     -g CMakeDeps -g CMakeToolchain --build=missing
	conan install . -of=out/build/gcc-release   -pr:h=./profiles/gcc-release   -pr:b=./profiles/gcc-release   -g CMakeDeps -g CMakeToolchain --build=missing
	conan install . -of=out/build/gcc-sanitize  -pr:h=./profiles/gcc-sanitize  -pr:b=./profiles/gcc-sanitize  -g CMakeDeps -g CMakeToolchain --build=missing

# Linux — Clang presets
conan-clang:
	conan install . -of=out/build/clang-debug    -pr:h=./profiles/clang-debug    -pr:b=./profiles/clang-debug    -g CMakeDeps -g CMakeToolchain --build=missing
	conan install . -of=out/build/clang-release  -pr:h=./profiles/clang-release  -pr:b=./profiles/clang-release  -g CMakeDeps -g CMakeToolchain --build=missing
	conan install . -of=out/build/clang-sanitize -pr:h=./profiles/clang-sanitize -pr:b=./profiles/clang-sanitize -g CMakeDeps -g CMakeToolchain --build=missing

# Windows — MSVC presets (run from PowerShell/cmd with backslash paths)
conan-msvc:
	conan install . -of=out/build/msvc-debug    -pr:h=.\profiles\msvc-debug    -pr:b=.\profiles\msvc-debug    -g CMakeDeps -g CMakeToolchain --build=missing
	conan install . -of=out/build/msvc-release  -pr:h=.\profiles\msvc-release  -pr:b=.\profiles\msvc-release  -g CMakeDeps -g CMakeToolchain --build=missing
	conan install . -of=out/build/msvc-sanitize -pr:h=.\profiles\msvc-sanitize -pr:b=.\profiles\msvc-sanitize -g CMakeDeps -g CMakeToolchain --build=missing

# Default init: installs all Linux presets (GCC + Clang)
init: conan-gcc conan-clang

# Clean build outputs (without rebuilding)
clean-msvc-debug:
	cmake --build --preset msvc-debug --target clean

clean-msvc-release:
	cmake --build --preset msvc-release --target clean

clean-msvc-sanitize:
	cmake --build --preset msvc-sanitize --target clean

clean-msvc: clean-msvc-debug clean-msvc-release clean-msvc-sanitize

clean-gcc-debug:
	cmake --build --preset gcc-debug --target clean

clean-gcc-release:
	cmake --build --preset gcc-release --target clean

clean-gcc-sanitize:
	cmake --build --preset gcc-sanitize --target clean

clean-gcc: clean-gcc-debug clean-gcc-release clean-gcc-sanitize

clean-clang-debug:
	cmake --build --preset clang-debug --target clean

clean-clang-release:
	cmake --build --preset clang-release --target clean

clean-clang-sanitize:
	cmake --build --preset clang-sanitize --target clean

clean-clang: clean-clang-debug clean-clang-release clean-clang-sanitize

clean: clean-msvc clean-gcc clean-clang
