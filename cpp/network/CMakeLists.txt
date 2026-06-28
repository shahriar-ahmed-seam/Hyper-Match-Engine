# Network_Server target. Raw TCP server with OS-native readiness notification
# and per-connection framing.
add_library(hme_network STATIC
    src/network.cpp
    src/tcp_server.cpp
)
add_library(hme::network ALIAS hme_network)

target_include_directories(hme_network PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(hme_network PUBLIC hme::codec)

# The real TCP server uses Winsock2 on Windows; link the socket library.
if(WIN32)
    target_link_libraries(hme_network PUBLIC ws2_32)
endif()

target_compile_features(hme_network PUBLIC cxx_std_20)
