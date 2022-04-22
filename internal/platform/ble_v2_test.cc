// Copyright 2020 Google LLC
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

#include "internal/platform/ble_v2.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "gmock/gmock.h"
#include "protobuf-matchers/protocol-buffer-matchers.h"
#include "gtest/gtest.h"
#include "internal/platform/bluetooth_adapter.h"
#include "internal/platform/count_down_latch.h"
#include "internal/platform/medium_environment.h"

namespace location {
namespace nearby {
namespace {

using FeatureFlags = FeatureFlags::Flags;

constexpr FeatureFlags kTestCases[] = {
    FeatureFlags{
        .enable_cancellation_flag = true,
    },
    FeatureFlags{
        .enable_cancellation_flag = false,
    },
};

using ::location::nearby::api::ble_v2::BleAdvertisementData;
using ::location::nearby::api::ble_v2::GattCharacteristic;
using ::location::nearby::api::ble_v2::PowerMode;

constexpr absl::Duration kWaitDuration = absl::Milliseconds(1000);
constexpr absl::string_view kAdvertisementString{"\x0a\x0b\x0c\x0d"};
constexpr absl::string_view kCopresenceServiceUuid{"F3FE"};
constexpr absl::string_view kFastAdvertisementServiceUuid = "FE2C";
constexpr PowerMode kPowerMode(PowerMode::kHigh);
constexpr absl::string_view kServiceIDA{
    "com.google.location.nearby.apps.test.a"};

// A stub BlePeripheral implementation.
class BlePeripheralStub : public api::ble_v2::BlePeripheral {
 public:
  explicit BlePeripheralStub(absl::string_view mac_address) {
    mac_address_ = mac_address;
  }

  std::string GetAddress() const override { return mac_address_; }

 private:
  std::string mac_address_;
};

class BleV2MediumTest : public ::testing::TestWithParam<FeatureFlags> {
 protected:
  BleV2MediumTest() { env_.Stop(); }

