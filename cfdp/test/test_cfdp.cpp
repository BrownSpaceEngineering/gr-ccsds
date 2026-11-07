#include "cfdp.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using Bytes = std::vector<uint8_t>;

bool check(const Bytes &actual, const Bytes &expected, const char *name) {
  if (actual != expected) {
    std::cerr << name << ": unexpected serialized bytes\n";
    return false;
  }
  try {
    if (CFDP_PDU::from_bytes(expected).to_bytes() == expected)
      return true;
  } catch (const std::exception &e) {
    std::cerr << name << ": decoder rejected expected bytes: " << e.what()
              << '\n';
    return false;
  }
  std::cerr << name << ": decode/encode round trip changed bytes\n";
  return false;
}

template <typename F> bool rejects(F &&f, const char *name) {
  try {
    f();
  } catch (const std::invalid_argument &) {
    return true;
  } catch (const std::length_error &) {
    return true;
  }
  std::cerr << name << ": invalid value accepted\n";
  return false;
}

int main() {
  bool ok = true;
  CFDP_PDU packet;
  Metadata_PDU metadata;
  metadata.closureRequested = true;
  metadata.checksumType = 2;
  metadata.fileSize = 3;
  metadata.sourceFileName = "a";
  metadata.destinationFileName = "b";
  metadata.options.push_back(TLV::message_to_user({0xde, 0xad}));
  packet.header.transmissionMode = PDU_Transmission_Mode::Unacknowledged;
  packet.body = metadata;
  ok &= check(packet.to_bytes(),
              {0x24, 0, 14, 0,   0, 0,   0,    0x07, 0x42, 0,   0,
               0,    3, 1,  'a', 1, 'b', 0x02, 2,    0xde, 0xad},
              "metadata");

  packet.header.transmissionMode = PDU_Transmission_Mode::Acknowledged;
  packet.header.entityLength = 2;
  packet.header.transactionSequenceNumberLength = 2;
  packet.header.sourceEntityID = 0x1234;
  packet.header.transactionSequenceNumber = 0x5678;
  packet.header.destinationEntityID = 0xabcd;
  packet.header.segmentationControl =
      PDU_Segmentation_Control::BoundariesPreserved;
  FileData_PDU data;
  data.offset = 0x01020304;
  data.recordContinuationState = Record_Continuation_State::Start;
  data.segmentMetadata = {0xaa};
  data.data = {0xde, 0xad};
  packet.body = data;
  ok &= check(packet.to_bytes(),
              {0x30, 0, 8, 0x99, 0x12, 0x34, 0x56, 0x78, 0xab, 0xcd, 0x41, 0xaa,
               1, 2, 3, 4, 0xde, 0xad},
              "file data");

  packet.header = {};
  packet.body = EOF_PDU{Condition_Code::NoError, 0x12345678, 5, {}};
  ok &=
      check(packet.to_bytes(),
            {0x20, 0, 10, 0, 0, 0, 0, 4, 0, 0x12, 0x34, 0x56, 0x78, 0, 0, 0, 5},
            "EOF");

  packet.header.largeFileFlag = PDU_Large_File_Flag::LargeFile;
  packet.body = EOF_PDU{Condition_Code::FileSizeError, 0, 0x100000000ULL, 2};
  ok &= check(packet.to_bytes(), {0x21, 0, 17, 0, 0, 0, 0, 4, 0x60, 0, 0, 0,
                                  0,    0, 0,  0, 1, 0, 0, 0, 0,    6, 1, 2},
              "large EOF");

  packet.header = {};
  Finished_PDU finished;
  finished.condition = Condition_Code::FilestoreRejection;
  finished.delivery = Delivery_Code::Incomplete;
  finished.fileStatus = File_Status::DiscardedByFilestore;
  finished.faultLocation = 2;
  finished.filestoreResponses.push_back(
      filestore_response(Filestore_Action::DeleteFile, 1, "a", {}, "x"));
  packet.body = finished;
  ok &= check(
      packet.to_bytes(),
      {0x20, 0, 12, 0, 0, 0, 0, 5, 0x45, 1, 5, 0x11, 1, 'a', 1, 'x', 6, 1, 2},
      "finished");

  packet.body = ACK_PDU{Acknowledged_Directive::Finished,
                        Condition_Code::NoError, Transaction_Status::Active};
  ok &= check(packet.to_bytes(), {0x20, 0, 3, 0, 0, 0, 0, 6, 0x51, 1}, "ACK");

  NAK_PDU nak;
  nak.startOfScope = 1;
  nak.endOfScope = 9;
  nak.segmentRequests.push_back({2, 4});
  packet.body = nak;
  ok &= check(packet.to_bytes(), {0x20, 0, 17, 0, 0, 0, 0, 8, 0, 0, 0, 1,
                                  0,    0, 0,  9, 0, 0, 0, 2, 0, 0, 0, 4},
              "NAK");

  packet.body = Prompt_PDU{Prompt_Type::KeepAlive};
  ok &= check(packet.to_bytes(), {0x20, 0, 2, 0, 0, 0, 0, 9, 0x80}, "prompt");
  packet.header.crcFlag = PDU_CRC_Flag::Present;
  ok &= check(packet.to_bytes(), {0x22, 0, 4, 0, 0, 0, 0, 9, 0x80, 0x93, 0x3f},
              "prompt with CRC");
  packet.header.crcFlag = PDU_CRC_Flag::NotPresent;
  packet.body = KeepAlive_PDU{0x12345678};
  ok &= check(packet.to_bytes(),
              {0x20, 0, 5, 0, 0, 0, 0, 0x0c, 0x12, 0x34, 0x56, 0x78},
              "keep alive");

  packet.header.entityLength = 1;
  packet.header.sourceEntityID = 256;
  ok &= rejects([&] { packet.to_bytes(); }, "entity ID width");
  packet.header = {};
  packet.body = Metadata_PDU{};
  std::get<Metadata_PDU>(packet.body).sourceFileName.assign(256, 'a');
  ok &= rejects([&] { packet.to_bytes(); }, "name length");
  packet.body = FileData_PDU{};
  std::get<FileData_PDU>(packet.body).data.resize(65536);
  ok &= rejects([&] { packet.to_bytes(); }, "PDU length");
  packet.body = EOF_PDU{Condition_Code::FileSizeError, 0, 0, {}};
  ok &= rejects([&] { packet.to_bytes(); }, "missing fault location");
  packet.body = Metadata_PDU{};
  std::get<Metadata_PDU>(packet.body).checksumType = 16;
  ok &= rejects([&] { packet.to_bytes(); }, "checksum type");
  std::get<Metadata_PDU>(packet.body).checksumType = 0;
  std::get<Metadata_PDU>(packet.body)
      .options.push_back({TLV_Type::FilestoreResponse, {}});
  ok &= rejects([&] { packet.to_bytes(); }, "invalid Metadata option");
  ok &= rejects([&] { filestore_request(Filestore_Action::RenameFile, "a"); },
                "filestore second name");

  if (ok)
    std::cout << "CFDP tests passed\n";
  return ok ? 0 : 1;
}
