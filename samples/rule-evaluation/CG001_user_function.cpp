namespace custom {char* strcpy(char* p,const char*){return p;}}
void probe(char* p){custom::strcpy(p,"text");}
