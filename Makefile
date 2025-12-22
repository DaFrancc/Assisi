BUILD_DIR := out/build
INSTALL_DIR := out/install
GENERATOR := Ninja

.PHONY: deps debug release clean

deps:
	cmake -P cmake/deps.cmake

debug: deps
	cmake -S . -B $(BUILD_DIR)/debug \
		-G $(GENERATOR) \
		-DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_INSTALL_PREFIX=$(INSTALL_DIR)/debug
	cmake --build $(BUILD_DIR)/debug

release: deps
	cmake -S . -B $(BUILD_DIR)/release \
		-G $(GENERATOR) \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX=$(INSTALL_DIR)/release
	cmake --build $(BUILD_DIR)/release

clean:
	cmake -E rm -rf out
