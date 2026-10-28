#pragma once
// BLEeh transmitter (BLE central) for ESP32 + NimBLE-Arduino 2.x.
// Used by the Nano-bay bridge; usable by any ESP32 that wants to drive a
// BLEeh receiver. All BLE work runs in its own FreeRTOS task, so
// sendChannels() never blocks the caller.
#include <Arduino.h>
#include "BLEehProtocol.h"

struct BLEehTxStatus {
    bool active = false;
    bool hasReceiver = false;      // a receiver address is stored
    bool pairing = false;
    bool connected = false;        // encrypted link up, RC frames flowing
    bleeh::Capabilities caps;  // read from the receiver on connect
    bool telemetryFresh = false;   // telemetry received within the last second
    bleeh::LinkTelemetry link; // uplink LQ / RSSI / failsafe reported by the receiver
    bool hasBattery = false;       // receiver has reported a battery at least once
    bleeh::BatteryTelemetry battery;
    int8_t downlinkRssi = 0;       // dBm of the receiver measured here
    uint8_t downlinkLq = 0;        // % of telemetry notifications received
    uint32_t rcSent = 0;
    uint32_t rcFailed = 0;
};

// Latest sensor values from the receiver; see takeSensors().
struct BLEehSensors {
    bleeh::BatteryTelemetry battery;
    bleeh::GpsTelemetry gps;
    bleeh::AttitudeTelemetry attitude;
    int32_t baroAltitudeCm = 0;
    int16_t varioCms = 0;
    char flightMode[bleeh::FLIGHT_MODE_MAX + 1] = {};
};
enum BLEehSensorBit : uint8_t {
    BLEEH_SENSOR_BATTERY = 1 << 0,
    BLEEH_SENSOR_GPS = 1 << 1,
    BLEEH_SENSOR_ATTITUDE = 1 << 2,
    BLEEH_SENSOR_BARO = 1 << 3,
    BLEEH_SENSOR_VARIO = 1 << 4,
    BLEEH_SENSOR_FLIGHT_MODE = 1 << 5,
};

class BLEehTx {
public:
    // NimBLEDevice::init() must already have been called by the application.
    void begin();
    void end();
    bool active() const { return active_; }

    // Thread-safe and non-blocking; call once per new set of channel values.
    void sendChannels(const uint16_t channels[bleeh::NUM_CHANNELS], bool sourceFresh);

    void startPairing(uint32_t durationMs = 60000);  // bond the nearest receiver in pairing mode
    void forgetReceiver();
    // Stored receiver (from NVS; works before begin()).
    bool receiverAddress(uint8_t address[6], uint8_t &type);
    BLEehTxStatus status() const;
    // Copies the latest sensor values; returns a BLEehSensorBit mask of the
    // sensors that arrived since the previous call.
    uint8_t takeSensors(BLEehSensors &out);

    // Internal: telemetry notification handler.
    void handleTelemetry(const uint8_t *data, size_t len);

private:
    static void taskEntry(void *self);
    void taskLoop();
    bool connectAndSetup();
    void runPairing();
    void dropLink();
    void loadPeer();
    void savePeer();

    mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
    TaskHandle_t task_ = nullptr;
    volatile bool active_ = false;
    volatile bool pairing_ = false;
    volatile bool forget_ = false;
    uint32_t pairingUntilMs_ = 0;

    bool peerLoaded_ = false;
    bool hasPeer_ = false;
    uint8_t peer_[6] = {};
    uint8_t peerType_ = 0;
    bool connected_ = false;
    bleeh::Capabilities caps_;

    // Channels handed over by sendChannels()
    uint16_t pendingChannels_[bleeh::NUM_CHANNELS] = {};
    bool pendingFresh_ = false;
    bool pendingValid_ = false;
    uint16_t sequence_ = 0;
    uint32_t rcSent_ = 0, rcFailed_ = 0;

    // Telemetry (written by the NimBLE host task)
    uint32_t lastTelemetryMs_ = 0;
    bleeh::LinkTelemetry link_;
    bleeh::SequenceQuality<uint8_t, 20> downlinkLq_;
    int8_t downlinkRssi_ = 0;
    uint32_t lastRssiMs_ = 0;
    BLEehSensors sensors_;
    uint8_t sensorsUpdated_ = 0;
    bool hasBattery_ = false;
};
