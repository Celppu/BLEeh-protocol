#include "BLEehTx.h"
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <string.h>

using namespace bleeh;

static NimBLEClient *g_client = nullptr;
static NimBLERemoteCharacteristic *g_rcChar = nullptr;
static BLEehTx *g_tx = nullptr;

static constexpr uint32_t CONNECT_TIMEOUT_MS = 1000;
static constexpr uint32_t RETRY_DELAY_MS = 100;
static constexpr uint32_t PAIR_SCAN_MS = 1500;
static constexpr uint32_t TELEMETRY_FRESH_MS = 1000;
static constexpr uint32_t RSSI_PERIOD_MS = 500;
// 7.5 ms connection interval (BLE minimum), 500 ms supervision timeout so a
// lost link is detected quickly and reconnection starts.
static constexpr uint16_t CONN_INTERVAL = 6;      // x1.25 ms
static constexpr uint16_t SUPERVISION_TIMEOUT = 50;  // x10 ms

void BLEehTx::begin() {
    if (active_) return;
    g_tx = this;
    loadPeer();
    if (!g_client) {
        g_client = NimBLEDevice::createClient();
        g_client->setConnectionParams(CONN_INTERVAL, CONN_INTERVAL, 0, SUPERVISION_TIMEOUT);
        g_client->setConnectTimeout(CONNECT_TIMEOUT_MS);
    }
    if (!task_) xTaskCreate(taskEntry, "bleeh_tx", 6144, this, 2, &task_);
    active_ = true;
    xTaskNotifyGive(task_);
}

void BLEehTx::end() {
    active_ = false;
    pairing_ = false;
    if (task_) xTaskNotifyGive(task_);
}

void BLEehTx::sendChannels(const uint16_t channels[NUM_CHANNELS], bool sourceFresh) {
    portENTER_CRITICAL(&mux_);
    memcpy(pendingChannels_, channels, sizeof(pendingChannels_));
    pendingFresh_ = sourceFresh;
    pendingValid_ = true;
    portEXIT_CRITICAL(&mux_);
    if (task_) xTaskNotifyGive(task_);
}

void BLEehTx::startPairing(uint32_t durationMs) {
    pairingUntilMs_ = millis() + durationMs;
    pairing_ = true;
    if (task_) xTaskNotifyGive(task_);
}

void BLEehTx::forgetReceiver() {
    forget_ = true;
    if (task_) xTaskNotifyGive(task_);
}

bool BLEehTx::receiverAddress(uint8_t address[6], uint8_t &type) {
    if (!peerLoaded_) loadPeer();
    if (!hasPeer_) return false;
    memcpy(address, peer_, 6);
    type = peerType_;
    return true;
}

BLEehTxStatus BLEehTx::status() const {
    BLEehTxStatus s;
    s.active = active_;
    s.hasReceiver = hasPeer_;
    s.pairing = pairing_;
    portENTER_CRITICAL(&mux_);
    s.connected = connected_;
    s.caps = caps_;
    s.telemetryFresh = lastTelemetryMs_ && millis() - lastTelemetryMs_ < TELEMETRY_FRESH_MS;
    s.link = link_;
    s.hasBattery = hasBattery_;
    s.battery = sensors_.battery;
    s.downlinkRssi = downlinkRssi_;
    s.downlinkLq = s.telemetryFresh ? downlinkLq_.percent() : 0;
    s.rcSent = rcSent_;
    s.rcFailed = rcFailed_;
    portEXIT_CRITICAL(&mux_);
    return s;
}

uint8_t BLEehTx::takeSensors(BLEehSensors &out) {
    portENTER_CRITICAL(&mux_);
    out = sensors_;
    const uint8_t updated = sensorsUpdated_;
    sensorsUpdated_ = 0;
    portEXIT_CRITICAL(&mux_);
    return updated;
}

