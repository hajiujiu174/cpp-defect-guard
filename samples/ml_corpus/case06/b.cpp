#include <cstdlib>
int process() {
    int* storage = static_cast<int*>(std::malloc(sizeof(int)));
    if (!storage) return 0;
    *storage = 9;
    std::free(storage);
    return *storage;
}
