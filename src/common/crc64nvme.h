/* Copyright (c) 2026 IBM Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/* CRC-64/NVME, the checksum S3 calls CRC64NVME, and its header forms.
 *
 * S3 renders the value as the base64 of its big-endian bytes, 12 characters:
 * "rosUhgp5mIg=" for the check string "123456789". Two headers carry it:
 *
 *   x-amz-checksum-crc64nvme: <base64>            the whole object
 *   x-amz-rdma-checksum-crc64nvme: <base64>       the bytes delivered
 *
 * The second is Ceph's: it covers exactly the bytes the server placed, so it
 * also checks a ranged GET, which S3's own checksum headers do not. */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace hipObj {

/* Continue a CRC-64/NVME over data. Start with 0; the result of one call is
 * the crc of the next, so crc64nvme(crc64nvme(0, a), b) is the CRC of a
 * followed by b. */
uint64_t crc64nvme(uint64_t crc, const void* data, size_t len);

/* The 12-character base64 of the big-endian value. */
std::string armorCrc64nvme(uint64_t crc);

/* Parse a header value: "CRC64NVME <base64>" or "<base64>", with optional
 * surrounding whitespace. Only the canonical 12-character form is accepted;
 * a multipart composite ("<base64>-<parts>") is not a checksum of bytes.
 * false for anything else. */
bool parseCrc64nvmeHeader(const std::string& value, uint64_t& crc);

} // namespace hipObj
