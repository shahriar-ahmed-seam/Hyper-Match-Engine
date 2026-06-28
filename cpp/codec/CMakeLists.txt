# C++ Binary_Codec target. Holds the shared Wire_Protocol header that both the
# engine and network components depend on.
add_library(hme_codec STATIC
    src/codec.cpp
)
add_library(hme::codec ALIAS hme_codec)

target_include_directories(hme_codec PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(hme_codec PUBLIC cxx_std_20)
