#include "api.h"
#include "cycle_a.h"
#ifndef CODEGUARD_FIXTURE
#error Compilation database definitions were not applied
#endif
static int helper(int n) { return n + 1; }
int alpha(int n) {
    if (n > 0 && n < 4) return beta(n - 1);
    return helper(n);
}
int invoke(int (*function)(int), int n) { return function(n); }
int entry() { return overloaded(alpha(3)); }
