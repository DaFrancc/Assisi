add_library(Assisi-Chiara)
Assisi_apply_defaults(Assisi-Chiara)

target_sources(Assisi-Chiara
  PRIVATE
    "src/Chiara.cpp"
  PUBLIC
    "include/Assisi/Chiara/Chiara.hpp"
    "include/Assisi/Chiara/Event.hpp"
    "include/Assisi/Chiara/Profile.hpp"
)

target_include_directories(Assisi-Chiara
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# The gate. PUBLIC because the macros are header-only: a translation unit that
# includes Profile.hpp must agree with the library about whether capture exists,
# or the no-op macros and the real ones end up in the same binary.
#
# Being PUBLIC is also why the -chiara presets get their own build directory
# rather than flipping a cache variable in place — toggling this rebuilds
# everything that transitively includes a Chiara header, i.e. the world.
if (ASSISI_ENABLE_CHIARA)
  target_compile_definitions(Assisi-Chiara PUBLIC ASSISI_CHIARA_ENABLED=1)

  # The only link dependency Chiara has, and only for pthread_setname_np /
  # thread_local on toolchains that still keep them out of libc. Nothing else:
  # a module with no dependencies cannot be dragged up the stack by one, which
  # is what lets Core (and therefore everything) depend on it.
  find_package(Threads REQUIRED)
  target_link_libraries(Assisi-Chiara PUBLIC Threads::Threads)
endif()

add_library(Assisi::Chiara ALIAS Assisi-Chiara)

if (ASSISI_BUILD_TESTS)
  add_subdirectory(tests)
endif()
