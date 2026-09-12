template<class T> int read_null(){return *static_cast<T*>(nullptr);}
int probe(){return read_null<int>();}
