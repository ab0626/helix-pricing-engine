#include "helix/common/version.hpp"
#include <iostream>
#include <string>
#ifdef __linux__
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
bool safe_shm(const std::string&n){return n.rfind("/helix_",0)==0;}
#endif
int main(int argc,char**argv){
std::string market="/helix_market",results="/helix_results";
#ifdef __linux__
std::string socket="/tmp/helix_"+std::to_string(getuid())+".sock";
#else
std::string socket="/tmp/helix.sock";
#endif
for(int i=1;i<argc;++i){std::string a=argv[i];if(a=="--version"){std::cout<<"helix_cleanup "<<helix::version<<'\n';return 0;}if(a=="--help"){std::cout<<"Usage: helix_cleanup [--market NAME] [--results NAME] [--socket PATH]\n";return 0;}if(i+1>=argc){std::cerr<<"missing value\n";return 2;}if(a=="--market")market=argv[++i];else if(a=="--results")results=argv[++i];else if(a=="--socket")socket=argv[++i];else{std::cerr<<"unknown argument\n";return 2;}}
#ifdef __linux__
if(!safe_shm(market)||!safe_shm(results)){std::cerr<<"refusing non-Helix shared-memory name\n";return 2;}int removed=0;if(shm_unlink(market.c_str())==0){std::cout<<"removed "<<market<<'\n';++removed;}if(shm_unlink(results.c_str())==0){std::cout<<"removed "<<results<<'\n';++removed;}struct stat st{};if(lstat(socket.c_str(),&st)==0){if(!S_ISSOCK(st.st_mode)||st.st_uid!=getuid()){std::cerr<<"refusing unowned or non-socket path\n";return 2;}if(unlink(socket.c_str())==0){std::cout<<"removed "<<socket<<'\n';++removed;}}std::cout<<"cleanup removed="<<removed<<'\n';return 0;
#else
std::cerr<<"Linux required\n";return 2;
#endif
}
