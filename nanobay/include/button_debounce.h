#pragma once
#include <stdint.h>

// Portable active-LOW debouncer. Supply raw pin level and milliseconds.
// Returns true exactly once per *debounced* press (not per release).
// Unsigned elapsed-time arithmetic tolerates millis() wrapping around.
class DebouncedButton {
public:
    explicit DebouncedButton(uint32_t debounceMs = 35) : debounceMs_(debounceMs) {}

    bool update(bool rawHigh, uint32_t now) {
        if (rawHigh != lastRawHigh_) {
            lastRawHigh_ = rawHigh;
            changedAt_ = now;
        }
        if (rawHigh != stableHigh_ && (uint32_t)(now - changedAt_) >= debounceMs_) {
            stableHigh_ = rawHigh;
            return !stableHigh_;
        }
        return false;
    }

private:
    uint32_t debounceMs_;
    uint32_t changedAt_ = 0;
    bool lastRawHigh_ = true;
    bool stableHigh_ = true;
};
