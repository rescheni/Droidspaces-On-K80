# Android 14 / Linux 6.1 profile without native namespace support

This source set is for 6.1 baselines that ship **no** `CONFIG_PID_NS`,
`CONFIG_IPC_NS`, `CONFIG_SYSVIPC` or `CONFIG_POSIX_MQUEUE` at all. `src/port61`
covers the opposite case (native IPC namespaces and POSIX message queues are
present and only PID plus SysV IPC are added), so on such a kernel the loader
fails before `__init` with unresolved symbols:

```text
droidspaces_ns: Unknown symbol init_ipc_ns (err -2)
droidspaces_ns: Unknown symbol mq_init_ns (err -2)
...
```

`port61full` is `port61` with the IPC layer restored to a self-contained
backend, taken from the pinned 6.1 source tree rather than from another KMI:

| Symbol | port61 | port61full |
| --- | --- | --- |
| `init_ipc_ns`, `mq_lock` | host-provided | defined here (`ipc.c`) |
| `mq_init_ns`, `mq_clear_sbinfo`, `mq_put_mnt` | host-provided | `mqueue.c` |
| `setup_mq_sysctls`, `retire_mq_sysctls` | host-provided | `mq_sysctl.c` |

Other differences from `port61`:

- Entry gate requires native `pidns_operations` and `ipcns_operations` to be
  **absent**, and namespace references tolerate a `NULL` `nsproxy->ipc_ns`
  before activation, because the host never sets that field.
- The POSIX message queue syscalls (`mq_open`, `mq_unlink`, `mq_timedsend`,
  `mq_timedreceive`, `mq_notify`, `mq_getsetattr`) are added to the replaced
  ARM64 syscall slots. On these kernels the slots alias `ni_syscall`, so the
  preflight compares the recorded slot with the weak alias before patching.
- `create_ipc_ns` keeps the 6.1 ordering (`mq_init_ns`, `setup_mq_sysctls`,
  then the SysV backends) and uses `mq_put_mnt`/`mq_clear_sbinfo`.
- `nsproxy_cachep` is resolved at load time instead of being linked: vendor
  kernels rename file-local data with the clang unique-internal-linkage suffix
  (`nsproxy_cachep.llvm.<hash>`), which the module loader cannot resolve by
  name. `ds_nsproxy_cache_init()` finds the variable through KallRecon and
  reads the cache pointer, so allocation and release keep using the cache the
  running kernel owns.

`copy_ipcs` and `put_ipc_ns` are owned by the module, so `ipcns_operations`
replaces the host definition and no IPC namespace objects exist before load.

See [ORIGIN](ORIGIN) for provenance and `docs/zorn-6.1.md` for the validated
device.
