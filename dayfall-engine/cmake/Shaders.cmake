# GLSL -> SPIR-V at build time. Uses glslangValidator from the Vulkan SDK or
# the system; if neither exists it builds glslang from source.
find_program(DF_GLSLANG glslangValidator HINTS $ENV{VULKAN_SDK}/Bin $ENV{VULKAN_SDK}/bin)
if(NOT DF_GLSLANG)
  message(STATUS "glslangValidator not found: building glslang from source")
  FetchContent_Declare(glslang GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG 15.1.0 GIT_SHALLOW TRUE)
  set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
  set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
  set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(glslang)
  set(DF_GLSLANG $<TARGET_FILE:glslang-standalone>)
  set(DF_GLSLANG_DEP glslang-standalone)
endif()

function(df_compile_shaders target src_dir out_dir)
  file(GLOB DF_SHADER_SRC CONFIGURE_DEPENDS ${src_dir}/*.vert ${src_dir}/*.frag ${src_dir}/*.comp)
  file(GLOB DF_SHADER_INC CONFIGURE_DEPENDS ${src_dir}/include/*.glsl)
  set(outputs)
  foreach(src ${DF_SHADER_SRC})
    get_filename_component(name ${src} NAME)
    set(out ${out_dir}/${name}.spv)
    add_custom_command(OUTPUT ${out}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
      COMMAND ${DF_GLSLANG} -V --target-env vulkan1.3 -I${src_dir}/include -o ${out} ${src}
      DEPENDS ${src} ${DF_SHADER_INC} ${DF_GLSLANG_DEP}
      COMMENT "glsl ${name}" VERBATIM)
    list(APPEND outputs ${out})
  endforeach()
  add_custom_target(${target} DEPENDS ${outputs})
endfunction()
