extern "C" char* strcpy(char*,const char*);
template<class T> void copy_to(T p){strcpy(p,"text");}
void probe(char* p){copy_to(p);}
