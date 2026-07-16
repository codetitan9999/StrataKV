#include "manifest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace stratakv {
namespace {

enum class ManifestRecordType : std::uint8_t {
  kTableAdded = 1,
  kTableDeleted = 2,
};

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

Status ValidateTableMetadata(const TableMetadata& metadata) {
  if (metadata.file_number == 0) {
    return Status::InvalidArgument("manifest table file number must be nonzero");
  }
  if (metadata.smallest_key.empty() || metadata.largest_key.empty()) {
    return Status::InvalidArgument("manifest table key range must be nonempty");
  }
  if (metadata.smallest_key > metadata.largest_key) {
    return Status::InvalidArgument("manifest table key range is inverted");
  }
  if (metadata.entry_count == 0) {
    return Status::InvalidArgument("manifest table entry count must be nonzero");
  }
  return Status::OK();
}

Status ValidateFileNumber(std::uint64_t file_number) {
  if (file_number == 0) {
    return Status::InvalidArgument("manifest table file number must be nonzero");
  }
  return Status::OK();
}

Status EncodeTableAdded(const TableMetadata& metadata, std::string* payload) {
  Status valid = ValidateTableMetadata(metadata);
  if (!valid.ok()) {
    return valid;
  }
  if (metadata.smallest_key.size() >
          std::numeric_limits<std::uint32_t>::max() ||
      metadata.largest_key.size() >
          std::numeric_limits<std::uint32_t>::max()) {
    return Status::InvalidArgument("manifest table key range is too large");
  }

  payload->clear();
  AppendFixed<std::uint8_t>(
      *payload, static_cast<std::uint8_t>(ManifestRecordType::kTableAdded));
  AppendFixed<std::uint64_t>(*payload, metadata.file_number);
  AppendFixed<std::uint64_t>(*payload, metadata.entry_count);
  AppendFixed<std::uint64_t>(*payload, metadata.file_size_bytes);
  AppendFixed<std::uint32_t>(
      *payload, static_cast<std::uint32_t>(metadata.smallest_key.size()));
  AppendFixed<std::uint32_t>(
      *payload, static_cast<std::uint32_t>(metadata.largest_key.size()));
  payload->append(metadata.smallest_key);
  payload->append(metadata.largest_key);
  return Status::OK();
}

Status EncodeTableDeleted(std::uint64_t file_number, std::string* payload) {
  Status valid = ValidateFileNumber(file_number);
  if (!valid.ok()) {
    return valid;
  }

  payload->clear();
  AppendFixed<std::uint8_t>(
      *payload, static_cast<std::uint8_t>(ManifestRecordType::kTableDeleted));
  AppendFixed<std::uint64_t>(*payload, file_number);
  return Status::OK();
}

Status DecodeRecord(std::string_view payload, ManifestEdit* edit) {
  std::size_t offset = 0;
  std::uint8_t type = 0;

  if (!ReadFixed(payload, &offset, &type)) {
    return Status::Corruption("short manifest record");
  }

  if (type == static_cast<std::uint8_t>(ManifestRecordType::kTableDeleted)) {
    std::uint64_t file_number = 0;
    if (!ReadFixed(payload, &offset, &file_number)) {
      return Status::Corruption("short manifest table deletion record");
    }
    if (offset != payload.size()) {
      return Status::Corruption("manifest deletion record length mismatch");
    }

    Status valid = ValidateFileNumber(file_number);
    if (!valid.ok()) {
      return valid;
    }
    edit->type = ManifestEditType::kTableDeleted;
    edit->file_number = file_number;
    return Status::OK();
  }

  if (type != static_cast<std::uint8_t>(ManifestRecordType::kTableAdded)) {
    return Status::Corruption("invalid manifest record type");
  }

  std::uint64_t file_number = 0;
  std::uint64_t entry_count = 0;
  std::uint64_t file_size_bytes = 0;
  std::uint32_t smallest_key_size = 0;
  std::uint32_t largest_key_size = 0;

  if (!ReadFixed(payload, &offset, &file_number) ||
      !ReadFixed(payload, &offset, &entry_count) ||
      !ReadFixed(payload, &offset, &file_size_bytes) ||
      !ReadFixed(payload, &offset, &smallest_key_size) ||
      !ReadFixed(payload, &offset, &largest_key_size)) {
    return Status::Corruption("short manifest record");
  }

  const std::uint64_t expected_size =
      static_cast<std::uint64_t>(offset) + smallest_key_size + largest_key_size;
  if (expected_size != payload.size()) {
    return Status::Corruption("manifest record length mismatch");
  }

  edit->type = ManifestEditType::kTableAdded;
  edit->file_number = file_number;
  edit->table.file_number = file_number;
  edit->table.entry_count = entry_count;
  edit->table.file_size_bytes = file_size_bytes;
  edit->table.smallest_key.assign(payload.substr(offset, smallest_key_size));
  offset += smallest_key_size;
  edit->table.largest_key.assign(payload.substr(offset, largest_key_size));
  return ValidateTableMetadata(edit->table);
}

}  // namespace

