// prism_ai: offline analytics for PRISM's AI layer. See prism_ai.hpp.

#include "prism/log.hpp"
#include "prism_ai.hpp"

#include <iostream>

int main(int argc, char** argv) {
    prism::log::set_program("prism_ai");
    return prism_ai::main_dispatch(argc, argv, std::cout, prism::log::err_stream());
}
