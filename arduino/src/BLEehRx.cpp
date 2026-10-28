#include "BLEehRx.h"
#include <NimBLEDevice.h>
#include <string.h>

using namespace bleeh;

// NimBLE callbacks are C++ objects without a user pointer, so the library
// supports one receiver instance per firmware (the normal case).
static BLEehRx *g_rx = nullptr;
static NimBLEServer *g_server = nullptr;
static NimBLECharacteristic *g_telemetryChar = nullptr;
static volatile bool g_encrypted = false;
static volatile bool g_wasBondedAtConnect = false;
static uint32_t g_connectMs = 0;
static volatile bool g_pendingReject = false;
static NimBLEAddress g_rejectAddress;

enum SensorBit : uint8_t {
    S_BATTERY = 1 << 0,
    S_GPS = 1 << 1,
    S_ATTITUDE = 1 << 2,
    S_BARO = 1 << 3,
    S_VARIO = 1 << 4,
    S_MODE = 1 << 5,
};

static constexpr uint32_t UNENCRYPTED_GRACE_MS = 3000;  // free the slot for the real transmitter
static constexpr uint32_t SENSOR_REFRESH_MS = 1000;     // resend all sensors so the radio keeps them
static constexpr uint32_t RSSI_PERIOD_MS = 500;
static constexpr uint32_t DEBOUNCE_MS = 35;

class RxServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *server, NimBLEConnInfo &info) override {
        g_wasBondedAtConnect = NimBLEDevice::isBonded(info.getIdAddress());
        g_encrypted = false;
        g_connectMs = millis();
        g_rx->handleConnect(info.getConnHandle());
        // Ask for 7.5-15 ms; the transmitter (central) makes the final choice.
        server->updateConnParams(info.getConnHandle(), 6, 12, 0, 100);
    }
    void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override {
        g_encrypted = false;
        g_rx->handleDisconnect();
    }
    void onAuthenticationComplete(NimBLEConnInfo &info) override {
        const NimBLEAddress id = info.getIdAddress();
        g_rx->handleAuthentication(info.isEncrypted() && info.isBonded(), !g_wasBondedAtConnect,
                                   id.getVal(), id.getType());
    }
};

class RcCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
        if (!g_encrypted) return;  // WRITE_ENC already enforces this; belt and braces
        const NimBLEAttValue v = c->getValue();
        g_rx->handleRcWrite(v.data(), v.size());
    }
};

bool BLEehRx::begin(const BLEehRxConfig &config) {
    config_ = config;
    g_rx = this;
    for (uint8_t i = 0; i < NUM_CHANNELS; ++i) failsafeValues_[i] = CHANNEL_MID;

    if (config_.pairButtonPin >= 0)
        pinMode(config_.pairButtonPin, config_.pairButtonActiveLow ? INPUT_PULLUP : INPUT_PULLDOWN);
    if (config_.statusLedPin >= 0) pinMode(config_.statusLedPin, OUTPUT);

    NimBLEDevice::init(config_.name);
    NimBLEDevice::setSecurityAuth(true, false, true);  // bonding, Just Works, LE Secure Connections
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEDevice::setMTU(64);
    NimBLEDevice::setPower(config_.txPowerDbm);

    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(new RxServerCallbacks());
    g_server->advertiseOnDisconnect(true);

    NimBLEService *svc = g_server->createService(SERVICE_UUID);
    NimBLECharacteristic *rc = svc->createCharacteristic(
        RC_UUID, NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);
    rc->setCallbacks(new RcCallbacks());
    g_telemetryChar = svc->createCharacteristic(
        TELEMETRY_UUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC);
    NimBLECharacteristic *capsChar = svc->createCharacteristic(CAPS_UUID, NIMBLE_PROPERTY::READ);

    Capabilities caps;
    caps.features = FEATURE_FAILSAFE_FLAG | FEATURE_LINK_TELEMETRY;
    for (uint8_t t = TELEM_LINK; t <= TELEM_FLIGHT_MODE; ++t) caps.telemetryTypes |= 1u << t;
    strncpy(caps.name, config_.name, CAPS_NAME_MAX);
    uint8_t capsBuf[CAPS_HEADER_LEN + CAPS_NAME_MAX];
    capsChar->setValue(capsBuf, encodeCapabilities(caps, capsBuf, sizeof(capsBuf)));
    g_server->start();

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(SERVICE_UUID);
    adv->setMinInterval(32);  // 20-30 ms: fast reconnect after resets / signal loss
    adv->setMaxInterval(48);
    NimBLEAdvertisementData scanResponse;
    scanResponse.setName(config_.name);
    adv->setScanResponseData(scanResponse);
    adv->enableScanResponse(true);

    setPairing(config_.pairWhenUnbound && NimBLEDevice::getNumBonds() == 0);  // starts advertising
    return true;
}

