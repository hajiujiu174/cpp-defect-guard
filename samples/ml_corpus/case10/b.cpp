int process() {
    int* data = new int[3]{1, 2, 3};
    int result = data[3];
    delete[] data;
    return result;
}
