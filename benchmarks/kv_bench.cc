#include "stratakv/db.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
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
  std::ostringstream key;
  key << "key-" << std::setw(20) << std::setfill('0') << i;
  return key.str();
}

std::string ValueFor(std::size_t i) {
  std::string value = "value-";
  value += std::to_string(i);
  value.append(96, 'x');
  return value;
}

std::size_t CountSSTables(const std::filesystem::path& db_path) {
  std::size_t count = 0;
  for (const auto& entry :
       std::filesystem::directory_iterator(db_path / "sst")) {
    if (entry.path().extension() == ".sst") ++count;
  }
  return count;
}

std::uintmax_t SSTableBytes(const std::filesystem::path& db_path) {
  std::uintmax_t bytes = 0;
  for (const auto& entry :
       std::filesystem::directory_iterator(db_path / "sst")) {
    if (entry.path().extension() == ".sst") bytes += entry.file_size();
  }
  return bytes;
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
  options.level0_compaction_trigger = 4;
  options.level1_compaction_trigger_bytes = 128 * 1024;
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
  const auto compaction_stats = db->GetCompactionStats();

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

  const auto negative_start = Clock::now();
  for (std::size_t index : order) {
    auto [value, status] =
        db->Get(stratakv::ReadOptions{}, KeyFor(index) + "-missing");
    if (status.code() != stratakv::Status::Code::kNotFound || !value.empty()) {
      std::cerr << "negative get failed: " << status << '\n';
      return 1;
    }
  }
  const auto negative_duration = Clock::now() - negative_start;
  const auto negative_stats = db->GetBlockCacheStats();

  std::size_t scanned = 0;
  const auto scan_start = Clock::now();
  auto iterator = db->NewIterator(stratakv::ReadOptions{});
  for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) ++scanned;
  const auto scan_duration = Clock::now() - scan_start;
  if (!iterator->status().ok() || scanned != operations) {
    std::cerr << "scan failed: " << iterator->status() << '\n';
    return 1;
  }

  const std::size_t range_begin = operations / 3;
  const std::size_t range_count =
      std::min<std::size_t>(1000, operations - range_begin);
  stratakv::ReadOptions range_options;
  range_options.lower_bound = KeyFor(range_begin);
  if (range_begin + range_count < operations) {
    range_options.upper_bound = KeyFor(range_begin + range_count);
  }
  std::size_t range_scanned = 0;
  const auto range_start = Clock::now();
  iterator = db->NewIterator(range_options);
  for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
    ++range_scanned;
  }
  const auto range_duration = Clock::now() - range_start;
  if (!iterator->status().ok() || range_scanned != range_count) {
    std::cerr << "range scan failed: " << iterator->status() << '\n';
    return 1;
  }

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
  std::cout << "SSTable point path: Bloom-filtered restart-indexed blocks\n";
  std::cout << "scan merge sources: " << CountSSTables(db_path) + 1 << '\n';
  std::cout << "SSTable bytes: " << SSTableBytes(db_path) << '\n';
  std::cout << "shared block cache: " << options.block_cache_size << " bytes\n";
  std::cout << "level-1 compaction target: "
            << options.level1_compaction_trigger_bytes << " bytes\n";
  std::uint64_t logical_write_bytes = 0;
  for (std::size_t i = 0; i < operations; ++i) {
    logical_write_bytes += KeyFor(i).size() + ValueFor(i).size();
  }
  const double compaction_write_amplification =
      logical_write_bytes == 0
          ? 0.0
          : static_cast<double>(compaction_stats.bytes_written) /
                static_cast<double>(logical_write_bytes);
  std::cout << "compaction jobs/input files/output files: "
            << compaction_stats.jobs << "/" << compaction_stats.input_files
            << "/" << compaction_stats.output_files << '\n';
  std::cout << "compaction bytes read/written: "
            << compaction_stats.bytes_read << "/"
            << compaction_stats.bytes_written << '\n';
  std::cout << "compaction elapsed: "
            << compaction_stats.elapsed_nanoseconds / 1000000.0 << " ms\n";
  std::cout << "compaction write amplification: "
            << compaction_write_amplification << "x\n";
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
  std::cout << "negative get throughput: "
            << operations / Seconds(negative_duration) << " ops/sec\n";
  std::cout << "negative-pass cache misses: "
            << negative_stats.misses - warm_stats.misses << '\n';
  std::cout << "full scan throughput: "
            << scanned / Seconds(scan_duration) << " entries/sec\n";
  std::cout << "bounded scan throughput (" << range_scanned << " entries): "
            << range_scanned / Seconds(range_duration) << " entries/sec\n";
  std::cout << "cache hits/misses/evictions: " << warm_stats.hits << "/"
            << warm_stats.misses << "/" << warm_stats.evictions << '\n';
  std::cout << "cache usage after cold pass: " << cold_stats.usage_bytes
            << " bytes\n";

  std::error_code ec;
  std::filesystem::remove_all(db_path, ec);
  return 0;
}
