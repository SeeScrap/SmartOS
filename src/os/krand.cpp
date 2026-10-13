#include "os/krand.h"

#include "soc/soc.h"
#include "soc/wdev_reg.h"

static volatile uint32_t s_state = 2463534242u;   // any non-zero start

void k_srand(uint32_t seed)
{
    // Zero is xorshift's one fixed point - it would return 0 forever - so it
    // means "take a seed from the hardware" instead.
    while (seed == 0) seed = REG_READ(WDEV_RND_REG);
    s_state = seed;
}

uint32_t k_rand(void)
{
    uint32_t x = s_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_state = x;
    return x;
}

uint32_t k_rand_below(uint32_t n)
{
    if (n == 0) return 0;
    // Rejection rather than a plain modulo, so small results are not more
    // likely than large ones when n does not divide 2^32.
    const uint32_t limit = UINT32_MAX - (UINT32_MAX % n);
    uint32_t r;
    do { r = k_rand(); } while (r >= limit);
    return r % n;
}
