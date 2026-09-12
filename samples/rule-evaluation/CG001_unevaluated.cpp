extern "C" char* strcpy(char*,const char*);
int probe(char* p){return sizeof(strcpy(p,"text"));}
