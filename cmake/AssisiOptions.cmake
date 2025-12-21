add_library(assisi-options INTERFACE)
add_library(assisi::options ALIAS assisi-options)

target_compile_features(assisi-options INTERFACE cxx_std_20)

if (MSVC)
  target_compile_options(assisi-options INTERFACE /W4 /permissive-)
else()
  target_compile_options(assisi-options INTERFACE -Wall -Wextra -Wpedantic)
endif()
