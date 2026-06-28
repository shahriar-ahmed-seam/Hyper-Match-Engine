ubddo what a professional engineer would d alaoo what a professional engineer would doruururlr

# Requirements Document

## Introduction

Hyper-Match-Engine is a simplified high-frequency-trading matching system built around a Limit Order Book. The primary objective is microsecond-scale order-processing latency on a single CPU core.

The system is composed of three cooperating components:

- A single-threaded, lock-free **Matching Engine** written in C++ that processes orders through pre-allocated ring buffers and maintains the order book.
- A raw **Network Server** written in C/C++ that accepts client connections over TCP using OS-native readiness notification (epoll on Linux, IOCP on Windows) without an external networking framework.
- A **Gateway** (Go or Rust) that translates client JSON/HTTP requests into the binary wire protocol consumed by the Matching Engine and translates engine responses back into JSON/HTTP.

A core engineering constraint is zero dynamic memory allocation in the hot path: the Matching Engine allocates all working memory once at startup. The system must demonstrate sustained throughput of at least 100,000 orders per second on a single core.

This document defines functional and non-functional requirements for the system. Implementation choices (specific data-structure layouts, syscall sequences, etc.) are deferred to the design phase except where a constraint is itself a measurable requirement (for example, zero hot-path allocation and the throughput target).

## Glossary

- **System**: The complete Hyper-Match-Engine, comprising the Gateway, Network_Server, and Matching_Engine.
- **Matching_Engine**: The single-threaded C++ component that maintains the Order_Book and matches orders.
- **Network_Server**: The raw TCP server component that manages client socket connections and transfers bytes between clients and the Gateway/Matching_Engine.
- **Gateway**: The component that converts client JSON/HTTP requests into Binary_Messages and converts engine responses back into JSON/HTTP.
- **Binary_Codec**: The encoder/decoder responsible for serializing and deserializing Binary_Messages of the Wire_Protocol.
- **Wire_Protocol**: The fixed-layout binary message format exchanged between the Gateway and the Matching_Engine.
- **Binary_Message**: A single encoded unit of the Wire_Protocol (for example, a NewOrder, CancelOrder, Trade, or Acknowledgement message).
- **Order**: A client instruction to buy or sell, containing an order identifier, side, limit price, and quantity.
- **Limit_Order**: An Order that rests in the Order_Book until it is matched or cancelled, executing only at its limit price or better.
- **Side**: The direction of an Order, either Buy or Sell.
- **Order_Book**: The data structure holding all unmatched resting Limit_Orders, organized by price and arrival time.
- **Best_Bid**: The highest-priced resting Buy Order in the Order_Book.
- **Best_Ask**: The lowest-priced resting Sell Order in the Order_Book.
- **Price_Time_Priority**: The matching rule that orders execute first by best price, then by earliest arrival time within the same price level.
- **Trade**: A record produced when a buy quantity and a sell quantity are matched, containing price, quantity, and the two participating order identifiers.
- **Resting_Order**: A Limit_Order, or remaining unmatched portion of an Order, currently held in the Order_Book.
- **Cancel_Request**: A client instruction to remove a specific Resting_Order from the Order_Book.
- **Ring_Buffer**: A fixed-capacity, pre-allocated circular buffer used to pass orders and events without runtime allocation.
- **Hot_Path**: The code path executed for every order from ingestion through matching to response emission.
- **Processing_Latency**: The elapsed time from when the Matching_Engine dequeues an Order to when the Matching_Engine emits the corresponding response event.
- **Sustained_Throughput**: The number of orders the Matching_Engine fully processes per second, measured over a continuous benchmark run.

## Requirements

### Requirement 1: Order Submission via Gateway

**User Story:** As a trading client, I want to submit orders over HTTP using JSON, so that I can interact with the engine without implementing the binary protocol.

#### Acceptance Criteria

