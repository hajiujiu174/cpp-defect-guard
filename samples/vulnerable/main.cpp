#include <cstdio>
#include <cstring>
#include <iostream>

int main(int argc, char** argv) {
    char name[8] = {};
    if (argc > 1) {
        strcpy(name, argv[1]);
    }

    int scores[4] = {1, 2, 3, 4};
    if (argc > 10) {
        scores[4] = 99;
    }

    int* buffer = new int[4];
    buffer[0] = 7;

    int* null_value = nullptr;
    if (argc > 10) {
        std::cout << *null_value << '\n';
    }

    int uninitialized;
    if (argc > 10) {
        std::cout << uninitialized << '\n';
    }

    FILE* file = fopen("missing.txt", "r");
    if (file != nullptr) {
        std::cout << "opened" << '\n';
    }

    if (argc = 2) {
        std::cout << name << scores[0] << buffer[0] << '\n';
    }
    return 0;
}
