#include <iostream>

#ifndef BITKV_VERSION
#define BITKV_VERSION "0.0.0"
#endif

int main() {
  std::cout << "BitKV " << BITKV_VERSION << '\n';
  return 0;
}
