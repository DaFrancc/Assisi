# Sigil (Simple Graph Language): a small text language for state machines,
# shared by animation and, later, AI. The runtime runs compiled expressions
# and is what the game links; the compiler turns source into them and is
# cook-side only.
add_subdirectory(runtime)
add_subdirectory(compile)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
