function(assisi_add_module module_target)
  add_library(${module_target})
  target_link_libraries(${module_target} PUBLIC assisi::options)
endfunction()

function(assisi_module_include module_target)
  target_include_directories(${module_target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
endfunction()
