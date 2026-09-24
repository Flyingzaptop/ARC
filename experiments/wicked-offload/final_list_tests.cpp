#include "FinalListCheck.h"
#include <array>
#include <cstdlib>
int main() {
    const std::array<uint8_t,5> expected{1,0,1,0,1};
    const std::array<uint32_t,3> valid{4,0,2}, duplicate{4,0,0}, wrong{4,0,1}, outOfRange{4,0,5};
    const std::array<uint32_t,2> missing{4,0};
    if(!ArcFinalListMatches(expected,valid) || ArcFinalListMatches(expected,duplicate) ||
       ArcFinalListMatches(expected,wrong) || ArcFinalListMatches(expected,outOfRange) ||
       ArcFinalListMatches(expected,missing) || !ArcFinalListMatches({},{})) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
