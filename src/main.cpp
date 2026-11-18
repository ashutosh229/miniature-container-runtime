#include <signal.h>

#include "cli/cli.hpp"
#include "process/process_manager.hpp"

int main(int argc, char** argv) {
    mc::ProcessManager::capture_argv(argc, argv);
    ::signal(SIGPIPE, SIG_IGN);  // a dead child must not kill the runtime while it writes to a pipe
    return mc::run_cli(argc, argv);
}
