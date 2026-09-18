# The packer: a cooked tree in, one pak archive out.
#
# Out of the default build, like the Game executable: only a build that wants a
# pak runs it, and those ask for it by name (a Makefile `p` target, or the Game
# tests' pak fixture).
add_executable(Assisi-Pack-Tool EXCLUDE_FROM_ALL)
Assisi_apply_defaults(Assisi-Pack-Tool)

target_sources(Assisi-Pack-Tool PRIVATE "src/PackMain.cpp")

target_link_libraries(Assisi-Pack-Tool
  PRIVATE
    Assisi::Cook
)

set_target_properties(Assisi-Pack-Tool PROPERTIES OUTPUT_NAME "assisi-pack")
