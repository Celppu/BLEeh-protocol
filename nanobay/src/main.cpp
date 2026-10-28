#include <Arduino.h>
#include <CRSFDude.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <NimBLEServer.h>
#include <NimBLEService.h>
#include <NimBLECharacteristic.h>
#include <NimBLEAdvertising.h>
#include <Preferences.h>
#include <BLEehTx.h>
#include "config.h"
#include "ui.h"
#include <driver/gpio.h>
#include <soc/soc_caps.h>
#ifndef SOC_UART_HP_NUM
#error "SOC_UART_HP_NUM missing: check the BAY PROBE UART selection for this ESP-IDF"
#endif

// ----------- CRSF: handled by the ESP32-C3 library (inverted, half-duplex, handshake) -----------
static CRSFDude crsf;
static uint16_t rcChannels[16] = {0};  // loop() snapshot
static uint32_t lastCrsfMs = 0;
static uint32_t frameCount = 0;

// CRSF runs in its own task so EdgeTX Device Pings are answered within ~1 ms.
// A late reply (e.g. while loop() blocks on OLED I2C) makes EdgeTX halt CRSF
// output until the radio is power-cycled.
static portMUX_TYPE crsfMux = portMUX_INITIALIZER_UNLOCKED;
static uint16_t sharedChannels[16] = {0};
static uint32_t sharedLastCrsfMs = 0;
static uint32_t sharedFrameCount = 0;

// ----------- BLEeh (BLEeh mode): BLE central to a BLEeh receiver -----------
static BLEehTx bleehTx;
static volatile bool bleehActive = false;
static volatile uint8_t txPowerIndex = TX_POWER_DEFAULT;

// Sends at most one CRSF telemetry frame to EdgeTX. Called right after an RC
// frame arrives, which is the radio's reply window on the half-duplex line.
// EdgeTX needs link statistics before it shows any other sensor.
static void forwardTelemetryToRadio(uint32_t now) {
    static uint32_t lastLinkStatsMs = 0;
    static uint8_t pending = 0;
    static BLEehSensors sensors;
    const BLEehTxStatus s = bleehTx.status();
    if (!s.connected || !s.telemetryFresh) return;
    if (now - lastLinkStatsMs >= BLEEH_LINK_STATS_MS) {
        lastLinkStatsMs = now;
        const uint8_t upRssi = (uint8_t)(-s.link.uplinkRssi);    // CRSF: positive -dBm
        const uint8_t downRssi = (uint8_t)(-s.downlinkRssi);
        crsf.sendLinkStats(upRssi, upRssi, s.link.uplinkLq, 0, 0, 0,
                           TX_POWERS[txPowerIndex].crsfPowerIndex, downRssi, s.downlinkLq, 0);
        return;
    }
    pending |= bleehTx.takeSensors(sensors);
    if (pending & BLEEH_SENSOR_BATTERY) {
        pending &= ~BLEEH_SENSOR_BATTERY;
        const auto &b = sensors.battery;
        crsf.sendBattery((b.millivolts + 50) / 100, b.currentDa, b.capacityMah, b.remainingPct);
    } else if (pending & BLEEH_SENSOR_FLIGHT_MODE) {
        pending &= ~BLEEH_SENSOR_FLIGHT_MODE;
        crsf.sendFlightMode(sensors.flightMode);
    } else if (pending & BLEEH_SENSOR_GPS) {
        pending &= ~BLEEH_SENSOR_GPS;
        const auto &g = sensors.gps;
        crsf.sendGPS(g.latitude, g.longitude, g.groundspeed, g.heading, g.altitude, g.satellites);
    } else if (pending & BLEEH_SENSOR_ATTITUDE) {
        pending &= ~BLEEH_SENSOR_ATTITUDE;
        crsf.sendAttitude(sensors.attitude.pitch, sensors.attitude.roll, sensors.attitude.yaw);
    } else if (pending & BLEEH_SENSOR_BARO) {
        pending &= ~BLEEH_SENSOR_BARO;
        crsf.sendBaroAltitude(sensors.baroAltitudeCm);
    } else if (pending & BLEEH_SENSOR_VARIO) {
        pending &= ~BLEEH_SENSOR_VARIO;
        crsf.sendVario(sensors.varioCms);
    }
}

