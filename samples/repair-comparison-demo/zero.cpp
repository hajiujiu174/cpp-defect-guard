int chosen();
int fallback();
int is_zero(int n) {
    if (n = 0) return chosen();
    return fallback();
}
