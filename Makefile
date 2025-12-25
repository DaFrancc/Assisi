# Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc")

BUILD_DIR    := out/build
INSTALL_DIR  := out/install
GENERATOR    ?= Ninja
CMAKE        ?= cmake

DEPS_STAMP   := out/.deps.ok

# -----------------------------
# Platform detection
# -----------------------------
ifeq ($(OS),Windows_NT)
  IS_WINDOWS := 1
else
  IS_WINDOWS := 0
endif

# -----------------------------
# Executable suffix
# -----------------------------
ifeq ($(IS_WINDOWS),1)
  EXEEXT := .exe
else
  EXEEXT :=
endif

# ---- Executable paths ----
EXE_DEBUG   := $(BUILD_DIR)/debug/apps/Sandbox/Assisi-Sandbox$(EXEEXT)
EXE_RELEASE := $(BUILD_DIR)/release/apps/Sandbox/Assisi-Sandbox$(EXEEXT)
EXE_ASAN    := $(BUILD_DIR)/asan/apps/Sandbox/Assisi-Sandbox$(EXEEXT)

.PHONY: deps debug release asan \
        run-debug run-release run-asan \
        reconfigure-debug reconfigure-release reconfigure-asan \
        clean msvc-check

# ============================================================
# Windows: MSVC bootstrap via generated .cmd (quote-proof)
# ============================================================

ifeq ($(IS_WINDOWS),1)

# Override if needed
VSWHERE ?= C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe
VCVARS_ARG ?= x64

# Path used for execution (no spaces -> no quotes needed)
MSVC_ENV_CMD_EXEC := out\.msvc_env.cmd
# Path used when writing the file (Make-friendly)
MSVC_ENV_CMD_FILE := out/.msvc_env.cmd

$(MSVC_ENV_CMD_FILE):
	@$(CMAKE) -E make_directory out
	@>  "$(MSVC_ENV_CMD_FILE)" echo @echo off
	@>> "$(MSVC_ENV_CMD_FILE)" echo setlocal enableextensions
	@>> "$(MSVC_ENV_CMD_FILE)" echo set "VSWHERE=$(VSWHERE)"
	@>> "$(MSVC_ENV_CMD_FILE)" echo if not exist "%%VSWHERE%%" exit /b 2
	@>> "$(MSVC_ENV_CMD_FILE)" echo for /f "usebackq tokens=* delims=" %%%%I in ^(`"%%VSWHERE%%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`^) do set "VSINSTALL=%%%%I"
	@>> "$(MSVC_ENV_CMD_FILE)" echo if "%%VSINSTALL%%"=="" exit /b 3
	@>> "$(MSVC_ENV_CMD_FILE)" echo set "VCVARS=%%VSINSTALL%%\VC\Auxiliary\Build\vcvarsall.bat"
	@>> "$(MSVC_ENV_CMD_FILE)" echo if not exist "%%VCVARS%%" exit /b 4
	@>> "$(MSVC_ENV_CMD_FILE)" echo call "%%VCVARS%%" $(VCVARS_ARG) ^>nul
	@>> "$(MSVC_ENV_CMD_FILE)" echo if errorlevel 1 exit /b 5
	@>> "$(MSVC_ENV_CMD_FILE)" echo rem Run the rest of the command line inside MSVC env:
	@>> "$(MSVC_ENV_CMD_FILE)" echo call %%*

msvc-check: $(MSVC_ENV_CMD_FILE)
	@cmd.exe /S /C "call $(MSVC_ENV_CMD_EXEC) echo MSVC env: OK" || (echo MSVC env check failed & exit /b 1)

# Run command inside MSVC env. Pass the command as-is; cmd will parse it.
define WITH_MSVC
cmd.exe /S /C "call $(MSVC_ENV_CMD_EXEC) $(1)"
endef

else

msvc-check:
	@true

endif

# ============================================================
# Deps (one-time gate)
# ============================================================

deps: $(DEPS_STAMP)

