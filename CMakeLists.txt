cmake_minimum_required(VERSION 3.16)
project(fafr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED sndfile)
pkg_check_modules(FFTW REQUIRED fftw3)

add_executable(fafr fafr.cpp)

target_include_directories(fafr PRIVATE ${SNDFILE_INCLUDE_DIRS} ${FFTW_INCLUDE_DIRS})
target_link_directories(fafr PRIVATE ${SNDFILE_LIBRARY_DIRS} ${FFTW_LIBRARY_DIRS})

target_link_libraries(fafr PRIVATE ${SNDFILE_LIBRARIES} ${FFTW_LIBRARIES})
target_compile_options(fafr PRIVATE ${SNDFILE_CFLAGS_OTHER} ${FFTW_CFLAGS_OTHER})