static void crsfTask(void *) {
    for (;;) {
        if (crsf.update()) {
            uint16_t ch[16];
            for (int i=0; i<16; ++i) ch[i] = crsf.getChannel(i);
            const uint32_t t = millis();
            portENTER_CRITICAL(&crsfMux);
            memcpy(sharedChannels, ch, sizeof(ch));
            sharedLastCrsfMs = t;
            ++sharedFrameCount;
            portEXIT_CRITICAL(&crsfMux);
            if (bleehActive) {
                bleehTx.sendChannels(ch, true);  // every radio frame, non-blocking
                forwardTelemetryToRadio(t);
            }
        }
        vTaskDelay(1);
    }
}

// ----------- BLE -----------
static NimBLEServer *server = nullptr;
static NimBLECharacteristic *hidInput = nullptr;
static NimBLECharacteristic *rawInput = nullptr;
static volatile bool bleConnected = false;
static bool pairingMode = false;
static volatile bool hidEnabled = true;
static BridgeMode currentMode = BridgeMode::Gamepad;
static UiController ui;
static uint16_t sequence = 0;
static uint32_t lastReportMs = 0;
static Preferences prefs;          // NVS namespace "bridge"
static uint8_t bleRateIndex = 0;   // into BLE_RATES, persisted as "ble_rate"
static uint32_t lastLogMs = 0;
// Experimental passive bay pin probes; ONLY enabled in BAY PROBE mode.
// Raw edge counts do not determine baud rate or decode data and may undercount
// at high serial baud rates. Never drive these pins from firmware.
static volatile uint32_t extTxEdgeCount = 0;
static volatile uint32_t extRxEdgeCount = 0;
static uint32_t extTxEdgesPerSecond = 0;
static uint32_t extRxEdgesPerSecond = 0;
static uint32_t lastProbeMs = 0;
static bool pinProbeActive = false;
static void IRAM_ATTR onExtTxEdge() { extTxEdgeCount = extTxEdgeCount + 1; }
static void IRAM_ATTR onExtRxEdge() { extRxEdgeCount = extRxEdgeCount + 1; }

// CRSF sniffer on UART2 (RX only, TX pin unassigned): cycles D0/D8 through
// baud and polarity settings and counts CRC-valid CRSF frames, comparing RC
// channels with the D7 stream. UART1 belongs to CRSFDude; the ESP32-C3 has no
// UART2, so there the sniffer compiles to nothing (BAY PROBE is S3-only anyway).
#if SOC_UART_HP_NUM > 2
#define PROBE_UART Serial2
#endif
struct ProbeUartConfig { int gpio; const char *pin; uint32_t baud; bool invert; };
static const ProbeUartConfig PROBE_UART_CONFIGS[] = {
    {PROBE_EXT_TX_GPIO, "D0", 420000, false}, {PROBE_EXT_TX_GPIO, "D0", 420000, true},
    {PROBE_EXT_TX_GPIO, "D0", 115200, false}, {PROBE_EXT_TX_GPIO, "D0", 115200, true},
    {PROBE_EXT_RX_GPIO, "D8", 420000, false}, {PROBE_EXT_RX_GPIO, "D8", 420000, true},
    {PROBE_EXT_RX_GPIO, "D8", 115200, false}, {PROBE_EXT_RX_GPIO, "D8", 115200, true},
};
static constexpr unsigned PROBE_UART_CONFIG_COUNT = sizeof(PROBE_UART_CONFIGS) / sizeof(PROBE_UART_CONFIGS[0]);
static constexpr uint32_t PROBE_UART_DWELL_MS = 2000;
static unsigned probeUartIndex = 0;
static uint32_t probeUartStartMs = 0;
static uint8_t probeFrame[64];
static uint8_t probeFrameLen = 0;
struct ProbeUartStats {
    uint32_t bytes, frames, badCrc, rcFrames, rcMatch;
    uint8_t lastTypes[4];
    uint8_t typeCount;
    uint16_t lastCh1, lastCh2;
};
static ProbeUartStats probeStats;

static void startProbeUart(unsigned index) {
#ifdef PROBE_UART
    const ProbeUartConfig &c = PROBE_UART_CONFIGS[index];
    PROBE_UART.end();
    PROBE_UART.setRxBufferSize(4096);
    PROBE_UART.begin(c.baud, SERIAL_8N1, c.gpio, -1, c.invert);
    // UART may enable an RX pull; keep the bay line unbiased. (pinMode() would detach the UART.)
    gpio_pullup_dis((gpio_num_t)c.gpio);
    gpio_pulldown_dis((gpio_num_t)c.gpio);
#endif
    probeUartIndex = index;
    probeUartStartMs = millis();
    probeFrameLen = 0;
    memset(&probeStats, 0, sizeof(probeStats));
}

