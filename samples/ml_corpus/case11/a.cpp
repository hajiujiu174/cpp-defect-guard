#include <cstdlib>
int process() {
    int* data = static_cast<int*>(std::malloc(3 * sizeof(int)));
    if (!data) return 0;
    data[3] = 21;
    int result = data[3];
    std::free(data);
    return result;
}
