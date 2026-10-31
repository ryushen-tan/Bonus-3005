#include "opt.hpp"

int main(int argc, char** argv) { return optTool({argv + 1, argv + argc}, std::cout); }
