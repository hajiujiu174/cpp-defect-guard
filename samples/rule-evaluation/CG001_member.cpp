struct Buffer {void strcpy(const char*){}};
void probe(Buffer& b){b.strcpy("text");}
