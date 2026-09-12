int probe(){auto fn=[](){return *static_cast<int*>(nullptr);};return fn();}