static void handleProbeFrame() {
    const uint8_t len = probeFrame[1];  // type + payload + crc
    // crsfCrc8() comes from CRSFDude crsf_protocol.h
    if (crsfCrc8(probeFrame + 2, (uint16_t)(len - 1)) != probeFrame[len + 1]) { ++probeStats.badCrc; return; }
    ++probeStats.frames;
    const uint8_t type = probeFrame[2];
    bool known = false;
    for (uint8_t i = 0; i < probeStats.typeCount; ++i) known |= probeStats.lastTypes[i] == type;
    if (!known && probeStats.typeCount < sizeof(probeStats.lastTypes)) probeStats.lastTypes[probeStats.typeCount++] = type;
    if (type == 0x16 && len == 24) {  // RC channels packed: 16 x 11 bits
        const uint8_t *p = probeFrame + 3;
        const uint16_t ch1 = (p[0] | (p[1] << 8)) & 0x7FF;
        const uint16_t ch2 = ((p[1] >> 3) | (p[2] << 5)) & 0x7FF;
        ++probeStats.rcFrames;
        probeStats.lastCh1 = ch1;
        probeStats.lastCh2 = ch2;
        portENTER_CRITICAL(&crsfMux);
        const uint16_t d7ch1 = sharedChannels[0];
        portEXIT_CRITICAL(&crsfMux);
        if (abs((int)ch1 - (int)d7ch1) <= 4) ++probeStats.rcMatch;
    }
}

static void pollProbeUart(uint32_t now) {
#ifdef PROBE_UART
    while (PROBE_UART.available()) {
        const uint8_t b = (uint8_t)PROBE_UART.read();
        ++probeStats.bytes;
        if (probeFrameLen == 0) {
            if (b == 0xC8 || b == 0xEE || b == 0xEA || b == 0xEC) probeFrame[probeFrameLen++] = b;
            continue;
        }
        if (probeFrameLen == 1 && (b < 2 || b > 62)) { probeFrameLen = 0; continue; }
        probeFrame[probeFrameLen++] = b;
        if (probeFrameLen >= 2 && probeFrameLen == probeFrame[1] + 2) {
            handleProbeFrame();
            probeFrameLen = 0;
        }
    }
#endif
    if ((uint32_t)(now - probeUartStartMs) < PROBE_UART_DWELL_MS) return;
    const ProbeUartConfig &c = PROBE_UART_CONFIGS[probeUartIndex];
    char types[24] = "";
    for (uint8_t i = 0; i < probeStats.typeCount; ++i)
        snprintf(types + strlen(types), sizeof(types) - strlen(types), "%02X ", probeStats.lastTypes[i]);
    Serial.printf("UART %s %6lu %s: bytes=%lu crsf_ok=%lu bad_crc=%lu rc=%lu rc_match_D7=%lu CH1=%u CH2=%u types=%s\n",
                  c.pin, (unsigned long)c.baud, c.invert ? "INV" : "NOR",
                  (unsigned long)probeStats.bytes, (unsigned long)probeStats.frames,
                  (unsigned long)probeStats.badCrc, (unsigned long)probeStats.rcFrames,
                  (unsigned long)probeStats.rcMatch, probeStats.lastCh1, probeStats.lastCh2, types);
    startProbeUart((probeUartIndex + 1) % PROBE_UART_CONFIG_COUNT);
}