1. WHEN the Gateway receives an HTTP request containing a JSON order with side equal to "buy" or "sell", a limit price between 0.01 and 999,999,999.99 inclusive, and an integer quantity between 1 and 1,000,000,000 inclusive, THE Gateway SHALL encode the order into a NewOrder Binary_Message and forward the NewOrder Binary_Message to the Matching_Engine.
2. WHEN the Gateway receives an HTTP request containing a JSON order without an order identifier, THE Gateway SHALL assign a unique order identifier to the order.
3. WHEN the Gateway receives an HTTP request containing a JSON order that includes an order identifier, THE Gateway SHALL preserve the supplied order identifier for the order.
4. IF the Gateway receives an HTTP request containing a JSON order whose order identifier matches an identifier already in use, THEN THE Gateway SHALL respond with HTTP status 409 and a JSON error description indicating a duplicate order identifier, and THE Gateway SHALL NOT forward a NewOrder Binary_Message to the Matching_Engine.
5. IF the Gateway receives an HTTP request whose body is not valid JSON, THEN THE Gateway SHALL respond with HTTP status 400 and a JSON error description, and THE Gateway SHALL NOT forward a Binary_Message to the Matching_Engine.
6. IF the Gateway receives a JSON order missing a required field, THEN THE Gateway SHALL respond with HTTP status 400 and a JSON error description identifying the missing field, and THE Gateway SHALL NOT forward a Binary_Message to the Matching_Engine.
7. IF the Gateway receives a JSON order with a side other than "buy" or "sell", a limit price outside the range 0.01 to 999,999,999.99 inclusive, or a quantity that is not an integer within the range 1 to 1,000,000,000 inclusive, THEN THE Gateway SHALL respond with HTTP status 400 and a JSON error description identifying the invalid field, and THE Gateway SHALL NOT forward a Binary_Message to the Matching_Engine.
8. WHEN the Gateway receives a response Binary_Message from the Matching_Engine, THE Gateway SHALL convert the response Binary_Message into a JSON response within 10 milliseconds and return the JSON response to the originating client.
9. IF the Matching_Engine is unavailable or does not return a response Binary_Message within 1000 milliseconds of forwarding a NewOrder Binary_Message, THEN THE Gateway SHALL respond with HTTP status 503 and a JSON error description indicating the Matching_Engine is unavailable.

### Requirement 2: Binary Wire Protocol Encoding and Decoding

**User Story:** As a systems developer, I want a precisely defined binary protocol with reliable encode and decode operations, so that the Gateway and Matching_Engine exchange messages without ambiguity.

#### Acceptance Criteria

1. WHEN the Binary_Codec receives a Binary_Message of a supported type, THE Binary_Codec SHALL encode it into a byte sequence whose field layout and total byte length are identical across all instances of that message type as defined by the Wire_Protocol.
2. WHEN the Binary_Codec receives a byte sequence that conforms to the Wire_Protocol, THE Binary_Codec SHALL decode the byte sequence into the corresponding Binary_Message without loss of any encoded field value.
3. IF the Binary_Codec receives a byte sequence whose declared message type is unknown, THEN THE Binary_Codec SHALL return a decode error indicating an unknown message type and SHALL NOT return a Binary_Message.
4. IF the Binary_Codec receives a byte sequence shorter than the length required by its declared message type, THEN THE Binary_Codec SHALL return a decode error indicating insufficient length and SHALL preserve its prior state without modification.
5. IF the Binary_Codec receives a byte sequence longer than the length required by its declared message type, THEN THE Binary_Codec SHALL return a decode error indicating excess trailing bytes and SHALL NOT return a Binary_Message.
6. IF the Binary_Codec receives a Binary_Message containing a field value outside the range permitted by the Wire_Protocol for that field, THEN THE Binary_Codec SHALL return an encode error indicating the out-of-range field and SHALL NOT return a byte sequence.
7. FOR ALL valid Binary_Messages, encoding a Binary_Message and then decoding the resulting byte sequence SHALL produce a Binary_Message equal to the original Binary_Message (round-trip property).
8. FOR ALL byte sequences that the Binary_Codec decodes successfully, decoding the byte sequence and then encoding the resulting Binary_Message SHALL produce a byte sequence equal to the original byte sequence (round-trip property).

### Requirement 3: Limit Order Matching

**User Story:** As a trading client, I want my orders matched against the order book by price-time priority, so that executions are fair and deterministic.

#### Acceptance Criteria

