# Shared kernel contracts

This directory holds the source-family-independent kernel inputs and the
contracts every product shares:

- [`abi/`](abi/README.md): the canonical plain-C host/kernel bridge, its
  identity, generated-copy authority, and import policy;
- `config/darwin-uml.fragment`: the configuration policy every product shares
  (4 KiB guest pages, 64 KiB task stacks, the printing task or CPU on every
  kernel message). Each product applies its own overlay
  after it, which lists only where that product differs;
- `artifacts/`: the prebuilt-kernel bundle schema, the local read-only verifier
  (source/ABI identity, page and module policy, ELF geometry, payload inventory,
  exact bridge imports), and the shared `KEY='value'` metadata parser. A
  release build of an application compares the accepted descriptors with the
  tracked final tree (see the [mainline README](../mainline-6.18/README.md));
- `patches/`: the generated topic series every maintained line applies first;
- `tooling/kernel_series.py`: replay, verification, and regeneration of a
  line's ordered series;
- `tooling/config-fragments.sh`: the configuration composition both kernel
  builds source; after `olddefconfig` it requires every requested symbol to hold
  the value its last fragment asked for;
- `tooling/verify-console-input.py` and `tooling/console_input_probe.cpp`: the
  console PTY assertion, run by the standalone kernel build with explicit
  source and build paths (Apple product configuration consumes only bundles);
- [`gpu/`](gpu/README.md): the private Virtio-GPU memory-report wire contract;
- [`input/`](input/README.md): the private Virtio-input registration notice of the
  hot-pluggable keyboard, mouse and pointer;
  and
- [`video/`](video/README.md): the private Virtio video wire contract.

Pinned commits, overlays, toolchains, source locations, and build commands stay
in the owning versioned line ([`mainline-6.18`](../mainline-6.18/README.md)).

## Series model

Every product is built with `ARCH=um SUBARCH=arm64 UML_HOST_OS=Darwin`, the
x86_64 slice included (an arm64 kernel for x86-64 userspace). The series
supports only the Darwin host: a topic carries no `arch/um/os-Linux` change.

A topic belongs in the shared `patches/` series, regenerated from the mainline
replay and applied unchanged to each line's own upstream base, unless its
behavior genuinely depends on the guest OS or the target product does not
exist. An overlay-only topic records that reason in its versioned README. The
mainline README has the regeneration commands; never edit a patch file.

## Contracts

Contracts are described once, whichever product builds the topic. A patch
header says how a topic works; the `guest-*` test named with a contract checks
it. The bridge fields used below are defined by the comments in
[`abi/`](abi/).

## Console, signals and run end

- Console output never discards unread terminal input, because input and output
  may share one host PTY, and a console write allocates no memory. Closing a
  serial line never closes a host descriptor, and opening one never waits for
  the host to read what its terminal holds. The console PTY assertion and
  `guest-console-write-memory` check the first two, `guest-console-late-reader`
  the last.
- Output a serial line has queued for a host channel that took less than a
  write offered goes out before the line's console text, and before the
  exitcalls close the line at power-off, halt or restart. The console waits up
  to 1 s for the channel and the exitcalls up to 2 s.
  `guest-console-queued-output` checks the power-off.
- Host-side kernel code changes a signal mask only with `pthread_sigmask()`
  (Darwin's `sigprocmask()` sets every thread's mask) and reaches variadic libc
  functions only through fixed-arity bridge functions or shims. The bridge
  import check rejects `sigprocmask` and `fcntl`.
- `uml_curr_cpu()` reads the carrier's CPU-number pthread key directly, because
  `pthread_getspecific()` is costly on iOS. Each carrier validates the Darwin
  layout as it comes up; a mismatch stops the direct read and prints a warning
  on stderr, which the guest integration harness and Kernarium automation runs
  treat as a failed boot.
- A carrier reports each host signal it takes, with its pc and, for a fault,
  its address and ESR, except a fault a nofault accessor recovers
  (`guest-kernel-nofault`). A host thread that carries no vCPU takes the host's
  default action for SIGSEGV, SIGBUS, SIGTRAP, SIGILL and SIGFPE, and SIGALRM
  never runs a guest tick there (`guest-run-end-stop`); embedders should keep
  SIGALRM blocked on their own threads.
