#include "run.hpp"
#include <iostream>

int main(int argc, char** argv) { Engine e; return run({argv + 1, argv + argc}, std::cout, e); }
