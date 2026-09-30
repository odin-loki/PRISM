// prism_ai: offline analytics for PRISM's AI layer. See prism_ai.hpp.

#include "prism_ai.hpp"

#include <iostream>

int main(int argc, char** argv) { return prism_ai::main_dispatch(argc, argv, std::cout, std::cerr); }
