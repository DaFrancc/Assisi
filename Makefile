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
clean-debug:
	cmake --build --preset msvc-debug --target clean

clean-release:
	cmake --build --preset msvc-release --target clean

clean-sanitize:
	cmake --build --preset msvc-sanitize --target clean

clean: clean-debug clean-release clean-sanitize