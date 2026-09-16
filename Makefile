# Build targets are generated from the axes below rather than written out one
# per combination, so a new compiler, config or variant is a line in a list.
# `make help` prints every family and its short aliases.

# Set explicitly because the generated alias rules below come before any
# hand-written target, and the first rule in a file is otherwise the default.
.DEFAULT_GOAL := help

COMPILERS := msvc gcc clang
CONFIGS   := debug dev ship

# gcc-debug, gcc-dev, ... and each one's Chiara twin. Each is a CMake preset.
PRESETS        := $(foreach c,$(COMPILERS),$(foreach k,$(CONFIGS),$(c)-$(k)))
CHIARA_PRESETS := $(addsuffix -chiara,$(PRESETS))
BUILDS         := $(PRESETS) $(CHIARA_PRESETS)

# Steps a build can be followed by, and every combination of them in the order
# they have to run: the game's code, then a cook of assets/, then a pak of that
# cook. Each is its own target on every build: gcc-debug-cook, gcc-debug-game-cook-pack.
STEPS    := game cook pack
SUFFIXES := game cook pack game-cook game-pack cook-pack game-cook-pack
STEP_BUILDS := $(foreach s,$(SUFFIXES),$(addsuffix -$(s),$(BUILDS)))

# Per config: the texture compression tier a cook uses, and the codec a pak is
# compressed with. A debug cook takes the fast tier because somebody is waiting
# on it, so a debug pak is never a packaging input.
TIER_debug  := fast
TIER_dev    := best
TIER_ship   := best
CODEC_debug := none
CODEC_dev   := lz4
CODEC_ship  := zstd

# Occasional-use presets that configure and build in one step.
SANITIZED := msvc-asan gcc-asan gcc-tsan clang-asan clang-tsan gcc-tsan-chiara

# One letter per compiler and per config, for the short aliases (gd, gvg-c, ...).
LETTER_msvc  := m
LETTER_gcc   := g
LETTER_clang := c
LETTER_debug := d
LETTER_dev   := v
LETTER_ship  := s

EMPTY :=
SPACE := $(EMPTY) $(EMPTY)

# $(call presets-of,<compiler>,<list>): that compiler's presets from <list>.
presets-of = $(filter $(1)-%,$(2))

# $(call config-of,<build>): debug, dev or ship.
config-of = $(word 2,$(subst -, ,$(1)))

# $(call build-dir,<build>): where that preset builds.
build-dir = $(CURDIR)/out/build/$(1)

# $(call step-letters,<suffix>): game-cook-pack -> gkp.
step-letters = $(subst game,g,$(subst cook,k,$(subst pack,p,$(subst -,,$(1)))))

# $(call run-steps,<build>,<suffix>): each step of <suffix> in order, as a
# separate make, stopping at the first failure. Separate makes so a parallel
# make cannot start the pack before the cook it reads has finished.
run-steps = $(MAKE) $(1)-$(firstword $(subst -, ,$(2)))$(patsubst %, && $(MAKE) $(1)-%,$(wordlist 2,$(words $(subst -, ,$(2))),$(subst -, ,$(2))))

# $(call build-each,<presets>): build them in order, stopping at the first
# failure. One `&&` chain rather than a shell loop, because the msvc targets
# run under cmd as well as sh.
build-each = cmake --build --preset $(firstword $(1))$(patsubst %, && cmake --build --preset %,$(wordlist 2,$(words $(1)),$(1)))

# Short aliases for one compiler/config pair: gd and gd-c.
define ALIASES
ALIAS_NAMES += $(LETTER_$(1))$(LETTER_$(2)) $(LETTER_$(1))$(LETTER_$(2))-c
$(LETTER_$(1))$(LETTER_$(2)): $(1)-$(2)
$(LETTER_$(1))$(LETTER_$(2))-c: $(1)-$(2)-chiara
endef
$(foreach c,$(COMPILERS),$(foreach k,$(CONFIGS),$(eval $(call ALIASES,$(c),$(k)))))

