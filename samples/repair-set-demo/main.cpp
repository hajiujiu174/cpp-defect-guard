int is_zero(int);
int is_one(int);
int main() {
    return is_zero(0) == 1 && is_zero(1) == 0 &&
           is_one(1) == 1 && is_one(0) == 0 ? 0 : 1;
}
