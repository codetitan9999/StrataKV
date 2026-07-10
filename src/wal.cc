#include "wal.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

namespace stratakv {
namespace {

template <typename UInt>
void AppendFixed(std::string& out, UInt value) {
  static_assert(std::is_unsigned_v<UInt>);
  for (std::size_t i = 0; i < sizeof(UInt); ++i) {
    out.push_back(static_cast<char>((value >> (8 * i)) & 0xffU));
  }
}

template <typename UInt>
bool ReadFixed(std::string_view input, std::size_t* offset, UInt* value) {
  static_assert(std::is_unsigned_v<UInt>);
  if (*offset + sizeof(UInt) > input.size()) {
    return false;
  }

  UInt result = 0;
  for (std::size_t i = 0; i < sizeof(UInt); ++i) {
    result |= static_cast<UInt>(
                  static_cast<unsigned char>(input[*offset + i]))
              << (8 * i);
  }

  *offset += sizeof(UInt);
  *value = result;
  return true;
}

std::uint32_t Checksum(std::string_view payload) {
  std::uint32_t hash = 2166136261u;
  for (const unsigned char byte : payload) {
    hash ^= byte;
    hash *= 16777619u;
  }
  return hash;
}

std::string EncodeRecord(const LogRecord& record) {
  std::string payload;
  payload.reserve(1 + sizeof(record.sequence) + 8 + record.key.size() +
                  record.value.size());

  AppendFixed<std::uint8_t>(payload, static_cast<std::uint8_t>(record.type));
  AppendFixed<std::uint64_t>(payload, record.sequence);
  AppendFixed<std::uint32_t>(payload,
                             static_cast<std::uint32_t>(record.key.size()));
  AppendFixed<std::uint32_t>(payload,
                             static_cast<std::uint32_t>(record.value.size()));
  payload.append(record.key);
  payload.append(record.value);
  return payload;
}

Status DecodeRecord(std::string_view payload, LogRecord* record) {
  std::size_t offset = 0;

  std::uint8_t type = 0;
  std::uint64_t sequence = 0;
  std::uint32_t key_size = 0;
  std::uint32_t value_size = 0;

  if (!ReadFixed(payload, &offset, &type) ||
      !ReadFixed(payload, &offset, &sequence) ||
      !ReadFixed(payload, &offset, &key_size) ||
      !ReadFixed(payload, &offset, &value_size)) {
    return Status::Corruption("short WAL record header");
  }

  if (type != static_cast<std::uint8_t>(RecordType::kPut) &&
      type != static_cast<std::uint8_t>(RecordType::kDelete)) {
    return Status::Corruption("invalid WAL record type");
  }

  const std::uint64_t total_size =
      static_cast<std::uint64_t>(offset) + key_size + value_size;
  if (total_size != payload.size()) {
    return Status::Corruption("WAL record length mismatch");
  }

  record->type = static_cast<RecordType>(type);
  record->sequence = sequence;
  record->key.assign(payload.substr(offset, key_size));
  offset += key_size;
  record->value.assign(payload.substr(offset, value_size));
  return Status::OK();
}

}  // namespace

WalWriter::WalWriter(std::filesystem::path path) : path_(std::move(path)) {}

WalWriter::~WalWriter() = default;

Status WalWriter::Open(bool append) {
  const auto mode = std::ios::binary | std::ios::out |
                    (append ? std::ios::app : std::ios::trunc);
  stream_.open(path_, mode);
  if (!stream_) {
    return Status::IOError("failed to open WAL for writing: " + path_.string());
  }
  return Status::OK();
}

Status WalWriter::Append(const LogRecord& record) {
  if (record.key.size() > std::numeric_limits<std::uint32_t>::max() ||
      record.value.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Status::InvalidArgument("WAL record key/value is too large");
  }

  const std::string payload = EncodeRecord(record);

  std::string header;
  header.reserve(8);
  AppendFixed<std::uint32_t>(header,
                             static_cast<std::uint32_t>(payload.size()));
  AppendFixed<std::uint32_t>(header, Checksum(payload));

  stream_.write(header.data(), static_cast<std::streamsize>(header.size()));
  stream_.write(payload.data(), static_cast<std::streamsize>(payload.size()));

  if (!stream_) {
    return Status::IOError("failed to append WAL record");
  }

  return Status::OK();
}

Status WalWriter::Sync() {
  stream_.flush();
  if (!stream_) {
    return Status::IOError("failed to flush WAL");
  }
  return Status::OK();
}

WalReader::WalReader(std::filesystem::path path) : path_(std::move(path)) {}

Status WalReader::Replay(
    const std::function<Status(const LogRecord&)>& apply) const {
  std::ifstream stream(path_, std::ios::binary);
  if (!stream) {
    return Status::IOError("failed to open WAL for reading: " + path_.string());
  }

  while (true) {
    std::array<char, 8> header{};
    stream.read(header.data(), static_cast<std::streamsize>(header.size()));

    if (stream.gcount() == 0 && stream.eof()) {
      return Status::OK();
    }

    if (stream.gcount() != static_cast<std::streamsize>(header.size())) {
      return Status::Corruption("truncated WAL record header");
    }

    std::size_t header_offset = 0;
    std::uint32_t payload_size = 0;
    std::uint32_t expected_checksum = 0;
    const std::string_view header_view(header.data(), header.size());
    ReadFixed(header_view, &header_offset, &payload_size);
    ReadFixed(header_view, &header_offset, &expected_checksum);

    std::string payload(payload_size, '\0');
    stream.read(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (stream.gcount() != static_cast<std::streamsize>(payload.size())) {
      return Status::Corruption("truncated WAL record payload");
    }

    if (Checksum(payload) != expected_checksum) {
      return Status::Corruption("WAL checksum mismatch");
    }

    LogRecord record;
    Status decode_status = DecodeRecord(payload, &record);
    if (!decode_status.ok()) {
      return decode_status;
    }

    Status apply_status = apply(record);
    if (!apply_status.ok()) {
      return apply_status;
    }
  }
}

}  // namespace stratakv
