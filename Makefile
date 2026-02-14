init:
	conan install . -of=out/build/msvc-debug -pr:h=.\profiles\msvc-debug -pr:b=.\profiles\msvc-debug -g CMakeDeps -g CMakeToolchain --build=missing
	conan install . -of=out/build/msvc-release -pr:h=.\profiles\msvc-release -pr:b=.\profiles\msvc-release -g CMakeDeps -g CMakeToolchain --build=missing
	conan install . -of=out/build/msvc-sanitize -pr:h=.\profiles\msvc-sanitize -pr:b=.\profiles\msvc-sanitize -g CMakeDeps -g CMakeToolchain --build=missing