static void startPinProbe() {
    if (pinProbeActive) return;
    if (!BAY_PROBE_SUPPORTED) {
        Serial.println("BAY PROBE disabled on C3: D0(GPIO2)/D8(GPIO8) are boot strapping pins. Use XIAO ESP32-S3.");
        return;
    }
    pinMode(PROBE_EXT_TX_GPIO, INPUT); // No pulls: never bias unknown bay signals
    pinMode(PROBE_EXT_RX_GPIO, INPUT);
    extTxEdgeCount = 0;
    extRxEdgeCount = 0;
    extTxEdgesPerSecond = extRxEdgesPerSecond = 0;
    lastProbeMs = millis();
    attachInterrupt(digitalPinToInterrupt(PROBE_EXT_TX_GPIO), onExtTxEdge, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PROBE_EXT_RX_GPIO), onExtRxEdge, CHANGE);
    pinProbeActive = true;
    startProbeUart(0);
    Serial.println("BAY PROBE active: D0 EXT_TX, D8 EXT_RX (INPUT ONLY; edges + UART2 CRSF sniffer)");
}
static void stopPinProbe() {
    if (!pinProbeActive) return;
#ifdef PROBE_UART
    PROBE_UART.end();
#endif
    detachInterrupt(digitalPinToInterrupt(PROBE_EXT_TX_GPIO));
    detachInterrupt(digitalPinToInterrupt(PROBE_EXT_RX_GPIO));
    pinMode(PROBE_EXT_TX_GPIO, INPUT);
    pinMode(PROBE_EXT_RX_GPIO, INPUT);
    pinProbeActive = false;
}
static void pollPinProbe(uint32_t now) {
    if (pinProbeActive) pollProbeUart(now);
    if (!pinProbeActive || (uint32_t)(now - lastProbeMs) < PROBE_SAMPLE_MS) return;
    uint32_t elapsed = now - lastProbeMs;
    lastProbeMs = now;
    noInterrupts();
    uint32_t a = extTxEdgeCount, b = extRxEdgeCount;
    extTxEdgeCount = 0;
    extRxEdgeCount = 0;
    interrupts();
    // Account for small variations in the sampling interval.
    extTxEdgesPerSecond = (uint32_t)((uint64_t)a * 1000u / elapsed);
    extRxEdgesPerSecond = (uint32_t)((uint64_t)b * 1000u / elapsed);
    Serial.printf("BAY EXT_TX(D0)=%d edges/s=%lu | EXT_RX(D8)=%d edges/s=%lu\n",
                  digitalRead(PROBE_EXT_TX_GPIO), (unsigned long)extTxEdgesPerSecond,
                  digitalRead(PROBE_EXT_RX_GPIO), (unsigned long)extRxEdgesPerSecond);
}


// Generic HID Gamepad: 16 digital buttons then eight 16-bit axes.
// HID axes = raw EdgeTX CH1..CH8, CH9..CH16 become buttons (threshold in config.h).
// All 16 full-resolution channel values are ALSO exposed through custom GATT.
static uint8_t reportMap[] = {
    0x05,0x01,       // Usage Page (Generic Desktop)
    0x09,0x05,       // Usage (Gamepad)
    0xA1,0x01,       // Collection (Application)
    0x85,0x01,       // Report ID (1)
    0x05,0x09,       // Usage Page (Buttons)
    0x19,0x01,       // Usage Minimum (Button 1)
    0x29,0x10,       // Usage Maximum (Button 16)
    0x15,0x00,       // Logical Minimum (0)
    0x25,0x01,       // Logical Maximum (1)
    0x75,0x01,       // Report Size (1)
    0x95,0x10,       // Report Count (16)
    0x81,0x02,       // Input (Data, Var, Abs)
    0x05,0x01,       // Usage Page (Generic Desktop)
    0x16,0x00,0x00,  // Logical Minimum (0)
    0x26,0xFF,0x7F,  // Logical Maximum (32767)
    0x75,0x10,       // Report Size (16 bits)
    0x95,0x08,       // Report Count (8 axes)
    0x09,0x30,       // X
    0x09,0x31,       // Y
    0x09,0x32,       // Z
    0x09,0x33,       // Rx
    0x09,0x34,       // Ry
    0x09,0x35,       // Rz
    0x09,0x36,       // Slider
    0x09,0x37,       // Dial
    0x81,0x02,       // Input (Data, Var, Abs)
    0xC0             // End Collection
};

static void requestConnInterval(uint16_t connHandle) {
    const BleRateOption &r = BLE_RATES[bleRateIndex];
    server->updateConnParams(connHandle, r.minIntervalUnits, r.maxIntervalUnits, 0,
                             BLE_SUPERVISION_TIMEOUT);
}

class BridgeServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *s, NimBLEConnInfo &info) override {
        (void)s;
        bleConnected = true;
        Serial.printf("BLE connected: %s\n", info.getAddress().toString().c_str());
        requestConnInterval(info.getConnHandle());
        // Keep advertising so a GATT client (python/monitor.py) can still find
        // the bridge while the OS HID driver holds a connection.
        if (hidEnabled) NimBLEDevice::getAdvertising()->start();
    }
    void onDisconnect(NimBLEServer *s, NimBLEConnInfo &info, int reason) override {
        (void)info;
        bleConnected = false;
        Serial.printf("BLE disconnected (reason %d)\n", reason);
        // Advertise again, allowing the paired host to reconnect.
        NimBLEDevice::getAdvertising()->setDiscoverableMode(
            pairingMode ? BLE_GAP_DISC_MODE_GEN : BLE_GAP_DISC_MODE_NON);
        if (hidEnabled) s->startAdvertising();
    }
    void onConnParamsUpdate(NimBLEConnInfo &info) override {
        Serial.printf("BLE conn params: interval=%.2fms latency=%u timeout=%ums\n",
                      info.getConnInterval() * 1.25f, info.getConnLatency(),
                      info.getConnTimeout() * 10u);
    }
    void onAuthenticationComplete(NimBLEConnInfo &info) override {
        Serial.printf("BLE auth: encrypted=%d bonded=%d\n",
                      info.isEncrypted(), info.isBonded());
        if (info.isBonded()) pairingMode = false;
    }
};

static uint32_t notifyFailures = 0;

class LogSubscribeCallbacks : public NimBLECharacteristicCallbacks {
public:
    explicit LogSubscribeCallbacks(const char *name) : name_(name) {}
    void onSubscribe(NimBLECharacteristic *, NimBLEConnInfo &info, uint16_t subValue) override {
        Serial.printf("GATT %s subscribe conn=%u value=%u\n", name_, info.getConnHandle(), subValue);
    }
private:
    const char *name_;
};

static void advertise(bool discoverable) {
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->setDiscoverableMode(discoverable ? BLE_GAP_DISC_MODE_GEN : BLE_GAP_DISC_MODE_NON);
    if (!adv->isAdvertising()) adv->start();
}

