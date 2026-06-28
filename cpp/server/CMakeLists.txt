# Server-assembly target. Wires the Network_Server framing, the ingress/egress
# Ring_Buffers, the Matching_Engine, and the response encoding into the single
# in-process data path.
#
# The assembly is header-only (it just composes the engine + network + codec
# components), so it is exposed as an INTERFACE library: consumers get the
# include path, the composed component libraries, and the C++20 requirement.
add_library(hme_server INTERFACE)
add_library(hme::server ALIAS hme_server)

target_include_directories(hme_server INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# The pipeline composes all three tiers' C++ pieces plus the shared codec.
target_link_libraries(hme_server INTERFACE
    hme::engine
    hme::network
    hme::codec
)
target_compile_features(hme_server INTERFACE cxx_std_20)

# --- Runnable engine server executable --------------------------------------
# The real TCP front-end: wraps the in-process ServerPipeline with the
# select()-based TcpServer + SelectReadinessPoller from hme::network and serves
# raw Wire_Protocol bytes over TCP. It moves bytes only; the pipeline frames and
# encodes the protocol.
add_executable(hme_engine_server src/main.cpp)
target_link_libraries(hme_engine_server PRIVATE
    hme::server
    hme::network
    hme::codec
    hme::engine
)
target_compile_features(hme_engine_server PRIVATE cxx_std_20)
