struct A{int n;};
int probe(){return static_cast<A*>(nullptr)->n;}
