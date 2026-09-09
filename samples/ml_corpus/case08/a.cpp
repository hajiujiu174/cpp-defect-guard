#include <cstdio>
int process() {
    std::FILE* stream = std::fopen("demo.txt", "r");
    if (!stream) return 0;
    int result = std::fgetc(stream);
    std::fclose(stream);
    return result;
}