# The same pair with a step suffix: gdg, gdkp, gdgkp-c.
define STEP_ALIASES
ALIAS_NAMES += $(LETTER_$(1))$(LETTER_$(2))$(call step-letters,$(3)) $(LETTER_$(1))$(LETTER_$(2))$(call step-letters,$(3))-c
$(LETTER_$(1))$(LETTER_$(2))$(call step-letters,$(3)): $(1)-$(2)-$(3)
$(LETTER_$(1))$(LETTER_$(2))$(call step-letters,$(3))-c: $(1)-$(2)-chiara-$(3)
endef
$(foreach c,$(COMPILERS),$(foreach k,$(CONFIGS),$(foreach s,$(SUFFIXES),$(eval $(call STEP_ALIASES,$(c),$(k),$(s))))))

# What `clean-<compiler>` removes: its presets, its sanitizer builds, and its
# Chiara builds.
define COMPILER_CLEAN
clean-$(1): $(addprefix clean-,$(call presets-of,$(1),$(PRESETS) $(filter-out %-chiara,$(SANITIZED)))) clean-$(1)-chiara
endef
$(foreach c,$(COMPILERS),$(eval $(call COMPILER_CLEAN,$(c))))

.PHONY: help format format-check clean clean-deps \
        $(COMPILERS) $(addsuffix -chiara,$(COMPILERS)) \
        $(addprefix configure-,$(COMPILERS) $(addsuffix -chiara,$(COMPILERS))) \
        $(BUILDS) $(STEP_BUILDS) $(ALIAS_NAMES) \
        $(SANITIZED) $(addprefix test-,$(SANITIZED)) \
        $(addprefix clean-,$(COMPILERS) $(addsuffix -chiara,$(COMPILERS)) $(PRESETS) $(filter-out %-chiara,$(SANITIZED)))

help:
	@echo "Builds (editor, tests and shaders; no Game):"
	@echo "  <compiler>-<config>             e.g. gcc-debug      alias gd"
	@echo "  <compiler>-<config>-chiara      with capture        alias gd-c"
	@echo "Steps after a build, in this order (any subset):"
	@echo "  <build>-game       the Game executable              alias gdg"
	@echo "  <build>-cook       cook assets/ into <build>/cooked alias gdk"
	@echo "  <build>-pack       pak the last cook beside Game    alias gdp"
	@echo "  <build>-game-cook-pack, <build>-cook-pack, ...      alias gdgkp, gdkp, ..."
	@echo "  <build> is <compiler>-<config> or <compiler>-<config>-chiara (alias suffix -c: gdgkp-c)"
	@echo "  cook tier / pak codec: debug fast/none, dev best/lz4, ship best/zstd"
	@echo "Whole compiler (configure, then every config):"
	@echo "  <compiler>  <compiler>-chiara  configure-<compiler>  configure-<compiler>-chiara"
	@echo "Sanitizers (configure + build; test- also runs ctest):"
	@echo "  $(SANITIZED)"
	@echo "Clean:"
	@echo "  clean-<preset>  clean-<compiler>  clean-<compiler>-chiara  clean  clean-deps"
	@echo "Formatting:"
	@echo "  format  format-check"
	@echo "Compilers: $(COMPILERS)    configs: $(CONFIGS)"
	@echo "Letters: m=msvc g=gcc c=clang, d=debug v=dev s=ship, then g=game k=cook p=pack, -c=Chiara"

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

# Configure presets (FetchContent downloads deps on first configure).
#
# Every configure goes through AssisiConfigureCached.cmake rather than calling
# `cmake --preset` directly, so the ~1.7 GB of dependency sources is cloned once
# and shared by every build tree instead of once per preset. The script explains
# the mechanism and what it costs; the short version is that a GIT_TAG bump needs
# a `make clean-deps` to take effect.
CONFIGURE_SCRIPT = "$(CURDIR)/cmake/AssisiConfigureCached.cmake"

$(addprefix configure-,$(COMPILERS)): configure-%:
	cmake -DPRESETS="$(subst $(SPACE),;,$(call presets-of,$*,$(PRESETS)))" -P $(CONFIGURE_SCRIPT)

# Full setup + build of every config for one compiler.
$(COMPILERS): %: configure-%
	$(call build-each,$(call presets-of,$*,$(PRESETS)))

# One preset. The Game executable is out of the default build, so this is the
# editor, the tests and the shaders.
$(PRESETS):
	cmake --build --preset $@

# One preset, then the Game executable in the same build tree.
$(addsuffix -game,$(BUILDS)): %-game: %
	cmake --build --preset $* --target Assisi-Game

