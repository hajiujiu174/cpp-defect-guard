#include <cstdio>

// Intentional logical defect: classify zero without changing the input.
int is_zero(int value) {
    if (value = 0) return 1;
    return 0;
}

// API-risk example only. The behavior test below does not call this function.
int format_label(const char* text) {
    char label[8];
    return std::sprintf(label, "%s", text);
}

int main() {
    return is_zero(0) == 1 && is_zero(1) == 0 ? 0 : 1;
}
