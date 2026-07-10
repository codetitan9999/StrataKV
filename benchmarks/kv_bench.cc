#include "stratakv/db.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
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
  auto [db, open_status] = stratakv::DB::Open(stratakv::Options{}, db_path);
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

  std::vector<std::size_t> order(operations);
  std::iota(order.begin(), order.end(), 0);
  std::mt19937_64 rng(42);
  std::shuffle(order.begin(), order.end(), rng);

  std::vector<double> get_latencies_us;
  get_latencies_us.reserve(operations);

  const auto read_start = Clock::now();
  for (std::size_t index : order) {
    const auto op_start = Clock::now();
    auto [value, status] = db->Get(stratakv::ReadOptions{}, KeyFor(index));
    const auto op_duration = Clock::now() - op_start;
    if (!status.ok() || value.empty()) {
      std::cerr << "get failed: " << status << '\n';
      return 1;
    }
    get_latencies_us.push_back(std::chrono::duration<double, std::micro>(
                                   op_duration)
                                   .count());
  }
  const auto read_duration = Clock::now() - read_start;

  std::sort(get_latencies_us.begin(), get_latencies_us.end());
  const auto percentile = [&](double p) {
    const std::size_t idx =
        std::min(get_latencies_us.size() - 1,
                 static_cast<std::size_t>(p * get_latencies_us.size()));
    return get_latencies_us[idx];
  };

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "StrataKV local benchmark\n";
  std::cout << "path: " << db_path << '\n';
  std::cout << "operations: " << operations << '\n';
  std::cout << "put throughput: "
            << operations / Seconds(write_duration) << " ops/sec\n";
  std::cout << "get throughput: "
            << operations / Seconds(read_duration) << " ops/sec\n";
  std::cout << "get latency p50: " << percentile(0.50) << " us\n";
  std::cout << "get latency p95: " << percentile(0.95) << " us\n";
  std::cout << "get latency p99: " << percentile(0.99) << " us\n";

  std::error_code ec;
  std::filesystem::remove_all(db_path, ec);
  return 0;
}
