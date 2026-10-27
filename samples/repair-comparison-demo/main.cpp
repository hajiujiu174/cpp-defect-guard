int is_zero(int);
int is_one(int);
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (argv[1][0] == 'z') return is_zero(0) == 1 ? 0 : 1;
    if (argv[1][0] == 'o') return is_one(0) == 0 ? 0 : 1;
    return argv[1][0] == 'i' ? 0 : 2;
}
