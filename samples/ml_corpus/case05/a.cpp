#include <cstdlib>
void process() {
    void* block = std::malloc(16);
    std::free(block);
    std::free(block);
}
