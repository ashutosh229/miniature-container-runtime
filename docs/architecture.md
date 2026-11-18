# Architecture

```
                 USER
                  |
             CLI (cli.cpp)  -- run | ps | inspect | stop | logs | clean
                  |
        CONTAINER MANAGER (container.cpp)  -- validation, orchestration, single cleanup path
                  |
   +--------------+--------------+------------------+
   |              |              |                  |
NamespaceMgr   ResourceMgr   FilesystemMgr      Registry (/run/minicontainer/*.state)
clone flags    cgroup v2      pivot_root, /proc,  ps / inspect / stop / logs
sethostname    memory.max     /dev, /tmp
loopback up    cpu.max        
               pids.max
   +--------------+--------------+
                  |
          PROCESS MANAGER (process_manager.cpp)
          clone() + sync pipe + error pipe + signal forwarding
                  |
     runner (host)  --clone-->  container init (PID 1)  --fork-->  workload (PID 2)
```

| Module | Files | Responsibility |
|---|---|---|
| CLI | `cli/cli.cpp` | argument parsing, subcommands, user-facing errors/exit codes |
| Container Manager | `container/container.cpp` | validate -> cgroup -> clone -> cgroup attach -> release -> wait -> stats -> cleanup |
| Namespace Manager | `namespace/` | `CLONE_NEWPID/UTS/NS/IPC(/NET)`, hostname, loopback, namespace inspection |
| Resource Manager | `resource/` | cgroup v2 hierarchy, limits, statistics, removal |
| Filesystem Manager | `filesystem/` | rootfs validation, `pivot_root`, `/proc`, `/dev`, `/tmp` |
| Process Manager | `process/` | child creation, parent/child sync, PID-1 init, signals, exit status |
| Common | `common/` | `SysError`, size parsing, file I/O, on-disk registry |

## Startup sequence (PRD section 6)

```
runner                                   container init (new namespaces)
------                                   -------------------------------
validate config, rootfs, command, limits
prepare cgroup hierarchy; mkdir <name>; write memory.max / cpu.max / pids.max
clone(NEWPID|NEWUTS|NEWNS|NEWIPC) -----> blocks reading the sync pipe
write child PID to cgroup.procs
save registry record
write 'G' to sync pipe ----------------> wakes up
                                         sethostname; make / rprivate; bind rootfs; pivot_root;
                                         umount2(old root, MNT_DETACH); mount /proc, /dev, /tmp
                                         fork() -> workload: drop caps, execvpe(command)
read error pipe (EOF == exec ok) <------ (CLOEXEC pipe closes on exec; failures are written to it)
waitpid(init), forwarding signals        init: waitpid(-1) loop, forwards signals, exits with workload status
read cgroup stats -> print summary
rm cgroup, rm registry record
```
The workload cannot run before it is inside its cgroup, which removes the "workload starts before limits apply" race.
