// BLEeh example: differential-drive car on a dual H-bridge, with battery
// voltage telemetry back to the radio.
//
// Hardware (any XIAO ESP32-C3 / ESP32-S3; pins are XIAO D-numbers):
//   H-bridge with two PWM inputs per motor (DRV8833, TB6612 in IN/IN mode, L298N...)
//     left motor  IN1 = D1, IN2 = D3
//     right motor IN1 = D4, IN2 = D5
//   Battery: divider 100k (to battery +) / 33k (to GND), midpoint to A2 (D2).
//            33/(100+33) scales an 8.4 V 2S pack to 2.08 V (ADC max ~2.5 V with 11 dB).
//   Pairing: triple-click the BOOT button (no extra wiring).
//
// Radio mapping (EdgeTX default AETR):
//   CH1 = steering (right stick X), CH2 = throttle (right stick Y, spring-centred),
//   CH5 = arm switch (motors only run when CH5 is high).
// Failsafe: motors coast when the link or the radio is lost.
#include <Arduino.h>
#include <BLEehRx.h>

// ---- Pins ----
static constexpr int LEFT_IN1 = D1, LEFT_IN2 = D3;
static constexpr int RIGHT_IN1 = D4, RIGHT_IN2 = D5;
static constexpr int BATTERY_PIN = A2;
#if defined(CONFIG_IDF_TARGET_ESP32C3)
static constexpr int BOOT_BUTTON = 9;
#else
static constexpr int BOOT_BUTTON = 0;
#endif
#ifdef LED_BUILTIN
static constexpr int STATUS_LED = LED_BUILTIN;  // XIAO ESP32-S3: user LED, active low
#else
static constexpr int STATUS_LED = -1;
#endif

// ---- Channels (0-based) ----
static constexpr uint8_t CH_STEERING = 0;
static constexpr uint8_t CH_THROTTLE = 1;
static constexpr uint8_t CH_ARM = 4;

// ---- Tuning ----
static constexpr float DEADBAND = 0.04f;
static constexpr uint32_t PWM_HZ = 20000;   // above hearing range
static constexpr uint8_t PWM_BITS = 10;
static constexpr float DIVIDER_RATIO = (100.0f + 33.0f) / 33.0f;
static constexpr uint8_t BATTERY_CELLS = 2;
static constexpr float CELL_EMPTY_V = 3.3f, CELL_FULL_V = 4.2f;

static BLEehRx rx;

static void driveMotor(int in1, int in2, float speed) {
    const uint32_t maxDuty = (1u << PWM_BITS) - 1;
    const uint32_t duty = (uint32_t)(fabsf(speed) * maxDuty);
    if (speed > 0) {
        ledcWrite(in1, duty);
        ledcWrite(in2, 0);
    } else if (speed < 0) {
        ledcWrite(in1, 0);
        ledcWrite(in2, duty);
    } else {
        ledcWrite(in1, 0);  // both low = coast
        ledcWrite(in2, 0);
    }
}

static float applyDeadband(float v) {
    if (fabsf(v) < DEADBAND) return 0;
    return (v - copysignf(DEADBAND, v)) / (1 - DEADBAND);
}

static void stopMotors() {
    driveMotor(LEFT_IN1, LEFT_IN2, 0);
    driveMotor(RIGHT_IN1, RIGHT_IN2, 0);
}

static void updateBattery() {
    static uint32_t lastMs = 0;
    static float filteredMv = 0;
    if (millis() - lastMs < 100) return;
    lastMs = millis();
    const float mv = analogReadMilliVolts(BATTERY_PIN) * DIVIDER_RATIO;
    filteredMv = filteredMv == 0 ? mv : filteredMv + 0.1f * (mv - filteredMv);
    const float cellV = filteredMv / 1000.0f / BATTERY_CELLS;
    float pct = (cellV - CELL_EMPTY_V) / (CELL_FULL_V - CELL_EMPTY_V) * 100;
    pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);  // rough linear estimate
    rx.setBattery((uint16_t)filteredMv, 0, 0, (uint8_t)pct);
}

static void onFailsafe(bool failsafe) {
    if (failsafe) stopMotors();
    Serial.println(failsafe ? "FAILSAFE: motors stopped" : "Link OK");
}

void setup() {
    Serial.begin(115200);
    for (int pin : {LEFT_IN1, LEFT_IN2, RIGHT_IN1, RIGHT_IN2}) ledcAttach(pin, PWM_HZ, PWM_BITS);
    stopMotors();
    analogSetPinAttenuation(BATTERY_PIN, ADC_11db);

    BLEehRxConfig cfg;
    cfg.name = "H-Bridge Car";
    cfg.pairButtonPin = BOOT_BUTTON;   // triple-click BOOT to pair
    cfg.failsafeTimeoutMs = 250;
    cfg.failsafeMode = BLEehFailsafe::Preset;  // all channels centre -> throttle 0, disarmed
    cfg.statusLedPin = STATUS_LED;
    cfg.statusLedActiveLow = true;
    rx.onFailsafeChanged = onFailsafe;
    rx.begin(cfg);
    Serial.println("H-Bridge Car ready. Triple-click BOOT to pair with a BLEeh transmitter.");
}

void loop() {
    rx.update();
    updateBattery();

    const bool armed = !rx.isFailsafe() && rx.channelMicros(CH_ARM) > 1700;
    rx.setFlightMode(rx.isFailsafe() ? "FAILSAFE" : (armed ? "ARMED" : "DISARMED"));
    if (!armed) {
        stopMotors();
        return;
    }
    if (rx.newFrame()) {
        const float throttle = applyDeadband(rx.channelNormalized(CH_THROTTLE));
        const float steering = applyDeadband(rx.channelNormalized(CH_STEERING));
        float left = throttle + steering;
        float right = throttle - steering;
        const float scale = fmaxf(1.0f, fmaxf(fabsf(left), fabsf(right)));
        driveMotor(LEFT_IN1, LEFT_IN2, left / scale);
        driveMotor(RIGHT_IN1, RIGHT_IN2, right / scale);
    }
}
