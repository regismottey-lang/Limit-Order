# Limit-Order
Limit Order Book and Matching Engine
A price-time priority matching engine of the kind used by exchanges and trading systems, with a self-test and a latency benchmark.

Demonstrates: data structure selection, STL container design, integer price handling, correctness testing, latency measurement.
How it works
Prices are integer ticks (never floating point).
Bids and asks are std::maps ordered best-price-first; each price level is a std::list<Order> used as a FIFO queue, which gives time priority within a price.
A std::unordered_map from order ID to its list position gives O(1) lookup for cancels.
Incoming orders match against the opposite side while prices cross. Trades execute at the resting (maker) price. Any unfilled remainder rests on the book.
Duplicate IDs and zero-quantity orders are rejected.
Self-test
Checks time priority at the same price, partial fills, cancels, and an aggressive order that sweeps a level and rests the remainder. It prints self-test: PASS or exits with an error.
Benchmark
Runs 1,000,000 operations (80% limit orders, 20% cancels, seeded for reproducibility) and prints throughput and p50/p99/p99.9 latency. Latency includes roughly 20 to 30 ns of timer overhead. Results vary by machine.
Build and run
g++ -std=c++20 -O2 -Wall -Wextra project8_order_book.cpp -o orderbook

./orderbook
Possible extensions
Replace std::map and std::list with flat price arrays, intrusive lists, and a pool allocator (see Project 2), then re-measure.
Add market orders, IOC/FOK order types, and order modification.