bool BLEehRx::isBonded() const { return NimBLEDevice::getNumBonds() > 0; }

void BLEehRx::startPairing() { setPairing(true); }

void BLEehRx::clearPairing() {
    if (connected_) g_server->disconnect(connHandle_);
    NimBLEDevice::deleteAllBonds();
    setPairing(true);
}

void BLEehRx::setPairing(bool on) {
    pairing_ = on;
    pairingStartMs_ = millis();
    // A pairing window only matters to a new transmitter; drop the current link
    // so the receiver advertises with the pairing flag.
    if (on && connected_) g_server->disconnect(connHandle_);
    updateAdvertising();
}

void BLEehRx::updateAdvertising() {
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->stop();
    uint8_t m[ADV_MANUFACTURER_LEN];
    encodeAdvManufacturer(pairing_ ? ADV_FLAG_PAIRING : 0, m);
    adv->setManufacturerData(m, sizeof(m));
    adv->setDiscoverableMode(pairing_ ? BLE_GAP_DISC_MODE_GEN : BLE_GAP_DISC_MODE_NON);
    if (!connected_) adv->start();
}

void BLEehRx::handleConnect(uint16_t connHandle) {
    connHandle_ = connHandle;
    connected_ = true;
}

void BLEehRx::handleDisconnect() {
    connected_ = false;
    connHandle_ = 0xFFFF;
}

void BLEehRx::handleAuthentication(bool encryptedAndBonded, bool newPeer,
                                       const uint8_t idAddress[6], uint8_t idType) {
    if (!encryptedAndBonded) {
        pendingDisconnect_ = true;
        return;
    }
    if (pairing_) {
        // Keep only this transmitter's bond (one transmitter per receiver, like ELRS).
        memcpy(keepAddress_, idAddress, 6);
        keepType_ = idType;
        pendingBondCleanup_ = true;
        g_encrypted = true;
    } else if (newPeer) {
        // Somebody bonded outside the pairing window: undo it.
        g_rejectAddress = NimBLEAddress(idAddress, idType);
        g_pendingReject = true;
        pendingDisconnect_ = true;
    } else {
        g_encrypted = true;
    }
}

void BLEehRx::handleRcWrite(const uint8_t *data, size_t len) {
    RcFrame f;
    if (!decodeRc(data, len, f)) return;
    const uint32_t now = millis();
    portENTER_CRITICAL(&mux_);
    frame_ = f;
    haveFrame_ = true;
    newFrame_ = true;
    lastFrameMs_ = now;
    lq_.received(f.sequence);
    portEXIT_CRITICAL(&mux_);
}

bool BLEehRx::newFrame() {
    portENTER_CRITICAL(&mux_);
    const bool n = newFrame_;
    newFrame_ = false;
    portEXIT_CRITICAL(&mux_);
    return n;
}

uint32_t BLEehRx::frameAgeMs() const {
    portENTER_CRITICAL(&mux_);
    const bool have = haveFrame_;
    const uint32_t last = lastFrameMs_;
    portEXIT_CRITICAL(&mux_);
    return have ? millis() - last : UINT32_MAX;
}

