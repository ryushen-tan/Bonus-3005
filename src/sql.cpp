#include "sql.hpp"

int main(int argc, char** argv) { return sqlTool({argv + 1, argv + argc}, std::cout); }
