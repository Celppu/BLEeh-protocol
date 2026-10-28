#pragma once
// BLEeh v1: RC link protocol over standard BLE GATT.
//
// Portable (no Arduino / ESP-IDF / Nordic SDK dependencies) so the same wire
// format can be unit tested on a PC and reused by an nRF port. See
// PROTOCOL.md in the repository root for the full specification.
#include <stddef.h>
#include <stdint.h>

namespace bleeh {

constexpr uint8_t PROTOCOL_VERSION = 1;
constexpr uint8_t NUM_CHANNELS = 16;
constexpr uint8_t PACKED_CHANNEL_BYTES = 22;  // 16 x 11 bit, same bit order as CRSF

// GATT layout. 128-bit UUIDs share the base "....-55eb-4435-bb6e-270656fee0da"
// (random v4 UUID; the b1ee prefix reads "BLEE").
constexpr const char *SERVICE_UUID   = "b1ee0001-55eb-4435-bb6e-270656fee0da";
constexpr const char *RC_UUID        = "b1ee0002-55eb-4435-bb6e-270656fee0da"; // write without response, encrypted
constexpr const char *TELEMETRY_UUID = "b1ee0003-55eb-4435-bb6e-270656fee0da"; // notify, encrypted
constexpr const char *CAPS_UUID      = "b1ee0004-55eb-4435-bb6e-270656fee0da"; // read

// Advertising manufacturer data: company 0xFFFF (test/unassigned), 'B','E',
// protocol version, flags.
constexpr uint16_t ADV_COMPANY_ID = 0xFFFF;
constexpr uint8_t ADV_FLAG_PAIRING = 0x01;  // receiver accepts a new bond now
constexpr size_t ADV_MANUFACTURER_LEN = 6;

// CRSF raw channel range as sent by EdgeTX: 172 = -100 %, 992 = 0, 1811 = +100 %.
constexpr uint16_t CHANNEL_MIN = 172;
constexpr uint16_t CHANNEL_MID = 992;
constexpr uint16_t CHANNEL_MAX = 1811;

// ---- RC frame (transmitter -> receiver, RC characteristic) ----
constexpr uint8_t RC_FLAG_SOURCE_FRESH = 0x01;  // transmitter is receiving live radio data
constexpr size_t RC_FRAME_LEN = 1 + 2 + PACKED_CHANNEL_BYTES;  // 25 bytes; needs ATT MTU >= 28
constexpr uint16_t MIN_ATT_MTU = RC_FRAME_LEN + 3;

struct RcFrame {
    uint8_t flags = 0;
    uint16_t sequence = 0;
    uint16_t channels[NUM_CHANNELS] = {};
};

void packChannels(const uint16_t channels[NUM_CHANNELS], uint8_t out[PACKED_CHANNEL_BYTES]);
void unpackChannels(const uint8_t in[PACKED_CHANNEL_BYTES], uint16_t channels[NUM_CHANNELS]);
size_t encodeRc(const RcFrame &frame, uint8_t out[RC_FRAME_LEN]);
// Accepts longer writes (future fields) and ignores the extra bytes.
bool decodeRc(const uint8_t *data, size_t len, RcFrame &frame);

// ---- Telemetry (receiver -> transmitter, notifications) ----
// Notification = [telemetry sequence u8] then records [type u8][len u8][payload].
// Unknown record types are skipped using len, so new sensors stay compatible.
enum TelemetryType : uint8_t {
    TELEM_LINK = 0x01,
    TELEM_BATTERY = 0x02,
    TELEM_GPS = 0x03,
    TELEM_ATTITUDE = 0x04,
    TELEM_BARO_ALTITUDE = 0x05,
    TELEM_VARIO = 0x06,
    TELEM_FLIGHT_MODE = 0x07,
    TELEM_APP_FIRST = 0x80,  // 0x80..0xFF: application-defined, ignored by bridges
};

constexpr uint8_t LINK_FLAG_FAILSAFE = 0x01;  // receiver outputs are in failsafe

struct LinkTelemetry {
    uint16_t ackSequence = 0;  // last RC sequence accepted
    uint8_t uplinkLq = 0;      // % of RC frames received
    int8_t uplinkRssi = 0;     // dBm measured by the receiver
    uint8_t flags = 0;
};
struct BatteryTelemetry {
    uint16_t millivolts = 0;
    uint16_t currentDa = 0;    // 0.1 A
    uint32_t capacityMah = 0;  // 24 bit on the wire
    uint8_t remainingPct = 0;
};
struct GpsTelemetry {            // units follow CRSFDude::sendGPS
    int32_t latitude = 0;        // degrees * 1e7
    int32_t longitude = 0;
    uint16_t groundspeed = 0;
    uint16_t heading = 0;        // degrees * 100
    uint16_t altitude = 0;       // m
    uint8_t satellites = 0;
};
struct AttitudeTelemetry {       // radians * 10000
    int16_t pitch = 0, roll = 0, yaw = 0;
};
constexpr size_t FLIGHT_MODE_MAX = 15;

constexpr size_t LINK_RECORD_LEN = 2 + 5;
constexpr size_t BATTERY_RECORD_LEN = 2 + 8;
constexpr size_t GPS_RECORD_LEN = 2 + 15;
constexpr size_t ATTITUDE_RECORD_LEN = 2 + 6;
constexpr size_t BARO_RECORD_LEN = 2 + 4;
constexpr size_t VARIO_RECORD_LEN = 2 + 2;

// Builds one notification; each add* returns false (and writes nothing) if
// the record does not fit, so the caller can send it in the next notification.
class TelemetryWriter {
public:
    TelemetryWriter(uint8_t *buffer, size_t capacity, uint8_t sequence);
    bool addLink(const LinkTelemetry &v);
    bool addBattery(const BatteryTelemetry &v);
    bool addGps(const GpsTelemetry &v);
    bool addAttitude(const AttitudeTelemetry &v);
    bool addBaroAltitude(int32_t altitudeCm);
    bool addVario(int16_t verticalSpeedCms);
    bool addFlightMode(const char *mode);
    bool addRaw(uint8_t type, const uint8_t *payload, uint8_t len);
    size_t length() const { return len_; }
    size_t recordCount() const { return records_; }
private:
    uint8_t *reserve(uint8_t type, uint8_t payloadLen);
    uint8_t *buf_;
    size_t cap_;
    size_t len_;
    size_t records_ = 0;
};

// Parsed view of one notification; fields are valid when their has* is set.
struct TelemetryPacket {
    uint8_t sequence = 0;
    bool hasLink = false, hasBattery = false, hasGps = false, hasAttitude = false;
    bool hasBaroAltitude = false, hasVario = false, hasFlightMode = false;
    LinkTelemetry link;
    BatteryTelemetry battery;
    GpsTelemetry gps;
    AttitudeTelemetry attitude;
    int32_t baroAltitudeCm = 0;
    int16_t varioCms = 0;
    char flightMode[FLIGHT_MODE_MAX + 1] = {};
};
// Returns false only for a malformed notification (truncated record).
bool decodeTelemetry(const uint8_t *data, size_t len, TelemetryPacket &out);

// ---- Capabilities (receiver, read once after connecting) ----
enum Feature : uint32_t {
    FEATURE_FAILSAFE_FLAG = 1u << 0,     // honours RC_FLAG_SOURCE_FRESH
    FEATURE_LINK_TELEMETRY = 1u << 1,
    FEATURE_COMMANDS = 1u << 2,          // reserved: command characteristic (future)
    FEATURE_CHANNEL_SOUNDING = 1u << 3,  // reserved: BLE channel sounding ranging (nRF54)
};
constexpr size_t CAPS_NAME_MAX = 20;
constexpr size_t CAPS_HEADER_LEN = 8;

struct Capabilities {
    uint8_t protocolVersion = PROTOCOL_VERSION;
    uint8_t channelCount = NUM_CHANNELS;
    uint32_t features = 0;
    uint16_t telemetryTypes = 0;  // bit n set = telemetry type n may be sent (1..15)
    char name[CAPS_NAME_MAX + 1] = {};
};
size_t encodeCapabilities(const Capabilities &caps, uint8_t *out, size_t capacity);
bool decodeCapabilities(const uint8_t *data, size_t len, Capabilities &out);

// ---- Advertising ----
void encodeAdvManufacturer(uint8_t flags, uint8_t out[ADV_MANUFACTURER_LEN]);
bool decodeAdvManufacturer(const uint8_t *data, size_t len, uint8_t &version, uint8_t &flags);

// ---- Helpers ----
// Same mapping as ELRS/EdgeTX: 172 -> 988 us, 992 -> 1500 us, 1811 -> 2012 us.
inline uint16_t channelToMicros(uint16_t raw) {
    return (uint16_t)(1500 + ((int32_t)raw - CHANNEL_MID) * 5 / 8);
}
inline float channelToNormalized(uint16_t raw) {
    const float v = ((float)raw - CHANNEL_MID) / (float)(CHANNEL_MAX - CHANNEL_MID);
    return v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
}

// Packet-loss estimate from sequence numbers, updated every `window` packets.
template <typename Seq, uint16_t Window>
class SequenceQuality {
public:
    void received(Seq seq) {
        if (!started_) {
            started_ = true;
            start_ = seq;
            count_ = 0;
        }
        const Seq span = (Seq)(seq - start_ + 1);
        if (span == 0 || span > (Seq)(Window * 4)) {  // restart or old packet: resync
            start_ = seq;
            count_ = 1;
            return;
        }
        ++count_;
        if (span >= Window) {
            const uint32_t pct = (uint32_t)count_ * 100u / span;
            percent_ = (uint8_t)(pct > 100 ? 100 : pct);
            start_ = (Seq)(seq + 1);
            count_ = 0;
        }
    }
    uint8_t percent() const { return percent_; }
    void reset() { started_ = false; percent_ = 0; }
private:
    bool started_ = false;
    Seq start_ = 0;
    uint16_t count_ = 0;
    uint8_t percent_ = 0;
};

}  // namespace bleeh
