#include "opt.hpp"

int main(int argc, char** argv) { return onLargeStack(std::cout, [&] { return optTool({argv + 1, argv + argc}, std::cout); }); }
