#include "BLEehProtocol.h"
#include <string.h>

namespace bleeh {

static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put24(uint8_t *p, uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get24(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16); }
static uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }

void packChannels(const uint16_t channels[NUM_CHANNELS], uint8_t out[PACKED_CHANNEL_BYTES]) {
    uint32_t acc = 0;
    uint8_t bits = 0;
    size_t idx = 0;
    for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
        acc |= (uint32_t)(channels[i] & 0x7FF) << bits;
        bits += 11;
        while (bits >= 8) {
            out[idx++] = acc & 0xFF;
            acc >>= 8;
            bits -= 8;
        }
    }
}

void unpackChannels(const uint8_t in[PACKED_CHANNEL_BYTES], uint16_t channels[NUM_CHANNELS]) {
    uint32_t acc = 0;
    uint8_t bits = 0;
    size_t idx = 0;
    for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
        while (bits < 11) {
            acc |= (uint32_t)in[idx++] << bits;
            bits += 8;
        }
        channels[i] = acc & 0x7FF;
        acc >>= 11;
        bits -= 11;
    }
}

size_t encodeRc(const RcFrame &frame, uint8_t out[RC_FRAME_LEN]) {
    out[0] = frame.flags;
    put16(out + 1, frame.sequence);
    packChannels(frame.channels, out + 3);
    return RC_FRAME_LEN;
}

bool decodeRc(const uint8_t *data, size_t len, RcFrame &frame) {
    if (len < RC_FRAME_LEN) return false;
    frame.flags = data[0];
    frame.sequence = get16(data + 1);
    unpackChannels(data + 3, frame.channels);
    return true;
}

TelemetryWriter::TelemetryWriter(uint8_t *buffer, size_t capacity, uint8_t sequence)
    : buf_(buffer), cap_(capacity), len_(capacity ? 1 : 0) {
    if (capacity) buf_[0] = sequence;
}

uint8_t *TelemetryWriter::reserve(uint8_t type, uint8_t payloadLen) {
    if (len_ == 0 || len_ + 2 + payloadLen > cap_) return nullptr;
    uint8_t *p = buf_ + len_;
    p[0] = type;
    p[1] = payloadLen;
    len_ += 2 + payloadLen;
    ++records_;
    return p + 2;
}

bool TelemetryWriter::addLink(const LinkTelemetry &v) {
    uint8_t *p = reserve(TELEM_LINK, 5);
    if (!p) return false;
    put16(p, v.ackSequence);
    p[2] = v.uplinkLq;
    p[3] = (uint8_t)v.uplinkRssi;
    p[4] = v.flags;
    return true;
}

bool TelemetryWriter::addBattery(const BatteryTelemetry &v) {
    uint8_t *p = reserve(TELEM_BATTERY, 8);
    if (!p) return false;
    put16(p, v.millivolts);
    put16(p + 2, v.currentDa);
    put24(p + 4, v.capacityMah > 0xFFFFFF ? 0xFFFFFF : v.capacityMah);
    p[7] = v.remainingPct;
    return true;
}

bool TelemetryWriter::addGps(const GpsTelemetry &v) {
    uint8_t *p = reserve(TELEM_GPS, 15);
    if (!p) return false;
    put32(p, (uint32_t)v.latitude);
    put32(p + 4, (uint32_t)v.longitude);
    put16(p + 8, v.groundspeed);
    put16(p + 10, v.heading);
    put16(p + 12, v.altitude);
    p[14] = v.satellites;
    return true;
}

bool TelemetryWriter::addAttitude(const AttitudeTelemetry &v) {
    uint8_t *p = reserve(TELEM_ATTITUDE, 6);
    if (!p) return false;
    put16(p, (uint16_t)v.pitch);
    put16(p + 2, (uint16_t)v.roll);
    put16(p + 4, (uint16_t)v.yaw);
    return true;
}

bool TelemetryWriter::addBaroAltitude(int32_t altitudeCm) {
    uint8_t *p = reserve(TELEM_BARO_ALTITUDE, 4);
    if (!p) return false;
    put32(p, (uint32_t)altitudeCm);
    return true;
}

bool TelemetryWriter::addVario(int16_t verticalSpeedCms) {
    uint8_t *p = reserve(TELEM_VARIO, 2);
    if (!p) return false;
    put16(p, (uint16_t)verticalSpeedCms);
    return true;
}

bool TelemetryWriter::addFlightMode(const char *mode) {
    size_t n = strlen(mode);
    if (n > FLIGHT_MODE_MAX) n = FLIGHT_MODE_MAX;
    uint8_t *p = reserve(TELEM_FLIGHT_MODE, (uint8_t)n);
    if (!p) return false;
    memcpy(p, mode, n);
    return true;
}

