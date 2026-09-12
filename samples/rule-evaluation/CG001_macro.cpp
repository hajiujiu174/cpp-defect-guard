extern "C" char* strcpy(char*,const char*);
#define COPY(p) strcpy(p,"text")
void probe(char* p){COPY(p);}