$(DEPS_STAMP):
	@echo Running dependency setup...
	@$(CMAKE) -E make_directory out
	@$(CMAKE) -P cmake/deps.cmake
	@echo Deps complete.
	@$(CMAKE) -E touch $(DEPS_STAMP)

# ============================================================
# Reconfigure helpers
# ============================================================

reconfigure-debug:
	@$(CMAKE) -E rm -rf $(BUILD_DIR)/debug

reconfigure-release:
	@$(CMAKE) -E rm -rf $(BUILD_DIR)/release

reconfigure-asan:
	@$(CMAKE) -E rm -rf $(BUILD_DIR)/asan

# ============================================================
# Debug
# - Windows: always MSVC environment
# - Linux: do not force compiler
# ============================================================

debug: msvc-check $(DEPS_STAMP)
ifeq ($(IS_WINDOWS),1)
	$(call WITH_MSVC,$(CMAKE) -S . -B $(BUILD_DIR)/debug -G "$(GENERATOR)" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=$(INSTALL_DIR)/debug -DASSISI_DEBUG_WARNINGS=ON)
	$(call WITH_MSVC,$(CMAKE) --build $(BUILD_DIR)/debug --parallel)
else
	$(CMAKE) -S . -B $(BUILD_DIR)/debug -G "$(GENERATOR)" \
		-DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_INSTALL_PREFIX=$(INSTALL_DIR)/debug \
		-DASSISI_DEBUG_WARNINGS=ON
	$(CMAKE) --build $(BUILD_DIR)/debug --parallel
endif

run-debug: debug
	@$(CMAKE) -E echo Running: $(EXE_DEBUG)
	@$(EXE_DEBUG)

# ============================================================
# Release
# ============================================================

release: msvc-check $(DEPS_STAMP)
ifeq ($(IS_WINDOWS),1)
	$(call WITH_MSVC,$(CMAKE) -S . -B $(BUILD_DIR)/release -G "$(GENERATOR)" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$(INSTALL_DIR)/release -DASSISI_RELEASE_OPTIMIZED=ON)
	$(call WITH_MSVC,$(CMAKE) --build $(BUILD_DIR)/release --parallel)
else
	$(CMAKE) -S . -B $(BUILD_DIR)/release -G "$(GENERATOR)" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX=$(INSTALL_DIR)/release \
		-DASSISI_RELEASE_OPTIMIZED=ON
	$(CMAKE) --build $(BUILD_DIR)/release --parallel
endif

run-release: release
	@$(CMAKE) -E echo Running: $(EXE_RELEASE)
	@$(EXE_RELEASE)

# ============================================================
# ASan
# - Windows: clang-cl inside MSVC env
# - Linux: project options control flags
# ============================================================

asan: msvc-check $(DEPS_STAMP)
ifeq ($(IS_WINDOWS),1)
	$(call WITH_MSVC,$(CMAKE) -S . -B $(BUILD_DIR)/asan -G "$(GENERATOR)" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL -DCMAKE_INSTALL_PREFIX=$(INSTALL_DIR)/asan -DASSISI_ENABLE_ASAN=ON -DASSISI_ASAN_HARDCORE=ON)
	$(call WITH_MSVC,$(CMAKE) --build $(BUILD_DIR)/asan --parallel)
else
	$(CMAKE) -S . -B $(BUILD_DIR)/asan -G "$(GENERATOR)" \
		-DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DCMAKE_INSTALL_PREFIX=$(INSTALL_DIR)/asan \
		-DASSISI_ENABLE_ASAN=ON \
		-DASSISI_ASAN_HARDCORE=ON
	$(CMAKE) --build $(BUILD_DIR)/asan --parallel
endif

run-asan: asan
	@$(CMAKE) -E echo Running with ASan: $(EXE_ASAN)
	@$(CMAKE) -E env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1:detect_stack_use_after_return=1:check_initialization_order=1 $(EXE_ASAN)

# ============================================================
# Clean
# ============================================================

clean:
	$(CMAKE) -E rm -rf out
