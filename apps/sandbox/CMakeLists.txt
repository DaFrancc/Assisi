add_executable(assisi-sandbox)

target_sources(assisi-sandbox
  PRIVATE
    src/main.cpp
)

target_link_libraries(assisi-sandbox
  PRIVATE
    assisi::core
)
