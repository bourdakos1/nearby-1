// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef CORE_INTERNAL_MEDIUMS_BLE_V2_UTILS_H_
#define CORE_INTERNAL_MEDIUMS_BLE_V2_UTILS_H_

#include <string>

#include "absl/strings/str_format.h"
#include "connections/implementation/mediums/ble_v2//ble_advertisement.h"
#include "connections/implementation/mediums/ble_v2/ble_advertisement_header.h"
#include "connections/implementation/mediums/ble_v2/ble_packet.h"
#include "connections/implementation/mediums/utils.h"
#include "connections/implementation/mediums/uuid.h"
#include "internal/platform/prng.h"

namespace location {
namespace nearby {
namespace connections {
namespace mediums {
namespace bleutils {

ABSL_CONST_INIT extern const absl::string_view kCopresenceServiceUuid;

// Return SHA256 hash.
//
// source - the string to be hashed.
// size   - size of returned byte array.
ByteArray GenerateHash(const std::string& source, size_t size);

// Return SHA256 hash of service ID.
//
// source  - service id.
// version - BleAdvertisement::Version. kV1 has been deprecated and just used
//           for testing.
ByteArray GenerateServiceIdHash(
    const std::string& service_id,
    BleAdvertisement::Version version = BleAdvertisement::Version::kV2);

// Returns device token generated by SHA256 hash in random uint32 and
// size of mediums::BleAdvertisement::kDeviceTokenLength
ByteArray GenerateDeviceToken();

// Return SHA256 hash of advertisement byte array.
//
// advertisement_bytes - advertisement byte array.
//  The generated hash size is determined by byte array size of input.
ByteArray GenerateAdvertisementHash(const ByteArray& advertisement_bytes);

// Generates a BLE characteristic UUID for an advertisement at the given slot.
//
// slot - the advertisement slot to generate a UUID for.
std::string GenerateAdvertisementUuid(int slot);

}  // namespace bleutils
}  // namespace mediums
}  // namespace connections
}  // namespace nearby
}  // namespace location

#endif  // CORE_INTERNAL_MEDIUMS_BLE_V2_UTILS_H_