  MediumEnvironment& env_{MediumEnvironment::Instance()};
};

TEST_P(BleV2MediumTest, CanConnectToService) {
  FeatureFlags feature_flags = GetParam();
  env_.SetFeatureFlags(feature_flags);
  env_.Start();
  BluetoothAdapter adapter_a_;
  BluetoothAdapter adapter_b_;
  BleV2Medium ble_a{adapter_a_};
  BleV2Medium ble_b{adapter_b_};
  std::string service_id(kServiceIDA);
  ByteArray advertisement_bytes{std::string(kAdvertisementString)};
  CountDownLatch found_latch(1);
  CountDownLatch lost_latch(1);

  BleV2ServerSocket server_socket = ble_b.OpenServerSocket(service_id);
  EXPECT_TRUE(server_socket.IsValid());

  // Assemble regular advertising and scan response data.
  BleAdvertisementData advertising_data = {};
  BleAdvertisementData scan_response_data;
  scan_response_data.service_uuids.insert(std::string(kCopresenceServiceUuid));
  scan_response_data.service_data.insert(
      {std::string(kCopresenceServiceUuid),
       ByteArray(std::string(kAdvertisementString))});
  ble_b.StartAdvertising(advertising_data, scan_response_data, kPowerMode);

  BleV2Peripheral discovered_peripheral;
  ble_a.StartScanning(
      {std::string(kCopresenceServiceUuid)}, kPowerMode,
      {
          .advertisement_found_cb =
              [&found_latch, &discovered_peripheral](
                  BleV2Peripheral peripheral,
                  const BleAdvertisementData& advertisement_data) {
                discovered_peripheral = std::move(peripheral);
                found_latch.CountDown();
              },
      });
  EXPECT_TRUE(found_latch.Await(absl::Milliseconds(1000)).result());
  BleV2Socket socket_a;
  BleV2Socket socket_b;
  EXPECT_FALSE(socket_a.IsValid());
  EXPECT_FALSE(socket_b.IsValid());
  {
    CancellationFlag flag;
    SingleThreadExecutor server_executor;
    SingleThreadExecutor client_executor;
    client_executor.Execute([&ble_a, &socket_a, &service_id,
                             discovered_peripheral =
                                 std::move(discovered_peripheral),
                             &server_socket, &flag]() {
      socket_a =
          ble_a.Connect(service_id, kPowerMode, discovered_peripheral, &flag);
      if (!socket_a.IsValid()) {
        server_socket.Close();
      }
    });
    server_executor.Execute([&socket_b, &server_socket]() {
      socket_b = server_socket.Accept();
      if (!socket_b.IsValid()) {
        server_socket.Close();
      }
    });
  }
  EXPECT_TRUE(socket_a.IsValid());
  EXPECT_TRUE(socket_b.IsValid());
  server_socket.Close();
  env_.Stop();
}

TEST_P(BleV2MediumTest, CanCancelConnect) {
  FeatureFlags feature_flags = GetParam();
  env_.SetFeatureFlags(feature_flags);
  env_.Start();
  BluetoothAdapter adapter_a_;
  BluetoothAdapter adapter_b_;
  BleV2Medium ble_a{adapter_a_};
  BleV2Medium ble_b{adapter_b_};
  std::string service_id(kServiceIDA);
  ByteArray advertisement_bytes{std::string(kAdvertisementString)};
  CountDownLatch found_latch(1);
  CountDownLatch lost_latch(1);

  BleV2ServerSocket server_socket = ble_b.OpenServerSocket(service_id);
  EXPECT_TRUE(server_socket.IsValid());

  // Assemble regular advertising and scan response data.
  BleAdvertisementData advertising_data = {};
  BleAdvertisementData scan_response_data;
  scan_response_data.service_uuids.insert(std::string(kCopresenceServiceUuid));
  scan_response_data.service_data.insert(
      {std::string(kCopresenceServiceUuid),
       ByteArray(std::string(kAdvertisementString))});
  ble_b.StartAdvertising(advertising_data, scan_response_data, kPowerMode);

  BleV2Peripheral discovered_peripheral;
  ble_a.StartScanning(
      {std::string(kCopresenceServiceUuid)}, kPowerMode,
      {
          .advertisement_found_cb =
              [&found_latch, &discovered_peripheral](
                  BleV2Peripheral peripheral,
                  const BleAdvertisementData& advertisement_data) {
                discovered_peripheral = std::move(peripheral);
                found_latch.CountDown();
              },
      });
  EXPECT_TRUE(found_latch.Await(absl::Milliseconds(1000)).result());
  BleV2Socket socket_a;
  BleV2Socket socket_b;
  EXPECT_FALSE(socket_a.IsValid());
  EXPECT_FALSE(socket_b.IsValid());
  {
    CancellationFlag flag(true);
    SingleThreadExecutor server_executor;
    SingleThreadExecutor client_executor;
    client_executor.Execute([&ble_a, &socket_a, &service_id,
                             discovered_peripheral =
                                 std::move(discovered_peripheral),
                             &server_socket, &flag]() {
      socket_a =
          ble_a.Connect(service_id, kPowerMode, discovered_peripheral, &flag);
      if (!socket_a.IsValid()) {
        server_socket.Close();
      }
    });
    server_executor.Execute([&socket_b, &server_socket]() {
      socket_b = server_socket.Accept();
      if (!socket_b.IsValid()) {
        server_socket.Close();
      }
    });
  }
  // If FeatureFlag is disabled, Cancelled is false as no-op.
  if (!feature_flags.enable_cancellation_flag) {
    EXPECT_TRUE(socket_a.IsValid());
    EXPECT_TRUE(socket_b.IsValid());
  } else {
    EXPECT_FALSE(socket_a.IsValid());
    EXPECT_FALSE(socket_b.IsValid());
  }
  server_socket.Close();
  env_.Stop();
}

INSTANTIATE_TEST_SUITE_P(ParametrisedBleMediumTest, BleV2MediumTest,
                         ::testing::ValuesIn(kTestCases));

TEST_F(BleV2MediumTest, ConstructorDestructorWorks) {
  env_.Start();
  BluetoothAdapter adapter_a_;
  BluetoothAdapter adapter_b_;
  BleV2Medium ble_a{adapter_a_};
  BleV2Medium ble_b{adapter_b_};

  // Make sure we can create functional mediums.
  ASSERT_TRUE(ble_a.IsValid());
  ASSERT_TRUE(ble_b.IsValid());

  // Make sure we can create 2 distinct mediums.
  EXPECT_NE(ble_a.GetImpl(), ble_b.GetImpl());
  env_.Stop();
}

TEST_F(BleV2MediumTest, CanStartFastScanningAndFastAdvertising) {
  env_.Start();
  BluetoothAdapter adapter_a_;
  BluetoothAdapter adapter_b_;
  BleV2Medium ble_a{adapter_a_};
  BleV2Medium ble_b{adapter_b_};
  CountDownLatch found_latch(1);

  EXPECT_TRUE(ble_a.StartScanning(
      {std::string(kFastAdvertisementServiceUuid)}, kPowerMode,
      {
          .advertisement_found_cb =
              [&found_latch](BleV2Peripheral peripheral,
                             const BleAdvertisementData& advertisement_data) {
                found_latch.CountDown();
              },
      }));

  // Assemble fast advertising and scan response data.
  BleAdvertisementData advertising_data;
  advertising_data.service_uuids.insert(
      std::string(kFastAdvertisementServiceUuid));
  BleAdvertisementData scan_response_data;
  scan_response_data.service_data.insert(
      {std::string(kFastAdvertisementServiceUuid),
       ByteArray(std::string(kAdvertisementString))});

  EXPECT_TRUE(
      ble_b.StartAdvertising(advertising_data, scan_response_data, kPowerMode));
  EXPECT_TRUE(found_latch.Await(kWaitDuration).result());
  EXPECT_TRUE(ble_a.StopScanning());
  EXPECT_TRUE(ble_b.StopAdvertising());
  env_.Stop();
}

TEST_F(BleV2MediumTest, CanStartScanningAndAdvertising) {
  env_.Start();
  BluetoothAdapter adapter_a_;
  BluetoothAdapter adapter_b_;
  BleV2Medium ble_a{adapter_a_};
  BleV2Medium ble_b{adapter_b_};
  CountDownLatch found_latch(1);

  EXPECT_TRUE(ble_a.StartScanning(
      {std::string(kCopresenceServiceUuid)}, kPowerMode,
      {
          .advertisement_found_cb =
              [&found_latch](BleV2Peripheral peripheral,
                             const BleAdvertisementData& advertisement_data) {
                found_latch.CountDown();
              },
      }));

  // Assemble regular advertising and scan response data.
  BleAdvertisementData advertising_data = {};
  BleAdvertisementData scan_response_data;
  scan_response_data.service_uuids.insert(std::string(kCopresenceServiceUuid));
  scan_response_data.service_data.insert(
      {std::string(kCopresenceServiceUuid),
       ByteArray(std::string(kAdvertisementString))});

  EXPECT_TRUE(
      ble_b.StartAdvertising(advertising_data, scan_response_data, kPowerMode));
  EXPECT_TRUE(found_latch.Await(kWaitDuration).result());
  EXPECT_TRUE(ble_a.StopScanning());
  EXPECT_TRUE(ble_b.StopAdvertising());
  env_.Stop();
}

TEST_F(BleV2MediumTest,
       CanStartFastAdvertisingButRegularScanningFailToFoundAdvertisement) {
  env_.Start();
  BluetoothAdapter adapter_a_;
  BluetoothAdapter adapter_b_;
  BleV2Medium ble_a{adapter_a_};
  BleV2Medium ble_b{adapter_b_};
  CountDownLatch found_latch(1);

  EXPECT_TRUE(ble_a.StartScanning(
      {std::string(kCopresenceServiceUuid)}, kPowerMode,
      {
          .advertisement_found_cb =
              [&found_latch](BleV2Peripheral peripheral,
                             const BleAdvertisementData& advertisement_data) {
                found_latch.CountDown();
              },
      }));

  // Assemble fast advertising and scan response data.
  BleAdvertisementData advertising_data;
  advertising_data.service_uuids.insert(
      std::string(kFastAdvertisementServiceUuid));
  BleAdvertisementData scan_response_data;
  scan_response_data.service_data.insert(
      {std::string(kFastAdvertisementServiceUuid),
       ByteArray(std::string(kAdvertisementString))});

  EXPECT_TRUE(
      ble_b.StartAdvertising(advertising_data, scan_response_data, kPowerMode));
  // Fail to found the advertiement.
  EXPECT_FALSE(found_latch.Await(kWaitDuration).result());
  EXPECT_TRUE(ble_a.StopScanning());
  EXPECT_TRUE(ble_b.StopAdvertising());
  env_.Stop();
}

TEST_F(BleV2MediumTest,
       CanStartAdvertisingButFastScanningFailToFoundAdvertisement) {
  env_.Start();
  BluetoothAdapter adapter_a_;
  BluetoothAdapter adapter_b_;
  BleV2Medium ble_a{adapter_a_};
  BleV2Medium ble_b{adapter_b_};
  CountDownLatch found_latch(1);

  EXPECT_TRUE(ble_a.StartScanning(
      {std::string(kFastAdvertisementServiceUuid)}, kPowerMode,
      {
          .advertisement_found_cb =
              [&found_latch](BleV2Peripheral peripheral,
                             const BleAdvertisementData& advertisement_data) {
                found_latch.CountDown();
              },
      }));

  // Assemble regular advertising and scan response data.
  BleAdvertisementData advertising_data = {};
  BleAdvertisementData scan_response_data;
  scan_response_data.service_uuids.insert(std::string(kCopresenceServiceUuid));
  scan_response_data.service_data.insert(
      {std::string(kCopresenceServiceUuid),
       ByteArray(std::string(kAdvertisementString))});

  EXPECT_TRUE(
      ble_b.StartAdvertising(advertising_data, scan_response_data, kPowerMode));
  // Fail to found the advertiement.
  EXPECT_FALSE(found_latch.Await(kWaitDuration).result());
  EXPECT_TRUE(ble_a.StopScanning());
  EXPECT_TRUE(ble_b.StopAdvertising());
  env_.Stop();
}

TEST_F(BleV2MediumTest, CanStartGattServer) {
  env_.Start();
  BluetoothAdapter adapter_;
  BleV2Medium ble{adapter_};
  std::string characteristic_uuid = "characteristic_uuid";

  std::unique_ptr<GattServer> gatt_server = ble.StartGattServer();

  ASSERT_NE(gatt_server, nullptr);

  std::vector<GattCharacteristic::Permission> permissions{
      GattCharacteristic::Permission::kRead};
  std::vector<GattCharacteristic::Property> properties{
      GattCharacteristic::Property::kRead};
  std::optional<GattCharacteristic> gatt_characteristic =
      gatt_server->CreateCharacteristic(std::string(kCopresenceServiceUuid),
                                        characteristic_uuid, permissions,
                                        properties);

  ASSERT_TRUE(gatt_characteristic.has_value());

  ByteArray any_byte("any");

  EXPECT_TRUE(
      gatt_server->UpdateCharacteristic(gatt_characteristic.value(), any_byte));

  gatt_server->Stop();

  env_.Stop();
}

TEST_F(BleV2MediumTest, GattClientConnectToGattServerWorks) {
  env_.Start();
  BluetoothAdapter adapter_a_;
  BluetoothAdapter adapter_b_;
  BleV2Medium ble_a{adapter_a_};
  BleV2Medium ble_b{adapter_b_};
  std::string characteristic_uuid = "characteristic_uuid";

  // Start GattServer
  std::unique_ptr<GattServer> gatt_server = ble_a.StartGattServer();

  ASSERT_NE(gatt_server, nullptr);

  std::vector<GattCharacteristic::Permission> permissions{
      GattCharacteristic::Permission::kRead};
  std::vector<GattCharacteristic::Property> properties{
      GattCharacteristic::Property::kRead};
  // Add characteristic and its value.
  std::optional<GattCharacteristic> server_characteristic =
      gatt_server->CreateCharacteristic(std::string(kCopresenceServiceUuid),
                                        characteristic_uuid, permissions,
                                        properties);
  ASSERT_TRUE(server_characteristic.has_value());
  ByteArray server_value("any");
  EXPECT_TRUE(gatt_server->UpdateCharacteristic(server_characteristic.value(),
                                                server_value));

  // Start GattClient
  auto ble_peripheral =
      std::make_unique<BlePeripheralStub>(/*mac_address=*/"ABCD");
  std::unique_ptr<GattClient> gatt_client = ble_b.ConnectToGattServer(
      BleV2Peripheral(ble_peripheral.get()), kPowerMode);

  ASSERT_NE(gatt_client, nullptr);

  // Discover service.
  EXPECT_TRUE(
      gatt_client->DiscoverService(std::string(kCopresenceServiceUuid)));

  // Discover characteristic.
  std::optional<GattCharacteristic> client_characteristic =
      gatt_client->GetCharacteristic(std::string(kCopresenceServiceUuid),
                                     characteristic_uuid);
  ASSERT_TRUE(client_characteristic.has_value());

  // Can read the characteristic value.
  std::optional<ByteArray> cient_value =
      gatt_client->ReadCharacteristic(*client_characteristic);
  ASSERT_TRUE(cient_value.has_value());
  EXPECT_EQ(*cient_value, server_value);

  gatt_client->Disconnect();
  gatt_server->Stop();
  env_.Stop();
}

}  // namespace
}  // namespace nearby
}  // namespace location