bool BLEehRx::isFailsafe() const {
    if (!connected_ || pairing_) return true;
    portENTER_CRITICAL(&mux_);
    const bool have = haveFrame_;
    const uint32_t last = lastFrameMs_;
    const uint8_t flags = frame_.flags;
    portEXIT_CRITICAL(&mux_);
    if (!have || millis() - last > config_.failsafeTimeoutMs) return true;
    return !(flags & RC_FLAG_SOURCE_FRESH);  // transmitter lost the radio's stream
}

uint16_t BLEehRx::channel(uint8_t index) const {
    if (index >= NUM_CHANNELS) return CHANNEL_MID;
    portENTER_CRITICAL(&mux_);
    const bool have = haveFrame_;
    const uint16_t last = frame_.channels[index];
    portEXIT_CRITICAL(&mux_);
    if (!isFailsafe()) return last;
    if (config_.failsafeMode == BLEehFailsafe::Hold && have) return last;
    return failsafeValues_[index];
}

uint16_t BLEehRx::channelMicros(uint8_t index) const { return channelToMicros(channel(index)); }
float BLEehRx::channelNormalized(uint8_t index) const { return channelToNormalized(channel(index)); }

void BLEehRx::setFailsafeValue(uint8_t index, uint16_t raw) {
    if (index < NUM_CHANNELS) failsafeValues_[index] = raw;
}

uint8_t BLEehRx::linkQuality() const {
    if (frameAgeMs() > config_.failsafeTimeoutMs) return 0;
    portENTER_CRITICAL(&mux_);
    const uint8_t lq = lq_.percent();
    portEXIT_CRITICAL(&mux_);
    return lq;
}

void BLEehRx::setBattery(uint16_t millivolts, uint16_t currentDa, uint32_t capacityMah,
                             uint8_t remainingPct) {
    battery_.millivolts = millivolts;
    battery_.currentDa = currentDa;
    battery_.capacityMah = capacityMah;
    battery_.remainingPct = remainingPct;
    present_ |= S_BATTERY;
    dirty_ |= S_BATTERY;
}

void BLEehRx::setGps(int32_t latitude, int32_t longitude, uint16_t groundspeed,
                         uint16_t heading, uint16_t altitude, uint8_t satellites) {
    gps_ = {latitude, longitude, groundspeed, heading, altitude, satellites};
    present_ |= S_GPS;
    dirty_ |= S_GPS;
}

void BLEehRx::setAttitude(int16_t pitch, int16_t roll, int16_t yaw) {
    attitude_ = {pitch, roll, yaw};
    present_ |= S_ATTITUDE;
    dirty_ |= S_ATTITUDE;
}

void BLEehRx::setBaroAltitude(int32_t altitudeCm) {
    baroCm_ = altitudeCm;
    present_ |= S_BARO;
    dirty_ |= S_BARO;
}

void BLEehRx::setVario(int16_t verticalSpeedCms) {
    varioCms_ = verticalSpeedCms;
    present_ |= S_VARIO;
    dirty_ |= S_VARIO;
}

void BLEehRx::setFlightMode(const char *mode) {
    if (strncmp(flightMode_, mode, FLIGHT_MODE_MAX) == 0 && (present_ & S_MODE)) return;
    strncpy(flightMode_, mode, FLIGHT_MODE_MAX);
    flightMode_[FLIGHT_MODE_MAX] = '\0';
    present_ |= S_MODE;
    dirty_ |= S_MODE;
}

void BLEehRx::pollPairButton(uint32_t now) {
    if (config_.pairButtonPin < 0) return;
    const bool pressed = (digitalRead(config_.pairButtonPin) == LOW) == config_.pairButtonActiveLow;
    if (pressed != btnLast_) {
        btnLast_ = pressed;
        btnChangedMs_ = now;
    }
    if (pressed != btnStable_ && now - btnChangedMs_ >= DEBOUNCE_MS) {
        btnStable_ = pressed;
        if (pressed) {
            if (clicks_ == 0 || now - firstClickMs_ > config_.pairClickWindowMs) {
                clicks_ = 0;
                firstClickMs_ = now;
            }
            if (++clicks_ >= config_.pairClicks) {
                clicks_ = 0;
                setPairing(true);
            }
        }
    }
    if (clicks_ && now - firstClickMs_ > config_.pairClickWindowMs) clicks_ = 0;
}

