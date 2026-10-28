#pragma once
// BLEeh receiver for ESP32 (Arduino + NimBLE-Arduino 2.x).
// Put this in your robot / RC car firmware: it advertises the BLEeh GATT
// service, accepts one bonded transmitter, and gives you 16 RC channels with
// failsafe plus a telemetry path back to the radio.
#include <Arduino.h>
#include "BLEehProtocol.h"

enum class BLEehFailsafe : uint8_t {
    Hold,    // keep the last received channel values
    Preset,  // switch to the failsafe values (default: all centre)
};

struct BLEehRxConfig {
    const char *name = "BLEeh RX";   // BLE name and capabilities name (max 20 chars)

    // Pairing button (optional). Pressing it `pairClicks` times within
    // `pairClickWindowMs` opens a pairing window. Use -1 for no button.
    int pairButtonPin = -1;
    bool pairButtonActiveLow = true;     // pressed = LOW, internal pull-up enabled
    uint8_t pairClicks = 3;
    uint32_t pairClickWindowMs = 3000;
    bool pairWhenUnbound = true;         // open pairing at boot if no transmitter is bonded
    uint32_t pairingTimeoutMs = 120000;  // 0 = stay in pairing until a transmitter bonds

    // Signal loss: failsafe after this long without a valid RC frame.
    uint32_t failsafeTimeoutMs = 250;
    BLEehFailsafe failsafeMode = BLEehFailsafe::Preset;

    uint32_t telemetryPeriodMs = 100;    // link telemetry rate (sensors ride along)
    int8_t txPowerDbm = 15;              // rounded to the chip's supported steps
    int statusLedPin = -1;               // fast blink = pairing, slow = no link, on = linked
    bool statusLedActiveLow = false;
};

class BLEehRx {
public:
    // Initializes NimBLE (owns NimBLEDevice::init) and starts advertising.
    bool begin(const BLEehRxConfig &config = BLEehRxConfig());
    // Call from loop() as often as possible.
    void update();

    // ---- Channels ----
    bool newFrame();                              // true once per received RC frame
    uint16_t channel(uint8_t index) const;        // raw CRSF units 172..992..1811
    uint16_t channelMicros(uint8_t index) const;  // 988..1500..2012 us
    float channelNormalized(uint8_t index) const; // -1..0..+1
    bool isFailsafe() const;    // no fresh frames, not linked, or transmitter lost its radio
    void setFailsafeValue(uint8_t index, uint16_t raw);
    void setFailsafeMode(BLEehFailsafe mode) { config_.failsafeMode = mode; }
    void setFailsafeTimeout(uint32_t ms) { config_.failsafeTimeoutMs = ms; }
    void (*onFailsafeChanged)(bool failsafe) = nullptr;  // called from update()

    // ---- Link state ----
    bool isConnected() const { return connected_; }
    bool isPairing() const { return pairing_; }
    bool isBonded() const;
    void startPairing();     // open the pairing window (like the button)
    void clearPairing();     // forget the bonded transmitter
    uint8_t linkQuality() const;   // % of RC frames received (0 when in failsafe)
    int8_t rssi() const { return rssi_; }
    uint32_t frameAgeMs() const;

    // ---- Telemetry to the radio (sent at telemetryPeriodMs while linked) ----
    void setBattery(uint16_t millivolts, uint16_t currentDa = 0, uint32_t capacityMah = 0,
                    uint8_t remainingPct = 0);
    void setGps(int32_t latitude, int32_t longitude, uint16_t groundspeed, uint16_t heading,
                uint16_t altitude, uint8_t satellites);
    void setAttitude(int16_t pitch, int16_t roll, int16_t yaw);
    void setBaroAltitude(int32_t altitudeCm);
    void setVario(int16_t verticalSpeedCms);
    void setFlightMode(const char *mode);

    // Internal: called from the NimBLE callbacks.
    void handleRcWrite(const uint8_t *data, size_t len);
    void handleConnect(uint16_t connHandle);
    void handleDisconnect();
    void handleAuthentication(bool encrypted, bool newBond, const uint8_t idAddress[6], uint8_t idType);

private:
    void setPairing(bool on);
    void updateAdvertising();
    void pollPairButton(uint32_t now);
    void sendTelemetry(uint32_t now);
    void updateLed(uint32_t now);

    BLEehRxConfig config_;
    mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;

    // Written from the BLE host task, read from loop().
    volatile bool connected_ = false;
    volatile uint16_t connHandle_ = 0xFFFF;
    bleeh::RcFrame frame_;
    bool haveFrame_ = false;
    bool newFrame_ = false;
    uint32_t lastFrameMs_ = 0;
    bleeh::SequenceQuality<uint16_t, 100> lq_;
    volatile bool pendingDisconnect_ = false;
    volatile bool pendingBondCleanup_ = false;
    uint8_t keepAddress_[6] = {};
    uint8_t keepType_ = 0;

    bool pairing_ = false;
    uint32_t pairingStartMs_ = 0;
    bool failsafe_ = true;
    int8_t rssi_ = 0;
    uint32_t lastRssiMs_ = 0;
    uint16_t failsafeValues_[bleeh::NUM_CHANNELS];

    // Pairing button
    bool btnStable_ = false, btnLast_ = false;
    uint32_t btnChangedMs_ = 0, firstClickMs_ = 0;
    uint8_t clicks_ = 0;

    // Telemetry
    uint8_t telemSeq_ = 0;
    uint32_t lastTelemMs_ = 0;
    uint8_t dirty_ = 0;  // bit per sensor waiting to be sent
    uint32_t lastFullRefreshMs_ = 0;
    bleeh::BatteryTelemetry battery_;
    bleeh::GpsTelemetry gps_;
    bleeh::AttitudeTelemetry attitude_;
    int32_t baroCm_ = 0;
    int16_t varioCms_ = 0;
    char flightMode_[bleeh::FLIGHT_MODE_MAX + 1] = {};
    uint8_t present_ = 0;  // bit per sensor the application has set
};
