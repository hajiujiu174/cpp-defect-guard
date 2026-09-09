// Intentional dormant examples for static-analysis demonstrations.
// Runtime tests exercise safe_value only; passing tests do not prove no defects.
extern "C" char* strcpy(char*, const char*);
int bad_index() { int values[2] = {}; return values[2]; }
int* bad_lifetime() { int local = 1; return &local; }
int constant_null() { return *((int*)0); }
int suspicious_condition(int n) { if (n = 2) return n; return 0; }
void risky_copy(char* output) { strcpy(output, "example"); }
int safe_value() { int values[2] = {3, 7}; return values[1]; }
