# Benchmark harness target. Records per-order Processing_Latency and reports
# summary statistics (median / p99) plus throughput and pass/fail against the
# 100k orders/sec target.
#
# The component is currently header-only, so it is exposed as an INTERFACE
# library: consumers (tests and the benchmark driver) just need the include path
# and the C++20 feature requirement.
add_library(hme_bench INTERFACE)
add_library(hme::bench ALIAS hme_bench)

target_include_directories(hme_bench INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(hme_bench INTERFACE hme::engine)
target_compile_features(hme_bench INTERFACE cxx_std_20)
