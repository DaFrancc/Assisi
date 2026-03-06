#!/usr/bin/env bash
# setup.sh — Assisi new-app scaffolding for Linux
# Usage: bash setup.sh

set -euo pipefail

# -----------------------
# Pretty output helpers
# -----------------------
info()    { printf '\033[0;36m[i] %s\033[0m\n' "$*"; }
ok()      { printf '\033[0;32m[+] %s\033[0m\n' "$*"; }
warn()    { printf '\033[0;33m[!] %s\033[0m\n' "$*"; }
err()     { printf '\033[0;31m[x] %s\033[0m\n' "$*"; }
heading() { printf '\n\033[0;35m=== %s ===\033[0m\n' "$*"; }

read_nonempty() {
  local prompt="$1" value=""
  while true; do
    read -rp "$prompt: " value
    value="${value//[[:space:]]/}"   # strip surrounding whitespace
    [[ -n "$value" ]] && { echo "$value"; return; }
    warn "Please enter a value."
  done
}

validate_game_name() {
  if [[ ! "$1" =~ ^[A-Za-z][A-Za-z0-9_-]*$ ]]; then
    err "Game name '$1' is invalid. Use letters/numbers/underscore/dash, start with a letter."
    exit 1
  fi
}

# -----------------------
# Repo root check
# -----------------------
REPO_ROOT="$(pwd)"
if [[ ! -f "$REPO_ROOT/CMakePresets.json" ]]; then
  err "Run this from the repository root (the folder containing CMakePresets.json). Current: $REPO_ROOT"
  exit 1
fi

heading "Assisi Game Setup"
info "Repo: $REPO_ROOT"

# -----------------------
# 1) Prompt user
# -----------------------
GAME_NAME="$(read_nonempty "Game name (e.g. CustomGame)")"
validate_game_name "$GAME_NAME"

GAME_DIR="$REPO_ROOT/apps/$GAME_NAME"
SRC_DIR="$GAME_DIR/src"

if [[ -d "$GAME_DIR" ]]; then
  err "apps/$GAME_NAME already exists."
  exit 1
fi

echo ""
info "Choose a compiler:"
select COMPILER in gcc clang; do
  [[ -n "$COMPILER" ]] && break
done

# -----------------------
# 2) Scaffold new app
# -----------------------
heading "Scaffolding"
info "Creating: apps/$GAME_NAME"

mkdir -p "$SRC_DIR"

cat > "$GAME_DIR/CMakeLists.txt" <<EOF
add_executable(Assisi-${GAME_NAME})
Assisi_apply_defaults(Assisi-${GAME_NAME})

target_sources(Assisi-${GAME_NAME}
  PRIVATE
    "src/main.cpp"
)

find_package(OpenGL REQUIRED)

target_link_libraries(Assisi-${GAME_NAME}
  PRIVATE
    Assisi::App
    OpenGL::GL
)

add_custom_command(TARGET Assisi-${GAME_NAME} POST_BUILD
    COMMAND \${CMAKE_COMMAND} -E copy_directory
        "\${CMAKE_SOURCE_DIR}/assets"
        "\$<TARGET_FILE_DIR:Assisi-${GAME_NAME}>/assets"
    COMMENT "Copying assets next to Assisi-${GAME_NAME}"
)
EOF

cat > "$SRC_DIR/main.cpp" <<EOF
#include <Assisi/App/Application.hpp>

class ${GAME_NAME}App : public Assisi::App::Application
{
  public:
    void OnStart() override {}
    void OnFixedUpdate(float dt) override { (void)dt; }
    void OnUpdate(float dt) override { (void)dt; }
    void OnRender() override {}
};

int main()
{
    ${GAME_NAME}App app;
    app.Run();
    return 0;
}
EOF

ok "Created apps/$GAME_NAME with a starter CMakeLists.txt and main.cpp"

# -----------------------
# 3) CMakeUserPresets.json
# -----------------------
heading "User Presets"
USER_PRESETS_PATH="$REPO_ROOT/CMakeUserPresets.json"

cat > "$USER_PRESETS_PATH" <<EOF
{
  "version": 6,
  "configurePresets": [
    {
      "name": "${COMPILER}-debug-user",
      "inherits": "${COMPILER}-debug",
      "cacheVariables": { "ASSISI_APP": "${GAME_NAME}" }
    },
    {
      "name": "${COMPILER}-release-user",
      "inherits": "${COMPILER}-release",
      "cacheVariables": { "ASSISI_APP": "${GAME_NAME}" }
    },
    {
      "name": "${COMPILER}-sanitize-user",
      "inherits": "${COMPILER}-sanitize",
      "cacheVariables": { "ASSISI_APP": "${GAME_NAME}" }
    }
  ],
  "buildPresets": [
    { "name": "${COMPILER}-debug-user",    "configurePreset": "${COMPILER}-debug-user" },
    { "name": "${COMPILER}-release-user",  "configurePreset": "${COMPILER}-release-user" },
    { "name": "${COMPILER}-sanitize-user", "configurePreset": "${COMPILER}-sanitize-user" }
  ]
}
EOF

ok "Wrote CMakeUserPresets.json (compiler=$COMPILER, ASSISI_APP=$GAME_NAME)"

# -----------------------
# 4) Conan installs
# -----------------------
heading "Dependencies (Conan via Make)"

if ! command -v make &>/dev/null; then
  err "make not found in PATH."
  exit 1
fi

info "Running: make conan-${COMPILER}"
make "conan-${COMPILER}"

ok "All Conan installs completed successfully."

# -----------------------
# Done
# -----------------------
echo ""
info "You can configure/build now with:"
printf '  cmake --preset %s-debug-user\n' "$COMPILER"
printf '  cmake --build --preset %s-debug-user\n' "$COMPILER"

heading "Done"
ok "Game: apps/$GAME_NAME"
ok "Preset: ${COMPILER}-debug-user"