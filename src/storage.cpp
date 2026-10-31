#include "storage.hpp"

int main(int argc, char** argv) { return storageTool({argv + 1, argv + argc}, std::cout); }
