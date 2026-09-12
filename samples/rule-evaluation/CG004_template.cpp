template<class T> T* local(){T a;return &a;}
int* probe(){return local<int>();}