1. WHEN the Matching_Engine processes an incoming Buy Order, THE Matching_Engine SHALL match the Buy Order against resting Sell Orders whose limit price is less than or equal to the Buy Order limit price, selecting the lowest-priced resting Sell Order first.
2. WHEN the Matching_Engine processes an incoming Sell Order, THE Matching_Engine SHALL match the Sell Order against resting Buy Orders whose limit price is greater than or equal to the Sell Order limit price, selecting the highest-priced resting Buy Order first.
3. WHILE multiple Resting_Orders exist at the same price level, THE Matching_Engine SHALL match the Resting_Order with the earliest arrival sequence number first, and IF two Resting_Orders share the same arrival sequence number, THEN THE Matching_Engine SHALL match the Resting_Order with the lower order identifier first.
4. WHEN the Matching_Engine matches two orders, THE Matching_Engine SHALL execute the Trade at the limit price of the Resting_Order.
5. WHEN a matched quantity is determined between an incoming Order and a Resting_Order, THE Matching_Engine SHALL set the Trade quantity to the smaller of the incoming Order remaining quantity and the Resting_Order remaining quantity.
6. WHILE the incoming Order has remaining quantity greater than 0 and at least one eligible opposite-side Resting_Order exists, THE Matching_Engine SHALL continue matching the incoming Order against successive eligible Resting_Orders in price-time priority until the incoming Order remaining quantity reaches 0 or no eligible opposite-side Resting_Order remains.
7. WHEN a Resting_Order remaining quantity reaches 0 following a Trade, THE Matching_Engine SHALL remove that Resting_Order from the Order_Book.
8. WHEN an incoming Order has remaining quantity greater than 0 and no eligible opposite-side Resting_Order exists, THE Matching_Engine SHALL insert the entire remaining quantity into the Order_Book as a Resting_Order at the incoming Order limit price.
9. IF an incoming Order has a quantity that is not an integer in the range 1 to 1,000,000 inclusive, or a price that is not in the range 0.01 to 999,999,999.99 inclusive, THEN THE Matching_Engine SHALL reject the incoming Order, emit a rejection event indicating the validation failure, and preserve the Order_Book unchanged.
10. WHEN the Matching_Engine produces a Trade, THE Matching_Engine SHALL emit a Trade event identifying the execution sequence number, the price, the quantity, the incoming order identifier, and the Resting_Order identifier.

### Requirement 4: Order Book Integrity

**User Story:** As an exchange operator, I want the order book to remain internally consistent after every operation, so that matching results are trustworthy.

#### Acceptance Criteria

1. WHEN the Matching_Engine completes processing an Order, THE Matching_Engine SHALL ensure that the Best_Bid limit price is strictly less than the Best_Ask limit price whenever both a Best_Bid and a Best_Ask exist in the Order_Book.
2. WHEN the Matching_Engine completes processing an Order, THE Matching_Engine SHALL ensure that every Resting_Order in the Order_Book has a remaining quantity strictly greater than zero.
3. WHEN the Matching_Engine fully fills a Resting_Order during processing of an Order, THE Matching_Engine SHALL remove that Resting_Order from the Order_Book.
4. WHEN the Matching_Engine completes processing an Order, THE Matching_Engine SHALL ensure that the incoming Order quantity equals the sum of all matched Trade quantities generated for that Order plus the quantity added to the Order_Book.
5. WHEN the Matching_Engine generates a Trade, THE Matching_Engine SHALL ensure that the Trade quantity is strictly greater than zero.
6. IF an operation during processing of an Order would cause the Best_Bid limit price to be greater than or equal to the Best_Ask limit price, or would leave a Resting_Order with remaining quantity less than or equal to zero, or would violate quantity conservation for that Order, THEN THE Matching_Engine SHALL reject the operation, restore the Order_Book to its state prior to the operation, and emit an error indication identifying the violated integrity invariant.

### Requirement 5: Order Cancellation

**User Story:** As a trading client, I want to cancel a resting order, so that I can withdraw liquidity I no longer want exposed.

#### Acceptance Criteria

1. WHEN the Matching_Engine processes a Cancel_Request that references a Resting_Order present in the Order_Book, THE Matching_Engine SHALL remove the referenced Resting_Order from the Order_Book.
2. WHEN the Matching_Engine removes a Resting_Order in response to a Cancel_Request, THE Matching_Engine SHALL emit exactly one cancellation acknowledgement containing the cancelled order identifier.
3. WHEN the Matching_Engine cancels a Resting_Order, THE Matching_Engine SHALL exclude the cancelled order identifier from all subsequent matching attempts.
4. IF the Matching_Engine processes a Cancel_Request that references an order identifier not present in the Order_Book, THEN THE Matching_Engine SHALL leave the Order_Book unchanged and emit exactly one cancellation rejection containing the order identifier and a reason indicating the order was not found.
5. IF the Matching_Engine processes a Cancel_Request that references an order identifier that has already been fully filled or already cancelled, THEN THE Matching_Engine SHALL leave the Order_Book unchanged and emit exactly one cancellation rejection containing the order identifier and a reason indicating the order is no longer resting.

### Requirement 6: Deterministic Single-Threaded Processing

**User Story:** As a systems developer, I want orders processed deterministically in arrival order on one thread, so that results are reproducible and free of race conditions.

#### Acceptance Criteria