void BLEehRx::sendTelemetry(uint32_t now) {
    if (!connected_ || !g_encrypted || now - lastTelemMs_ < config_.telemetryPeriodMs) return;
    lastTelemMs_ = now;
    if (now - lastFullRefreshMs_ >= SENSOR_REFRESH_MS) {
        lastFullRefreshMs_ = now;
        dirty_ |= present_;
    }

    uint8_t buf[64];
    const uint16_t mtu = g_server->getPeerMTU(connHandle_);
    const size_t capacity = (mtu > 3 && mtu - 3u < sizeof(buf)) ? mtu - 3u : sizeof(buf);
    TelemetryWriter w(buf, capacity, telemSeq_++);

    LinkTelemetry link;
    portENTER_CRITICAL(&mux_);
    link.ackSequence = frame_.sequence;
    portEXIT_CRITICAL(&mux_);
    link.uplinkLq = linkQuality();
    link.uplinkRssi = rssi_;
    link.flags = isFailsafe() ? LINK_FLAG_FAILSAFE : 0;
    w.addLink(link);
    // Sensors that do not fit stay dirty and go in the next notification.
    if ((dirty_ & S_BATTERY) && w.addBattery(battery_)) dirty_ &= ~S_BATTERY;
    if ((dirty_ & S_MODE) && w.addFlightMode(flightMode_)) dirty_ &= ~S_MODE;
    if ((dirty_ & S_GPS) && w.addGps(gps_)) dirty_ &= ~S_GPS;
    if ((dirty_ & S_ATTITUDE) && w.addAttitude(attitude_)) dirty_ &= ~S_ATTITUDE;
    if ((dirty_ & S_BARO) && w.addBaroAltitude(baroCm_)) dirty_ &= ~S_BARO;
    if ((dirty_ & S_VARIO) && w.addVario(varioCms_)) dirty_ &= ~S_VARIO;
    g_telemetryChar->notify(buf, w.length());
}

void BLEehRx::updateLed(uint32_t now) {
    if (config_.statusLedPin < 0) return;
    bool on;
    if (pairing_) on = (now / 100) % 2;
    else if (!isFailsafe()) on = true;
    else on = (now / 500) % 2;
    digitalWrite(config_.statusLedPin, on != config_.statusLedActiveLow ? HIGH : LOW);
}

void BLEehRx::update() {
    const uint32_t now = millis();
    pollPairButton(now);

    if (pairing_ && config_.pairingTimeoutMs && now - pairingStartMs_ > config_.pairingTimeoutMs)
        setPairing(false);

    if (pendingBondCleanup_) {
        pendingBondCleanup_ = false;
        const NimBLEAddress keep(keepAddress_, keepType_);
        for (int i = NimBLEDevice::getNumBonds() - 1; i >= 0; --i) {
            const NimBLEAddress a = NimBLEDevice::getBondedAddress(i);
            if (!(a == keep)) NimBLEDevice::deleteBond(a);
        }
        pairing_ = false;
        updateAdvertising();  // clears the pairing flag for the next reconnect
    }
    if (pendingDisconnect_) {
        pendingDisconnect_ = false;
        if (connected_) g_server->disconnect(connHandle_);
    }
    if (g_pendingReject) {
        g_pendingReject = false;
        NimBLEDevice::deleteBond(g_rejectAddress);
    }
    if (connected_ && !g_encrypted && now - g_connectMs > UNENCRYPTED_GRACE_MS)
        g_server->disconnect(connHandle_);

    if (connected_ && now - lastRssiMs_ >= RSSI_PERIOD_MS) {
        lastRssiMs_ = now;
        int8_t r;
        if (ble_gap_conn_rssi(connHandle_, &r) == 0) rssi_ = r;
    }

    const bool fs = isFailsafe();
    if (fs != failsafe_) {
        failsafe_ = fs;
        if (onFailsafeChanged) onFailsafeChanged(fs);
    }
    sendTelemetry(now);
    updateLed(now);
}
