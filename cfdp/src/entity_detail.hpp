#pragma once

#include "cfdp.hpp"

namespace cfdp {

inline bool acknowledged(const PDU_Header &header) {
  return header.transmissionMode == PDU_Transmission_Mode::Acknowledged;
}

inline PDU_Header reverse(PDU_Header header) {
  header.direction = PDU_Direction::ToFileSender;
  return header;
}

} // namespace cfdp
