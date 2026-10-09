# Sigil (Simple Graph Language): a small text language for state machines,
# shared by animation and, later, AI. Only the compiler exists so far; the
# evaluator the game runs will sit beside it as a library of its own.
add_subdirectory(compile)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
