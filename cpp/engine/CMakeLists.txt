# Matching_Engine target. Deterministic single-threaded matching core.
add_library(hme_engine STATIC
    src/engine.cpp
    src/alloc_counter.cpp
)
add_library(hme::engine ALIAS hme_engine)

target_include_directories(hme_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(hme_engine PUBLIC hme::codec)
target_compile_features(hme_engine PUBLIC cxx_std_20)
