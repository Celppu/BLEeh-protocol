#include <cassert>
#include "button_debounce.h"
int main() {
    DebouncedButton b(35);
    assert(!b.update(true, 0));
    assert(!b.update(false, 10));
    assert(!b.update(true, 16));    // bounce
    assert(!b.update(false, 20));
    assert(!b.update(false, 54));
    assert(b.update(false, 55));     // debounced press
    assert(!b.update(false, 60));    // held -> no repeats
    assert(!b.update(true, 65));
    assert(!b.update(true, 100));    // release
    assert(!b.update(false, 120));
    assert(b.update(false, 155));    // next press
    assert(!b.update(true, 200));
    assert(!b.update(true, 235));
    assert(!b.update(false, 240));
    assert(b.update(false, 275));
    return 0;
}
