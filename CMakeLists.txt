cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)
include(rive)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs rive::runtime)
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/rive_probe.cpp
    src/rive_file.cpp
    deps/rive-runtime/utils/no_op_factory.cpp
)

if(APPLE)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/rive_renderer_mac.mm)
  set_source_files_properties(
    src/rive_renderer_mac.mm
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-fno-rtti"
  )
elseif(WIN32)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/rive_renderer_win.cpp)
  set_source_files_properties(
    src/rive_renderer_win.cpp
    PROPERTIES COMPILE_OPTIONS "/GR-"
  )
endif()

# rive-runtime is vendored under our strict warning set and compiled without
# RTTI; match those flags for the files we pull in from that tree so the
# typeinfo for rive's render base classes (which we don't define here) isn't
# referenced.
set_source_files_properties(
  deps/rive-runtime/utils/no_op_factory.cpp
  PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-error;-fno-rtti"
)
add_dependencies(${CMAKE_PROJECT_NAME} rive_runtime_build)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
