cmake_minimum_required(VERSION 3.16)

project(hyper_match_engine_cpp
    VERSION 0.1.0
    DESCRIPTION "Hyper-Match-Engine: C++ Matching_Engine, Network_Server, and Binary_Codec"
    LANGUAGES CXX
)

# C++20 across the whole project: the Matching_Engine, Network_Server, and the
# C++ Binary_Codec are C++20.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(HME_BUILD_TESTS "Build the C++ unit and property-based tests" ON)

# --- Component libraries -----------------------------------------------------
add_subdirectory(codec)
add_subdirectory(engine)
add_subdirectory(network)
add_subdirectory(bench)
add_subdirectory(server)

# --- Tests (Catch2 unit runner + RapidCheck property-based testing) ----------
if(HME_BUILD_TESTS)
    include(FetchContent)

    # Unit-test runner.
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.5.2
        GIT_SHALLOW    TRUE
    )

    # Property-based testing library.
    FetchContent_Declare(
        rapidcheck
        GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE
    )

    FetchContent_MakeAvailable(Catch2 rapidcheck)

    enable_testing()
    add_subdirectory(tests)
endif()
