cmake_minimum_required(VERSION 3.16)
project(fafr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
pkg_check_modules(FFTW REQUIRED IMPORTED_TARGET fftw3)

add_library(fafr_lib
  src/fafr_lib.cpp
)

target_include_directories(fafr_lib PUBLIC include)
target_link_libraries(fafr_lib PUBLIC PkgConfig::SNDFILE PkgConfig::FFTW)

add_executable(fafr fafr.cpp)
target_link_libraries(fafr PRIVATE fafr_lib)

if(APPLE)
  option(FAFR_BUILD_MACOS_COMPONENT "Build macOS .component bundle (scaffold)" ON)
  set(FAFR_COMPONENT_INSTALL_DIR "$ENV{HOME}/Library/Audio/Plug-Ins/Components"
      CACHE PATH "Install path for FAFR AudioComponent bundle")

  if(FAFR_BUILD_MACOS_COMPONENT)
    add_library(fafr_component MODULE
      macos/FafrComponent.cpp
      macos/FafrAudioFileComponent.cpp
    )
    set_target_properties(fafr_component PROPERTIES
      BUNDLE TRUE
      BUNDLE_EXTENSION component
      OUTPUT_NAME "Fafr"
      MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist
    )
    target_link_libraries(fafr_component PRIVATE fafr_lib "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation")
    target_include_directories(fafr_component PRIVATE include)
    install(TARGETS fafr_component DESTINATION ${FAFR_COMPONENT_INSTALL_DIR})
  endif()
endif()

if(APPLE)
  add_executable(fafr_diag tools/fafr_diag.cpp)
  target_link_libraries(fafr_diag PRIVATE "-framework AudioToolbox" "-framework CoreFoundation")
endif()