ManifestWriter::ManifestWriter(std::filesystem::path path)
    : path_(std::move(path)) {}

Status ManifestWriter::Open(bool append) {
  const auto mode = std::ios::binary | std::ios::out |
                    (append ? std::ios::app : std::ios::trunc);
  stream_.open(path_, mode);
  if (!stream_) {
    return Status::IOError("failed to open manifest for writing: " +
                           path_.string());
  }
  return Status::OK();
}

Status ManifestWriter::AppendTable(const TableMetadata& metadata) {
  std::string payload;
  Status encode_status = EncodeTableAdded(metadata, &payload);
  if (!encode_status.ok()) {
    return encode_status;
  }
  if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Status::InvalidArgument("manifest record is too large");
  }

  std::string header;
  AppendFixed<std::uint32_t>(header,
                             static_cast<std::uint32_t>(payload.size()));
  AppendFixed<std::uint32_t>(header, Checksum(payload));

  stream_.write(header.data(), static_cast<std::streamsize>(header.size()));
  stream_.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  if (!stream_) {
    return Status::IOError("failed to append manifest record");
  }

  return Status::OK();
}

Status ManifestWriter::DeleteTable(std::uint64_t file_number) {
  std::string payload;
  Status encode_status = EncodeTableDeleted(file_number, &payload);
  if (!encode_status.ok()) {
    return encode_status;
  }

  std::string header;
  AppendFixed<std::uint32_t>(header,
                             static_cast<std::uint32_t>(payload.size()));
  AppendFixed<std::uint32_t>(header, Checksum(payload));

  stream_.write(header.data(), static_cast<std::streamsize>(header.size()));
  stream_.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  if (!stream_) {
    return Status::IOError("failed to append manifest deletion record");
  }

  return Status::OK();
}

Status ManifestWriter::Sync() {
  stream_.flush();
  if (!stream_) {
    return Status::IOError("failed to flush manifest");
  }
  return Status::OK();
}

ManifestReader::ManifestReader(std::filesystem::path path)
    : path_(std::move(path)) {}

Status ManifestReader::Replay(
    const std::function<Status(const ManifestEdit&)>& apply) const {
  if (!std::filesystem::exists(path_)) {
    return Status::OK();
  }

  std::ifstream stream(path_, std::ios::binary);
  if (!stream) {
    return Status::IOError("failed to open manifest for reading: " +
                           path_.string());
  }

  while (true) {
    std::array<char, 8> header{};
    stream.read(header.data(), static_cast<std::streamsize>(header.size()));

    if (stream.gcount() == 0 && stream.eof()) {
      return Status::OK();
    }

    if (stream.gcount() != static_cast<std::streamsize>(header.size())) {
      return Status::Corruption("truncated manifest record header");
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
      return Status::Corruption("truncated manifest record payload");
    }

    if (Checksum(payload) != expected_checksum) {
      return Status::Corruption("manifest checksum mismatch");
    }

    ManifestEdit edit;
    Status decode_status = DecodeRecord(payload, &edit);
    if (!decode_status.ok()) {
      return decode_status;
    }

    Status apply_status = apply(edit);
    if (!apply_status.ok()) {
      return apply_status;
    }
  }
}

}  // namespace stratakv
