#include "api.h"
static int helper(int n) { return n - 1; }
int beta(int n) {
    if (n > 0) return alpha(n - 1);
    return helper(n);
}
int overloaded(int n) { return n; }
double overloaded(double n) { return n; }
int global_value = 1;