bool TelemetryWriter::addRaw(uint8_t type, const uint8_t *payload, uint8_t len) {
    uint8_t *p = reserve(type, len);
    if (!p) return false;
    memcpy(p, payload, len);
    return true;
}

bool decodeTelemetry(const uint8_t *data, size_t len, TelemetryPacket &out) {
    out = TelemetryPacket();
    if (len < 1) return false;
    out.sequence = data[0];
    size_t i = 1;
    while (i < len) {
        if (i + 2 > len) return false;
        const uint8_t type = data[i];
        const uint8_t n = data[i + 1];
        const uint8_t *p = data + i + 2;
        if (i + 2 + n > len) return false;
        switch (type) {
        case TELEM_LINK:
            if (n < 5) break;
            out.hasLink = true;
            out.link.ackSequence = get16(p);
            out.link.uplinkLq = p[2];
            out.link.uplinkRssi = (int8_t)p[3];
            out.link.flags = p[4];
            break;
        case TELEM_BATTERY:
            if (n < 8) break;
            out.hasBattery = true;
            out.battery.millivolts = get16(p);
            out.battery.currentDa = get16(p + 2);
            out.battery.capacityMah = get24(p + 4);
            out.battery.remainingPct = p[7];
            break;
        case TELEM_GPS:
            if (n < 15) break;
            out.hasGps = true;
            out.gps.latitude = (int32_t)get32(p);
            out.gps.longitude = (int32_t)get32(p + 4);
            out.gps.groundspeed = get16(p + 8);
            out.gps.heading = get16(p + 10);
            out.gps.altitude = get16(p + 12);
            out.gps.satellites = p[14];
            break;
        case TELEM_ATTITUDE:
            if (n < 6) break;
            out.hasAttitude = true;
            out.attitude.pitch = (int16_t)get16(p);
            out.attitude.roll = (int16_t)get16(p + 2);
            out.attitude.yaw = (int16_t)get16(p + 4);
            break;
        case TELEM_BARO_ALTITUDE:
            if (n < 4) break;
            out.hasBaroAltitude = true;
            out.baroAltitudeCm = (int32_t)get32(p);
            break;
        case TELEM_VARIO:
            if (n < 2) break;
            out.hasVario = true;
            out.varioCms = (int16_t)get16(p);
            break;
        case TELEM_FLIGHT_MODE: {
            const size_t m = n > FLIGHT_MODE_MAX ? FLIGHT_MODE_MAX : n;
            memcpy(out.flightMode, p, m);
            out.flightMode[m] = '\0';
            out.hasFlightMode = true;
            break;
        }
        default:
            break;  // unknown or application-defined: skip
        }
        i += 2 + n;
    }
    return true;
}

size_t encodeCapabilities(const Capabilities &caps, uint8_t *out, size_t capacity) {
    size_t nameLen = strlen(caps.name);
    if (nameLen > CAPS_NAME_MAX) nameLen = CAPS_NAME_MAX;
    if (capacity < CAPS_HEADER_LEN + nameLen) return 0;
    out[0] = caps.protocolVersion;
    out[1] = caps.channelCount;
    put32(out + 2, caps.features);
    put16(out + 6, caps.telemetryTypes);
    memcpy(out + CAPS_HEADER_LEN, caps.name, nameLen);
    return CAPS_HEADER_LEN + nameLen;
}

bool decodeCapabilities(const uint8_t *data, size_t len, Capabilities &out) {
    out = Capabilities();
    if (len < CAPS_HEADER_LEN) return false;
    out.protocolVersion = data[0];
    out.channelCount = data[1];
    out.features = get32(data + 2);
    out.telemetryTypes = get16(data + 6);
    size_t nameLen = len - CAPS_HEADER_LEN;
    if (nameLen > CAPS_NAME_MAX) nameLen = CAPS_NAME_MAX;
    memcpy(out.name, data + CAPS_HEADER_LEN, nameLen);
    out.name[nameLen] = '\0';
    return true;
}

void encodeAdvManufacturer(uint8_t flags, uint8_t out[ADV_MANUFACTURER_LEN]) {
    put16(out, ADV_COMPANY_ID);
    out[2] = 'B';
    out[3] = 'E';
    out[4] = PROTOCOL_VERSION;
    out[5] = flags;
}

bool decodeAdvManufacturer(const uint8_t *data, size_t len, uint8_t &version, uint8_t &flags) {
    if (len < ADV_MANUFACTURER_LEN || get16(data) != ADV_COMPANY_ID ||
        data[2] != 'B' || data[3] != 'E')
        return false;
    version = data[4];
    flags = data[5];
    return true;
}

}  // namespace bleeh
