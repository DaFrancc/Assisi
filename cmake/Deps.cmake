cmake_minimum_required(VERSION 3.22)

set(ASSISI_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/..")

file(MAKE_DIRECTORY "${ASSISI_EXTERNAL_DIR}")
file(MAKE_DIRECTORY "${ASSISI_DEPS_DIR}")

# -------------------------
# Helpers
# -------------------------

function(assisi_clone_repo name url ref dir)
  if (EXISTS "${dir}/.git")
    message(STATUS "${name}: already present")
    return()
  endif()

  message(STATUS "${name}: cloning into ${dir}")

  execute_process(
    COMMAND git clone --depth 1 --branch "${ref}" "${url}" "${dir}"
    RESULT_VARIABLE rc
  )
  if (NOT rc EQUAL 0)
    message(FATAL_ERROR "${name}: git clone failed")
  endif()
endfunction()

function(assisi_download_file name url out_file)
  if (EXISTS "${out_file}")
    message(STATUS "${name}: already present")
    return()
  endif()

  message(STATUS "${name}: downloading")

  file(DOWNLOAD
    "${url}"
    "${out_file}"
    STATUS status
    TLS_VERIFY ON
  )

  list(GET status 0 code)
  if (NOT code EQUAL 0)
    message(FATAL_ERROR "${name}: download failed (${url})")
  endif()
endfunction()

function(assisi_build_and_install_glfw config)
  set(GLFW_SRC_DIR "${ASSISI_EXTERNAL_DIR}/glfw")
  set(GLFW_BUILD_DIR "${ASSISI_DEPS_DIR}/glfw-build-${config}")
  set(GLFW_INSTALL_DIR "${ASSISI_DEPS_DIR}/glfw-install/${config}")

  if (EXISTS "${GLFW_INSTALL_DIR}/lib")
    message(STATUS "glfw (${config}): already installed")
    return()
  endif()

  message(STATUS "glfw (${config}): configuring, building, installing")

  # Ensure install path is treated as a single value (avoid ';' list issues).
  set(GLFW_INSTALL_DIR_ESC "${GLFW_INSTALL_DIR}")
  string(REPLACE ";" "\\;" GLFW_INSTALL_DIR_ESC "${GLFW_INSTALL_DIR_ESC}")

  execute_process(
    COMMAND cmake -S "${GLFW_SRC_DIR}" -B "${GLFW_BUILD_DIR}"
            -DCMAKE_BUILD_TYPE=${config}
            -DCMAKE_INSTALL_PREFIX:PATH=${GLFW_INSTALL_DIR_ESC}
            -DGLFW_BUILD_DOCS=OFF
            -DGLFW_BUILD_TESTS=OFF
            -DGLFW_BUILD_EXAMPLES=OFF
            -Wno-dev
    RESULT_VARIABLE rc
  )
  if (NOT rc EQUAL 0)
    message(FATAL_ERROR "glfw (${config}): configure failed")
  endif()

  execute_process(
    COMMAND cmake --build "${GLFW_BUILD_DIR}" --config ${config}
    RESULT_VARIABLE rc
  )
  if (NOT rc EQUAL 0)
    message(FATAL_ERROR "glfw (${config}): build failed")
  endif()

  execute_process(
    COMMAND cmake --install "${GLFW_BUILD_DIR}" --config ${config}
    RESULT_VARIABLE rc
  )
  if (NOT rc EQUAL 0)
    message(FATAL_ERROR "glfw (${config}): install failed")
  endif()
endfunction()

# -------------------------
# GLFW (source in external/, artifacts in out/Deps/)
# -------------------------

assisi_clone_repo(
  glfw
  "https://github.com/glfw/glfw.git"
  "3.3.9"
  "${ASSISI_EXTERNAL_DIR}/glfw"
)

# Build both configs so you never rebuild it during normal dev.
assisi_build_and_install_glfw(Debug)
assisi_build_and_install_glfw(Release)

# -------------------------
# stb (single header)
# -------------------------

file(MAKE_DIRECTORY "${ASSISI_EXTERNAL_DIR}/stb")
assisi_download_file(
  stb_image
  "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h"
  "${ASSISI_EXTERNAL_DIR}/stb/stb_image.h"
)

# -------------------------
# GLM (flatten into external/glm/, no nested folder)
# -------------------------

set(GLM_DIR "${ASSISI_EXTERNAL_DIR}/glm")

if (NOT EXISTS "${GLM_DIR}/glm.hpp")
  message(STATUS "glm: downloading archive and extracting into external/glm")

  file(MAKE_DIRECTORY "${ASSISI_DEPS_DIR}/_tmp")
  set(GLM_ZIP "${ASSISI_DEPS_DIR}/_tmp/glm.zip")
  set(GLM_TMP "${ASSISI_DEPS_DIR}/_tmp/glm-extract")

  file(DOWNLOAD
    "https://github.com/g-truc/glm/archive/refs/heads/master.zip"
    "${GLM_ZIP}"
    STATUS status
    TLS_VERIFY ON
  )

  list(GET status 0 code)
  if (NOT code EQUAL 0)
    message(FATAL_ERROR "glm: download failed")
  endif()

  file(REMOVE_RECURSE "${GLM_TMP}")
  file(MAKE_DIRECTORY "${GLM_TMP}")

  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xzf "${GLM_ZIP}"
    WORKING_DIRECTORY "${GLM_TMP}"
    RESULT_VARIABLE rc
  )
  if (NOT rc EQUAL 0)
    message(FATAL_ERROR "glm: extract failed")
  endif()

  file(GLOB GLM_ROOT_DIRS LIST_DIRECTORIES true "${GLM_TMP}/*")
  list(GET GLM_ROOT_DIRS 0 GLM_ROOT)

  if (NOT EXISTS "${GLM_ROOT}/glm/glm.hpp")
    message(FATAL_ERROR "glm: extracted archive missing glm/glm.hpp")
  endif()

  file(REMOVE_RECURSE "${GLM_DIR}")
  file(MAKE_DIRECTORY "${GLM_DIR}")

  # Copy CONTENTS of glm/ into external/glm (so external/glm/glm.hpp exists)
  file(COPY "${GLM_ROOT}/glm/" DESTINATION "${GLM_DIR}")

  message(STATUS "glm: ready at ${GLM_DIR}")
else()
  message(STATUS "glm: already present")
endif()
