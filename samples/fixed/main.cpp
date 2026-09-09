#include <array>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const std::string name = argc > 1 ? argv[1] : "";
    const std::array<int, 4> scores = {1, 2, 3, 4};
    const std::vector<int> buffer = {7, 0, 0, 0};
    const int initialized = 0;

    using FilePointer = std::unique_ptr<FILE, decltype(&fclose)>;
    FilePointer file(fopen("missing.txt", "r"), &fclose);
    if (file) {
        std::cout << "opened" << '\n';
    }

    if (argc == 2) {
        std::cout << name << scores.at(0) << buffer.at(0) << initialized << '\n';
    }
    return 0;
}

