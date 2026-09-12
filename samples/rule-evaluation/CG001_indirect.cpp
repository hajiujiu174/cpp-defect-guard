extern "C" char* strcpy(char*,const char*);
void probe(char* p){auto fn=&strcpy;fn(p,"text");}
