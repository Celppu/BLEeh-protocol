// Host test for the portable BLEeh protocol. Prints each encoded frame as
// "<name> <hex>"; tests/test_protocol.py compares them with test-vectors/v1.json.
#include <cassert>
#include <cstdio>
#include <cstring>
#include "BLEehProtocol.h"

using namespace bleeh;

static const uint16_t VECTOR_CHANNELS[NUM_CHANNELS] = {
    172, 992, 1811, 0, 2047, 1000, 1234, 1500, 300, 400, 500, 600, 700, 800, 900, 1024};

static void printHex(const char *name, const uint8_t *data, size_t len) {
    printf("%s ", name);
    for (size_t i = 0; i < len; ++i) printf("%02x", data[i]);
    printf("\n");
}

static void testChannelRoundTrip() {
    uint8_t packed[PACKED_CHANNEL_BYTES];
    uint16_t out[NUM_CHANNELS];
    packChannels(VECTOR_CHANNELS, packed);
    unpackChannels(packed, out);
    assert(memcmp(out, VECTOR_CHANNELS, sizeof(out)) == 0);
}

static void testRcFrame() {
    RcFrame f;
    f.flags = RC_FLAG_SOURCE_FRESH;
    f.sequence = 0xBEEF;
    memcpy(f.channels, VECTOR_CHANNELS, sizeof(f.channels));
    uint8_t buf[RC_FRAME_LEN + 4] = {};
    assert(encodeRc(f, buf) == RC_FRAME_LEN);
    RcFrame g;
    assert(!decodeRc(buf, RC_FRAME_LEN - 1, g));
    assert(decodeRc(buf, RC_FRAME_LEN + 4, g));  // extra trailing bytes ignored
    assert(g.flags == f.flags && g.sequence == f.sequence);
    assert(memcmp(g.channels, f.channels, sizeof(g.channels)) == 0);
    printHex("rc", buf, RC_FRAME_LEN);
}

static void testTelemetry() {
    uint8_t buf[64];
    TelemetryWriter w(buf, sizeof(buf), 7);
    LinkTelemetry link;
    link.ackSequence = 513;
    link.uplinkLq = 98;
    link.uplinkRssi = -61;
    link.flags = LINK_FLAG_FAILSAFE;
    BatteryTelemetry bat;
    bat.millivolts = 7420;
    bat.currentDa = 15;
    bat.capacityMah = 1234567;
    bat.remainingPct = 80;
    assert(w.addLink(link));
    assert(w.addBattery(bat));
    assert(w.addFlightMode("ARMED"));
    const uint8_t app[3] = {1, 2, 3};
    assert(w.addRaw(0x90, app, 3));  // unknown to the decoder: must be skipped
    assert(w.addVario(-120));

    TelemetryPacket p;
    assert(decodeTelemetry(buf, w.length(), p));
    assert(p.sequence == 7);
    assert(p.hasLink && p.link.ackSequence == 513 && p.link.uplinkLq == 98);
    assert(p.link.uplinkRssi == -61 && p.link.flags == LINK_FLAG_FAILSAFE);
    assert(p.hasBattery && p.battery.millivolts == 7420 && p.battery.capacityMah == 1234567);
    assert(p.hasFlightMode && strcmp(p.flightMode, "ARMED") == 0);
    assert(p.hasVario && p.varioCms == -120);
    assert(!p.hasGps);
    printHex("telemetry", buf, w.length());

    assert(!decodeTelemetry(buf, w.length() - 1, p));  // truncated record

    // Records that do not fit are rejected without corrupting the buffer.
    uint8_t small[12];
    TelemetryWriter s(small, sizeof(small), 1);
    assert(s.addLink(link));
    assert(!s.addBattery(bat));
    assert(s.length() == 1 + LINK_RECORD_LEN);
}

static void testCapabilities() {
    Capabilities c;
    c.features = FEATURE_FAILSAFE_FLAG | FEATURE_LINK_TELEMETRY;
    c.telemetryTypes = (1u << TELEM_LINK) | (1u << TELEM_BATTERY);
    strcpy(c.name, "H-Bridge Car");
    uint8_t buf[CAPS_HEADER_LEN + CAPS_NAME_MAX];
    const size_t n = encodeCapabilities(c, buf, sizeof(buf));
    assert(n == CAPS_HEADER_LEN + strlen("H-Bridge Car"));
    Capabilities d;
    assert(decodeCapabilities(buf, n, d));
    assert(d.protocolVersion == PROTOCOL_VERSION && d.channelCount == 16);
    assert(d.features == c.features && d.telemetryTypes == c.telemetryTypes);
    assert(strcmp(d.name, "H-Bridge Car") == 0);
    printHex("capabilities", buf, n);
}

static void testAdvertising() {
    uint8_t m[ADV_MANUFACTURER_LEN];
    encodeAdvManufacturer(ADV_FLAG_PAIRING, m);
    uint8_t version = 0, flags = 0;
    assert(decodeAdvManufacturer(m, sizeof(m), version, flags));
    assert(version == PROTOCOL_VERSION && flags == ADV_FLAG_PAIRING);
    printHex("adv_manufacturer", m, sizeof(m));
    m[2] = 'X';
    assert(!decodeAdvManufacturer(m, sizeof(m), version, flags));
}

static void testSequenceQuality() {
    SequenceQuality<uint16_t, 100> q;
    for (int i = 0; i < 100; ++i) q.received((uint16_t)(65500 + i));  // wraps
    assert(q.percent() == 100);
    for (int s = 100; s < 300; s += 2) q.received((uint16_t)s);  // 50 % loss after resync
    assert(q.percent() >= 49 && q.percent() <= 51);
}

static void testMicros() {
    assert(channelToMicros(172) == 988);
    assert(channelToMicros(992) == 1500);
    assert(channelToMicros(1811) == 2011 || channelToMicros(1811) == 2012);
}

int main() {
    testChannelRoundTrip();
    testRcFrame();
    testTelemetry();
    testCapabilities();
    testAdvertising();
    testSequenceQuality();
    testMicros();
    return 0;
}