- An embedded run returns to its host only once no kernel thread can execute
  the image: the run end joins every carrier and stops the RTC alarm helper
  itself, since a host stop skips Linux exitcalls. A public stop reaches every
  carrier. `guest-run-end-stop` checks it.
- The kernel reports how the guest ends with `darwin_uml_guest_shutdown()`
  (power-off, halt, restart with its command) before the exitcalls run. A panic
  reports nothing; it aborts the process. The exit status keeps its meaning.

## Machine description and host threads

- Every kernel describes the machine in SMBIOS tables behind an SMBIOS 3.0
  entry point in guest RAM, so `/sys/class/dmi/id` and `systemd-detect-virt`
  (`vm-other`) work. The system, baseboard, chassis, BIOS, processor, and
  memory vendor is `Kernarium`; the product, family, and board are
  `Virtual Machine`; the BIOS version is the kernel release; the chassis type
  is Other; the processor is `Kernarium Virtual CPU` with the possible CPUs as
  its cores and threads; one memory device is the RAM the guest was given. No
  serial number, UUID, speed, or firmware date is claimed. No kernel has
  `/dev/mem`: UML's physical address 0 is the kernel image, so a mapping would
  expose kernel memory. `guest-kernel-dmi` checks both.
- Every host thread the kernel creates names itself (`uml-vcpu-N-carrier`,
  `uml-irq-notify`, `uml-rtc-alarm`); `guest-kernel-thread-names` checks them.
  A Darwin kernel never changes the host process's session, process group, or
  controlling terminal, so a signal to the launcher's process group reaches the
  emulator (`guest-console-session`).

## Stack traces and user memory

- A kernel stack trace walks frame records from its starting stack pointer, so
  a Darwin kernel without frame pointers fails to build; a task running on
  another vCPU prints no trace. `guest-kernel-nofault` requires reliable
  entries.
- A string copied from user memory is copied a page at a time and, as with
  `strncpy_from_user()`, leaves the destination past the terminator as it was.
  `guest-user-strings` checks it.
- A system call's access to user memory reaches only guest RAM. One that names
  a mapping of other memory, such as a host-visible Virtio-GPU blob in the
  shared-memory aperture, fails with `EFAULT`; the process still reads and
  writes the mapping itself (`guest-virtgpu-blob-uaccess`).

## arm64 task interface

- An arm64 task has a `[vdso]` in addition to the `[sigpage]`, which
  `AT_SYSINFO_EHDR` names. It is an ELF shared object shaped like native
  arm64's vDSO (a `LINUX_2.6.39` version node, both hash tables, unwind
  tables) with three of native's entries, `__kernel_clock_gettime`,
  `__kernel_gettimeofday` and `__kernel_clock_getres`, each a plain system
  call: the guest has no counter a task could read, so every clock and its
  resolution come from the kernel. Signal return stays on the `[sigpage]`, so
  `__kernel_rt_sigreturn` is not exported. The mapping is sealed where
  `CONFIG_MSEAL_SYSTEM_MAPPINGS` is set, as Android's configuration does. An
  AArch32 task and the x86_64 slice's tasks have no vDSO. `guest-vdso-gadgets`
  and `-jit` check the object, its entries and what a task can do to its own
  mapping with no C library, `guest-vdso-libc-gadgets` and `-jit` that glibc
  calls them, and `android-vdso` that bionic does and the mapping is sealed.
- A signal handler installed without `SA_RESTORER` returns to a trampoline in
  the read-only `[sigpage]` above `STACK_TOP`, where the host runs
  `rt_sigreturn`, and a frame record lets unwinders cross a signal frame as on
  native arm64 (`guest-signal-unwind`). Signal delivery restarts or fails only a
  system call the task is still in; any other entry leaves its return register
  untouched (`guest-signal-restart`). A task's stack pointer is its own
  (`guest-stack-pointer`).
- A tracer sees system calls as on native arm64: x0 holds the first argument,
  `PTRACE_SET_SYSCALL_INFO` and `NT_ARM_SYSTEM_CALL` work, and x7 (r12 for
  AArch32) marks entry or exit. The x86_64 slice keeps x86's convention.
  `guest-ptrace-syscall-args-gadgets` and `-jit` check A64 and AArch32 tracees.
