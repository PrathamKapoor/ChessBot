// Engine entry point.
//
// Reads UCI commands on stdin and writes UCI replies on stdout, which is what a
// GUI or a match runner expects. Nothing else may be printed to stdout, because
// a GUI parses that stream as protocol: diagnostics belong on stderr.
#include <iostream>

#include "engine/uci.hpp"

int main() {
  std::ios::sync_with_stdio(false);
  pocket::UciEngine engine;
  engine.loop(std::cin, std::cout);
  return 0;
}