void BLEehTx::handleTelemetry(const uint8_t *data, size_t len) {
    TelemetryPacket p;
    if (!decodeTelemetry(data, len, p)) return;
    const uint32_t now = millis();
    portENTER_CRITICAL(&mux_);
    lastTelemetryMs_ = now;
    downlinkLq_.received(p.sequence);
    if (p.hasLink) link_ = p.link;
    if (p.hasBattery) {
        sensors_.battery = p.battery;
        sensorsUpdated_ |= BLEEH_SENSOR_BATTERY;
        hasBattery_ = true;
    }
    if (p.hasGps) { sensors_.gps = p.gps; sensorsUpdated_ |= BLEEH_SENSOR_GPS; }
    if (p.hasAttitude) { sensors_.attitude = p.attitude; sensorsUpdated_ |= BLEEH_SENSOR_ATTITUDE; }
    if (p.hasBaroAltitude) { sensors_.baroAltitudeCm = p.baroAltitudeCm; sensorsUpdated_ |= BLEEH_SENSOR_BARO; }
    if (p.hasVario) { sensors_.varioCms = p.varioCms; sensorsUpdated_ |= BLEEH_SENSOR_VARIO; }
    if (p.hasFlightMode) {
        memcpy(sensors_.flightMode, p.flightMode, sizeof(sensors_.flightMode));
        sensorsUpdated_ |= BLEEH_SENSOR_FLIGHT_MODE;
    }
    portEXIT_CRITICAL(&mux_);
}

void BLEehTx::loadPeer() {
    Preferences prefs;
    prefs.begin("bleeh_tx", true);
    hasPeer_ = prefs.getBytes("peer", peer_, sizeof(peer_)) == sizeof(peer_);
    peerType_ = prefs.getUChar("ptype", 0);
    prefs.end();
    peerLoaded_ = true;
}

void BLEehTx::savePeer() {
    Preferences prefs;
    prefs.begin("bleeh_tx", false);
    if (hasPeer_) {
        prefs.putBytes("peer", peer_, sizeof(peer_));
        prefs.putUChar("ptype", peerType_);
    } else {
        prefs.remove("peer");
        prefs.remove("ptype");
    }
    prefs.end();
}

void BLEehTx::dropLink() {
    portENTER_CRITICAL(&mux_);
    connected_ = false;
    portEXIT_CRITICAL(&mux_);
    g_rcChar = nullptr;
    if (g_client->isConnected()) g_client->disconnect();
}

bool BLEehTx::connectAndSetup() {
    const NimBLEAddress addr(peer_, peerType_);
    if (!g_client->connect(addr, true, false, true)) return false;
    // Encrypt with the stored bond (or create it while pairing).
    if (!g_client->secureConnection()) {
        Serial.println("BLEeh: encryption failed (receiver lost its bond? pair again)");
        g_client->disconnect();
        return false;
    }
    NimBLERemoteService *svc = g_client->getService(SERVICE_UUID);
    NimBLERemoteCharacteristic *rc = svc ? svc->getCharacteristic(RC_UUID) : nullptr;
    NimBLERemoteCharacteristic *telem = svc ? svc->getCharacteristic(TELEMETRY_UUID) : nullptr;
    NimBLERemoteCharacteristic *capsChar = svc ? svc->getCharacteristic(CAPS_UUID) : nullptr;
    if (!rc || !telem || !capsChar) {
        Serial.println("BLEeh: receiver is missing the BLEeh service");
        g_client->disconnect();
        return false;
    }
    if (g_client->getMTU() < MIN_ATT_MTU) {
        Serial.printf("BLEeh: ATT MTU %u < %u, cannot send RC frames\n",
                      g_client->getMTU(), MIN_ATT_MTU);
        g_client->disconnect();
        return false;
    }
    Capabilities caps;
    const NimBLEAttValue v = capsChar->readValue();
    if (!decodeCapabilities(v.data(), v.size(), caps)) {
        g_client->disconnect();
        return false;
    }
    if (caps.protocolVersion != PROTOCOL_VERSION)
        Serial.printf("BLEeh: receiver speaks protocol v%u, this transmitter v%u\n",
                      caps.protocolVersion, PROTOCOL_VERSION);
    if (!telem->subscribe(true, [](NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
            if (g_tx) g_tx->handleTelemetry(data, len);
        })) {
        g_client->disconnect();
        return false;
    }
    g_rcChar = rc;
    portENTER_CRITICAL(&mux_);
    caps_ = caps;
    connected_ = true;
    downlinkLq_.reset();
    portEXIT_CRITICAL(&mux_);
    Serial.printf("BLEeh: connected to \"%s\" (v%u, features 0x%08lx, MTU %u)\n",
                  caps.name, caps.protocolVersion, (unsigned long)caps.features, g_client->getMTU());
    return true;
}

