#pragma once

#include <atomic>
#include <csignal>
#include <cstdint>

#include "store.hpp"

namespace edition {

inline constexpr int kServerWaitIntervalMilliseconds = 500;

void handle_connection(int descriptor, Store& store,
                       volatile std::sig_atomic_t& stop_requested,
                       std::atomic<std::int64_t>& next_received_order);

}  // namespace edition
