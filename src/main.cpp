// Cova — entry point.
//
// Kept deliberately trivial: all behaviour lives in `cova::cli::run` so that the
// CLI can be driven from tests with captured streams.
#include "cli/cli.hpp"

int main(int argc, char** argv) { return cova::cli::run(argc, argv); }
