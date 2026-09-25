// Catch2 entry point. Written out rather than linked from Catch2's own main so
// the test target does not depend on how the package's main target is named.
#include <catch2/catch_session.hpp>

int main(int argc, char** argv) {
    return Catch::Session().run(argc, argv);
}
