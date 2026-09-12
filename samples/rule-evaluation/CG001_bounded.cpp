extern "C" char* strncpy(char*,const char*,__SIZE_TYPE__);
void probe(char* p){strncpy(p,"text",4);}
