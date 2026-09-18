#include <cassert>
#include <iostream>

int main() {
    // This must fail even when assert itself has been compiled away.
    int evaluated = 0;
    assert(++evaluated == 1);
    if (evaluated != 1) {
        std::cerr << "Assertions were disabled in a test executable\n";
        return 1;
    }
    return 0;
}
