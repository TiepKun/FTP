#include <iostream>
#include <functional>
#include <string>
int main(int argc, char**argv){
    if(argc<2){return 0;} 
    std::hash<std::string> h; 
    std::cout<<std::hex<<h(argv[1])<<"\n";
    return 0;
}
