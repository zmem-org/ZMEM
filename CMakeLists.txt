cmake_minimum_required(VERSION 3.20)
project(zmem_benchmarks LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Fetch Glaze (zmem branch)
FetchContent_Declare(
  glaze
  GIT_REPOSITORY https://github.com/stephenberry/glaze.git
  GIT_TAG zmem
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glaze)

# Add a simple ZMEM-only benchmark target
add_executable(zmem_bench benchmarks/zmem_bench.cpp)
target_link_libraries(zmem_bench PRIVATE glaze::glaze)

# Option to build comparison benchmarks (requires Cap'n Proto and FlatBuffers)
option(ZMEM_BENCH_COMPARISONS "Build comparison benchmarks against Cap'n Proto and FlatBuffers" OFF)

if(ZMEM_BENCH_COMPARISONS)
  # Find Cap'n Proto
  find_package(CapnProto CONFIG QUIET)
  if(NOT CapnProto_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(CAPNP capnp)
    endif()
  endif()

  # Find FlatBuffers
  find_package(Flatbuffers CONFIG QUIET)
  if(NOT Flatbuffers_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(FLATBUFFERS flatbuffers)
    endif()
  endif()

  # Check if we can build the comparison benchmark
  set(CAN_BUILD_COMPARISON TRUE)

  if(NOT CapnProto_FOUND AND NOT CAPNP_FOUND)
    message(WARNING "Cap'n Proto not found - comparison benchmark will not be built")
    set(CAN_BUILD_COMPARISON FALSE)
  endif()

  if(NOT Flatbuffers_FOUND AND NOT FLATBUFFERS_FOUND)
    message(WARNING "FlatBuffers not found - comparison benchmark will not be built")
    set(CAN_BUILD_COMPARISON FALSE)
  endif()

  if(CAN_BUILD_COMPARISON)
    # Fetch bencher library
    FetchContent_Declare(
      bencher
      GIT_REPOSITORY https://github.com/stephenberry/bencher.git
      GIT_TAG main
      GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(bencher)

    # Add comparison benchmark executable
    add_executable(zmem_benchmark
      benchmarks/zmem_benchmark.cpp
      benchmarks/benchmark.capnp.c++
    )

    target_include_directories(zmem_benchmark PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks
      ${bencher_SOURCE_DIR}/include
    )

    target_link_libraries(zmem_benchmark PRIVATE glaze::glaze)

    # Link Cap'n Proto
    if(CapnProto_FOUND)
      target_link_libraries(zmem_benchmark PRIVATE CapnProto::capnp)
    else()
      target_include_directories(zmem_benchmark PRIVATE ${CAPNP_INCLUDE_DIRS})
      target_link_libraries(zmem_benchmark PRIVATE ${CAPNP_LIBRARIES})
      target_compile_options(zmem_benchmark PRIVATE ${CAPNP_CFLAGS_OTHER})
    endif()

    # Link FlatBuffers
    if(Flatbuffers_FOUND)
      target_link_libraries(zmem_benchmark PRIVATE flatbuffers::flatbuffers)
    else()
      target_include_directories(zmem_benchmark PRIVATE ${FLATBUFFERS_INCLUDE_DIRS})
      target_link_libraries(zmem_benchmark PRIVATE ${FLATBUFFERS_LIBRARIES})
    endif()

    # Suppress warnings in generated code
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(zmem_benchmark PRIVATE
        -Wno-missing-field-initializers
        -Wno-unused-parameter
      )
    endif()

    message(STATUS "Comparison benchmark enabled")
  endif()
endif()