# Cooks assets/ into the build tree. Only what the cook needs is built — the
# tool and the compiled shaders it packs — not the editor. Incremental: an
# unchanged asset costs a hash, not an encode.
$(addsuffix -cook,$(BUILDS)): %-cook:
	cmake --build --preset $* --target Assisi-Cook-Tool Assisi-VkShaders
	"$(call build-dir,$*)/apps/cook/assisi-cook" --source "$(CURDIR)/assets" --out "$(call build-dir,$*)/cooked" --texture-tier $(TIER_$(call config-of,$*))

# Packs the build tree's last cook into assets.pak beside the Game executable,
# which is where the game looks for it with no override.
$(addsuffix -pack,$(BUILDS)): %-pack:
	cmake --build --preset $* --target Assisi-Pack-Tool
	"$(call build-dir,$*)/apps/pack/assisi-pack" --cooked "$(call build-dir,$*)/cooked" --out "$(call build-dir,$*)/apps/game/assets.pak" --compress $(CODEC_$(call config-of,$*))

# Every combination of steps, run in order.
define STEP_COMBO
$(addsuffix -$(1),$(BUILDS)): %-$(1):
	$$(call run-steps,$$*,$(1))
endef
$(foreach s,$(filter-out $(STEPS),$(SUFFIXES)),$(eval $(call STEP_COMBO,$(s))))

# Sanitizer builds (occasional-use: configure + build in one step; the
# configure is a cached no-op after the first run). test-* builds then runs
# the whole suite under the sanitizer.
#
# gcc-tsan-chiara exists because Chiara's own race tests need tsan *and* the
# capture compiled in; gcc-tsan inherits gcc-debug, where Chiara is off, so it
# cannot cover them.
$(SANITIZED):
	cmake -DPRESETS="$@" -P $(CONFIGURE_SCRIPT)
	cmake --build --preset $@

$(addprefix test-,$(SANITIZED)): test-%: %
	ctest --preset $*

# Chiara builds: the same configs with the capture system compiled in.
# Off by default everywhere, so these are the only way to get it. Split into
# configure and build targets like the plain compiler targets, so a build does
# not re-run configure.
#
# They get their own build directories rather than flipping a cache variable in
# place: ASSISI_CHIARA_ENABLED is a PUBLIC define, so toggling it rebuilds
# everything that includes a Chiara header. Separate dirs mean switching back
# and forth costs nothing, and the FetchContent deps stay shared.
#
# `make gs-c` is the one to reach for: an optimized build with capture. Profiling
# a debug build mostly measures the debug build.
$(addprefix configure-,$(addsuffix -chiara,$(COMPILERS))): configure-%-chiara:
	cmake -DPRESETS="$(subst $(SPACE),;,$(call presets-of,$*,$(CHIARA_PRESETS)))" -P $(CONFIGURE_SCRIPT)

$(addsuffix -chiara,$(COMPILERS)): %-chiara: configure-%-chiara
	$(call build-each,$(call presets-of,$*,$(CHIARA_PRESETS)))

$(CHIARA_PRESETS):
	cmake --build --preset $@

# Clean build outputs — removes the entire build directory.
# After cleaning, re-run the relevant configure-* target before building.
$(addprefix clean-,$(PRESETS) $(filter-out %-chiara,$(SANITIZED))): clean-%:
	cmake -E rm -rf "$(CURDIR)/out/build/$*"

$(addprefix clean-,$(addsuffix -chiara,$(COMPILERS))): clean-%-chiara:
	cmake -E rm -rf $(foreach d,$(call presets-of,$*,$(CHIARA_PRESETS) $(filter %-chiara,$(SANITIZED))),"$(CURDIR)/out/build/$(d)")

clean: $(addprefix clean-,$(COMPILERS))

# The shared dependency sources, which `clean` deliberately leaves alone — they
# are the expensive part to rebuild and nothing a compiler flag can invalidate.
# Delete them to re-clone at the current GIT_TAG pins: CMake never runs a git
# operation against a cached source directory, so a bump does nothing until this
# runs. Every configured build tree points here, so all of them need configuring
# again afterwards.
clean-deps:
	cmake -E rm -rf "$(CURDIR)/out/_deps-src"