void BLEehTx::runPairing() {
    dropLink();
    if ((int32_t)(millis() - pairingUntilMs_) > 0) {
        pairing_ = false;
        Serial.println("BLEeh: pairing timed out");
        return;
    }
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(false);
    scan->setInterval(30);
    scan->setWindow(30);
    NimBLEScanResults results = scan->getResults(PAIR_SCAN_MS, false);
    const NimBLEAdvertisedDevice *best = nullptr;
    for (int i = 0; i < results.getCount(); ++i) {
        const NimBLEAdvertisedDevice *d = results.getDevice(i);
        if (!d->isAdvertisingService(NimBLEUUID(SERVICE_UUID))) continue;
        uint8_t version, flags;
        bool pairingFlag = false;
        for (uint8_t m = 0; m < d->getManufacturerDataCount(); ++m) {
            const std::string md = d->getManufacturerData(m);
            if (decodeAdvManufacturer((const uint8_t *)md.data(), md.size(), version, flags))
                pairingFlag = flags & ADV_FLAG_PAIRING;
        }
        if (pairingFlag && (!best || d->getRSSI() > best->getRSSI())) best = d;
    }
    if (!best) {
        scan->clearResults();
        return;  // keep scanning until the pairing window ends
    }
    const NimBLEAddress found = best->getAddress();
    Serial.printf("BLEeh: pairing with %s (RSSI %d)\n", found.toString().c_str(), best->getRSSI());
    scan->clearResults();

    if (hasPeer_) NimBLEDevice::deleteBond(NimBLEAddress(peer_, peerType_));
    memcpy(peer_, found.getVal(), 6);
    peerType_ = found.getType();
    if (!connectAndSetup()) return;  // retry while the window is open
    hasPeer_ = true;
    savePeer();
    pairing_ = false;
    Serial.println("BLEeh: receiver bonded and saved");
}

void BLEehTx::taskEntry(void *self) { static_cast<BLEehTx *>(self)->taskLoop(); }

void BLEehTx::taskLoop() {
    uint8_t frame[RC_FRAME_LEN];
    for (;;) {
        if (forget_) {
            forget_ = false;
            dropLink();
            if (hasPeer_) NimBLEDevice::deleteBond(NimBLEAddress(peer_, peerType_));
            hasPeer_ = false;
            savePeer();
        }
        if (!active_) {
            dropLink();
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (pairing_) {
            runPairing();
            continue;
        }
        if (!hasPeer_) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
            continue;
        }
        if (!g_client->isConnected() || !connected_) {
            // Reconnect forever: survives receiver resets and signal loss.
            dropLink();
            if (!connectAndSetup()) vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            continue;
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        RcFrame f;
        bool have;
        portENTER_CRITICAL(&mux_);
        have = pendingValid_;
        pendingValid_ = false;
        memcpy(f.channels, pendingChannels_, sizeof(f.channels));
        f.flags = pendingFresh_ ? RC_FLAG_SOURCE_FRESH : 0;
        portEXIT_CRITICAL(&mux_);
        if (have && g_rcChar) {
            f.sequence = ++sequence_;
            encodeRc(f, frame);
            const bool ok = g_rcChar->writeValue(frame, sizeof(frame), false);
            portENTER_CRITICAL(&mux_);
            ok ? ++rcSent_ : ++rcFailed_;
            portEXIT_CRITICAL(&mux_);
        }
        const uint32_t now = millis();
        if (now - lastRssiMs_ >= RSSI_PERIOD_MS) {
            lastRssiMs_ = now;
            const int r = g_client->getRssi();
            portENTER_CRITICAL(&mux_);
            if (r != 0) downlinkRssi_ = (int8_t)r;
            portEXIT_CRITICAL(&mux_);
        }
    }
}
