extern "C" char* strcpy(char*,const char*);
void probe(char* p){if constexpr(false){strcpy(p,"text");}}
