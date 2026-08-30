#include "RedisMarketData.h"

#include <hiredis/hiredis.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
// Redis stores everything as strings. std::to_string(double) gives 6 decimals,
// which is plenty for prices here and avoids any printf-format subtleties.
std::string num(double d)        { return std::to_string(d); }
std::string num(long long v)     { return std::to_string(v); }
std::string num(std::uint64_t v) { return std::to_string(static_cast<unsigned long long>(v)); }

// Wall-clock time in nanoseconds since the Unix epoch (kept local so this file
// doesn't need to include the journal headers just for a timestamp).
std::int64_t epochNanosNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

RedisMarketData::RedisMarketData(std::string host, int port)
    : host_(std::move(host)), port_(port) {
    // Connect on the constructing thread first so a bad config fails fast and
    // loudly (rather than silently inside the background thread).
    if (!ensureConnected()) {
        throw std::runtime_error("RedisMarketData: cannot connect to Redis at " +
                                 host_ + ":" + std::to_string(port_));
    }
    publisherThread_ = std::thread(&RedisMarketData::publisherLoop, this);
}

RedisMarketData::~RedisMarketData() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (publisherThread_.joinable()) publisherThread_.join();
    if (ctx_) redisFree(ctx_);
}

bool RedisMarketData::ensureConnected() {
    if (ctx_ && ctx_->err == 0) return true;      // still good
    if (ctx_) {                                    // had a broken one - try reconnect
        if (redisReconnect(ctx_) == REDIS_OK) return true;
        redisFree(ctx_);
        ctx_ = nullptr;
    }
    ctx_ = redisConnect(host_.c_str(), port_);     // fresh connect
    if (!ctx_ || ctx_->err) {
        if (ctx_) {
            std::cerr << "[RedisMarketData] connect error: " << ctx_->errstr << "\n";
        }
        return false;
    }
    return true;
}

// Matching-thread entry points: build a Msg, drop it on the queue, return. No
// network here.
void RedisMarketData::onTrade(const Trade& trade) {
    Msg m;
    m.kind = Msg::TRADE;
    m.buyId = trade.buyOrderId;
    m.sellId = trade.sellOrderId;
    m.price = trade.price;
    m.quantity = trade.quantity;
    m.epochNanos = epochNanosNow();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.push_back(m);
    }
    cv_.notify_one();
}

void RedisMarketData::onTopOfBook(bool hasBid, double bid, bool hasAsk, double ask) {
    Msg m;
    m.kind = Msg::BOOK;
    m.hasBid = hasBid;
    m.hasAsk = hasAsk;
    m.bid = bid;
    m.ask = ask;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.push_back(m);
    }
    cv_.notify_one();
}

void RedisMarketData::publisherLoop() {
    while (true) {
        std::deque<Msg> batch;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [&] { return !queue_.empty() || stopping_; });
            if (stopping_ && queue_.empty()) break;
            batch.swap(queue_); // take everything queued so far in one shot
        }

        if (!ensureConnected()) {
            // Redis is a best-effort real-time cache, NOT the source of truth.
            // If it's down we drop this batch and keep the engine running -
            // losing a market-data tick is survivable; blocking the engine isn't.
            continue;
        }

        // ---- PIPELINE the whole batch: queue all commands locally, flush once,
        // then read all the replies. One network round-trip for many commands. ----
        int pipelined = 0;
        for (const Msg& m : batch) {
            if (m.kind == Msg::TRADE) {
                // "buyId,sellId,price,qty,epoch" - compact payload for subscribers.
                std::string payload = num(m.buyId) + "," + num(m.sellId) + "," +
                                      num(m.price) + "," + num(static_cast<long long>(m.quantity)) +
                                      "," + num(static_cast<long long>(m.epochNanos));
                redisAppendCommand(ctx_, "PUBLISH md:trades %s", payload.c_str());
                redisAppendCommand(ctx_, "SET md:last_price %s", num(m.price).c_str());
                redisAppendCommand(ctx_, "INCR md:trade_count");
                pipelined += 3;
            } else { // BOOK: cache the current top of book so late joiners can GET it
                if (m.hasBid) redisAppendCommand(ctx_, "SET md:best_bid %s", num(m.bid).c_str());
                else          redisAppendCommand(ctx_, "DEL md:best_bid");
                if (m.hasAsk) redisAppendCommand(ctx_, "SET md:best_ask %s", num(m.ask).c_str());
                else          redisAppendCommand(ctx_, "DEL md:best_ask");
                std::string book = (m.hasBid ? num(m.bid) : std::string("-")) + "," +
                                   (m.hasAsk ? num(m.ask) : std::string("-"));
                redisAppendCommand(ctx_, "PUBLISH md:book %s", book.c_str());
                pipelined += 3;
            }
        }

        // Drain the replies. If any reply comes back null the connection broke
        // mid-batch; bail and let the next wake reconnect.
        bool broke = false;
        for (int i = 0; i < pipelined; ++i) {
            redisReply* reply = nullptr;
            if (redisGetReply(ctx_, reinterpret_cast<void**>(&reply)) != REDIS_OK || !reply) {
                broke = true;
                break;
            }
            freeReplyObject(reply);
        }
        if (broke) {
            std::cerr << "[RedisMarketData] pipeline error: "
                      << (ctx_ ? ctx_->errstr : "unknown") << "\n";
            continue; // ensureConnected() on the next iteration will reconnect
        }

        commandsSent_ += static_cast<std::uint64_t>(pipelined);
    }
}
