# Unit + property-based test executable. Catch2 is the unit runner and
# RapidCheck drives the property-based tests.
add_executable(hme_tests
    wire_protocol_test.cpp
    ring_buffer_test.cpp
    prop_ring_buffer_backpressure_test.cpp
    binary_message_test.cpp
    alloc_counter_test.cpp
    codec_test.cpp
    prop_codec_round_trip_test.cpp
    prop_codec_byte_round_trip_test.cpp
    prop_codec_reject_malformed_test.cpp
    order_book_test.cpp
    matching_engine_test.cpp
    matching_engine_validation_test.cpp
    matching_engine_cancel_test.cpp
    prop_price_time_priority_test.cpp
    prop_trade_pricing_test.cpp
    prop_terminal_condition_test.cpp
    prop_quantity_conservation_test.cpp
    prop_invalid_order_preserves_book_test.cpp
    prop_cancel_removes_resting_test.cpp
    prop_cancel_non_resting_test.cpp
    prop_price_ordering_invariant_test.cpp
    prop_positive_quantity_invariant_test.cpp
    integrity_guard_test.cpp
    integrity_guard_fault_injection_test.cpp
    engine_processor_test.cpp
    prop_fifo_processing_order_test.cpp
    prop_deterministic_output_test.cpp
    prop_continue_after_invalid_test.cpp
    engine_startup_test.cpp
    engine_startup_failure_test.cpp
    cross_impl_byte_compat_test.cpp
    latency_stats_test.cpp
    frame_buffer_test.cpp
    connection_registry_test.cpp
    connection_lifecycle_integration_test.cpp
    prop_zero_hotpath_alloc_test.cpp
    prop_network_framing_round_trip_test.cpp
    prop_oversize_frame_rejection_test.cpp
    prop_latency_stat_invariants_test.cpp
    benchmark_reporting_test.cpp
    prop_benchmark_failed_order_test.cpp
    server_pipeline_wiring_test.cpp
    end_to_end_integration_test.cpp
)

target_link_libraries(hme_tests PRIVATE
    hme::codec
    hme::engine
    hme::network
    hme::bench
    hme::server
    Catch2::Catch2WithMain
    rapidcheck
)

target_compile_features(hme_tests PRIVATE cxx_std_20)

include(Catch)
catch_discover_tests(hme_tests)
