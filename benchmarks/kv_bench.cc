#include "stratakv/db.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <tuple>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::filesystem::path FreshBenchDir() {
  const auto now = Clock::now().time_since_epoch().count();
  auto path = std::filesystem::temp_directory_path() /
              ("stratakv-bench-" + std::to_string(now));
  std::filesystem::create_directories(path);
  return path;
}

std::string KeyFor(std::size_t i) {
  std::string key = "key-";
  key += std::to_string(i);
  return key;
}

std::string ValueFor(std::size_t i) {
  std::string value = "value-";
  value += std::to_string(i);
  value.append(96, 'x');
  return value;
}

double Seconds(Clock::duration duration) {
  return std::chrono::duration<double>(duration).count();
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t operations = 10000;
  if (argc > 1) {
    operations = static_cast<std::size_t>(std::stoull(argv[1]));
  }

  const auto db_path = FreshBenchDir();
  stratakv::Options options;
  options.write_buffer_size = 64 * 1024;
  options.block_cache_size = 8 * 1024 * 1024;
  auto [db, open_status] = stratakv::DB::Open(options, db_path);
  if (!open_status.ok()) {
    std::cerr << "open failed: " << open_status << '\n';
    return 1;
  }

  const auto write_start = Clock::now();
  for (std::size_t i = 0; i < operations; ++i) {
    stratakv::Status status =
        db->Put(stratakv::WriteOptions{}, KeyFor(i), ValueFor(i));
    if (!status.ok()) {
      std::cerr << "put failed: " << status << '\n';
      return 1;
    }
  }
  const auto write_duration = Clock::now() - write_start;

  db.reset();
  std::tie(db, open_status) = stratakv::DB::Open(options, db_path);
  if (!open_status.ok()) {
    std::cerr << "reopen failed: " << open_status << '\n';
    return 1;
  }

  std::vector<std::size_t> order(operations);
  std::iota(order.begin(), order.end(), 0);
  std::mt19937_64 rng(42);
  std::shuffle(order.begin(), order.end(), rng);

  const auto read_pass = [&](std::vector<double>* latencies) {
    latencies->reserve(operations);
    const auto start = Clock::now();
    for (std::size_t index : order) {
      const auto op_start = Clock::now();
      auto [value, status] = db->Get(stratakv::ReadOptions{}, KeyFor(index));
      if (!status.ok() || value.empty()) {
        std::cerr << "get failed: " << status << '\n';
        return Clock::duration::zero();
      }
      latencies->push_back(std::chrono::duration<double, std::micro>(
                               Clock::now() - op_start).count());
    }
    return Clock::now() - start;
  };
  std::vector<double> cold_latencies_us;
  const auto cold_duration = read_pass(&cold_latencies_us);
  const auto cold_stats = db->GetBlockCacheStats();
  std::vector<double> warm_latencies_us;
  const auto warm_duration = read_pass(&warm_latencies_us);
  const auto warm_stats = db->GetBlockCacheStats();
  if (cold_duration == Clock::duration::zero() ||
      warm_duration == Clock::duration::zero()) return 1;

  const auto percentile = [](std::vector<double> latencies, double p) {
    std::sort(latencies.begin(), latencies.end());
    const std::size_t idx =
        std::min(latencies.size() - 1,
                 static_cast<std::size_t>(p * latencies.size()));
    return latencies[idx];
  };

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "StrataKV local benchmark\n";
  std::cout << "path: " << db_path << '\n';
  std::cout << "operations: " << operations << '\n';
  std::cout << "read source: reopened SSTables + final WAL tail\n";
  std::cout << "shared block cache: " << options.block_cache_size << " bytes\n";
  std::cout << "put throughput: "
            << operations / Seconds(write_duration) << " ops/sec\n";
  std::cout << "cold get throughput: "
            << operations / Seconds(cold_duration) << " ops/sec\n";
  std::cout << "cold get latency p50/p95/p99: "
            << percentile(cold_latencies_us, 0.50) << "/"
            << percentile(cold_latencies_us, 0.95) << "/"
            << percentile(cold_latencies_us, 0.99) << " us\n";
  std::cout << "warm get throughput: "
            << operations / Seconds(warm_duration) << " ops/sec\n";
  std::cout << "warm get latency p50/p95/p99: "
            << percentile(warm_latencies_us, 0.50) << "/"
            << percentile(warm_latencies_us, 0.95) << "/"
            << percentile(warm_latencies_us, 0.99) << " us\n";
  std::cout << "cache hits/misses/evictions: " << warm_stats.hits << "/"
            << warm_stats.misses << "/" << warm_stats.evictions << '\n';
  std::cout << "cache usage after cold pass: " << cold_stats.usage_bytes
            << " bytes\n";

  std::error_code ec;
  std::filesystem::remove_all(db_path, ec);
  return 0;
}
