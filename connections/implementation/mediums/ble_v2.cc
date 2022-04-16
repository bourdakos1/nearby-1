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

#include "connections/implementation/mediums/ble_v2.h"

#include <memory>
#include <string>
#include <utility>

#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "connections/implementation/mediums/ble_v2/ble_advertisement.h"
#include "connections/implementation/mediums/ble_v2/ble_advertisement_header.h"
#include "connections/implementation/mediums/ble_v2/ble_utils.h"
#include "connections/implementation/mediums/ble_v2/bloom_filter.h"
#include "connections/implementation/mediums/bluetooth_radio.h"
#include "connections/implementation/mediums/utils.h"
#include "connections/power_level.h"
#include "internal/platform/byte_array.h"
#include "internal/platform/cancelable_alarm.h"
#include "internal/platform/logging.h"
#include "internal/platform/mutex_lock.h"

namespace location {
namespace nearby {
namespace connections {

namespace {

using ::location::nearby::api::ble_v2::BleAdvertisementData;
using ::location::nearby::api::ble_v2::GattCharacteristic;
using ::location::nearby::api::ble_v2::PowerMode;

constexpr int kMaxAdvertisementLength = 512;
constexpr int kDummyServiceIdLength = 128;
constexpr absl::Duration kPeripheralLostTimeout = absl::Seconds(3);

}  // namespace

BleV2::BleV2(BluetoothRadio& radio)
    : radio_(radio), adapter_(radio_.GetBluetoothAdapter()) {}

BleV2::~BleV2() {
  // Destructor is not taking locks, but methods it is calling are.
  while (!scanned_service_ids_.empty()) {
    StopScanning(scanned_service_ids_.begin()->data());
  }
  while (!advertising_service_ids_.empty()) {
    StopAdvertising(advertising_service_ids_.begin()->data());
  }

  serial_executor_.Shutdown();
  alarm_executor_.Shutdown();
}

bool BleV2::IsAvailable() const {
  MutexLock lock(&mutex_);

  return IsAvailableLocked();
}

// TODO(edwinwu): Break down the function.
bool BleV2::StartAdvertising(
    const std::string& service_id, const ByteArray& advertisement_bytes,
    PowerLevel power_level,
    const std::string& fast_advertisement_service_uuid) {
  MutexLock lock(&mutex_);

  if (advertisement_bytes.Empty()) {
    NEARBY_LOGS(INFO)
        << "Refusing to turn on BLE advertising. Empty advertisement data.";
    return false;
  }

  if (advertisement_bytes.size() > kMaxAdvertisementLength) {
    NEARBY_LOG(INFO,
               "Refusing to start BLE advertising because the advertisement "
               "was too long. Expected at most %d bytes but received %d.",
               kMaxAdvertisementLength, advertisement_bytes.size());
    return false;
  }

  if (IsAdvertisingLocked(service_id)) {
    NEARBY_LOGS(INFO)
        << "Failed to BLE advertise because we're already advertising.";
    return false;
  }

  if (!radio_.IsEnabled()) {
    NEARBY_LOGS(INFO)
        << "Can't start BLE scanning because Bluetooth was never turned on";
    return false;
  }

  if (!IsAvailableLocked()) {
    NEARBY_LOGS(INFO) << "Can't turn on BLE advertising. BLE is not available.";
    return false;
  }

  // Wrap the connections advertisement to the medium advertisement.
  const bool is_fast_advertisement = !fast_advertisement_service_uuid.empty();
  ByteArray service_id_hash = mediums::bleutils::GenerateHash(
      service_id, mediums::BleAdvertisement::kServiceIdHashLength);
  ByteArray medium_advertisement_bytes{mediums::BleAdvertisement{
      mediums::BleAdvertisement::Version::kV2,
      mediums::BleAdvertisement::SocketVersion::kV2,
      /*service_id_hash=*/is_fast_advertisement ? ByteArray{} : service_id_hash,
      advertisement_bytes, mediums::bleutils::GenerateDeviceToken(),
      mediums::BleAdvertisementHeader::kDefaultPsmValue}};
  if (medium_advertisement_bytes.Empty()) {
    NEARBY_LOGS(INFO) << "Failed to BLE advertise because we could not wrap a "
                         "connection advertisement to medium advertisement.";
    return false;
  }

  // Assemble AdvertisingData and ScanResponseData.
  BleAdvertisementData advertising_data;
  BleAdvertisementData scan_response_data;
  if (is_fast_advertisement) {
    advertising_data.is_connectable = true;
    advertising_data.tx_power_level =
        BleAdvertisementData::kUnspecifiedTxPowerLevel;
    advertising_data.service_uuids.insert(fast_advertisement_service_uuid);

    scan_response_data.is_connectable = true;
    scan_response_data.tx_power_level =
        BleAdvertisementData::kUnspecifiedTxPowerLevel;
    scan_response_data.service_data.insert(
        {fast_advertisement_service_uuid, medium_advertisement_bytes});
  } else {
    // Stop the current advertisement GATT server if there are no incoming
    // sockets connected to this device.
    //
    // The reason for aggressively restarting a GATT server is to make sure this
    // class is not using a stale server object that may not be actually running
    // anymore (possibly due to Bluetooth being turned off).
    //
    // Changing one's GATT server while a remote device is connected to it leads
    // to a loss of GATT callbacks for that remote device. The only time a
    // remote device is indefinitely connected to this device's GATT server is
    // when it has a BLE socket connection.
    // TODO(b/213835576): Check the BLE Connections is off. We set the fake
    // value for the time being till connections is implemented.
    bool no_incoming_ble_sockets = true;
    if (no_incoming_ble_sockets) {
      NEARBY_LOGS(VERBOSE)
          << "Aggressively stopping any pre-existing advertisement GATT "
             "servers because no incoming BLE sockets are connected";
      StopAdvertisementGattServerLocked();
    }

    // Start a GATT server to deliver the full advertisement data. If fail to
    // advertise the header, we must shut this down before the method returns.
    if (!IsAdvertisementGattServerRunningLocked()) {
      if (!StartAdvertisementGattServerLocked(service_id,
                                              medium_advertisement_bytes)) {
        NEARBY_LOGS(ERROR)
            << "Failed to start BLE advertising for service_id=" << service_id
            << " because the advertisement GATT server failed to start.";
        return false;
      }
    }

    ByteArray advertisement_header_bytes(CreateAdvertisementHeader());
    if (advertisement_header_bytes.Empty()) {
      NEARBY_LOGS(INFO) << "Failed to BLE advertise because we could not "
                           "create an advertisement header.";
      // Failed to start BLE advertising, so stop the advertisement GATT
      // server.
      StopAdvertisementGattServerLocked();
      return false;
    }

    advertising_data.is_connectable = true;
    advertising_data.tx_power_level =
        BleAdvertisementData::kUnspecifiedTxPowerLevel;

    scan_response_data.is_connectable = true;
    scan_response_data.tx_power_level =
        BleAdvertisementData::kUnspecifiedTxPowerLevel;
    scan_response_data.service_uuids.insert(
        std::string(mediums::bleutils::kCopresenceServiceUuid));
    scan_response_data.service_data.insert(
        {std::string(mediums::bleutils::kCopresenceServiceUuid),
         advertisement_header_bytes});
  }

  if (!medium_.StartAdvertising(advertising_data, scan_response_data,
                                PowerLevelToPowerMode(power_level))) {
    NEARBY_LOGS(ERROR)
        << "Failed to turn on BLE advertising with advertisement bytes="
        << absl::BytesToHexString(advertisement_bytes.data())
        << ", is_fast_advertisement=" << is_fast_advertisement
        << ", fast advertisement service uuid="
        << (is_fast_advertisement ? fast_advertisement_service_uuid
                                  : "[empty]");

    // If BLE advertising was not successful, stop the advertisement GATT
    // server.
    StopAdvertisementGattServerLocked();
    return false;
  }

  NEARBY_LOGS(INFO) << "Started BLE advertising with advertisement bytes="
                    << absl::BytesToHexString(advertisement_bytes.data())
                    << " for service_id=" << service_id;
  advertising_service_ids_.insert(service_id);
  return true;
}

bool BleV2::StopAdvertising(const std::string& service_id) {
  MutexLock lock(&mutex_);

  if (!IsAdvertisingLocked(service_id)) {
    NEARBY_LOGS(INFO) << "Cannot stop BLE advertising for service_id="
                      << service_id << " because it never started.";
    return false;
  }

  // Remove the GATT advertisements.
  gatt_advertisements_.clear();

  // TODO(b/213835576): Check the BLE Connections is off. We set the fake
  // value for the time being till connections is implemented.
  bool no_incoming_ble_sockets = true;
  // Set the value of characteristic to empty if there is still an advertiser.
  if (!hosted_gatt_characteristics_.empty()) {
    ByteArray empty_value = {};
    for (const auto& characteristic : hosted_gatt_characteristics_) {
      if (!gatt_server_->UpdateCharacteristic(characteristic, empty_value)) {
        NEARBY_LOGS(ERROR)
            << "Failed to clear characteristic uuid=" << characteristic.uuid
            << " after stopping BLE advertisement for service_id="
            << service_id;
      }
    }
    hosted_gatt_characteristics_.clear();
  } else if (no_incoming_ble_sockets) {
    // Otherwise, if we aren't restarting the BLE advertisement, then shutdown
    // the gatt server if it's not in use.
    NEARBY_LOGS(VERBOSE)
        << "Aggressively stopping any pre-existing advertisement GATT servers "
           "because no incoming BLE sockets are connected.";
    StopAdvertisementGattServerLocked();
  }

  NEARBY_LOGS(INFO) << "Turned off BLE advertising with service_id="
                    << service_id;
  advertising_service_ids_.erase(service_id);
  return medium_.StopAdvertising();
}

bool BleV2::IsAdvertising(const std::string& service_id) const {
  MutexLock lock(&mutex_);

  return IsAdvertisingLocked(service_id);
}

bool BleV2::StartScanning(const std::string& service_id, PowerLevel power_level,
                          DiscoveredPeripheralCallback callback,
                          const std::string& fast_advertisement_service_uuid) {
  MutexLock lock(&mutex_);

  if (service_id.empty()) {
    NEARBY_LOGS(INFO) << "Can not start BLE scanning with empty service id.";
    return false;
  }

  if (IsScanningLocked(service_id)) {
    NEARBY_LOGS(INFO) << "Cannot start scan of BLE peripherals because "
                         "scanning is already in-progress.";
    return false;
  }

  if (!radio_.IsEnabled()) {
    NEARBY_LOGS(INFO)
        << "Can't start BLE scanning because Bluetooth is disabled";
    return false;
  }

  if (!IsAvailableLocked()) {
    NEARBY_LOGS(INFO)
        << "Can't scan BLE peripherals because BLE isn't available.";
    return false;
  }

  // Start to track the advertisement found for specific `service_id`.
  discovered_peripheral_tracker_.StartTracking(service_id, std::move(callback),
                                               fast_advertisement_service_uuid);

  // Check if scan has been activated, if yes, no need to notify client
  // to scan again.
  if (!scanned_service_ids_.empty()) {
    scanned_service_ids_.insert(service_id);
    NEARBY_LOGS(INFO) << "Turned on BLE scanning with service id=" << service_id
                      << " without start client scanning";
    return true;
  }

  scanned_service_ids_.insert(service_id);
  // TODO(b/213835576): We should re-start scanning once the power level is
  // changed.
  std::vector<std::string> scanning_service_uuids;
  if (!fast_advertisement_service_uuid.empty()) {
    scanning_service_uuids.push_back(fast_advertisement_service_uuid);
  } else {
    scanning_service_uuids.push_back(
        std::string(mediums::bleutils::kCopresenceServiceUuid));
  }
  if (!medium_.StartScanning(
          scanning_service_uuids, PowerLevelToPowerMode(power_level),
          {
              .advertisement_found_cb =
                  [this](BleV2Peripheral peripheral,
                         const BleAdvertisementData& advertisement_data) {
                    RunOnBleThread([this, peripheral = std::move(peripheral),
                                    &advertisement_data]() {
                      MutexLock lock(&mutex_);
                      discovered_peripheral_tracker_
                          .ProcessFoundBleAdvertisement(
                              std::move(peripheral), advertisement_data,
                              GetAdvertisementFetcher());
                    });
                  },
          })) {
    NEARBY_LOGS(INFO) << "Failed to start scan of BLE services.";
    discovered_peripheral_tracker_.StopTracking(service_id);
    // Erase the service id that is just added.
    scanned_service_ids_.erase(service_id);
    return false;
  }

  // Set up lost alarm.
  lost_alarm_ = std::make_unique<CancelableAlarm>(
      "BLE.StartScanning() onLost",
      [this]() {
        MutexLock lock(&mutex_);
        discovered_peripheral_tracker_.ProcessLostGattAdvertisements();
        lost_alarm_->Run();
      },
      kPeripheralLostTimeout, &alarm_executor_);

  NEARBY_LOGS(INFO) << "Turned on BLE scanning with service id=" << service_id;
  return true;
}

bool BleV2::StopScanning(const std::string& service_id) {
  MutexLock lock(&mutex_);

  if (!IsScanningLocked(service_id)) {
    NEARBY_LOGS(INFO) << "Can't turn off BLE scanning because we never "
                         "started scanning.";
    return false;
  }

  discovered_peripheral_tracker_.StopTracking(service_id);
  scanned_service_ids_.erase(service_id);
  NEARBY_LOGS(INFO) << "Turned off BLE scanning with service id=" << service_id;

  // If still has scanner, don't stop the client scanning.
  if (!scanned_service_ids_.empty()) {
    return true;
  }

  // If no more scanning activities, then stop client scanning.
  NEARBY_LOGS(INFO) << "Turned off BLE client scanning";
  if (lost_alarm_->IsValid()) {
    lost_alarm_->Cancel();
  }
  return medium_.StopScanning();
}

bool BleV2::IsScanning(const std::string& service_id) const {
  MutexLock lock(&mutex_);

  return IsScanningLocked(service_id);
}

bool BleV2::IsAvailableLocked() const { return medium_.IsValid(); }

bool BleV2::IsAdvertisingLocked(const std::string& service_id) const {
  return advertising_service_ids_.contains(service_id);
}

bool BleV2::IsScanningLocked(const std::string& service_id) const {
  return scanned_service_ids_.contains(service_id);
}

bool BleV2::IsAdvertisementGattServerRunningLocked() {
  return gatt_server_ && gatt_server_->IsValid();
}

bool BleV2::StartAdvertisementGattServerLocked(
    const std::string& service_id, const ByteArray& gatt_advertisement) {
  if (IsAdvertisementGattServerRunningLocked()) {
    NEARBY_LOGS(INFO) << "Advertisement GATT server is not started because one "
                         "is already running.";
    return false;
  }

  std::unique_ptr<GattServer> gatt_server = medium_.StartGattServer();
  if (!gatt_server || !gatt_server->IsValid()) {
    NEARBY_LOGS(INFO) << "Unable to start an advertisement GATT server.";
    return false;
  }

  if (!GenerateAdvertisementCharacteristic(
          /*slot=*/0, gatt_advertisement, *gatt_server)) {
    gatt_server->Stop();
    return false;
  }

  // Insert the advertisements into their open advertisement slots.
  gatt_advertisements_.insert(
      {/*slot=*/0, std::make_pair(service_id, gatt_advertisement)});

  gatt_server_ = std::move(gatt_server);
  return true;
}

bool BleV2::GenerateAdvertisementCharacteristic(
    int slot, const ByteArray& gatt_advertisement, GattServer& gatt_server) {
  std::vector<GattCharacteristic::Permission> permissions{
      GattCharacteristic::Permission::kRead};
  std::vector<GattCharacteristic::Property> properties{
      GattCharacteristic::Property::kRead};

  absl::optional<GattCharacteristic> gatt_characteristic =
      gatt_server.CreateCharacteristic(
          std::string(mediums::bleutils::kCopresenceServiceUuid),
          mediums::bleutils::GenerateAdvertisementUuid(slot), permissions,
          properties);
  if (!gatt_characteristic.has_value()) {
    NEARBY_LOGS(INFO) << "Unable to create and add a characterstic to the gatt "
                         "server for the advertisement.";
    return false;
  }
  if (!gatt_server.UpdateCharacteristic(gatt_characteristic.value(),
                                        gatt_advertisement)) {
    NEARBY_LOGS(INFO) << "Unable to write a value to the GATT characteristic.";
    return false;
  }
  hosted_gatt_characteristics_.insert(gatt_characteristic.value());

  return true;
}

std::unique_ptr<mediums::AdvertisementReadResult>
BleV2::ProcessFetchGattAdvertisementsRequest(
    BleV2Peripheral peripheral, int num_slots, int psm,
    const std::vector<std::string>& interesting_service_ids,
    std::unique_ptr<mediums::AdvertisementReadResult>
        advertisement_read_result) {
  MutexLock lock(&mutex_);

  if (!advertisement_read_result) {
    advertisement_read_result =
        std::make_unique<mediums::AdvertisementReadResult>();
  }

  if (!peripheral.IsValid()) {
    NEARBY_LOGS(INFO) << "Can't read from an advertisement GATT server because "
                         "ble peripheral is null.";
    return advertisement_read_result;
  }

  if (!radio_.IsEnabled()) {
    NEARBY_LOGS(INFO) << "Can't read from an advertisement GATT server because "
                         "Bluetooth was never turned on.";
    return advertisement_read_result;
  }

  if (!IsAvailableLocked()) {
    NEARBY_LOGS(INFO) << "Can't read from an advertisement GATT server because "
                         "BLE is not available.";
    return advertisement_read_result;
  }

  return InternalReadAdvertisementFromGattServerLocked(
      std::move(peripheral), num_slots, psm, interesting_service_ids,
      std::move(advertisement_read_result));
}

std::unique_ptr<mediums::AdvertisementReadResult>
BleV2::InternalReadAdvertisementFromGattServerLocked(
    BleV2Peripheral peripheral, int num_slots, int psm,
    const std::vector<std::string>& interesting_service_ids,
    std::unique_ptr<mediums::AdvertisementReadResult>
        advertisement_read_result) {
  // Connect to a GATT server, reads advertisement data, and then disconnect
  // from the GATT server.
  bool read_success = true;
  std::unique_ptr<GattClient> gatt_client = medium_.ConnectToGattServer(
      std::move(peripheral), PowerLevelToPowerMode(PowerLevel::kHighPower));
  if (!gatt_client || !gatt_client->IsValid()) {
    advertisement_read_result->RecordLastReadStatus(false);
    return advertisement_read_result;
  }

  // Always use kCopresenceServiceUuid for service uuid.
  std::string service_uuid =
      std::string(mediums::bleutils::kCopresenceServiceUuid);
  if (!gatt_client->DiscoverService(service_uuid)) {
    NEARBY_LOGS(WARNING) << "GATT client can't discover service.";
    advertisement_read_result->RecordLastReadStatus(false);
    return advertisement_read_result;
  }

  // Read all advertisements from all slots that we haven't read from yet.
  for (int slot = 0; slot < num_slots; ++slot) {
    // Make sure we haven't already read this advertisement before.
    if (advertisement_read_result->HasAdvertisement(slot)) {
      continue;
    }

    // Make sure the characteristic even exists for this slot number. If
    // the characteristic doesn't exist, we shouldn't count the fetch as a
    // failure because there's nothing we could've done about a
    // non-existed characteristic.
    auto gatt_characteristic = gatt_client->GetCharacteristic(
        std::string(mediums::bleutils::kCopresenceServiceUuid),
        mediums::bleutils::GenerateAdvertisementUuid(slot));
    if (!gatt_characteristic.has_value()) {
      continue;
    }

    // Read advertisement data from the characteristic associated with this
    // slot.
    auto characteristic_byte =
        gatt_client->ReadCharacteristic(gatt_characteristic.value());
    if (characteristic_byte.has_value()) {
      advertisement_read_result->AddAdvertisement(slot, *characteristic_byte);
      NEARBY_LOGS(VERBOSE) << "Successfully read advertisement at slot="
                           << slot;
    } else {
      NEARBY_LOGS(WARNING) << "Can't read advertisement for slot=" << slot;
      read_success = false;
    }
    // Whether or not we succeeded with this slot, we should try reading the
    // other slots to get as many advertisements as possible before
    // returning a success or failure.
  }
  gatt_client->Disconnect();

  advertisement_read_result->RecordLastReadStatus(read_success);
  return advertisement_read_result;
}

bool BleV2::StopAdvertisementGattServerLocked() {
  if (!IsAdvertisementGattServerRunningLocked()) {
    NEARBY_LOGS(INFO) << "Unable to stop the advertisement GATT server because "
                         "it's not running.";
    return false;
  }

  gatt_server_.reset();
  return true;
}

ByteArray BleV2::CreateAdvertisementHeader() {
  // Create a randomized dummy service id to anonymize the header with.
  ByteArray dummy_service_id_bytes =
      Utils::GenerateRandomBytes(kDummyServiceIdLength);
  std::string dummy_service_id{dummy_service_id_bytes};

  mediums::BloomFilter bloom_filter(
      std::make_unique<mediums::BitSetImpl<
          mediums::BleAdvertisementHeader::kServiceIdBloomFilterByteLength>>());
  bloom_filter.Add(dummy_service_id);

  ByteArray advertisement_hash =
      mediums::bleutils::GenerateAdvertisementHash(dummy_service_id_bytes);
  for (const auto& item : gatt_advertisements_) {
    const std::string& service_id = item.second.first;
    const ByteArray& gatt_advertisement = item.second.second;
    bloom_filter.Add(service_id);

    // Compute the next hash according to the algorithm in
    // https://source.corp.google.com/piper///depot/google3/java/com/google/android/gmscore/integ/modules/nearby/src/com/google/android/gms/nearby/mediums/bluetooth/BluetoothLowEnergy.java;rcl=428397891;l=1043
    std::string advertisement_bodies = absl::StrCat(
        advertisement_hash.AsStringView(), gatt_advertisement.AsStringView());

    advertisement_hash = mediums::bleutils::GenerateAdvertisementHash(
        ByteArray(std::move(advertisement_bodies)));
  }

  return ByteArray(mediums::BleAdvertisementHeader(
      mediums::BleAdvertisementHeader::Version::kV2,
      /*extended_advertisement=*/false,
      /*num_slots=*/gatt_advertisements_.size(), ByteArray(bloom_filter),
      advertisement_hash, /*psm=*/0));
}

PowerMode BleV2::PowerLevelToPowerMode(PowerLevel power_level) {
  switch (power_level) {
    case PowerLevel::kHighPower:
      return PowerMode::kHigh;
    case PowerLevel::kLowPower:
      // Medium power is about the size of a conference room.
      // Any lower and we won't be visible at a distance.
      return PowerMode::kMedium;
    default:
      return PowerMode::kUnknown;
  }
}

void BleV2::RunOnBleThread(Runnable runnable) {
  serial_executor_.Execute(std::move(runnable));
}

mediums::DiscoveredPeripheralTracker::AdvertisementFetcher
BleV2::GetAdvertisementFetcher() {
  return {
      .fetch_advertisements =
          [this](BleV2Peripheral peripheral, int num_slots, int psm,
                 const std::vector<std::string>& interesting_service_ids,
                 std::unique_ptr<mediums::AdvertisementReadResult>
                     advertisement_read_result)
          -> std::unique_ptr<mediums::AdvertisementReadResult> {
        return ProcessFetchGattAdvertisementsRequest(
            std::move(peripheral), num_slots, psm, interesting_service_ids,
            std::move(advertisement_read_result));
      },
  };
}

}  // namespace connections
}  // namespace nearby
}  // namespace location
