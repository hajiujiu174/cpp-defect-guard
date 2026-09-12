template<int N> int at(){int a[2]={};if constexpr(N<2)return a[N];else return 0;}
int probe(){return at<3>();}
