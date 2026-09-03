#include "helix/common/version.hpp"
#include "helix/posix/abi_v3.hpp"
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#ifdef __linux__
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
template <class T> void inspect(const std::string &name, const char *kind) {
  int fd = shm_open(name.c_str(), O_RDONLY, 0);
  if (fd < 0) { std::cout << "{\"kind\":\"" << kind << "\",\"name\":\"" << name << "\",\"present\":false}\n"; return; }
  void *p = mmap(nullptr, sizeof(T), PROT_READ, MAP_SHARED, fd, 0); close(fd);
  if (p == MAP_FAILED) { std::cerr << "mmap: " << std::strerror(errno) << '\n'; return; }
  auto *r = static_cast<const T *>(p);
  std::cout << "{\"kind\":\"" << kind << "\",\"name\":\"" << name
            << "\",\"present\":true,\"magic_valid\":"
            << (r->header.magic == helix::abi_v3::magic ? "true" : "false")
            << ",\"abi\":" << r->header.abi << ",\"size\":"
            << r->header.total_size << ",\"creator_pid\":"
            << r->header.creator_pid << ",\"generation\":"
            << r->header.generation.load() << ",\"errors\":"
            << r->header.errors.load() << "}\n";
  munmap(p, sizeof(T));
}
#endif
int main(int argc, char **argv) {
  std::string market="/helix_market", results="/helix_results";
  for(int i=1;i<argc;++i){std::string a=argv[i];if(a=="--version"){std::cout<<"helix_inspect "<<helix::version<<'\n';return 0;}if(a=="--help"){std::cout<<"Usage: helix_inspect [--market NAME] [--results NAME]\n";return 0;}if(i+1>=argc){std::cerr<<"missing value\n";return 2;}if(a=="--market")market=argv[++i];else if(a=="--results")results=argv[++i];else{std::cerr<<"unknown argument\n";return 2;}}
#ifdef __linux__
  inspect<helix::abi_v3::MarketRegion>(market,"market");
  inspect<helix::abi_v3::ResultRegion>(results,"results"); return 0;
#else
  std::cerr << "Linux required\n"; return 2;
#endif
}
