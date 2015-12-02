#include <arpa/inet.h>
#include <boost/crc.hpp>
#include <glog/logging.h>
#include "protocol.h"

namespace ldd {
namespace net {

Header::Parser::Parser(const char* buf)
    : buf_(buf) {
    CHECK_NOTNULL(buf_);
}

bool Header::Parser::IsValid() const
{
    uint8_t byte = buf_[0];
    if (byte != kMagic) {
        LOG(ERROR) << "Parsed invliad header magic: " << (int)byte;
        return false;
    }

    boost::crc_ccitt_type result;
    result.process_bytes(buf_, 14);
    uint16_t crc16_new = result.checksum();
    uint16_t crc16_origin = ntohs(
        *reinterpret_cast<const uint16_t *>(&buf_[kByteSize - 2]));

    if (crc16_new != crc16_origin) {
        LOG(ERROR) << "invalid header, wrong crc16 ";
        return false;
    }

    return true;
}

uint8_t Header::Parser::type() const {
    return buf_[1];
}

uint32_t Header::Parser::id() const {
    return ntohl(*reinterpret_cast<const uint32_t*>(buf_ + 2));
}

uint16_t Header::Parser::body_type() const {
    return ntohs(*reinterpret_cast<const uint16_t*>(buf_ + 6));
}

uint32_t Header::Parser::body_size() const {
    return ntohl(*reinterpret_cast<const uint32_t*>(buf_ + 8)) >> 8;
}

uint8_t Header::Parser::ext_count() const {
    return *reinterpret_cast<const uint8_t*>(buf_ + 11);
}

uint16_t Header::Parser::ext_len() const {
    return ntohs(*reinterpret_cast<const uint16_t*>(buf_ + 12));
}


Header::Builder::Builder(char* buf)
    : buf_(buf) {
    CHECK_NOTNULL(buf_);
}

void Header::Builder::Build() {
    buf_[0] = kMagic;
    boost::crc_ccitt_type result;
    result.process_bytes(buf_, 14);
    uint16_t crc16 = result.checksum();
    *reinterpret_cast<uint16_t*>(&buf_[kByteSize - 2]) = htons(crc16);
}

void Header::Builder::set_type(uint8_t type) {
    buf_[1] = static_cast<char>(type);
}

void Header::Builder::set_id(uint32_t id) {
    *reinterpret_cast<uint32_t*>(buf_ + 2) = htonl(id);
}

void Header::Builder::set_body_type(uint16_t body_type) {
    *reinterpret_cast<uint16_t*>(buf_ + 6) = htons(body_type);
}

void Header::Builder::set_body_size(uint32_t body_size) {
    uint8_t x = *reinterpret_cast<const uint8_t*>(buf_ + 11);
    *reinterpret_cast<uint32_t*>(buf_ + 8) = htonl(body_size << 8);
    *reinterpret_cast<uint8_t*>(buf_ + 11) = x;
}

void Header::Builder::set_ext_count(uint8_t ext_count) {
    *reinterpret_cast<uint8_t*>(buf_ + 11) = ext_count;
}

void Header::Builder::set_ext_len(uint16_t ext_len) {
    *reinterpret_cast<uint16_t*>(buf_ + 12) = htons(ext_len);
}


ExtHeader::Parser::Parser(uint8_t ext_count, const char* buf) 
    : ext_count_(ext_count), ext_buf_(buf)
{
}

bool ExtHeader::Parser::IsValid(uint16_t ext_len) const
{
    uint16_t len = 0;

    for (int i = 0; i < (int)ext_count_; i++) {
        int pos = i * kUnitSize;
        len += static_cast<uint8_t>(*(ext_buf_ + pos + 1));
    }

    if (len != ext_len) {
        LOG(ERROR) << "invalid extras length, expected len=" 
            << ext_len << ", but actual len=" << len;
        return false;
    }

    return true;
}

bool ExtHeader::Parser::GetExtItem(int index, uint8_t *type, uint8_t *len)
{
    if (index > ext_count_) {
        LOG(WARNING) << "invalid extras index: " << index;
        return false;
    }

    int pos = index * kUnitSize;
    *type = static_cast<uint8_t>(*(ext_buf_ + pos));
    *len = static_cast<uint8_t>(*(ext_buf_ + pos + 1));

    return true;
}

ExtHeader::Builder::Builder(char* buf) 
    : ext_count_(0), ext_buf_(buf)
{
}

void ExtHeader::Builder::AddExtItem(uint8_t type, uint8_t len)
{
    int pos = ext_count_ * kUnitSize;
    *(ext_buf_ + pos) = static_cast<char>(type);
    *(ext_buf_ + pos + 1) = static_cast<char>(len);
    ext_count_++;

    return;
}

} // namespace net
} // namespace ldd

