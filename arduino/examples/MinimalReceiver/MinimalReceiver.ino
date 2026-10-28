// BLEeh minimal receiver: prints the first four channels and reports a
// battery voltage to the radio. Works on any ESP32 with BLE (not ESP32-S2).
//
// Pairing: a fresh board starts in pairing mode by itself. Later, triple-click
// the BOOT button to pair again, then choose PAIR RECEIVER on the transmitter.
#include <BLEehRx.h>

#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6)
static constexpr int BOOT_BUTTON = 9;
#else
static constexpr int BOOT_BUTTON = 0;
#endif

BLEehRx rx;

void setup() {
    Serial.begin(115200);

    BLEehRxConfig cfg;
    cfg.name = "My Robot";              // shown when pairing, max 20 chars
    cfg.pairButtonPin = BOOT_BUTTON;    // triple-click within 3 s to pair
    cfg.failsafeTimeoutMs = 250;        // signal-loss time
    cfg.failsafeMode = BLEehFailsafe::Preset;
    rx.setFailsafeValue(2, 172);        // CH3 (throttle) low on failsafe, others centre
    rx.begin(cfg);
}

void loop() {
    rx.update();  // call often: pairing button, failsafe, telemetry

    static uint32_t lastPrint = 0;
    if (millis() - lastPrint >= 200) {
        lastPrint = millis();
        Serial.printf("%s CH1=%d CH2=%d CH3=%d CH4=%d us  LQ=%u%%\n",
                      rx.isFailsafe() ? "FAILSAFE" : "OK      ",
                      rx.channelMicros(0), rx.channelMicros(1),
                      rx.channelMicros(2), rx.channelMicros(3), rx.linkQuality());
        rx.setBattery(7400, 0, 0, 80);  // 7.40 V, 80 % -> RxBt on the radio
    }
}