1. THE Matching_Engine SHALL execute all order-processing logic on exactly one dedicated thread, with no order-processing work performed on any other thread.
2. THE Matching_Engine SHALL process orders strictly in the first-in-first-out sequence in which they are dequeued from the ingress Ring_Buffer, with no reordering, skipping, or concurrent processing of dequeued orders.
3. WHEN the Matching_Engine processes an identical sequence of input messages starting from an identical initial Order_Book state, THE Matching_Engine SHALL produce an output event sequence that is identical in event content and event ordering across every repeated run.
4. WHILE processing a given input sequence, THE Matching_Engine SHALL produce an output event sequence that is independent of wall-clock time, message inter-arrival timing, and host system load.
5. IF a dequeued message fails validation, THEN THE Matching_Engine SHALL reject the message, emit an error event indicating the rejection reason, and continue processing the next dequeued order without altering the deterministic processing sequence of the remaining orders.

### Requirement 7: Zero Hot-Path Allocation

**User Story:** As a performance engineer, I want the engine to avoid dynamic memory allocation during runtime, so that latency stays predictable and free of allocator-induced jitter.

#### Acceptance Criteria

1. WHEN the Matching_Engine starts up and before it transitions to its operational state, THE Matching_Engine SHALL reserve all Ring_Buffer and Order_Book working memory at their configured fixed capacities, such that no further reservation occurs once the operational state is entered.
2. WHILE processing orders in the Hot_Path, THE Matching_Engine SHALL perform exactly zero dynamic memory allocations, measured as a dynamic-allocation count of 0 from order ingress to order disposition.
3. WHILE processing orders in the Hot_Path, THE Matching_Engine SHALL expose a dynamic-allocation counter whose value remains at 0, so that an independent observer can verify zero-allocation behavior without inspecting engine internals.
4. IF an ingress Ring_Buffer is full when a new Order arrives, THEN THE Matching_Engine SHALL reject the incoming Order, return a back-pressure indication to the caller, and preserve all previously buffered Orders unchanged, without performing any dynamic memory allocation.
5. IF the configured Ring_Buffer or Order_Book working memory cannot be reserved during startup, THEN THE Matching_Engine SHALL abort startup, report an error indicating insufficient working memory, and remain outside the operational state so that no Order is processed.

### Requirement 8: Raw TCP Network Server

**User Story:** As a trading client, I want to connect to the engine over TCP, so that I can stream orders with low overhead.

#### Acceptance Criteria

1. WHEN an inbound TCP connection request arrives, THE Network_Server SHALL accept the connection and register the client socket for OS-native readiness notification.
2. IF accepting an inbound connection would cause the number of concurrent client connections to exceed 10,000, THEN THE Network_Server SHALL reject the connection request and release any resources allocated for it.
3. WHEN bytes are available on a connected client socket, THE Network_Server SHALL read the available bytes and deliver each complete Binary_Message to the consumer.
4. WHILE a partial Binary_Message has been received on a socket, THE Network_Server SHALL retain the partial bytes in a per-connection buffer not exceeding the maximum Binary_Message size of 65,536 bytes until the remaining bytes of the Binary_Message arrive.
5. IF the bytes received for a single Binary_Message exceed the maximum Binary_Message size of 65,536 bytes, THEN THE Network_Server SHALL close the client connection, release its associated resources, and stop monitoring its readiness events.
6. WHEN a client closes a TCP connection, THE Network_Server SHALL release the connection resources associated with that client within 100 milliseconds and stop monitoring its readiness events.
7. IF a client socket reports an error condition, THEN THE Network_Server SHALL close the client connection, release its associated resources, and stop monitoring its readiness events.

### Requirement 9: Throughput and Latency Benchmarking

**User Story:** As a performance engineer, I want measured proof of throughput and latency, so that I can verify the engine meets its performance targets.

#### Acceptance Criteria

1. THE System SHALL provide a benchmark that submits a configurable volume of orders, settable within the range of 1,000 to 1,000,000,000 orders, to the Matching_Engine and records Sustained_Throughput in orders per second and Processing_Latency in microseconds for each processed order.
2. WHEN the benchmark runs on a single core for a continuous measurement window of at least 60 seconds, THE Matching_Engine SHALL achieve a Sustained_Throughput of at least 100,000 orders per second averaged across the measurement window.
3. WHEN the benchmark completes, THE benchmark SHALL report the median Processing_Latency and the 99th-percentile Processing_Latency in microseconds.
4. WHEN the benchmark completes, THE benchmark SHALL report the total number of orders processed and the elapsed wall-clock time in seconds.
5. IF the measured Sustained_Throughput is below 100,000 orders per second at the end of the measurement window, THEN THE benchmark SHALL terminate with a non-success result indicating that the throughput target was not met and SHALL report the achieved Sustained_Throughput.
6. IF an order submitted during the benchmark is rejected or fails to be processed by the Matching_Engine, THEN THE benchmark SHALL record the count of failed orders and SHALL exclude those orders from the Processing_Latency measurements.