- The tagged-address ABI is native arm64's without MTE and is offered only
  while `darwin_uml_guest_top_byte_ignore()` is 1. The one difference is that
  `si_addr` never carries the tag (`SA_EXPOSE_TAGBITS`). The x86_64 slice has
  none. `guest-tagged-addr-gadgets`, `-jit` and `-no-tbi` check it.
- With SME (`darwin_uml_guest_sme_vector_length()` non-zero) an arm64 task gets
  the behavior of `arch/arm64/kernel/fpsimd.c` on a CPU whose only SME vector
  length is that SVL: SVCR, TPIDR2 and a pointer to the task's SME storage
  follow the FP registers in the bridge tail (the kernel allocates the storage
  on the host's first SME access event), and signal frames and ptrace carry the
  state. `guest-sme-gadgets` and `-jit` check it; `guest-sme-off-gadgets`
  and `-jit` check that none of it shows without SME.
- The virtual CPU has no SVE outside streaming mode: there is no `NT_ARM_SVE`,
  `PR_SVE_*` fails with `EINVAL`, and the bridge tail holds no SVE state.
- Because XNU delivers a signal to a thread in streaming mode without leaving
  it, a carrier of an SME run blocks SIGUSR1 and the process-directed SIGINT,
  SIGTERM, SIGHUP, SIGQUIT, and SIGCHLD throughout, its idle wait included,
  which its wake pipe ends. Without SME no carrier blocks them.

## Memory and address space

- The native mapping window holds guest RAM plus an auxiliary allowance equal
  to RAM (1 to 4 GiB) for vmapped stacks and kernel mappings, then the fixed
  vhost-user shared-memory reservation and the stub pages. The host admits the
  same span (`UML_NATIVE_VHOST_USER_SHMEM_RESERVATION` and
  `UML_NATIVE_STUB_PAGES` in the top-level CMake layout), so the sizes move
  together. `mmap()` refuses a user mapping that would enter the window.
- A load from a user mapping succeeds when the mapping permits any access, for
  a task and for the kernel's own access, as on native arm64 and x86: write
  implies read, and an execute-only mapping reads. Only `PROT_NONE` refuses
  (`guest-write-only-mapping`).
- The kernel negotiates the vhost-user shared-memory transport
  (`VHOST_USER_PROTOCOL_F_SHMEM_MAP`) and reserves a 1 GiB guarded aperture
  outside RAM with 16 KiB mapping alignment (the host page). Virtio-GPU
  host-visible blobs use it, and Android's Venus Vulkan HAL needs it.
- Guest RAM is committed on demand: every chunk of the anonymous root starts
  released, the allocator commits a chunk when it first hands out a page of it,
  and free-page reporting releases whole reported chunks. Physical iOS wires
  only committed chunks. `page_cache_trim` reclaims inactive page cache above a
  floor (`page_cache_trim.floor_mb`, by default 1/32 of RAM) and only when the
  host asks: the console's `trim on` and `trim off` start and stop periodic
  passes, which back off while what they reclaimed is read back, and `trim once`
  trims, compacts, and releases free pages, then replies with what it freed.
  `guest-page-cache-trim` checks it.
- A user `mm` replays its mappings to the host in batches that publish only
  while the invalidation epoch still matches; `init_mm`, which the host maps
  natively, has one synchronizing owner at a time. One 16 KiB host slot carries
  four `init_mm` PTEs and is one shared host page only when the four are
  present, name consecutive frames from a 16 KiB-aligned offset of one backing,
  and agree in protection; any other slot is a host-private copy coherent with
  the frames only at a synchronization point (`guest-tlb-replay-runs`).
- A fork invalidates on the host only the parent's writable private VMAs, the
  entries it write-protects; every other VMA of the parent keeps its host
  mappings and code stamps, and any other flush of an `mm` covers all of it
  (`guest-fork-flush-*`, `guest-mmu-stress`). That rests on `dup_mmap()`
  flushing the parent after `arch_dup_mmap()` on every path that changed one
  of its entries; a kernel uprev rechecks it.
- A user PTE the kernel sends as guest code carries the immutable fact, bit 1
  of `guest_code` (bridge ABI 29), when it names an up-to-date page-cache page
  of a file on EROFS, decided from the PTE under its page-table lock. Kernel
  mappings, anonymous pages (a copy-on-write page of a private file mapping),
  other file systems and DAX never carry it, and the host runs rewritten
  code, and shares a verification between address spaces, for no other page.
  The AOT share rows (`aot-guest-workloads`) check it.
- Because of that slot rule, a kernel alias outside the linear map shares
  memory with the frames' other views only in whole aligned groups of four
  pages, so the architecture reports a vmap group of four. vmalloc and `vmap()`
  allocate in groups, which keeps memory shared with user space (BPF and perf
  rings, io_uring regions) shared, `flush_cache_vunmap()` publishes the private
  slots of a `vm_map_ram()` alias before it goes, and io_uring refuses with
  `EINVAL` user-owned ring memory that is neither one folio nor whole aligned
  groups. `guest-kernel-user-shared-memory` and `guest-erofs-lz4` check the
  sharing ([`tests/integration/kernel`](../../tests/integration/kernel)).
- In the x86_64 slice a guest entry carries `DARWIN_UML_DISPATCH_MM_SHARED`
  while another agent may observe the task's `mm` memory: another task may run
  on it (a vfork child does not count its waiting parent), it maps a
  `VM_MAYSHARE` vma (any `MAP_SHARED`, SysV shm, memfd), or io_uring holds
  pinned memory of it. The kernel recomputes this for every entry, so the flag
  clears once none holds, and from bridge ABI 19 every store another agent made
  before then is visible to the first entry without it. Writes through
  `ptrace()`, `process_vm_writev()` or `/proc/<pid>/mem` do not mark the `mm`.
  Only the x86-64 JIT reads the flag, so the work is under
  `UML_DARWIN_MM_SHARED`, which the arm64 kernels do not build.
  `tests/integration/x86_64/run_guest_x86_64_mm_shared.py` and
  `run_guest_x86_64_tso_litmus.py` check it.
- From bridge ABI 20 an x86_64-slice entry carries its mm's LDT in `fp` and the
  i386 TLS descriptors of a task of either ABI in `compat_tls`; the arm64
  kernels set neither.

## CPU identity and AArch32

- An arm64 task reads `/proc/cpuinfo` as arm64 Linux's `c_show()` prints it for
  a 64-bit task, and both engines give the same bytes. `Features` names exactly
  the AT_HWCAP and AT_HWCAP2 bits every task receives. `guest-gadgets-smoke` and
  `guest-jit-smoke` check it.
- The x86_64 slice describes its emulated x86 CPU instead. The virtual CPU is
  `Kernarium Virtual CPU` (`darwin/cpu/guest_identity.hpp`), which is also both
  x86 engines' CPUID brand string and every kernel's SMBIOS processor version.
  The Android image names it as `ro.soc.manufacturer` and `ro.soc.model`, and a
  tooling test requires them to agree.
- The arm64 kernels run AArch32 tasks (armhf and Android's armeabi-v7a) beside
  AArch64 ones, as arm64 Linux's `COMPAT` does, through two shared topics: the
  generic `arch/um` per-task ABI hooks and the arm64 port's `arch/arm64/um`
  support. The kuser helpers and signal-return pages are ordinary code pages the
  kernel installs, and `cacheflush()` reaches the engines through
  `darwin_uml_vm_invalidate_code()`, which the x86_64 kernel does not import.
  Mainline and Android build it; the x86_64 slice turns `UML_ARM64_AARCH32`
  off.
- Bridge ABI 22 carries the AArch32 task (see the comments on
  `struct darwin_uml_guest_registers`). `abi.cp15_barrier` (0 to 2, default 1)
  is passed as `DARWIN_UML_DISPATCH_A32_CP15BEN`. The engines raise the
  undefined-instruction event for SWP and SWPB, and `abi.swp` (default 0, as on
  arm64 Linux; Android's init sets 1) decides whether the kernel delivers SIGILL
  or emulates the exchange atomically. SETEND is UNDEFINED, with no switch.
- AArch32 is on only when `darwin_uml_guest_compat_hwcap()` is non-zero. While
  it is zero, a 32-bit ELF file fails with `ENOEXEC`, `personality(PER_LINUX32)`
  with `EINVAL`, and an AArch32 entry is `FATAL`. Under `PER_LINUX32` `uname()`
  reports `armv8l`. `tests/integration/aarch32` runs stock armhf userspace,
  whose programs are laid out for 4 KiB pages.

## Scheduling and interrupts

- Darwin's cooperative scheduler services timer deadlines and pending IPI
  vectors at safe kernel spin, retry, TLB-walk, and `cond_resched()` boundaries
  and in `schedule()` when the caller stays runnable, so a kernel retry loop
  must service them: the IPI signal is only a wake edge. An idle virtual CPU
  waits in one `kevent()` on its own queue, which holds its wake pipe, the
  host's stop descriptor and, on CPU0, the event the notification thread
  triggers. Its deadline is a one-shot critical timer that the call adds,
  because Darwin coalesces a wait's own timeout. An idle virtual CPU never
  blocks while one of its IPI vectors is pending. The byte in its wake pipe
  only ends a wait and the vectors are the pending mask, so an idle exit
  whose wait found the pipe empty drains it only if a vector is pending, and
  a byte that lands later costs at most one spurious wait
  (`kernel-darwin-ipi-drain`).
- Darwin has no per-thread SIGIO, so a CPU collects the host's descriptor
  events at its clock tick and when it leaves idle, one CPU at a time: a
  non-blocking token guards the dispatch, and a CPU that finds it taken skips.
  CPU0 notes that it did, and the holder dispatches once more after it
  releases the token, so no timed poll is needed. CPU0 always tries. Another
  CPU tries only when no CPU has begun a dispatch for two ticks (20 ms), which
  is how an event waits no longer when the host holds CPU0's carrier, since a
  busy CPU0 collects at its tick only. The `uml-irq-notify` thread takes no
  lock a CPU holds, so a stopped CPU0 cannot hold it up. It wakes a CPU0
  that idles, and for an event no dispatch has followed within 20 ms it also
  wakes an idle secondary through the pipe its idle wait reads. While CPU0 is
  not idle it parks after one event until CPU0's next idle entry or its 20 ms
  deadline, so a busy guest does not wake it per event. Handlers and
  their softirqs therefore run on any online CPU. Still waiting for CPU0 are
  the wake-ups the guest places on CPU0 (an RT worker kept on its previous CPU,
  CFS preferring a previous CPU that looks idle, NAPI and timers already
  there) and a dispatch the host stops CPU0 inside of, which holds the token.
  During a system suspend a secondary can take the token from the platform's
  prepare step until it goes offline, and throughout s2idle, where the
  secondaries stay online; a dispatch does nothing while the IRQs are
  suspended. `guest-irq-takeover` holds CPU0's carrier and checks a vsock
  echo with every CPU busy, with CPU0 only busy and with every CPU idle.
  CPU0's idle wait also dispatches before it sleeps, and does not sleep when
  that dispatch collected an event (`DARWIN_IRQ_HANDLED`): a handler may have
  made a task runnable there, which only the idle loop notices when the wait
  returns (`kernel-darwin-ipi-drain` for the wait, `kernel-darwin-irq-edge`
  for the result).
- A sleeping uaccess retries its fault until the handler refuses it or the task
  is dying, as native uaccess does, so a sibling's `fork()` never fails the copy
  with `-EFAULT`.
- Every kernel performs futex updates atomically against native userspace on
  other vCPUs (the optional `guest-futex-atomic` test).

## Block devices and filesystems

- Releasing an ext4 inode preallocation allocates nothing, and the block-group
  index and extent-status inserts use atomic allocations with `__GFP_NOWARN`; a
  failed insert costs a worse choice, never an allocation failure.
  `guest-ext4-prealloc-reclaim` evicts inodes under pressure and requires no
  `__GFP_NOFAIL` page allocation.
- Direct UBD images advertise discard ranges up to 128 MiB, each one host hole
  punch. Accepting or running a UBD request allocates no memory, even during
  writeback under pressure (`guest-ubd-request-memory`). The project adds no
  behavior to upstream UML's COW implementation, and Apple launchers never
  construct a COW image pair.
- A UBD read ends each page it filled with `flush_dcache_page()`. On x86-64 a
  page some process runs, pinned before the read (`O_DIRECT`, an io_uring fixed
  buffer), is named to the host so that its translation is not stale; an arm64
  program maintains its own caches (`run_guest_x86_64_pinned_code_write.py`).
- A copy into a buffer-vector destination (a loop device's or an io_uring
  fixed buffer's read of a file, an nbd or sunrpc receive) ends each page it
  wrote with `flush_dcache_page()` in `iterate_bvec()`, so a page some process
  runs is named to the host as a UBD read names it, and an arm64 kernel's call is
  empty (`fixed-file` and `loop-fixed` of the same runner).
- While the guest runs, a full host volume holds a direct UBD disk's I/O and
  the file system does not see a failure. A write, a flush or a write of zeroes
  that the host refuses with `ENOSPC` is redone, after a back-off that doubles
  from 10 ms to 1 s, until the host has room. The driver prints one line when a
  hold begins and one when it ends. The requests behind the held one wait, on
  every disk, and so does whatever needs a held write: a guest's own power-off
  or restart ends only when the host has room again. The worker sleeps as
  `TASK_IDLE` and the driver has no timeout handler, so no watchdog reports it.
  A discard and a COW disk are not held and fail as before. Once the kernel's
  exitcall begins, a held request fails (the journal aborts); a host stop ends
  the run without waiting for it. `kernel-darwin-ubd-hold` checks the
  decisions.

## Shutdown and device removal

- The UBD exitcall quiesces every disk before destroying the workqueue, because
  UML runs exitcalls while other vCPUs still run tasks (`guest-ubd-poweroff`).
- A management-console halt or reboot, an unbind, and a lost vhost-user backend
  reach a driver's removal without `device_shutdown()`. The Virtio video
  driver's removal resets the device, answers every pending command as failed,
  and keeps its state until the last open file closes
  (`guest-video-teardown`). A synchronous video command waits five seconds and
  owns its response memory, so a late answer or the removal frees it
  (`guest-video-late-answer`).
- The virtio-uml exit call stops every command-line device's connection-broken
  work before it removes a device, and a device being removed queues none, so a
  backend lost during the removal at power off does not have it unregistered a
  second time (`guest-vhost-user-power-off`). One loss report per device queues
  the work (an atomic bit), and the work holds the platform device's lock
  around its lookup of the virtio device, so an unbind cannot free what it
  breaks.

## Devices

- The Darwin network receive path uses NAPI, and the display-mapped absolute
  pointer is a direct touchscreen.
- The host's location is the guest's `/dev/gnssN` (type NMEA), behind a local
  experimental Virtio device (ID 65534; the wire contract is
  `darwin/device/gnss/README.md`). The host powers its receiver from the
  node's first open to its last close and sends nothing while it is off. A
  suspend disables the device's queues, so the host fills them after the
  resume, and the host wakes a guest that suspended itself while the receiver
  is powered; the driver is not a wake source. `guest-gnss-receiver` checks it.
- The Virtio-input driver follows its device's configuration changes: a device
  that reports no event type has no input device, every change reads every
  bitmap again and registers the input device afresh when one differs, and the
  driver answers with the private registration notice ([wire
  contract](input/README.md)). It posts its event buffers at probe whether or
  not it registered an input device, because the host plugs a device only after
  that. A device that reports its events at probe registers as before.
  `guest-hardware-input` checks it for the hot-pluggable keyboard, mouse and
  pointer.
- The host's cameras are `/dev/video10` and up, a capture node and a metadata
  node for each, behind a local experimental Virtio device (ID 65533; the wire
  and the nodes are in [`video/README.md`](video/README.md#camera-device-id-65533)).
  Opening a node starts no capture, and a stream survives a suspend.
  `guest-v4l2-capture` checks the nodes on the synthetic camera, live and
  blocked, and `guest-v4l2-capture-suspend` the suspend.
- A virtio-gpu PRIME export is always read-write. A device with 3D refuses a
  dma-buf another driver exported with `-ENODEV`; do not restore the stable
  change that routed it to the generic importer. Hardware video and Android's
  gralloc import only the device's own exports.
- The resource-creation ioctls (a 2D or classic 3D resource, and a host3d or
  guest blob) wait at most 15 s for the host's answer, and an out-of-memory
  answer fails them with `-ENOMEM` (any other error with `-EINVAL`) before the
  guest holds a handle, so Mesa raises `GL_OUT_OF_MEMORY` and gralloc fails the
  allocation. A blob the host refused is never a buffer other processes can
  import (`guest-virtgpu-blob-refusal`). The in-kernel host blob and a dma-buf import hold locks and do not
  wait. That wait and the get-caps ioctl's capset wait are freezable, so a
  drawing guest suspends while the host holds its GPU answers (`set_gpu_hold`);
  the waits that hold a lock (a host-visible blob's `mmap()`, the control
  queue's ring-full wait) are not.
- A classic 3D resource is coherent with the GPU through its dma-buf around
  `DMA_BUF_IOCTL_SYNC`; CPU access must be bracketed and a producer must flush
  its commands first. The optional `guest-gbm-dmabuf` test checks it.
- The driver paces each CRTC with the DRM vblank timer at its mode's refresh
  rate: a flip's event and out-fence signal at the next tick. The timer runs
  only while something holds a vblank reference. A restart after an idle stop
  keeps the grid: the first tick is a whole number of periods after the last
  one, and the vblank count and timestamp the core derives for the gap stay a
  pair on that grid. A mode change, or the CRTC being turned off and on, starts
  a new grid. The optional `guest-drm-vblank` test checks the rate, the grid
  across idle gaps and the kernel's vblank trace.
- A Virtio-GPU context's debug name ends in its creating process,
  `<name>|pid=<tgid>|comm=<comm>`: the thread group ID in the initial PID
  namespace and the group leader's command name (a `|` in it becomes `?`), cut
  from `<name>` to fit the 64-byte field so the suffix survives. The host reads
  it to credit graphics memory to a process
  ([`docs/graphics.md`](../../docs/graphics.md#device-memory-budget);
  `guest-virtgpu-context-owner`).
- With `VIRTIO_GPU_F_UML_MEMORY_REPORT` ([wire contract](gpu/README.md)) the
  driver serves `gpu_memory_report` on the primary DRM minor
  (`/sys/class/drm/card0`), mode 0600: a header line (`seq`, `age_ms`, `state`,
  `refused`, `budget`, `used`, `held`, `armed`, `generation`) and up to 16
  holder lines `<pid> <bytes> <idle_ms> <contexts> <comm>`, the host's last
  report. A read formats that report and never waits for the host. A write of
  `arm <min_bytes> <min_idle_ms>`, `refresh` or `disarm` queues one query and
  returns, and a stored report calls `sysfs_notify()`. The file goes with the
  minor's directory when the DRM device is unplugged, at shutdown as at
  removal, and a power-off prints nothing about it. Without the feature there
  is no file, and no ioctl or other interface changes
  (`guest-virtgpu-memory-report`).
- A file's handles opened before its Virtio-GPU context exists are attached when
  the context is created. The primary plane accepts ARGB8888 as well as
  XRGB8888 (the first format) and the host ignores the alpha byte; the
  [Android product](../mainline-6.18/README.md#android-product) offers the other
  32-bit RGB orders.
- An in-band vhost-user kick asks for no acknowledgement, whatever the device
  but the console, so the kicking CPU, which holds the device's socket with its
  interrupts off, never waits for the backend's thread. A console kick waits
  for its answer and serves the backend's requests meanwhile, as every other
  request on the socket does: the console driver spins on its queue after a
  kick, and its backend may be waiting for the guest's answer to a request of
  its own.
- The Virtio sound driver runs a substream's elapsed-period work on its
  device's kthread worker (`virtsnd-<device>`) at the lowest real-time
  priority, not on the system workqueue.

## Suspend, wake and the management console

- Lifecycle quiesce uses full Linux system suspend. The management console
  acknowledges only after the suspend entry boundary and remains the explicit
  wake source, so guest uptime excludes time while an Apple host froze the
  application. The console socket is `/tmp/uml-<umid>-mconsole`, or
  `<dir>/mconsole` when the host passes `uml_dir=<dir>`; a path that does not
  fit Darwin's 104-byte `sun_path` is refused, never truncated.
- The console's `mem` replies `total=<n> available=<n> free=<n>` in decimal
  bytes, so it answers where a policy keeps the console from `/proc/meminfo`.
- The console's `sync` writes every file system and block device back, as
  `sync(2)` does, and replies `synced in <n> ms` only once that has returned,
  whatever the SysRq mask is; `sysrq s` only starts a writeback and is refused
  with the mask off. It runs on a work item of its own, so a sync that has to
  wait (the host holds a block device while its disk is full) sends no reply
  and holds up no other request in the console's dispatcher, though a request
  that needs the same I/O still waits for it; the host bounds its wait. The
  reply says the pass returned, not that no I/O error occurred. A suspend syncs
  only while the guest's `sync_on_suspend` is set, and says so in its reply:
  `suspended, synced` once the PM core's sync of this suspend has returned,
  `suspended, not synced` when there was none (a failed or given-up suspend
  replies with an error and no statement). A kernel without the statement
  replies `suspended`, which confirms nothing, and so does a request that
  finds a suspend the guest started itself already holding the transition,
  past its sync or not: the statement is made only for the transition the
  request's own task runs. The host reads either text as the old reply. The
  sync runs before tasks are frozen, so a write between the two is not
  covered (`guest-mconsole-console-clients`).
- A command that answers with what the kernel prints (`sysrq`, `stack`)
  sends its client what the consoles write while it runs, whichever vCPU
  writes it, and nothing after its final reply. The final reply names the
  command and what it did: `sysrq s ok`, `stack ok`, or an error,
  `sysrq s disabled` when SysRq or the operation is off and `sysrq a failed`
  for a key without an operation. A client keys on that reply, not on the
  printed text, which a console may write late or not at all
  (`guest-mconsole-console-clients`). `sysrq b` replies first, since the guest
  restarts without another chance.
- The management console, the UML RTC alarm, VNC input, and a vhost-user device
  whose wakeup is enabled in sysfs wake any suspend. The Darwin network
  interface wakes only a suspend the guest started itself, as a phone's wireless
  interface does, and holds the guest awake 500 ms
  (`darwin_net_kern.guest_wake=0` turns this off). A suspend the host asked for
  ends only with the host's wake; a host that forwards a key also sends the
  wake, because Virtio input is not a wake source.
- The virtual Wi-Fi station stays joined through every suspend: virt_wifi
  offers wake-on-WLAN for any activity, so cfg80211 does not disconnect the
  station as the guest sleeps.
- A wake request that arrives while a suspend is being entered is answered at
  once and still ends that suspend. It ends every host-asked suspend that
  arrived before it, too, including one that waits behind another console
  request: that suspend replies `Suspend failed: -16` when its turn comes and
  does not put the guest to sleep, in whichever order queued requests run. A
  host-asked suspend that ends with `-EBUSY` is entered again up to three more
  times unless a suspend of the guest's own has answered the request meanwhile,
  and a request that gives up replies `Suspend failed`. The retries rely on the
  kernels not being preemptible (`CONFIG_PREEMPT_NONE=y` in every
  configuration): the worker has no scheduling point between its last look at
  the request and the PM core's trylock but the sleep that the look follows, so
  a suspend of the guest's own cannot answer the request in that time. A
  configuration that enables preemption must make the look inside the PM
  transition, with its mutex held.
  `guest-suspend-holds` checks the host's suspend under traffic, early wakes,
  and every guest-started wake path; `kernel-darwin-mconsole-suspend` checks
  the bookkeeping.
- The Virtio sound driver starts a substream afresh after a suspend and pads the
  last period of a drain with silence (`guest-virtio-sound-suspend`).

## BPF

The kernel exposes BTF at `/sys/kernel/btf/vmlinux` for CO-RE programs. BPF runs
on the interpreter; no BPF code is generated at run time. Callback helpers such
as `bpf_loop` use a fixed pool of 1024 entry points, and a load that finds none
free fails with `ENOSPC`. The interpreter rejects kfunc calls (so `bpf_wq` is
unavailable) and tail calls in a program with callbacks or other subprograms.
Tracing program types need `CONFIG_BPF_EVENTS`, which UML does not provide.
