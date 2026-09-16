# The cooker: the source asset tree in, a cooked tree out.
#
# An executable rather than a build-time script because cooking a level means
# *loading* it, which needs every ACOMP and AASSET registration this project
# declares — the same set the game links. assisi_link_reflections is what pulls
# them all in; a static library would let the linker drop the registrations as
# unreferenced, and the cook would then refuse every component it could not find.
add_executable(Assisi-Cook-Tool)
Assisi_apply_defaults(Assisi-Cook-Tool)

target_sources(Assisi-Cook-Tool PRIVATE "src/CookMain.cpp")

target_link_libraries(Assisi-Cook-Tool
  PRIVATE
    Assisi::Cook
    # The project's own code. Needed for its *bodies*, not its declarations:
    # assisi_link_reflections below pulls in every generated registration in the
    # build, and this project's ASYSTEM registrations name functions that live
    # here. Without it the cooker does not link at all.
    Assisi-GameLib
)

# Every reflected type in the build, not a chosen subset: a level may name any
# component, and one the cooker cannot find is a level it would refuse. This
# already includes the game's own generated objects, so they are not listed
# again — a second copy would be a duplicate definition of every registrar.
assisi_link_reflections(Assisi-Cook-Tool)

set_target_properties(Assisi-Cook-Tool PROPERTIES OUTPUT_NAME "assisi-cook")

# --- Running it.
#
# Deliberately NOT in ALL. No developer build reads the cooked output — the
# source tree is still the only asset root — so a cook on every build would be
# minutes spent on bytes nothing opens. The target exists so a packaging build
# can depend on it and so `cook` is a thing a person can ask for.
#
# **Run it from an optimised preset.** The work is BC7 at its exhaustive tier
# over every texture in the tree, which is the kind of tight search loop an
# unoptimised build makes an order of magnitude slower:
#
#     cmake --build out/build/gcc-ship --target cook
#
# The target is defined in every preset rather than gated to the optimised ones,
# because gating it would make `--target cook` fail with "no such target" in a
# debug tree, which reads as a broken build rather than as a slow choice.
#
# It depends on the shaders because `.spv` files are build outputs: on a clean
# clone there are none, and the cooker would then name every shader under a
# derived id with no bytes behind it.
add_custom_target(cook
  COMMAND $<TARGET_FILE:Assisi-Cook-Tool> --source "${CMAKE_SOURCE_DIR}/assets"
                                          --out "${CMAKE_BINARY_DIR}/cooked"
  DEPENDS Assisi-Cook-Tool Assisi-VkShaders
  COMMENT "Cooking assets/ -> ${CMAKE_BINARY_DIR}/cooked"
  VERBATIM
)
