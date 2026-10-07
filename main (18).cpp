// Project 8: Limit order book + price-time priority matching engine.
// Build: g++ -std=c++20 -O2 -Wall -Wextra project8_order_book.cpp -o orderbook
// Upgrade path for the resume: swap std::map/std::list for flat price arrays,
// intrusive lists and a pool allocator (see comments), then re-measure.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <list>
#include <map>
#include <optional>
#include <random>
#include <unordered_map>
#include <vector>

enum class Side : uint8_t { Buy, Sell };

struct Order {
    uint64_t id;
    Side side;
    int64_t price;  // integer ticks, never floating point
    uint32_t qty;
};

struct Trade {
    uint64_t taker_id, maker_id;
    int64_t price;  // trades execute at the resting (maker) price
    uint32_t qty;
};

class OrderBook {
    using Level = std::list<Order>;  // FIFO queue at a price level = time priority
    struct Loc { Side side; int64_t price; Level::iterator it; };

public:
    // Returns false for duplicate ids or zero quantity.
    bool add(Order o) {
        if (o.qty == 0 || index_.count(o.id)) return false;
        if (o.side == Side::Buy)
            match(o, asks_, [](int64_t ask, int64_t limit) { return ask <= limit; });
        else
            match(o, bids_, [](int64_t bid, int64_t limit) { return bid >= limit; });
        if (o.qty > 0) rest(o);  // unfilled remainder rests on the book
        return true;
    }

    bool cancel(uint64_t id) {
        auto f = index_.find(id);
        if (f == index_.end()) return false;
        Loc loc = f->second;
        if (loc.side == Side::Buy) erase_from(bids_, loc);
        else erase_from(asks_, loc);
        index_.erase(f);
        return true;
    }

    std::optional<int64_t> best_bid() const {
        return bids_.empty() ? std::nullopt : std::optional(bids_.begin()->first);
    }
    std::optional<int64_t> best_ask() const {
        return asks_.empty() ? std::nullopt : std::optional(asks_.begin()->first);
    }
    size_t resting_orders() const { return index_.size(); }
    const std::vector<Trade>& trades() const { return trades_; }
    void clear_trades() { trades_.clear(); }

private:
    template <class Book, class Crosses>
    void match(Order& o, Book& book, Crosses crosses) {
        while (o.qty > 0 && !book.empty()) {
            auto lvl = book.begin();  // best price on the opposite side
            if (!crosses(lvl->first, o.price)) break;
            Level& q = lvl->second;
            while (o.qty > 0 && !q.empty()) {
                Order& maker = q.front();  // oldest order first
                uint32_t fill = std::min(o.qty, maker.qty);
                trades_.push_back({o.id, maker.id, lvl->first, fill});
                o.qty -= fill;
                maker.qty -= fill;
                if (maker.qty == 0) {
                    index_.erase(maker.id);
                    q.pop_front();
                }
            }
            if (q.empty()) book.erase(lvl);
        }
    }

    void rest(const Order& o) {
        if (o.side == Side::Buy) {
            Level& lvl = bids_[o.price];
            lvl.push_back(o);
            index_[o.id] = {o.side, o.price, std::prev(lvl.end())};
        } else {
            Level& lvl = asks_[o.price];
            lvl.push_back(o);
            index_[o.id] = {o.side, o.price, std::prev(lvl.end())};
        }
    }

    template <class Book>
    static void erase_from(Book& book, const Loc& loc) {
        auto lvl = book.find(loc.price);
        lvl->second.erase(loc.it);
        if (lvl->second.empty()) book.erase(lvl);
    }

    std::map<int64_t, Level, std::greater<>> bids_;  // highest price first
    std::map<int64_t, Level, std::less<>> asks_;     // lowest price first
    std::unordered_map<uint64_t, Loc> index_;        // O(1) cancel by order id
    std::vector<Trade> trades_;
};

static void self_test() {
    OrderBook b;
    b.add({1, Side::Sell, 101, 10});
    b.add({2, Side::Sell, 101, 5});   // same price, later => lower priority
    b.add({3, Side::Sell, 102, 8});
    b.add({4, Side::Buy, 100, 7});    // rests
    // A buy for 12 @ 101 fills order 1 (10) then order 2 (2), price-time priority.
    b.add({5, Side::Buy, 101, 12});
    const auto& t = b.trades();
    bool ok = t.size() == 2 && t[0].maker_id == 1 && t[0].qty == 10 && t[1].maker_id == 2 &&
              t[1].qty == 2 && t[0].price == 101;
    ok = ok && b.best_ask() == 101 && b.best_bid() == 100;  // 3 lots left at 101 from order 2
    ok = ok && b.cancel(2) && b.best_ask() == 102 && !b.cancel(2);
    // Aggressive sell sweeps the bid and rests the remainder.
    b.clear_trades();
    b.add({6, Side::Sell, 99, 10});
    ok = ok && b.trades().size() == 1 && b.trades()[0].price == 100 && b.trades()[0].qty == 7 &&
         b.best_ask() == 99;
    std::cout << "self-test: " << (ok ? "PASS" : "FAIL") << "\n";
    if (!ok) std::exit(1);
}

int main() {
    self_test();

    // Benchmark: random mix of limit orders (80%) and cancels (20%) around a mid price.
    const size_t kOps = 1'000'000;
    std::mt19937_64 rng(42);
    OrderBook book;
    std::vector<uint64_t> live;
    std::vector<double> lat;
    lat.reserve(kOps);
    uint64_t next_id = 1;
    size_t total_trades = 0;

    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < kOps; ++i) {
        auto s = std::chrono::steady_clock::now();
        if (!live.empty() && rng() % 5 == 0) {
            size_t k = rng() % live.size();
            book.cancel(live[k]);
            live[k] = live.back();
            live.pop_back();
        } else {
            Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            int64_t px = 10000 + static_cast<int64_t>(rng() % 41) - 20 + (side == Side::Buy ? -3 : 3);
            Order o{next_id++, side, px, static_cast<uint32_t>(1 + rng() % 100)};
            book.add(o);
            live.push_back(o.id);  // may already be filled; cancel then just returns false
        }
        lat.push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - s).count());
        if (book.trades().size() > 4096) { total_trades += book.trades().size(); book.clear_trades(); }
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    total_trades += book.trades().size();

    std::sort(lat.begin(), lat.end());
    auto pct = [&](double p) { return lat[static_cast<size_t>(p * (lat.size() - 1))]; };
    std::cout << kOps << " ops in " << secs << "s => " << kOps / secs / 1e6 << " M ops/sec\n"
              << "trades: " << total_trades << ", resting orders: " << book.resting_orders() << "\n"
              << "latency ns (incl. ~20-30ns timer overhead): p50=" << pct(0.50) << " p99=" << pct(0.99)
              << " p99.9=" << pct(0.999) << "\n";
}
