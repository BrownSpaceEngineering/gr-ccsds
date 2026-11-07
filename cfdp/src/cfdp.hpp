#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

// CCSDS 727.0-B-5. Enum values are the wire codes
enum class PDU_Type : uint8_t { FileDirective = 0, FileData = 1 };
enum class PDU_Direction : uint8_t { ToFileReceiver = 0, ToFileSender = 1 };
enum class PDU_Transmission_Mode : uint8_t {
  Acknowledged = 0,
  Unacknowledged = 1
};
enum class PDU_CRC_Flag : uint8_t { NotPresent = 0, Present = 1 };
enum class PDU_Large_File_Flag : uint8_t { SmallFile = 0, LargeFile = 1 };
enum class PDU_Segmentation_Control : uint8_t {
  BoundariesNotPreserved = 0,
  BoundariesPreserved = 1
};
enum class PDU_Segment_Metadata_Flag : uint8_t { NotPresent = 0, Present = 1 };

struct PDU_Header {
  uint8_t version = 1;
  PDU_Type type = PDU_Type::FileDirective;
  PDU_Direction direction = PDU_Direction::ToFileReceiver;
  PDU_Transmission_Mode transmissionMode = PDU_Transmission_Mode::Acknowledged;
  PDU_CRC_Flag crcFlag = PDU_CRC_Flag::NotPresent;
  PDU_Large_File_Flag largeFileFlag = PDU_Large_File_Flag::SmallFile;
  uint16_t fieldLength = 0;
  PDU_Segmentation_Control segmentationControl =
      PDU_Segmentation_Control::BoundariesNotPreserved;
  uint8_t entityLength = 1;
  PDU_Segment_Metadata_Flag segmentMetadataFlag =
      PDU_Segment_Metadata_Flag::NotPresent;
  uint8_t transactionSequenceNumberLength = 1;
  uint64_t sourceEntityID = 0;
  uint64_t transactionSequenceNumber = 0;
  uint64_t destinationEntityID = 0;
};

enum class TLV_Type : uint8_t {
  FilestoreRequest = 0x00,
  FilestoreResponse = 0x01,
  MessageToUser = 0x02,
  FaultHandlerOverrides = 0x04,
  FlowLabel = 0x05,
  EntityID = 0x06
};

// TLVs are used in Metadata options, Finished responses, and fault locations
struct TLV {
  TLV_Type type = TLV_Type::MessageToUser;
  std::vector<uint8_t> value;

  static TLV message_to_user(std::vector<uint8_t> message);
  static TLV flow_label(std::vector<uint8_t> label);
  static TLV entity_id(uint64_t id, uint8_t length);
};

struct Metadata_PDU {
  bool closureRequested = false;
  uint8_t checksumType =
      0;                 // Four-bit SANA checksum ID; zero is modular checksum.
  uint64_t fileSize = 0; // Zero for an unbounded file.
  std::string sourceFileName;
  std::string destinationFileName;
  std::vector<TLV> options;
};

enum class Record_Continuation_State : uint8_t {
  NoBoundary = 0,
  Start = 1,
  End = 2,
  Complete = 3
};

struct FileData_PDU {
  uint64_t offset = 0;
  std::vector<uint8_t> data;
  Record_Continuation_State recordContinuationState =
      Record_Continuation_State::NoBoundary;
  std::vector<uint8_t> segmentMetadata; // Up to 63 octets.
};

enum class Condition_Code : uint8_t {
  NoError = 0,
  AckLimitReached = 1,
  KeepAliveLimitReached = 2,
  InvalidTransmissionMode = 3,
  FilestoreRejection = 4,
  FileChecksumFailure = 5,
  FileSizeError = 6,
  NakLimitReached = 7,
  InactivityDetected = 8,
  InvalidFileStructure = 9,
  CheckLimitReached = 10,
  UnsupportedChecksumType = 11,
  SuspendRequested = 14,
  CancelRequested = 15
};

enum class Filestore_Action : uint8_t {
  CreateFile = 0,
  DeleteFile = 1,
  RenameFile = 2,
  AppendFile = 3,
  ReplaceFile = 4,
  CreateDirectory = 5,
  RemoveDirectory = 6,
  DenyFile = 7,
  DenyDirectory = 8
};

enum class Fault_Handler : uint8_t {
  Cancel = 1,
  Suspend = 2,
  Ignore = 3,
  Abandon = 4
};

// Helpers encode standard TLV values, while TLV also supports raw values.
TLV filestore_request(Filestore_Action action, std::string firstName,
                      std::optional<std::string> secondName = std::nullopt);
TLV filestore_response(Filestore_Action action, uint8_t status,
                       std::string firstName,
                       std::optional<std::string> secondName,
                       std::string message = {});
TLV fault_handler_override(Condition_Code condition, Fault_Handler handler);

struct EOF_PDU {
  Condition_Code condition = Condition_Code::NoError;
  uint32_t fileChecksum = 0;
  uint64_t fileSize = 0;
  std::optional<uint64_t> faultLocation; // Required for a nonzero condition.
};

enum class Delivery_Code : uint8_t { Complete = 0, Incomplete = 1 };
enum class File_Status : uint8_t {
  DiscardedDeliberately = 0,
  DiscardedByFilestore = 1,
  Retained = 2,
  Unreported = 3
};

struct Finished_PDU {
  Condition_Code condition = Condition_Code::NoError;
  Delivery_Code delivery = Delivery_Code::Complete;
  File_Status fileStatus = File_Status::Unreported;
  std::vector<TLV> filestoreResponses;
  std::optional<uint64_t> faultLocation; // Required for a nonzero condition.
};

enum class Acknowledged_Directive : uint8_t {
  EndOfFile = 0x04,
  Finished = 0x05
};
enum class Transaction_Status : uint8_t {
  Undefined = 0,
  Active = 1,
  Terminated = 2,
  Unrecognized = 3
};

struct ACK_PDU {
  Acknowledged_Directive directive = Acknowledged_Directive::EndOfFile;
  Condition_Code condition = Condition_Code::NoError;
  Transaction_Status transactionStatus = Transaction_Status::Undefined;
};

struct Segment_Request {
  uint64_t startOffset = 0;
  uint64_t endOffset = 0; // Exclusive.
};

struct NAK_PDU {
  uint64_t startOfScope = 0;
  uint64_t endOfScope = 0;
  std::vector<Segment_Request> segmentRequests;
};

enum class Prompt_Type : uint8_t { NAK = 0, KeepAlive = 1 };
struct Prompt_PDU {
  Prompt_Type response = Prompt_Type::NAK;
};
struct KeepAlive_PDU {
  uint64_t progress = 0;
};

using PDU_Body = std::variant<Metadata_PDU, FileData_PDU, EOF_PDU, Finished_PDU,
                              ACK_PDU, NAK_PDU, Prompt_PDU, KeepAlive_PDU>;

struct CFDP_PDU {
  PDU_Header header;
  PDU_Body body;

  // Serializes a complete PDU; rejects fields that cannot fit their wire
  // widths.
  std::vector<uint8_t> to_bytes() const;
  static CFDP_PDU from_bytes(const std::vector<uint8_t> &bytes);
};

class PDU_CRC_Error : public std::invalid_argument {
public:
  PDU_CRC_Error() : std::invalid_argument("CFDP CRC mismatch") {}
};

// CCSDS 727.0-B-5 section 4.2.5. Supports a final partial word.
uint32_t modular_checksum(const std::vector<uint8_t> &bytes);
bool checksum_type_supported(uint8_t type);
uint32_t file_checksum(const std::vector<uint8_t> &bytes, uint8_t type);
