#include "shell.hpp"

int main(int argc, char** argv) { return onLargeStack(std::cout, [&] { return shellTool({argv + 1, argv + argc}, std::cin, std::cout); }); }
