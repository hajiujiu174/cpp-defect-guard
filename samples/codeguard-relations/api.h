#pragma once
int alpha(int n);
int beta(int n);
int overloaded(int n);
double overloaded(double n);
struct Counter {
    int value;
    int read() const { return value; }
};
