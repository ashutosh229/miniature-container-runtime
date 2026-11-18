#pragma once
namespace mc {
// Parses argv, dispatches run/ps/inspect/stop/logs/clean/help and returns the process exit status.
int run_cli(int argc, char** argv);
}  // namespace mc