static void startBLE() {
    NimBLEDevice::init(BLE_DEVICE_NAME);
    NimBLEDevice::setSecurityAuth(true, false, true); // bond, no MITM (no display/keypad), LE secure connections
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEDevice::setMTU(64); // BLEeh RC frames need >= 28; gamepad reports <= 20

    server = NimBLEDevice::createServer();
    server->setCallbacks(new BridgeServerCallbacks());
    NimBLEHIDDevice *hid = new NimBLEHIDDevice(server);
    hid->setManufacturer("DIY Pocket Bridge");
    // Product ID is a DIY prototype identifier, NOT a real vendor-assigned VID/PID.
    hid->setPnp(0x02, 0x0000, 0x0001, 0x0100);
    hid->setHidInfo(0x00, 0x01);
    hid->setReportMap(reportMap, sizeof(reportMap));
    hidInput = hid->getInputReport(1);
    hid->setBatteryLevel(100);

    NimBLEService *rawService = server->createService(RC_SERVICE_UUID);
    rawInput = rawService->createCharacteristic(RC_CHAR_UUID,
                                                NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    uint8_t zeros[20] = {};
    rawInput->setValue(zeros, sizeof(zeros));
    rawInput->setCallbacks(new LogSubscribeCallbacks("raw"));
    hidInput->setCallbacks(new LogSubscribeCallbacks("hid"));
    server->start();

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->setName(BLE_DEVICE_NAME);
    adv->setAppearance(0x03C4); // HID Gamepad appearance
    adv->addServiceUUID("1812"); // HID service
    // Do not advertise 128-bit custom UUID: leave room for name + HID service.
    adv->enableScanResponse(true);

    // The BLEeh receiver's bond does not count as a paired PC.
    uint8_t rxAddr[6], rxType;
    int gamepadBonds = NimBLEDevice::getNumBonds();
    if (bleehTx.receiverAddress(rxAddr, rxType) && NimBLEDevice::isBonded(NimBLEAddress(rxAddr, rxType)))
        --gamepadBonds;
    pairingMode = (gamepadBonds == 0);
    advertise(pairingMode);
    Serial.printf("BLE started; saved bonds=%d; pairing=%d; rate=%s\n",
                  NimBLEDevice::getNumBonds(), pairingMode, BLE_RATES[bleRateIndex].label);
}

static void sendReports(bool fresh); // forward declaration

// Applies to advertising, scanning and existing connections (gamepad + BLEeh).
static void applyTxPower() {
    const int8_t dbm = TX_POWERS[txPowerIndex].dbm;
    NimBLEDevice::setPower(dbm);
    const esp_power_level_t level = static_cast<esp_power_level_t>(
        (dbm % 3 == 2 ? dbm + 1 : dbm) / 3 + ESP_PWR_LVL_N0);
    for (int h = ESP_BLE_PWR_TYPE_CONN_HDL0; h <= ESP_BLE_PWR_TYPE_CONN_HDL8; ++h)
        esp_ble_tx_power_set(static_cast<esp_ble_power_type_t>(h), level);
}

static void cycleTxPower() {
    txPowerIndex = (uint8_t)((txPowerIndex + 1) % TX_POWER_COUNT);
    prefs.putUChar("tx_pwr", txPowerIndex);
    applyTxPower();
    Serial.printf("TX power: %s, saved\n", TX_POWERS[txPowerIndex].label);
}

// BLEeh mode drives a BLEeh receiver over BLE; the gamepad side is
// disconnected and stops advertising while it runs.
static void selectMode(BridgeMode mode) {
    if (mode == currentMode) return;
    if (mode == BridgeMode::PinProbe && !BAY_PROBE_SUPPORTED) {
        Serial.println("Probe unavailable on C3 wiring: choose an S3 or use a logic analyzer before wiring bay pins.");
        return;
    }
    if (currentMode == BridgeMode::PinProbe) stopPinProbe();
    if (currentMode == BridgeMode::Bleeh) {
        bleehActive = false;
        bleehTx.end();
    }
    if (mode != BridgeMode::Gamepad) {
        if (bleConnected && hidEnabled) sendReports(false);
        hidEnabled = false; // do not restart BLE HID advertising on disconnect
        NimBLEDevice::getAdvertising()->stop();
        if (bleConnected) {
            for (uint16_t handle : server->getPeerDevices()) server->disconnect(handle);
        }
    } else {
        hidEnabled = true;
        advertise(pairingMode);
    }
    if (mode == BridgeMode::PinProbe) startPinProbe();
    if (mode == BridgeMode::Bleeh) {
        bleehTx.begin();
        bleehActive = true;
    }
    currentMode = mode;
    Serial.printf("Active mode: %s\n", mode == BridgeMode::Gamepad ? "GAMEPAD" :
                  (mode == BridgeMode::Bleeh ? "BLEeh" : "BAY PROBE (input only)"));
}

static void pairReceiver() {
    selectMode(BridgeMode::Bleeh);
    bleehTx.startPairing();
    Serial.println("BLEeh: pairing, triple-click the receiver's pairing button");
}

static void enterPairingMode() {
    // BOOT x3 / PAIR PC GAMEPAD always pairs the PC gamepad side.
    selectMode(BridgeMode::Gamepad);
    Serial.println("BOOT x3: PAIR MODE. Removing old PC bonds and advertising.");
    pairingMode = true;
    // If you reset a bond, forget/remove the old Bluetooth device on your PC as well.
    if (bleConnected) {
        for (uint16_t handle : server->getPeerDevices()) {
            server->disconnect(handle);
        }
        delay(20);
    }
    // Delete gamepad bonds but keep the BLEeh receiver's.
    uint8_t rxAddr[6], rxType;
    const bool haveRx = bleehTx.receiverAddress(rxAddr, rxType);
    for (int i = NimBLEDevice::getNumBonds() - 1; i >= 0; --i) {
        const NimBLEAddress a = NimBLEDevice::getBondedAddress(i);
        if (haveRx && a == NimBLEAddress(rxAddr, rxType)) continue;
        NimBLEDevice::deleteBond(a);
    }
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->stop();
    advertise(true);
}

static void cycleBleRate() {
    bleRateIndex = (uint8_t)((bleRateIndex + 1) % BLE_RATE_COUNT);
    prefs.putUChar("ble_rate", bleRateIndex);
    if (bleConnected) {
        for (uint16_t handle : server->getPeerDevices()) requestConnInterval(handle);
    }
    Serial.printf("BLE rate: %s (report every %lu ms), saved\n",
                  BLE_RATES[bleRateIndex].label,
                  (unsigned long)BLE_RATES[bleRateIndex].reportPeriodMs);
}

static void processPairButton() {
    // BOOT/ISP remains a separate triple-click pairing shortcut.
    static DebouncedButton boot(DEBOUNCE_MS);
    static uint32_t firstPressAt = 0;
    static uint8_t clicks = 0;
    const uint32_t now = millis();
    if (boot.update(digitalRead(PAIR_BUTTON_GPIO) != LOW, now)) {
        if (clicks == 0 || (uint32_t)(now - firstPressAt) > TRIPLE_PRESS_WINDOW_MS) {
            firstPressAt = now;
            clicks = 0;
        }
        if (++clicks >= 3) {
            clicks = 0;
            enterPairingMode();
        }
    }
    if (clicks && (uint32_t)(now - firstPressAt) > TRIPLE_PRESS_WINDOW_MS) clicks = 0;
}

static uint16_t rawToHid(uint16_t raw) {
    raw = constrain(raw, CRSF_RAW_MIN, CRSF_RAW_MAX);
    return (uint32_t)(raw - CRSF_RAW_MIN) * 32767UL /
           (CRSF_RAW_MAX - CRSF_RAW_MIN);
}

static void sendReports(bool fresh) {
    if (!bleConnected) return;
    uint16_t channels[16];
    for (unsigned i=0; i<16; ++i) channels[i] = fresh ? rcChannels[i] : CRSF_RAW_CENTER;

    // USB/BLE HID report ID is handled by HID characteristic (do not prepend 0x01).
    uint8_t hidReport[18] = {};
    uint16_t buttons = 0;
    for (int i=8; i<16; ++i)
        if (fresh && channels[i] > BUTTON_THRESHOLD) buttons |= (uint16_t)(1u << (i-8));
    hidReport[0] = buttons & 0xFF;
    hidReport[1] = buttons >> 8;
    for (int i=0; i<8; ++i) {
        const uint16_t v = rawToHid(channels[i]);
        hidReport[2 + i*2] = v & 0xFF;
        hidReport[3 + i*2] = v >> 8;
    }
    // notify(data, len) sends this buffer; notify() would send whatever value
    // the characteristic holds when NimBLE builds the packet, which may already
    // have been overwritten.
    hidInput->setValue(hidReport, sizeof(hidReport));
    if (!hidInput->notify(hidReport, sizeof(hidReport))) ++notifyFailures;

    // Custom GATT service: each 20-byte notification = <uint16 seq,
    // uint8 bank(0/1), uint8 flags, 8 * uint16 raw channels little-endian>.
    // Two notifications have the same seq and provide all 16 channels.
    ++sequence;
    for (uint8_t bank=0; bank<2; ++bank) {
        uint8_t pkt[20];
        pkt[0] = sequence & 0xFF;
        pkt[1] = sequence >> 8;
        pkt[2] = bank;
        pkt[3] = (fresh ? 1 : 0) | (pairingMode ? 2 : 0);
        for (unsigned i=0; i<8; ++i) {
            const uint16_t raw = channels[bank*8 + i];
            pkt[4+i*2] = raw & 0xFF;
            pkt[5+i*2] = raw >> 8;
        }
        rawInput->setValue(pkt, sizeof(pkt));
        if (!rawInput->notify(pkt, sizeof(pkt))) ++notifyFailures;
    }
}

void setup() {
    Serial.begin(115200);
    // USB CDC: drop log output instead of blocking loop() (~2 s per print)
    // when USB is plugged in but no terminal is reading.
    Serial.setTxTimeoutMs(0);
    delay(300);
    pinMode(PAIR_BUTTON_GPIO, INPUT_PULLUP);
    prefs.begin("bridge", false);
    bleRateIndex = prefs.getUChar("ble_rate", 0);
    if (bleRateIndex >= BLE_RATE_COUNT) bleRateIndex = 0;
    const uint8_t pwr = prefs.getUChar("tx_pwr", TX_POWER_DEFAULT);
    txPowerIndex = pwr < TX_POWER_COUNT ? pwr : TX_POWER_DEFAULT;
    ui.begin();
    Serial.println("RadioMaster Pocket CRSF -> BLE gamepad / BLEeh + OLED menu");
    // Initialize BLE, then CRSF immediately before loop() to service radio pings.
    startBLE();
    applyTxPower();
    crsf.begin(CRSF_GPIO, CRSF_BAUD);
    // Higher priority than loopTask (1) so OLED/BLE/serial work cannot delay it.
    xTaskCreate(crsfTask, "crsf", 4096, nullptr, 5, nullptr);
    lastReportMs = millis();
}

void loop() {
    processPairButton();
    portENTER_CRITICAL(&crsfMux);
    memcpy(rcChannels, sharedChannels, sizeof(rcChannels));
    lastCrsfMs = sharedLastCrsfMs;
    frameCount = sharedFrameCount;
    portEXIT_CRITICAL(&crsfMux);
    const uint32_t now = millis();
    if ((uint32_t)(now - lastReportMs) >= BLE_RATES[bleRateIndex].reportPeriodMs) {
        lastReportMs = now;
        const bool fresh = frameCount > 0 && (uint32_t)(now - lastCrsfMs) < CRSF_STALE_MS;
        if (hidEnabled) sendReports(fresh);
    }
    const bool crsfFresh = frameCount > 0 && (uint32_t)(now - lastCrsfMs) < CRSF_STALE_MS;
    static uint32_t lastStaleSendMs = 0;
    if (bleehActive && !crsfFresh && (uint32_t)(now - lastStaleSendMs) >= BLEEH_STALE_RESEND_MS) {
        lastStaleSendMs = now;
        bleehTx.sendChannels(rcChannels, false);  // receiver goes to failsafe immediately
    }
    const UiEvent event = ui.poll(now);
    if (event == UiEvent::ChooseGamepad) selectMode(BridgeMode::Gamepad);
    if (event == UiEvent::ChooseBleeh) selectMode(BridgeMode::Bleeh);
    if (event == UiEvent::ChoosePinProbe) selectMode(BridgeMode::PinProbe);
    if (event == UiEvent::Pair) enterPairingMode(); // same as BOOT x3
    if (event == UiEvent::PairReceiver) pairReceiver();
    if (event == UiEvent::CycleBleRate) cycleBleRate();
    if (event == UiEvent::CycleTxPower) cycleTxPower();
    pollPinProbe(now);
    // Hand the status to the OLED task; drawing and I2C happen there.
    UiStatus status;
    status.mode = currentMode;
    status.crsfFresh = crsfFresh;
    status.bleConnected = bleConnected;
    status.pairingMode = pairingMode;
    status.bleRateLabel = BLE_RATES[bleRateIndex].label;
    status.txPowerLabel = TX_POWERS[txPowerIndex].label;
    BLEehTxStatus bleehStatus;
    if (bleehActive) {
        bleehStatus = bleehTx.status();
        status.bleehConnected = bleehStatus.connected;
        status.bleehPairing = bleehStatus.pairing;
        status.bleehHasReceiver = bleehStatus.hasReceiver;
        status.bleehRxFailsafe = bleehStatus.link.flags & bleeh::LINK_FLAG_FAILSAFE;
        status.bleehUplinkLq = bleehStatus.link.uplinkLq;
        status.bleehUplinkRssi = bleehStatus.link.uplinkRssi;
        status.bleehDownlinkLq = bleehStatus.downlinkLq;
        status.bleehBatteryValid = bleehStatus.telemetryFresh && bleehStatus.hasBattery;
        status.bleehBatteryMv = bleehStatus.battery.millivolts;
    }
    status.ch1 = rcChannels[0];
    status.ch2 = rcChannels[1];
    status.extTxEdgesPerSecond = extTxEdgesPerSecond;
    status.extRxEdgesPerSecond = extRxEdgesPerSecond;
    if (pinProbeActive) {
        status.extTxLevel = digitalRead(PROBE_EXT_TX_GPIO);
        status.extRxLevel = digitalRead(PROBE_EXT_RX_GPIO);
    }
    ui.setStatus(status);
    if ((uint32_t)(now - lastLogMs) >= STATUS_LOG_PERIOD_MS) {
        lastLogMs = now;
        Serial.printf("CRSF frames=%lu replies=%lu CH1=%u CH2=%u BLE=%d bonds=%d freshness_ms=%lu notify_fail=%lu\n",
                      (unsigned long)frameCount, (unsigned long)crsf.txPacketCount,
                      rcChannels[0], rcChannels[1],
                      bleConnected, NimBLEDevice::getNumBonds(),
                      (unsigned long)(now - lastCrsfMs), (unsigned long)notifyFailures);
    }
    delay(1);
}
