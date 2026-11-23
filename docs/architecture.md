# Architecture

This document states the boundaries and the cross-module contracts an agent
must not break. Exact values (budgets, deadlines, sizes, option lists) live in
the header, script or test named beside each rule.

## Dependency direction

The enforced first-party dependency direction is:

```text
app -> emulator / device
emulator -> runtime / device / api
runtime -> cpu / memory / device contracts / platform
cpu -> memory / platform
memory -> platform
device -> memory / platform
```

`darwin/CMakeLists.txt` composes owning subdirectories. Each module owns its
targets and source manifest. The neutral CPU factory selects the gadget or JIT
adapter; engines do not depend on one another, and shared CPU code depends on
neither. `architecture-audit` enforces this on each CPU target's declared
engine kind and on textual includes between the engine directories.

An emulator archive is a self-contained final product: CMake merges object
libraries and interface-carried objects into it, never a linked static archive,
and `scripts/build-xcframework.sh` rejects a release archive that leaves a
first-party symbol undefined or exports `uml_emulator_` functions beyond the
public headers under `darwin/api/include/uml`. The assembly-visible CPU and TLB
layout is owned by `darwin/cpu/arm64/layout/assembly_offsets.h`; test-only hooks
belong in test/support targets.

## Runtime ownership

Each `uml_emulator` handle owns its runtime context, engine selection, stop
state, and copied network callback table, and admits at most one run at a time.
A stop request affects only that handle's current or queued run. Runs on
different handles serialize through a process address-space gate, because an
embedded kernel's fixed host mappings cannot be shared; a callback that
resolves no single run fails closed. The caller owns provider lifetimes; the
library has no process-global network provider.

A process runs one guest at a time. After a kernel has claimed guest RAM, a
later run is refused until the guest has been handed back
(`runtime/guest_hand_back`): automatically by the next run on a handle whose
guest restarted, or by `uml_emulator_release_guest` after a clean power-off or
halt. A failed or host-stopped guest cannot be released and requires a fresh
process. `uml_emulator_get_run_end` says how a run ended: the guest's own report
(see [guest lifecycle](#guest-lifecycle)), the host's stop request ahead of it,
or a failure when the kernel said nothing; it also says whether the guest's
console reported that an Android guest finished booting (`boot_completed`) and
whether init's output reached the host's reading of it at all (`init_observed`),
both read from the console where the kernel writes it
(`runtime/console_boot_signal.hpp`). Every run of a process uses the same JIT
cache budget and guest CPU count.

The hand-back refuses with `UML_EMULATOR_RUNTIME_ERROR` while a host service
borrows guest RAM or a kernel thread runs, and a refusal changes nothing.
Otherwise it gives back everything the kernel took and did not return: the
descriptors, allocations and mappings the loader recorded
(`runtime/kernel_resources`), the process state the kernel changed
(`KernelSignalScope`), the guest-file records, suspension and hibernation, and
the guest RAM, whose window becomes one protectionless reservation again. The
`guest-restart` test requires it to leave nothing in the engine's
[domain heaps](#domain-heaps) but the per-thread scratch.

## Memory and executable code

All products use a 128 TiB sparse software-MMU guest task range and 4 KiB guest
pages on the 16 KiB Apple host page. The native kernel window holds guest RAM,
a RAM-scaled auxiliary mapping allowance, and the guarded aperture and stub
pages the kernel reserves; host and kernel derive that reservation from the same
constants (`runtime/native_kernel_layout.hpp`). The window is a reservation that
never returns address space while the process lives: the first run reserves it
once (`runtime::reserve_process_native_window`) and every later run takes the
same window. Guest pages are mapped over it with `MAP_FIXED`, and releasing one
hands its range back as a reservation, never with `munmap`, which would let an
unrelated `mmap(NULL)` take the gap. Before the kernel starts, a run checks the
geometry of the guest's size and reserves its window, and then the process's
heap region (`runtime::reserve_process_heap`), whose first block the kernel's
mapping takes; no host's placement of the window is displaced by the region,
and a size with no layout reserves nothing. A start refused the region leaves the
process's window as it was before the start: what the start reserved for itself
(the window's growth, or a new window beside the held one) is given back and
nothing else (`runtime::undo_process_window`), so that a smaller guest tried
next has its room and the signed kernel text an earlier load kept stays where
it is; a region once reserved stays. The run
then maps its RAM beyond the first 64 MiB of the window (the kernel image and
its reserved prefix, where a signed text alias of an earlier guest is
permanent) as the kernel will and hands the range back to the reservation
(`runtime::native_guest_memory_mappable`); a refusal of that trial keeps the
window and the region. A host that will not reserve the window or the region or
map the RAM ends the run with `-UML_EMULATOR_OUT_OF_MEMORY`, which leaves the
process's guest unspent, where the kernel would end the process, and marks the
run of that handle (`emulator/src/start_refusal.hpp`): the same status also
comes from a failed allocation of the library's own, so the Kernarium runtime
returns its own result (`KERNARIUM_RUNTIME_RESULT_MEMORY_REFUSED`) for a
marked run of its handle alone, whatever another handle's run did. Guest
RAM that a host-service view still borrows stays mapped until the last view is
released. The linear physical window
is the anonymous RAM root; consumers borrow it or create non-copying Mach
aliases, and disks stay file-backed.

`mem=` is a ceiling, not a reservation: the guest commits RAM before using it
and releases what it frees (`darwin_uml_physmem_commit`/`_release`,
`memory/guest_ram_commitment.cpp`), and released pages leave the process
footprint. Physical iOS also wires a page while it is committed; the one window
with a held page unwired is a [hibernation](#hibernation). The kernel trims its
page cache only at the host's request (`uml_emulator_trim_page_cache()` and
every quiesce) and, in small passes, while its periodic trimmer runs (the
system's memory pressure below, or the application's request). The host GPU's
memory is not carved out of the guest's RAM, and the host provides no Virtio
balloon device.

**Memory pressure.** Only a critical level responds
(`runtime/memory_pressure_response.hpp`): it turns the guest's own page-cache
trimmer on, which trims the cold file cache in small passes (the request is one
short message that waits for nothing), and frees the host GPU's cached driver
allocations, each time. No level asks for one trim of all the cold cache, which
stalls frames, and a warning starts no response: the system's levels describe
the device, not this process's headroom, and a warning is frequent. The trimmer
is on, for the pressure, exactly while the level held is critical; a warning,
the normal level and the quiet period's return turn it off at once. What the
application asked of it (`uml_emulator_set_page_cache_trimming`) is kept apart
and survives each of these. A response does not start while another runs. The
emulator keeps what the application and the response want of the trimmer and
what the guest last took, and a request the guest did not take (a failed send,
or a quiesced guest) is sent by the next call or by the resume, which replays it
after the wake. A request skipped for a guest the host has quiesced is not
retried; one that could not be written, or meets another lifecycle call of the
guest or a console request in flight, is retried up to four times, a second
apart, until a response of its own or a level that no longer asks for it takes
its place. A lifecycle call (quiesce, resume, stop) goes before a pressure
request, which gives way and is retried. A quiesce counts its trim's reply wait
from its call. A request wakes a guest that suspended itself. macOS and iOS, and
both applications, get it through the runtime. Each notification the system
delivers is also reported to the application (the level, and whether a response
starts or is already running), once and before that notification's work, the
normal level included (`set_pressure_observer`, and the runtime API's
`set_memory_pressure_callback`). The source's handler only records the delivery
and a second serial queue handles the deliveries one at a time, in the order
they arrived: a level raised during a response is reported and answered after
it, and a notification the system itself merged reads as its most serious flag.
At most 64 deliveries wait, and the oldest is dropped beyond that. An iPad's
source never delivers the normal level, so the level held returns to normal
itself, the same way and with a report, after 240 s of device awake time (the
clock stops while the device sleeps) in which the source delivered no level but
normal (the policy's timer, no thread of its own); a process suspended for
longer meets the return at its resume, which only asks the trimmer off. The
observer lives for the process. Each level change and each response is one
rate-limited line: the footprint, the failed guest-RAM wiring operations and, on
a response line, the state of the trimmer and, for a critical level, the bytes
of graphics the GPU release freed.

**Budgets.** The process's memory limit, L, is the figure the memory budgets
derive from. iOS enforces one per application
(`platform::process_memory_limit_bytes()`); macOS enforces none, so a
measurement run states it (`--memory-limit=`) and every budget computes on the
Mac as on the device (`runtime/virtual_jetsam.hpp` marks where iOS would have
ended the process). The
host GPU's budget, B, is computed once at launch and fixed for the run
(`platform/gpu_budget.hpp`, planned by `emulator/src/gpu_budget_plan.*`):

```text
B = max(L - A - G - E - S, 352 MiB)
```

G is the guest's RAM, E the guest side's host memory, A the application's
expected ceiling and S the unused headroom; the guest's size does not bound B.
With no limit there is nothing to derive B from and B is G. A, the rest
of E and S are launch parameters defaulting to the conservative launch set in
`runtime/guest_memory_budget.hpp`. L is the limit the system advertises to the
process; shortage elsewhere in the system is not counted. A launch under the
floor is never refused: B is the floor and the launch says the process is likely
to be ended. [`graphics.md`](graphics.md) owns what B bounds. A live camera
session holds host memory outside the GPU (the camera device's reserve,
`device/camera/camera_tiers.hpp`), and the GPU's pool is B less it
(`gpu_pool_bytes`), taken after the clamp; blocked cameras and none take
nothing, a reserve that takes all of B refuses the launch (a pool of zero is no
limit to the renderer), and the applications show the pool. The reserve is the
use the host's cameras compose, which the runtime answers before the boot
(`plan_camera_reserve`, the next boot's figure while the cameras, the source
and the calibration data are unchanged); the loader's ceiling, which bounds the
system cameras, is the figure only where no runtime can say.

The guest side's memory follows the vCPU count, v, and that header is the one
place that says how; every caller evaluates it:

```text
E(v, G) = c(v, G) + x(v) + 4 MiB * v + E_rest
c(v, G) = min(derive_budget(G, L), cap(v))      where L is set; derive_budget(G, L) alone otherwise
```

`cap(v)` is a table of the gadget cache's ceiling per vCPU count, and `x(v)` a
table of the most the cache's charge (open accounts plus those asked to shed)
exceeds its budget in an Android guest's burst: the cache sheds to `c(v, G)`,
and `x(v)` is what the guest side holds beyond that while shed accounts wait
for their last holders. The largest
guest RAM a limit leaves, `G_max(v)`, is the largest G (in 64 MiB steps, from
512 MiB) that keeps B at its floor or above, up to the host's ceiling, C
(`guest_memory_ceiling_mib`): the RAM the product's native kernel window holds
and no more than the host's own RAM. Nothing else bounds it, so the offered
maximum is what the budget gives the device at the vCPU count. C is 9152 MiB
on an iOS device and in the Simulator, where the window is packed into the
signed 14 GiB envelope (`device_window_guest_memory_maximum_mib`, pinned by the
layout tests against `runtime/native_kernel_layout.hpp`), and the host's RAM
(at most 1 TiB, the largest limit a launch states) in a macOS window, which a
Mac and an iOS application on a Mac use; where the host sets no limit `G_max`
is C. In an iOS application signed without Extended Virtual Addressing C is also
held to the address space the process has: the system gives such an application
one run of a few GiB (its highest mappable address is under 512 GiB), and the
guest's window (`native_window_bytes`) and the process's heap region (1 GiB,
`memory/heap_region_size.hpp`, which has no other hole of that size there) must
fit in it. The loader measures the run once at launch, for that process alone
(`platform/apple/os_address_space.hpp`: the Simulator, a macOS process and an
entitled application are not measured and are answered as they always were), and
`address_space_guest_memory_mib` turns it into the largest RAM that fits, which
bounds C, the offered sizes and the defaults alike. A run starts exactly the sizes from 512 MiB through C in
64 MiB steps (`guest_memory_admitted`, with `process_guest_memory_ceiling_mib`
for a process): the runtime refuses any other, and the loader answers both
applications with the same two functions. The settings of Kernarium and
Andarium offer no more than `G_max(v)` under a set that counts none of A,
E_rest and S (`maximum_memory_set`), so every size they offer is one a run
starts; each application's own copy of the range is held to the loader's by a
hosted test. The
conservative size is `G_max(v)` under the conservative launch set, in 0.5 GiB
steps from 1.5 GiB and never above that maximum
(`conservative_guest_memory_mib`); a guest with more is likely to be ended by
the system, and Andarium says so on Home and beside its memory picker
([Andarium](../Andarium/README.md)). A new guest's RAM is the conservative
size held to 4 GiB (`default_guest_memory_mib`), however much the host offers,
and on a device whose maximum at one vCPU reaches 4 GiB at every vCPU count
also no more than the largest 0.5 GiB step that leaves B at 1 GiB or more,
since a 4 GiB guest otherwise runs the host GPU short long before it runs the
process short; that step is never under 2 GiB, because every limit that offers
4 GiB at one vCPU leaves a 2 GiB guest more than 1 GiB at every count. A guest
with more than the default but no more than the conservative size is likely to
run out of graphics memory, and Andarium's System sheet says so beside its
estimate of B for the chosen RAM and vCPUs
(`gpu_budget`), from the default where the graphics rule lowered it
(`default_guest_memory`, `holds_gpu_budget`) and otherwise, where the cap of
4 GiB or the limit holds the default, only from the size at which B falls
under 1 GiB. The macOS launchers take the same default for a launch that
states a memory limit. The applications' vCPU limit is the rule
`application_vcpu_limit` in the same header: the host's physical cores, at most
8. It is the default and the maximum of Kernarium, Andarium and the macOS
launchers (`emulator/src/options.hpp`); an explicit `ncpus=` on a launcher
keeps the runtime's own range. Every iOS memory control has a macOS
counterpart, so a Mac run stays an accurate ground for measuring the device.

A kernel-range flush completes or the kernel stops: `flush_cache_vmap()` and
`flush_cache_vunmap()` are both `flush_tlb_kernel_range()`, its result is never
discarded and a persistent host failure panics, because nothing faults on a
mapping that should not exist. Software-MMU mappings are published only by a
page-table walker's batch, which is rejected when an invalidation arrives before
it ends; a 16 KiB host slot is a shared host page only when its four PTEs name
one aligned run of a single backing (`arch_vmap_group_order()`).

### Domain heaps

The default malloc zone returns what a restarted guest freed on its own
schedule, so a guest's metadata (the MMU's indexes, the native address space's
state, the engines' program and record tables, batch plans) stays out of it,
except for the holder words' roots and leaves of a mapped backing
([Translation caches](#translation-caches-and-guest-debugging)).
`memory/domain_heap.hpp` provides per-domain heaps carved from one reserved
region and released whole when their owner retires, and thread-local scratch for
one operation's temporaries. The GPU stack, Metal, system frameworks, devices and
the process's own objects stay in the default zone, whose residue is what
remains.

### x86-64 frame accounting

An x86 guest sees a store through any alias of a frame at once, so a code
change must reach every address at which some executor holds a translation of
the frame. The memory layer (`memory/mmu/frame_accounting.hpp`) keeps, for each
4 KiB frame of a backing:

- the writer locations: the {generation, page} pairs whose mapping is writable.
  A location exists for as long as its writable mapping: the four mapping
  primitives add it before the mapping is installed and take it back after the
  mapping is gone, and a change that fails or is abandoned takes back only
  what it added;
- the holders: the locations at which an x86 translator registered the frame
  before reading a byte of it, each with a translatable word (guest code with
  execute) naming the frame. Registration also returns the writer locations
  whose write translations may predate it; the translator retires them and
  waits for the participants inside the engine before it caches anything. A
  debt larger than the translator's buffer is asked for again with room for it;
  when the room cannot be had, or the frame's writers are not known, the
  translator retires every write translation of every participant instead; and
- one flush serial per generation, which every walk of the generation moves
  after removing its holders, so that a translation recorded before the walk
  never stamp-hits after it.

A write fill tags a store for publication when the frame has a holder other
than the writer's own location, and a tagged store, a kernel write through the
kernel's own alias of the frame, and `invalidate_code` publish at every holder.
A kernel write publishes at the frame its user word names and at the frame the
host's table of the kernel's direct ranges (32 of them, split on a partial unmap
and counted when full) resolves its buffer to; a buffer the table does not
resolve, or resolves to a descriptor with no backing alive, is counted and makes
every generation read every translation again. Once the process runs x86 code,
the kernel's call after it wrote another process's page
(`darwin_uml_vm_invalidate_code_frame()`, which names the frame written)
publishes at that frame's holders, whatever the target maps at the address now,
whatever its view of the frame permits and whatever the arm64 holder word
names. A write that names no frame is an unknown frame, which moves every
generation's flush serial and publishes a whole-space code change (the range
{0, 0}) in each, and so does a page `invalidate_code` (an AArch32
`cacheflush()`, by address) meets that the target has no entry for yet; for the
pages the target does map, `invalidate_code` publishes their frames. A kernel
write into a page-cache, memfd or anonymous frame (a write(2), a UBD read, or a
copy into a buffer-vector destination such as a loop device's read or an io_uring
fixed-buffer read of a file, into a page a process pinned and runs) ends in the
x86_64 kernel's `flush_dcache_folio()` or `flush_dcache_page()`, which names the
frames (`darwin_uml_vm_invalidate_code_frames()`, called only for a folio queued
for the host with guest-code permission, which is a page of a `VM_EXEC` mapping)
and publishes at their holders, or returns without work when no x86 code has
run or the frame has no record. Three limits remain. The kernel's stores into
an executed io_uring ring whose pages the program supplied itself
(`IORING_SETUP_NO_MMAP`) are not announced. A loop device's zero fill of a read
past the end of its file is warned about once and not announced when its
completion runs in a softirq, as it does on a guest with more than one vCPU. The
announcement follows the write, so a second vCPU may finish instructions it had
already begun after the bytes changed.
When a run of the emulator ends it appends the process's counts so far, which
nothing resets (unresolved kernel writes, dropped direct ranges, global holds,
registrations that reached the address space and their failures, refusals,
lost stamp hits and the frames a kernel write named), to the file
`UML_X86_ACCOUNTING_LOG` names; the unresolved writes and global holds stay
near zero in an ordinary guest.

Every change of a frame's locations happens under that frame's stripe lock, and
nothing under it allocates, makes a record or takes another lock: the index
leaf, the frame's record and the room a list needs are made before the lock,
and a lookup under it only finds. The alias ids that name a generation in a
location resolve through a per-domain table that readers need no lock for. All
of the accounting's metadata (the writer index, the records and their lists,
the alias table, and a plan's entries in thread scratch) comes from the domain
heap, so a guest's frames never grow the default zone: about 2 MiB per GiB of
guest memory mapped writable, and about 230 bytes for each frame with two or
more writers. The x86-64 guest alone compiles it.

### Executable code

Gadget streams, MMU backing, and shadow mappings are data. Only
`darwin/cpu/jit_memory/executable_memory.cpp` may allocate executable memory,
publish RW pages as RX, control JIT write protection, or publish the
instruction cache; the arm64 JIT arena and FEXCore (through hooks the embedder
installs) reach it only through that file, and the `native` engine kind and the
source audit keep the gadget engines and shared CPU code from linking or
including it. The one exception is Mesa llvmpipe's own LLVM JIT in the iOS
Simulator product, whose GPU prefix alone carries LLVM; prefix verification and
packaging reject it in the macOS and iOS device slices. On macOS the owner serves
JIT memory from a window near the host image, because generated code calls into
it and a far buffer made x87-heavy FEXCore code about a third slower; memory
served again may hold its old bytes.

An iOS device has no `MAP_JIT` for a process that no debugger launched, so the
owner makes executable memory there from one region that a debugger the user
attached blesses once per process (`prepare_debugger_region`). The owner maps
the region read-execute below 1 TiB near the image, makes a writable alias of
it, and executes `brk #0xf00d` with `x16` 1, `x0` the address and `x1` the
length; the debugger writes one byte into each 16 KiB page of it, and a second
request (`x16` 0) makes it detach. The pages stay executable with no debugger,
the owner proves them by running a stub in the first and the last granule under
a fault guard, and wires the region through the alias. It serves allocations
from the region in 16 KiB granules; the JIT arena writes through
`writable_alias` and publishes the instruction cache at the executable address.
The region is sized from the process's JIT cache budget, which is the gadget
cache's term of the guest memory budget (executors times the arena, the tracked
store and the single-instruction arena), so the GPU budget and the memory limit
do not move; a process prepares it once, for the guest memory and vCPU count of
its first run. Only the iOS device product compiles it
(`UML_IOS_DEBUGGER_JIT`). A start that cannot have the JIT runs on gadgets and
says why; it never fails. Andarium asks for the preparation through
runtime API 30 (`prepare_jit`, `use_jit`) as it opens, because StikDebug spins
until it is served and is ended by the system within a minute; the region then
sits wired until the process ends, and a run that does not use it (the JIT is
off, or the guest changed after it was prepared) carries it beside its own
cache, which the run's GPU budget plan counts as engine memory.

The owner reads `P_TRACED` right before every request and sends the `brk` from a
thread that has `SIGTRAP` unblocked, under a process-wide `SIGTRAP` handler that
steps over a `brk #0xf00d`, counts it and passes any other trap to the
disposition it found. A debugger that vanishes while a request waits (its
debugserver holds the process traced for a while, then lets go) leaves the
request unanswered instead of ending the process; the answer is "no debugger"
when none is attached afterwards, and a region it never prepared is given back.
The debugger is StikDebug, and
`tests/integration/support/ios_jit_debugger.py` is the harness's: it attaches to
the process of its own stopped launch and serves the request at launch.

Every product that embeds a kernel carries its ELF as a host-page-aligned
Mach-O `__TEXT,__uml_elf` section, which the emulator runs in place: the loader
aliases the immutable executable pages, copies only data and BSS, and rejects
writable executable segments or relocations into signed text. On iOS the image
must be that section, because no other code can be made. The loader admits only
an AArch64 image, so the x86-64 userspace of the macOS compatibility product is
translated by its engines, never loaded. Kernarium's app embeds every selectable
runtime framework, links none at launch, and loads exactly one guest flavour for
the process lifetime.

The macOS product builds at a 27.0 minimum and the iOS device and iOS Simulator
products at 26.0 (`cmake/toolchains/Apple.cmake`); the iPad applications that
also run on a Mac name that 27.0 in their `LSMinimumSystemVersion`, which the
App Store would otherwise take from the iOS minimum. Code that reaches an iOS API
newer than 26.0 sits behind an availability check with the earlier API as its
fallback, and a floor-driven fallback names the release that introduced the
API. Apple products consume accepted kernel bundles
and never invoke Kbuild: one mainline artifact for arm64 userspace on macOS and
the iOS device, a separately identified 4 KiB mainline slice for macOS x86_64
userspace, and a separate accepted 4 KiB `android-6.18` artifact (the same
mainline tree with the Android configuration) for Android. Kernarium and
Andarium are iOS products ([`../Kernarium/README.md`](../Kernarium/README.md),
[`../Andarium/README.md`](../Andarium/README.md)); the device slice also runs on
an Apple silicon Mac, and the Simulator has a UI-test variant with the
CPU-rendering GPU prefix ([`graphics.md`](graphics.md)).

## CPU execution and scheduling

Darwin owns one persistent pthread per configured vCPU, including vCPU0
(`uml-vcpu-N-carrier`). Linux task contexts remain on guest kernel stacks and
migrate between carriers; guest task creation does not create host threads. An
embedded run returns to the loader, which unmaps the image, only after the
kernel has joined every carrier and stopped its RTC alarm helper.

**Exit contract.** A host can call `exit()` while carriers still run, as an iOS
application and an XCTest host do. State that carriers use until their process
ends therefore has no exit-time destructor: it is constant-initialized or never
destroyed. That covers the arm64 gadget contract and twin indexes, the
shared-backing state, the active-access registry, the gadget cache ledger, the
MMU's thread-slot and domain state, and the console relay's and loader's output
registrations and locks (`arm64-gadget-exit`).

Darwin signals wake carriers but do not execute Linux IPIs asynchronously;
IRQ-enabled spin checkpoints service pending IPIs. Early UML calibration uses
process-directed `SIGALRM`, which application and launcher threads keep blocked
and only the vCPU0 carrier unmasks; Kernarium runs the guest on its
`uml-launcher` thread, never the main thread. The embedded kernel's signal
handlers and interval timer outlive its entry, and Darwin never moves a
process-directed signal off the thread it chose, so before it releases the image
the loader disarms `ITIMER_REAL`, discards pending instances of every signal
whose handler lies in the image, and reinstalls the host's dispositions and mask.

The product has two selectable engines: the gadget engine executes arrays of
precompiled function pointers and data, and the JIT emits native blocks through
its sole executable arena. Guest-visible exceptions, fault priority, retirement,
and feature exposure must agree across engines and with the independently
derived expectations of the A64 instruction corpus. Instruction classes the JIT's
native lowering leaves to a helper run in the JIT's own semantic slow path, never
in another engine, and a helper treats any word outside its routed classes as a
fatal internal error. Each engine's internals are in its README
([gadgets](../darwin/cpu/arm64/gadgets/README.md),
[JIT](../darwin/cpu/arm64/jit/README.md)); FP trap-enable semantics are decided
for both by `darwin/cpu/arm64/detail/floating_exception.hpp`.

The arm64 gadget code is one region in `darwin/cpu/arm64/gadgets/engine.S`: the
bank, the sections its gadgets push variants and cold paths into, an island of
veneers, and last the run part, whose entries branch into the bank and call
only helpers the bank already reaches through the island. Every image
that carries it links `UML::GadgetRegionLayout`, which orders it first in
`__TEXT,__text`, so the region starts on a 16 KiB page, each
part starts on its boundary plus the phase `engine.S` defines, and its bytes
depend on `engine.S` alone. Private labels take the Mach-O assembler-temporary
prefix `L`, and a branch to one in another part encodes its distance past the
nearest named symbol as a signed 24-bit addend, so such a target must lie within
8 MiB of one (`gadget-engine-temporary-labels`). The bank holds no padding: a new
entry or family is appended after the established ones of its part, while a
removal moves every later entry and is measured on the iPad like any gadget
change (`arm64-gadget-region-layout`). The x86-64 gadget code is pinned the same
way, a bank and a cold part of fixed slots, one per group, from
`engine/region.inc` (`x86-64-gadget-region-layout`; the
[x86-64 gadget README](../darwin/cpu/x86_64/gadgets/README.md) says what a change
moves).

Data-alignment faults retain the effective data address and the faulting PC,
and every kernel product delivers `SIGBUS/BUS_ADRALN` so a guest handler can
repair the access and resume. Both engines advertise USCAT exactly when the host
implements FEAT_LSE2; then a misaligned LSE, exclusive or acquire/release access
inside one 16-byte block is single-copy atomic, one that crosses a block faults,
and without host LSE2 every such access faults.

**Exclusive monitor.** A load/store-exclusive loop makes progress whatever the
TLB holds, and nothing an engine does between the pair without an exception fails
the store. An exclusive load fills the write TLB entry and records the host
address and loaded values in the `CpuState` exclusive fields
(`engine/exclusive.inc`, reused by the AArch32 exclusives), and a store-exclusive
that still misses completes in software with one host compare-and-swap of the
recorded values. The JIT's semantic path keeps a software reservation (versions
of 16-byte host granules), so a store to the reserved granule, including the
kernel's futex and SWP emulation, fails the store-exclusive. The JIT runs an
A64 or AArch32 pair it fuses on the host's own monitor, and a side exit of a
fused run rolls back to the guest state at the load-exclusive, which then
executes on the software reservation; no exit clears the monitor and continues.
Only a host interruption of a natively fused pair fails its store-exclusive,
never systematically, so a loop that meets the architecture's progress
conditions makes progress. The monitor ends at a store-exclusive, a CLREX, an exception and a
store to the reserved granule; a store of the guest's own to another granule
leaves it standing, as on hardware (`arm64-exclusive-progress`,
`arm64-exclusive-events`, `arm64-jit-exclusive_completion`,
`arm64-jit-reservation_alias`, `arm64-jit-exclusive_run_fetch`,
`arm64-jit-exclusive_run_runtime`, `arm64-jit-exclusive_run_contention`,
`arm64-jit-a32-exclusive`,
`guest-exclusive-progress-gadgets`/`-jit`).

Both engines complete WFE and WFI at once and treat SEV and SEVL as no-ops, so
no wait depends on an event. Completing WFI deliberately deviates from arm64
Linux, which kills the program with SIGILL; AT_HWCAP therefore always carries
EVTSTRM.

### SME and SVE

The SME profile (FEAT_SME with FEAT_SME_F64F64 and FEAT_SME_I16I64, plus
FEAT_SME2 where the host has it) runs at the host's streaming vector length,
because native execution needs the guest's SVL to equal the host's;
[`cpu/arm64/sme_profile.hpp`](../darwin/cpu/arm64/sme_profile.hpp) derives it
for both engines. It is on by default wherever the host has it
(`DARWIN_UML_GUEST_SME=0` forces it off). An SVE or SME word an engine does not
implement is the illegal-instruction exit, and both engines are `complete`, so a
guest is told of SME1 only. Only the M4's SVL of 64 bytes has been checked on
hardware, and the iOS SME path is unvalidated on hardware.

The virtual CPU has no SVE outside streaming mode and advertises none
(`platform::without_sve`); every SVE word outside streaming mode, other than the
family below, is the illegal-instruction exit in both engines, so a binary that
computes with SVE there takes SIGILL. One deliberate departure covers a CPU with
FEAT_SME: CNT[BHWD], RDVL, ADDVL, ADDPL, the scalar INC and DEC forms, and LDR and
STR of Z and P registers with a scalar base and MUL VL offset execute at
PSTATE.SM = 0 for one constant vector length of 128 bits. The architecture makes
them UNDEFINED there, but compilers target armv9-a, so a locally-streaming
function reads the non-streaming vector length first and would take SIGILL on
every SME host. The family keeps SME code running and is no promise to run SVE
code;
[`cpu/arm64/detail/sm0_vector_length.hpp`](../darwin/cpu/arm64/detail/sm0_vector_length.hpp)
defines it for both engines, and the corpus holds the results as project-defined
(`arm64-sm0-vl`).

### AArch32 and top-byte ignore

Both arm64 engines run a 32-bit Arm guest: AArch32 EL0, A32 and T32 with
interworking and IT, for the armhf userspace of Debian and Android's
armeabi-v7a, which needs hardware VFPv4-D32 and Advanced SIMD. The profile is in
[`cpu/arm64/aarch32/a32_profile.hpp`](../darwin/cpu/arm64/aarch32/a32_profile.hpp)
and every arm64 product presents all of it with no switch;
`CpuEngine::run_aarch32` is the entry for a task in AArch32 state. The PC is a
translation key
([`cpu/arm64/aarch32/guest_key.hpp`](../darwin/cpu/arm64/aarch32/guest_key.hpp))
that carries the ISA, so one gadget cache, JIT arena and shared tier serve A64,
A32 and T32. The compat `AT_HWCAP`/`AT_HWCAP2` words derive from the host the
selected engine presents, and a non-zero word is the kernel's whole evidence
that a run executes AArch32. An entry in a state the kernel may not hand over is
fatal, as is a fault address wider than 32 bits, which must never become a guest
SIGSEGV whose compat siginfo would hide the upper half.

One decoder serves every AArch32 consumer: `darwin/cpu/arm64/aarch32` decodes
A32, T16 and T32 words from a table generated from the pinned Arm AArch32 XML,
and whatever the Armv8-A AArch32 EL0 profile lacks is undefined, never guessed.
It is a CPU-layer `shared` target that includes no engine
([README](../darwin/cpu/arm64/aarch32/README.md)). The semantic library
([`cpu/arm64/aarch32/semantics.hpp`](../darwin/cpu/arm64/aarch32/semantics.hpp))
is shared C++ semantics, not another engine: it works on `CpuState` alone and
reaches guest memory only through the caller's `A32Memory`. The engines route
their slow paths to it, and a word outside `a32_semantics_covers()` is a fatal
internal error.

Under top-byte ignore, which every arm64 run has unless the host passes
`--guest-no-top-byte-ignore`, both engines strip the top byte of a data address
before they translate it, so a data abort carries the translated address. The
kernel therefore does not untag the fault address in `segv()`, and offers the
tagged-address ABI only while the host reports the ignore; the contract is in
the [shared kernel README](../kernel/common/README.md).

### x86-64 userspace

The macOS 4 KiB x86 compatibility slice keeps native arm64 UML kernel-mode
execution while exposing the x86_64 and i386 userspace ABIs. It cannot load
arm64 userspace; per-task mode selects only an x86-family operand and address
width. The host routes userspace instruction execution through immutable,
data-only streams of pointers to precompiled handwritten arm64 gadgets, with no
semantic C++ fallback ([gadget README](../darwin/cpu/x86_64/gadgets/README.md)).
What any x86 engine shares with the gadget engine forms `uml_x86_64_target` in
`darwin/cpu/x86_64/`, a `shared` CPU target that the source audit keeps free of
engine includes. The pinned FEX handwritten corpora are the external
instruction acceptance gate, which is not proof of an arbitrary distribution or
of complete Linux syscall compatibility; AVX-512 and later, RTM/MPX, 16-bit code
segments and privileged or virtualization instructions are outside the profile.
The normal mainline slices and Android remain arm64 kernels with none of the
x86_64 kernel overlay.

On macOS the product can instead run its tasks on the x86-64 JIT, FEXCore
embedded behind the static MMU (`--execution-engine jit`; the default is the
gadget engine); the [JIT README](../darwin/cpu/x86_64/jit/README.md) records its
contracts. The JIT is a macOS product only: the `macos` dependency prefix alone
carries FEXCore, configuration refuses `UML_BUILD_X86_64_JIT` elsewhere,
`architecture-audit` rejects the JIT target and any FEXCore link fragment outside
a macOS tree, and `scripts/check-no-fexcore.py` fails the iOS packaging on any
FEXCore symbol.

### Translation caches and guest debugging

The JIT's shared translation tier holds immutable guest identity and
relocatable code only, links are scoped to an address space, and no persistent
executor survives its runtime memory domain. The gadget cache tags slots and
links by address space and reuses a same-generation stale program only after
exact page-bounded word validation. Sweeping, renewal and the process byte
budget are in the header comments of `darwin/cpu/gadget_cache_geometry.hpp`,
`darwin/cpu/gadget_shared_space.hpp` and `darwin/cpu/gadget_cache_budget.hpp`,
summarized in [`../darwin/README.md`](../darwin/README.md).

To single-step a task, Linux sets the architectural step bit in the entry
registers (arm64 PSTATE.SS, x86 RFLAGS.TF); every engine then retires exactly
one instruction and reports a single-step exit, which Linux delivers as
`SIGTRAP`. Linux writes another process's memory (ptrace, `/proc/pid/mem`,
`process_vm_writev()`) through its own view of the frame, so no translated store
publishes the change: those paths call `darwin_uml_vm_invalidate_code_frame()`
for the target mm's generation and the page written, naming the frame (`(fd,
offset)`, as for a mapping) they hold, so the change reaches that frame's
holders even when the target maps another frame at the address by then. An
AArch32 `cacheflush()` writes nothing and calls
`darwin_uml_vm_invalidate_code()`, which names an address.

An arm64 guest that rewrites code and synchronizes it (`IC IVAU`, broadcast on
hardware and keyed by the physical address) changes what every process running
the frame executes, but a store or a publication reaches only its own address
space's generation. A guest's code synchronization is made in the generation of
the execution context its thread has entered, not in the generation the thread
last activated for the kernel's calls. So each frame of a mapped backing has a
holder word (`darwin/memory/mmu/code_holders.hpp`): the arm64 instruction-fetch
path registers the fetching generation in it before it reads the frame's
bytes, and a guest code synchronization or the kernel's remote-write paths take
the word and publish a whole-space code change in every generation they named. Both
also move the frame's code-page stamp (version and epoch) where some mapping
executes the frame: through the publisher's own view of the frame, whether or
not that view can execute, or, for a frame the kernel names that the target
does not map at the address, through the frame's record. The step is ordered
before the publisher reads the word, and a verifier's registration before it
reads the stamp, so a verifier is told of a publication or shown its step. A
kernel write that names no frame (a page the target's address space does not
map, or a frame Linux cannot name) moves no stamp, so it moves the epoch of
every page at once instead. A publication consumes the word, so a reader
registers again at its next fetch; a publisher that has emptied a word stays
counted until its publication finishes, and one that meets such a count
publishes to every generation, so an empty word never hides a reader another
publisher has yet to tell. A remote write takes the word of the frame it names,
whatever the target maps at the address, and publishes to every generation,
the target included, for a frame it cannot name or when it meets an unfinished
take. `invalidate_code()` takes the word of every page the target maps,
whatever its permission, and does the same for a page the target has not
mapped yet (its software page table fills at faults, so the frame is
unknown). The target itself hears of a change at every address (a whole-space
change) when it holds the frame or the list goes to every generation, because
it may run or have verified the frame at an address other than the view
written, one that cannot execute or none at all; a target that holds nothing
hears of the range written. The words
live in 4 KiB leaves made on a frame's first registration (a leaf per 512
frames), and the leaves and their roots (4 KiB per GiB of backing, made with
the backing in every build) come from the ordinary allocator, sized by the
backing and freed with it, not from a domain heap. A backing whose leaf cannot
be made, or a word that names more generations than it holds, publishes to
every live generation instead.
The x86 engines neither register nor consult it: they hold their frames in the
[frame accounting](#x86-64-frame-accounting), which the arm64 engines never
consult, and the kernel's remote-write and flush paths run both publications,
each on its own condition.

### Ahead-of-time code

The Android image's own arm64 libraries (`tools/aot/libraries.txt`) and the
compiled Java of its boot image (every `boot*.oat` with code) are rewritten
ahead of time by `tools/aot` into one blob, which the gadget engine runs as
native code in place of its translation. The blob is a release artifact
(`tools/aot/release_artifact.py`): built once from an accepted Android release,
bound to its release ID, its system partition and the files the rewrite is a
function of, and accepted with that release by
`android/lineage-24/metadata/accepted-aot.env`. A product wraps the blob in
`UmlArm64Aot.framework` and generates nothing; macOS, the iOS Simulator and the
iOS device each do. Only a build that qualifies a release or measures a
variant generates the blob from the release's image (`UML_AOT_IMAGE`; the
diagnostics build needs it). A new accepted image needs the AOT differential
run, its scan of every word of every function included, before its artifact is
accepted: the scan judges the image it was built against.
`UML_AOT_MODE` (`AUTO`, `ON`, `OFF`) says whether a build has the component
at all: `AUTO` builds it when an artifact is available, and a build without
one has no AOT target, and the gadget executor compiles none of its hooks. The
framework is text and read-only tables only:
no data segment, no relocation, no executable allocation, so its pages are
clean and file-backed and one copy serves every process at any load bias.
`darwin/cpu/arm64/aot` is a gadget-kind CPU component whose only caller is
`GadgetExecutor::run_aot`; it includes no engine and reaches memory only
through the executor's TLB and `StaticGuestMemory::fetch`.

- Switch: the component is on wherever a product carries it and the launch boots
  the release the blob was made from (Release, below). `DARWIN_UML_AOT=0`
  in the environment, or `--aot=off` on the launch, runs the engine alone;
  `--aot=on` (or `--aot`) fails a launch whose build has none, and the launch's
  word beats the environment. Each run makes the choice before its first
  executor and any per-thread table exist, and only once it owns the process
  (the address-space admission), so no other handle reads or changes the
  choice between a session's end and the next one's; a restart in the same
  process decides again, and a release the blob no longer fits runs on the
  engine.
  A session that ran without the component does not turn it on later, but the
  release of its guest after Android's clean power-off ends the session, and
  the next boot in the process decides again, for the release it states. An
  application's own word is the boot request's `android_aot_disabled`, which
  keeps the release out of the launch (Release, below): Andarium's Home switch
  sets it at each start, on by default. An
  archive of Andarium's Release configuration carries the component only when
  `accepted-aot.env` sets `LINEAGE24_ACCEPTED_AOT_ARCHIVES` to `yes`: the iOS
  device runtimes take the accepted artifact only then.
  `scripts/check-embedded-artifacts.py --aot` fails a
  packaged framework without an accepted artifact or while the field is `no`,
  one whose blob is not the accepted digest or was not made from the packaged
  system (the three values the launch compares, below), and an application whose
  packaged runtime links the framework (its load commands name it) while the
  framework is absent, since dyld refuses to launch it; the application's
  optional embed step (`embed-device-framework.sh --optional`) stops the build
  for the same missing slice.
- Release: the blob runs only with the release it was made from. Its header
  carries the release ID, the SHA-256 of the release manifest and the SHA-256
  of the system partition; the launcher (macOS and iOS) reads the same three
  values from the verified release it boots (`Storage::releaseIdentity()`) and
  states them to the emulator (`--aot-release=`). Selection compares them
  before anything else: another release, a proof or test image, an updated
  image whose blob is stale, or a launch that states none (a Linux guest, a
  release with no identity, or a boot request with `android_aot_disabled`) runs
  without the component under `auto` and fails
  UNSUPPORTED under `on`, and allocates no AOT state. Only a launcher that
  attaches an Android release states the release; the Linux launcher refuses
  `--aot-release=` on its command line. The tests of a build with
  `UML_AOT_DIAGNOSTICS` select it without a stated release; a product has no
  such entry.
- Units: each library is a self-contained unit of the framework. Branches
  between libraries mapped in one address space stay in rewritten code when
  the target page is verified (below); any other target leaves to the engine.
- Binding: the framework stores the SHA-256 of every guest page it stands
  for, whole and untruncated. A mapped text page authorizes native execution
  only after its own SHA-256 equals the stored one, made at the first entry
  into that page and again after the address space's code-change history
  names it or no longer reaches back; a 64-bit digest of a page only finds
  which library it belongs to. Rewritten code therefore runs for bytes other
  than the ones it was built from only on a collision of SHA-256, and no
  weaker digest, truncation or checksum takes its place. The hash is
  CommonCrypto's `CC_SHA256`, part of libSystem on macOS, iOS and the iOS
  Simulator, on the host's SHA-2 instructions. Every edge that leaves a
  verified page (a branch, a fall-through, an indirect branch, a call between
  libraries) tests the state of the page it reaches, so rewritten code never
  runs from a page that was not verified. A page the
  guest has not mapped yet is not entered and is tried again later; a page
  that differs retires the library's instance for that address space, and the
  engine runs the code. A page is entered only while its mapping carries the
  kernel's immutable fact (bit 1 of the map call's `guest_code`): without it
  another alias or process can change the bytes with no word to the address
  space that compared them, because a store that no code synchronization
  follows reaches no holder. A mapping without the fact is the engine's
  (counted as refused), and one that loses it is published as a code change of
  the page, so the pages compared on its strength are compared again or
  refused. Verifying a page registers the address space as a holder of its
  frame (a fetch of one byte) before the first stamp is read, whether the page
  is then judged by the shared table or by hashing, so another address space's
  code synchronization or the kernel's write of the frame clears the compared
  pages of every address space that compared the page, and a verdict is never
  answered under a stamp that a publication has moved.
- Shared verdicts: the SHA-256 comparison of a physical page is kept for
  every address space that maps the page, under the page's identity and
  epoch, only when the kernel reports the page immutable in the mapping
  being verified (bit 1 of the map call's `guest_code`) and the page's record
  counts its executable mappings (a record that ran out of count or of epoch
  answers none, for good). The epoch moves at a publication that names the
  page's frame, through any view of it the publisher has or the frame the
  kernel names, and when its last executable mapping departs; a kernel write
  that names no frame (a target page its address space does not map, or a
  frame Linux cannot name) moves the epoch of every page at once (a
  process-wide count of such writes, which at its limit turns sharing off for
  good). So a verdict is answered only while the page has
  stayed executable in some address space since it was filed and no change has
  reached it. A mapping that cannot execute or has no stamp is never answered,
  and one that is not immutable is not entered; the table (process-global,
  direct-mapped) is a hint that may forget.
  `UML_AOT_SHARED_VERDICTS=OFF` builds a runtime that never asks, to measure
  what sharing buys; the differential's shared rows expect a second hash
  there and pass in both settings.
- Entry: only at an instruction the framework covers, in a state the engine
  would run the same way: no single step, PSTATE.SM clear, no FPCR trap
  enable, FPCR.FIZ, AH and NEP clear (read as the guest's, whether or not its
  floating-point state is loaded yet), no open exclusive monitor, top-byte
  ignore on, and a 16-byte aligned SP. Guest state is canonical at every
  instruction boundary, so entry and exit are possible at any covered
  instruction. Rewritten code keeps SP aligned: an instruction that can leave
  it misaligned ends the entry after it commits, so the engine's check
  applies to every SP-based access.
- Exit: whatever the framework does not inline stops with committed state and
  the engine runs exactly that instruction, so faults, system calls, signals
  and debugger exits are the engine's own. When such an instruction faults,
  the engine also runs it again where the guest resumes, without entering
  rewritten code for it. An entry runs at most the engine's chaining limit, plus one unpaid run of
  at most `RUN_LIMIT` (32) instructions and two more (the differential test's
  `entry_bound`), and the executor goes on into the next entry only while
  nothing the dispatcher checks has changed (the interrupt, mapping and
  code-change sequences, SVCR, the FPCR trap enables), so interrupts and
  shootdowns are noticed as they are in the engine. The limit counts
  instructions, not bytes or time: one memory copy or set is one instruction
  here as it is in the engine.
- Admission: an address whose entries keep retiring too few instructions to
  repay the cost of entering and leaving is left to the engine, and is
  entered again from time to time to find out whether it has become worth
  it. That history and the retry above are hints kept per executor and keyed
  by address alone. They can only decline an entry; every entry that is taken
  is validated against the address space as it is at that moment, so changed
  text, a remap or another address space can cost a declined entry and never
  run stale code.
- Memory and atomics: an inline probe of the engine's TLB entry, refilled
  through the executor on a miss (a scalar of more than one byte indexes the
  entry by its last byte and compares the tag with its first, so a page
  crossing misses; a stack access with an immediate offset inside one aligned
  16-byte granule cannot cross a page and is probed by its first byte);
  ordered accesses, the LSE atomic operations
  and compare-and-swap run as the host's own instruction on naturally aligned
  addresses. Advanced SIMD structure loads and stores (LD1 to LD4, ST1 to
  ST4, the lane forms and LDnR) run as the host's own instruction on the
  host address of the guest bytes when both ends are in pages the probe
  holds. A page-crossing access, an unaligned address, an SP base, a pair
  and an exclusive are the engine's. The epilogue of CPY, CPYF and SET (the
  engine's option B, `darwin/cpu/arm64/detail/memory_copy_set.hpp`) runs in
  rewritten code for a small forward copy or set inside mapped pages; every
  other case calls the engine's own routine on the same registers. A fault,
  or registers the prologue cannot have produced, steps the instruction with
  the registers committed at that region, so the engine takes the fault with
  its own state.
- Floating point and SIMD run under the guest FPCR/FPSR in the host registers
  the way gadget code does; the engine restores the host environment at the
  dispatcher boundary.
- Diagnostics: the CMake option `UML_AOT_DIAGNOSTICS` (off) composes a second
  compilation of the runtime with counters, a report file written by a thread
  of its own, and the test switches, and links a second compilation of the
  static MMU with the executable-mapping count audit the report reads
  (`UML_STATIC_MMU_TARGET` names the one a composition links). The product
  targets hold none of it in any configuration (`arm64-aot-runtime-product`
  and `memory-product-carries-no-count-audit` check the objects;
  `scripts/check-no-aot-diagnostics.py` fails the XCFramework and Kernarium
  packaging and the Andarium archive phase on a binary that linked a
  diagnostics compilation of either), and
  in no build does a vCPU thread do file I/O or allocate while it is inside
  rewritten code. The report says why the gadget engine ran instead of
  rewritten code (the engine's dispatches by refusal or find result, with the
  instructions it ran on covered pages), why a memory probe refilled the TLB
  (an empty slot, another page of the same slot, a page access of the other
  kind, a page-crossing access), and how many lookups a dispatch made; the
  executor's counters exist only in a diagnostics tree. A per-site refill
  histogram goes to a second file when the environment names one.

Build and tests: `tools/aot/README.md`.

## Kernel and Android ownership

`kernel/mainline-6.18` owns the pinned source identity, configuration,
generated patch series, and standalone build policy of both the Linux and the
Android kernel products. `kernel/common` owns only contracts shared across
families. The Apple CMake graph imports compact artifacts and checks their
bridge ABI, product family, and `6.18` release series; the manifest admits only
the 4 KiB guest page size. Patch releases, commit identities, and producer
provenance do not become runtime compatibility keys.

`android/lineage-24` owns the Android product, static GPT layout, ramdisks,
rootfs release, and producer tools. Kernel and rootfs releases are selected
independently. Apple emulator builds consume only the Android kernel artifact;
rootfs and command-line paths are launch inputs. The `uml-darwin-android17`
product opens one directly writable supplied GPT.

Guest bundles, disk capacity, and compressed rootfs delivery belong to the
application ([`../Kernarium/README.md`](../Kernarium/README.md)). Runtime
frameworks receive an ordinary disk image and no capacity policy, and reject a
GPT whose declared extent differs from its file; the guest only grows a
filesystem after observing that its containing partition is larger.

## Devices and graphics

The vhost-user transport in `darwin/device/vhost_user` owns connection, queue
framing, and deferred used-ring publication. GPU, video, input, sound, console,
battery, and network providers own their protocol behavior independently. Only
the GPU provider depends on `UML_BUILD_GPU`; the others build on the transport
alone. A disabled ring is given no new data: a buffer the backend only fills (a
virtio-input event) stays in the ring until it is enabled again, and a buffer
that carries a request is consumed and dropped, because its sender may be
waiting with interrupts off; only the GPU, whose requests are asynchronous, the
vsock and GNSS devices, whose requests the guest never waits on, and the video
devices, whose streams outlive a suspend, ask to keep them. A ring disable also
resets a device that holds a deferred request, and says so on stderr, unless the
device asks to keep it
(`keepsDeferredRequestsWhileDisabled()`, which the GPU, video and camera
devices do): the transport then leaves the device alone, keeps what it
completes while a ring is disabled and writes it to the ring at its enable, in
the order the device finished it. A fenced GPU command still on the host when
the guest suspends so keeps its contexts and resources, and its answer reaches
the guest after the enable. A message that changes the memory, the
features, a ring's layout or base, or ends the driver (the `GET_FEATURES` that
follows a teardown's disables included) resets such a device first. While the
transport waits for the front-end to acknowledge a backend request (a
shared-memory map or unmap, a configuration change, a call), that
`GET_FEATURES` is answered after the wait and behind every message already
queued, rather than inside it, since a reset sends unmaps of its own; so a
device with a shared-memory region may keep its requests too. The wait ends with
the acknowledgement, the end of the backend-request channel, the end of the main
socket, which aborts the operation before it writes anything the acknowledgement
would have released, even while the acknowledgement is being received, or a
service stop. Once a reply the front-end waits for depends on the wait, because
a query or a request that asks for an acknowledgement is queued behind it or
because the connection has ended and a reset's unmaps are all that is left, the
wait has a budget of ten seconds (`backendAcknowledgementBound`). Its own polls
spend it, each at most the interval it asked for plus 25 ms, not the clock: an
interval in which the whole process could not run, as for an application the
system suspended, costs one poll. An awake front-end that stays silent gets ten
seconds of charged poll time; real time is longer by the work between polls and
by an overrun beyond a poll's cap. A frame received during such a wait, from
either socket, has the same time from its first byte, within the wait's budget
when it has one; ordinary request reception is not timed. Past that the wait
fails, says so on stderr, and closes the backend-request channel: nothing a late
acknowledgement would release is written, the reset or the connection ends
without the unmaps it can no longer wait for, and the guest's driver drops the
device. A wait that nothing waits behind, such as a notification a suspended
guest has not yet read, has no budget. A reset whose unmap is refused or whose
channel has ended also drops every range the backend published from the host
index. That `GET_FEATURES` also retires every ring still disabled, which the
guest then frees: the transport reads it no more until its next `SET_VRING_KICK`
or in-band `VRING_KICK`, whereas an enabled ring, and a disabled one no teardown
followed, stay read and are refused when their available flags are reserved. A
write to the control or backend-request socket that fails because the front-end
closed its end (`EPIPE`, `ECONNRESET`, `ENOTCONN`) ends the connection without a
failure report; a write to a queue's call descriptor that fails still reports. A
`VRING_KICK` is accepted whatever the state of the rings: a kick on a disabled
ring is handled as that ring's buffers are, and a kick that asks for no
acknowledgement gets none.

**Idle transport.** An in-process transport blocks while it is idle: its thread
wakes for a message from the guest, for its device's event descriptor, for the
request gate and for `stop()`, which shuts the connection down, and for nothing
else; a queue in polling mode adds a 1 ms wake. A device must signal its event
descriptor whenever it produces work that no guest message starts: a
completion, a config change, an event for a buffer the guest queued, a timer. A
config change that `claimConfigChange()` withholds, or that a claim leaves
partly pending, is asked for again every 100 ms until it is claimed in full.

**Request gate.** A service can be given a request gate, which another thread
closes to hold the device's requests back: a closed gate lets no request reach
the device, waits a bounded time for the one the device is running, and leaves
later requests in their rings. Kernarium closes the GPU's gate as it enters the
background, because a request that reached Metal after iOS revokes the host GPU
would make the renderer's Vulkan device lost for good. A guest that ends cannot
wait behind a held gate: the kernel reports its end through
`darwin_uml_guest_shutdown()`, the devices in the guest-end registry
(`darwin/device/guest_end/guest_end.hpp`) are told, and the GPU service turns a
closed gate to discarding, so every request completes with the device's answer
for a request that did not run. `startNextConnection()` returns the gate to a
plain hold for the next guest.

The [Virtio video devices](../darwin/device/video/README.md) bridge the shared
V4L2 drivers to the platform VideoToolbox decoder and encoder over their own
vhost-user connections and need no GPU context. Their event queues never defer,
and the devices keep the buffers a stream has queued, so a stream and an idle
device alike survive a guest suspend without a reset, and the platform
sessions the system invalidates are rebuilt without a word to the guest. The
[Virtio vsock device](../darwin/device/vsock/README.md) carries a byte stream
from the host application to a service in the guest (adbd) with no listening
socket on the host; the guest cannot open a connection to the host. The
[Virtio GNSS device](../darwin/device/gnss/README.md) is the guest's
`/dev/gnssN`: it carries the host's fixes as an NMEA stream while the guest has
the receiver open, and nothing else of the host's location. The
[Virtio camera device](../darwin/device/camera/README.md) serves the host's cameras
through the video wire, on a platform camera the caller supplies; a blocked
session never opens a capture session.

**Lost backend.** A virtqueue call or kick descriptor, or the backend request
stream, that reports end of file or an error other than would-block means the
backend is gone. The guest stops that interrupt source at once, reports it once
per queue, and hands the device to the connection-broken path, which breaks its
virtqueues and unregisters it so that pending and later requests fail instead of
waiting (`guest-vhost-user-backend-gone`). A backend lost while the device is
being removed, as at power off, leaves the removal to the path that began it:
the exit call stops the connection-broken work before it removes a device, so
that a device is unregistered once (`guest-vhost-user-power-off`). The work is
queued by one loss report per device and holds the platform device's lock
around its lookup of the virtio device, so an unbind cannot free what it
breaks. Recovery
ends there: a lost GPU backend kills the guest's Vulkan client within seconds,
but a client waiting on a full control queue stays blocked until Mesa's own
ring timeout.

**Control socket.** A queue is kicked by a request on the control socket. Only a
console kick asks for an answer, which the backend sends after it has run the
queue: the console driver spins on its queue after a kick. Every other kick asks
for none, so that the kicking vCPU, which holds the socket with its interrupts
off, never waits for the backend's thread. While the GPU and sound backends run
a queue they tell the guest not to kick (`VRING_USED_F_NO_NOTIFY`) until it has
run dry. The guest endpoint is blocking with no deadline, because
`virtqueue_notify()` latches a virtqueue broken for good when a notification
reports failure; a backend whose answer never comes, or is malformed, is gone
and retired by the lost-backend path. Only their order pairs a reply with its
request, so the guest holds the socket from a request that has a reply until it
has read the reply and serves the backend request stream during every
synchronous wait.

The default Apple graphics path, from guest Mesa through Virgl and virglrenderer
to Zink on KosmicKrisp and the shared Metal presentation, is in
[`graphics.md`](graphics.md). GPU commands and rendering stay off the UI thread;
the shared presentation layer keeps the newest frame and at most one command
buffer in flight; only unsupported resource shapes use CPU readback. A guest
drives the renderer, so no host write into guest memory is sized by the renderer
alone: the device bounds a translated guest range to one negotiated memory
region, never writes a reply past the writable descriptors the guest supplied,
and the renderer ends a Venus reply stream at the window the guest declared.

Every GPU build also carries an independent Vulkan route, which the host always
advertises and which does not replace or enter the GL facade:

```text
Linux Vulkan -> Mesa Venus -> virglrenderer capset 4
    -> directly linked Mesa KosmicKrisp -> Metal
```

Its fences ride the same deferred fence pipeline as the default path. A fence the
renderer does not retire never costs the guest its GPU device: the first expiry
releases the ordering that held back guest commands and grants a second window,
and only a second expiry retires the fence without its renderer work, so a
wedged submission fails the workload that owns it while the guest can finish and
power off.

Venus keeps shareable Zink scanouts in their renderer-optimal Metal layout. For
a guest whose buffer allocator is not the Vulkan driver, the renderer gives an
imported GL resource a host shared-memory segment of its linear layout and hands
out the descriptor; a row is padded to a multiple of 16 bytes, the guest
allocator pads by the same rule, and ordering between the consumers is the
guest's own fences.

The in-process GPU service keeps the display apart from the guest's GPU
connection. The display (the VNC listener, the native window or Kernarium view)
lasts until the service stops; the connection (the Virtio-GPU device, the
renderer, and every context and resource the guest created) lasts as long as one
guest. A guest that restarts itself is a new guest on the same machine, so
`startNextConnection()` ends the connection, resets the display to its idle
picture and opens a fresh one, while clients stay connected. The request gate
belongs to the service, not the connection. Nothing enforces that a new guest's
host-visible blob starts zeroed.

### Local stream sockets

On Apple hosts, never wait for the end of a local (`AF_UNIX`) stream socket
with a blocking `read` or `recv`. Wait for readiness first (`poll`, kqueue or a
Dispatch source), then read, or read non-blocking after readiness. An end made
by `shutdown()` can lose the wakeup of a reader that is just going to sleep:
the peer's `SHUT_WR` or `SHUT_RDWR`, and a `shutdown` of the reader's own end
used to wake it. The reader then stays blocked although `poll` reports the end,
and with `SO_RCVTIMEO` the loss shows as a false `EAGAIN`. A `close()` by the
peer is not affected, and a data wakeup is never lost. Python's blocking
`socket.recv` is exposed in the same way; after `settimeout()` CPython polls
first, which is safe.

## Guest lifecycle

The platform layer tracks every writable host descriptor opened for the guest.
`uml_emulator_sync()` has the guest write back through the management console's
`sync` command, then flushes those descriptors. The command runs `sync(2)` in
the guest's process context and replies only after it returns, whatever the
magic SysRq mask is. `uml_emulator_quiesce()` has the guest write back first,
so that what it dirtied becomes reclaimable, then trim its cold page cache and
hand its free memory to the host, enters full Linux system suspend and waits
for the platform sleep boundary before flushing the host.
`uml_emulator_quiesce_resident()` does the same and keeps the guest's RAM
in the process where a quiesce would [hibernate](#hibernation) it, for a host
short of the space an image takes. `uml_emulator_resume()` sends the console
wake edge, and
`uml_emulator_wake()` sends it to a guest that is not quiesced, as a phone's
power key does. Every console request ends the suspend, and a wake also ends
a suspend still queued behind another request; the
[shared kernel README](../kernel/common/README.md) gives the kernel's side.

The suspend request has one bounded deadline; a refusal, or a guest that stays
busy, is not frozen and the host treats it as not suspended. A request with no
reply at the deadline is aborted: only a "suspended" reply that arrived before
the wake edge counts, otherwise the suspension is unconfirmed, never
hibernated, and woken at the resume. The management console's own stop is no
fallback, because it halts only one vCPU. If the physical iOS sandbox cannot
bind the console, host-file flushing still runs but guest write-back and freeze
are unavailable.

Each of the three calls reports what the guest confirmed through a nullable
`uml_emulator_settle`, and whether it sleeps. The guest's storage is confirmed
only by an explicit statement of its own in that call: its reply to a `sync`
request, or a suspend reply that states the suspend's own sync returned
(`suspended, synced`). A suspend reply without the statement, as every older
kernel's, confirms nothing: the kernel syncs on the way to suspend only while
the guest's `sync_on_suspend` is set. Otherwise there
was no guest, or the storage is unconfirmed (no reply in time, a busy lifecycle
call or console, or a kernel that only started a writeback), refused, or
unreachable (its console could not be bound). Only confirmed and no-guest are
settled: an application never logs "settled" for another outcome (`GuestSettle`
in `GuestHostCore`, filled from the runtime's `kernarium_runtime_settle`), and
the session record counts them. The zero value of the storage report claims
nothing, so a report the call never wrote is no confirmation.

A reply confirms only as "synced in <n> ms" whole, in one well-formed datagram
per part; text that starts alike, a truncated datagram and an error flagged in
any part of the reply are refusals. A kernel without the `sync` command answers
exactly "Unknown command", and the host then asks for `sysrq s`, which only
starts a writeback and needs SysRq enabled (the Kernarium runtime boots its
guests with it for that case): the result is unconfirmed. One deadline, from the
call, covers a writeback request in full: the console's lock, every part of the
reply, the fallback and its settling period. The header
([`darwin/api/include/uml/emulator.h`](../darwin/api/include/uml/emulator.h))
states each call's bounds. A quiesced guest the host holds is reported again by a later call; a
guest whose threads could not be held, or whose suspension is unconfirmed, may
have run and written since, so a later call asks it again. The guest runs each
`sync` on a work item of its own, so the console's dispatcher is never blocked
behind one, and a reply that comes after the host stopped waiting finds the
request's own socket closed. The runtime's `sync` pins the handle instead of
holding the lifecycle mutex, which a quiesce keeps for as long as it takes, so
the library's own bounded wait is the only one.

The kernel says how the guest ends before it shuts its drivers down, with the
bridge import `darwin_uml_guest_shutdown()`: power-off, halt, or restart with the
command the guest passed. The runtime keeps the kind and a bounded copy of the
command in the run's `Context`. A panic reports nothing and aborts the process
after the runtime moves the kernel frames on every thread's stack and registers
from the kernel's alias to the same instructions in the framework's
`__TEXT,__uml_elf` section (`runtime/kernel_crash_frames`), where the runtime's
dSYM, which alone names the kernel's functions, symbolizes them in a crash
report. The panicking thread takes the panic's message as its name, which the
report shows.

A launch that passes `--session-record=PATH` gets a record of how its session
ended (`runtime/session_record`): "running" from the boot on, overwritten by
every end that runs code, a panic with its message and the kernel's last
output included. A record still "running" at the next launch means the process
was ended from outside without running, as a memory-limit end is. For an
Android guest the state also says whether the boot finished (`boot completed`;
`boot unfinished` once init's output was seen without it; `boot unobserved`
when the record saw none of init's output) and how many times the guest
restarted before the session (`restarts N`); until the boot completes the
checkpoint thread rewrites the state with the console's last output whenever
that grew, so a process ended mid-boot leaves where the boot had got, and a
stop, restart or power-off before the boot completed quotes that output too.
The output a record quotes is its own boot's, never an earlier boot's of the
same process. It is quoted without what identifies the installation: the
values of the launch's `androidboot.serialno`, `androidboot.kernarium.adb_key`
and `androidboot.kernarium.timezone` arguments wherever they stand (also a
write that ends inside one), the assignments of those names and any hardware
address in its colon spelling are replaced by `<masked>`, `<time zone>` and
`<address>`, so no reader of the file has them. An end is the last state on disk unless the process dies between
a delayed checkpoint's write and its repair: the end formats its record before
it writes it, and a checkpoint whose write lands after it writes that record
again at once. The file is text of a fixed layout (`uml-session 8`) of 19200
bytes (`session_record_max_bytes`) from a session's begin: an 8 KiB memory slot,
a 4 KiB stall slot, a 512-byte network slot, a 256-byte storage slot, a 1 KiB
camera slot and the state with the kernel output it quotes, 5 KiB. Each part is
rewritten in place by its own writer, so nothing grows with the session, and
each state is one write of its whole area, padded with newlines that the
readers ignore, so a reader never sees two states mixed; layouts 1 to 7, which
lack the camera slot or the storage slot or the figures of the run or the
network slot or the stall slot, still parse.
The storage slot counts the session's settles and those that were not
confirmed, and names the last one's call, outcome and time: words and numbers
only, blank until the first. The camera slot says how the platform presented
each of the guest's cameras (its mounting, the angles of its connections, the
photo output's orientation compensation and when it was read, its aspect ratio:
the camera device reports each change through `onCameraPresentation`), what the
platform found of the front devices (how many, whether any has a square format
or lists ratios, how many formats it did not offer) in a line of its own that
stays whichever camera was used, counts the stills of the
guest's cameras that did not end in a picture and names why for the newest two
that fit beside the front camera's lines (the camera, the stage it stopped at,
the sizes asked for, resolved and encoded, the photo's Exif orientation, and the
framework's error as a domain word and a code; the camera device reports each
one through `onStillFailure`), and drops other cameras' lines, then older
failures, when it is full. Andarium's bug report spells the slot out in words,
in an entry of its own. The
first 8 KiB are a memory checkpoint that a thread of its own rewrites every 2 s
(`memory_checkpoint_interval_ms`): the footprint, its peak and what the
process's memory limit still allows, the large terms of the footprint (guest
RAM, the host GPU's use against its budget, graphics, the gadget cache, the
domain heaps and malloc) now and at the peak, and a bounded history of them.
Each history row also says how the run went in the time it covers: the
scanout flushes (the device counts each) and the longest gap between two, the
host's thermal state and memory pressure as the stall watch observed them, the
gadget cache's reliefs, shed accounts, renewals and largest open account, the
CPU time of the process and of the vCPU carriers, the performance-core share
and energy from the task's power information (`TASK_POWER_INFO_V2`) and, on
macOS only, the cycles from `proc_pid_rusage`; a row of the older history
sums the checkpoints it stands for, and the checkpoint reads them all without a
lock, a thread or a timer of its own (`RunFigures`).
Reading malloc's term stalls every thread that allocates, so it is read only at
the first checkpoint and when the footprint climbs 64 MiB past the highest
reading's (`memory_malloc_reading_due`); every checkpoint also writes the
footprint less the other terms. Andarium names the file in its boot request and
reads it at launch.

The stall slot records why the vCPUs stopped running. In every build a thread
of the process, `uml-stall-watch`, looks every 50 ms while a carrier is outside
its idle wait (`runtime/stall_watch`) at each vCPU carrier's run state and CPU
time, and the loader's wrappers mark the
host waits a carrier sits in (`platform/host_wait.hpp`). A carrier that for
100 ms or more is runnable but gets under a fifth of the time, sits in an
uninterruptible wait, or sits in one host wait while it is not using the CPU is
stalled; a carrier that is busy is never held to a wait it was merely sampled
in. A late tick of the watch itself, measured against its own deadline and not
against the work it did, is a stall of the whole process. Stalls form events
that carry the host's thermal, power, memory and main-queue state
(`runtime/stall_record`). The slot is bounded: the newest events that fit in
4 KiB, at most 32, the last 8 changes of each host state, and counts of the idle
and unmarked waits the watch saw, which show whether the markers reach the
wrappers. Times are milliseconds after a base wall time on the system's
continuous clock, which runs through a sleep.

The watch looks only while a carrier is outside its idle wait, and sleeps with
no wake-up of its own while all are inside one: a guest at rest, with the screen
on or off, suspended or quiesced, costs it none. Each carrier reports its idle
waits (`runtime/carrier_activity`, through the loader's wrappers; the kernel's
other threads report nothing). The watch wakes when a carrier has stayed outside
its wait for a short fixed time, when one that was due to wake from a timed wait
has not run that long after its deadline, and when the host wakes the guest (a
wake, a resume, or a console request that ends a suspend); a carrier that
returns within that time never wakes it. The deadlines run on the absolute
clock, which the carriers' waits run on, and the record's times on the
continuous one. The watch sleeps again after two looks with every carrier idle
and nothing suspected, counted afresh after each sleep; its first look after a
sleep starts the measurement over, so the sleep is nobody's starvation. A stall
that starts as a carrier leaves its idle wait is recorded about 150 ms later.
While some carrier is outside its idle wait, or does not report, the watch looks
every 50 ms.

What the sleeping watch cannot see. A carrier woken before the limit of its
wait by a guest wake source (another vCPU's IPI, or the kernel's notification
thread, which alone wakes CPU0 for a host descriptor), while every other
carrier is idle, is seen from its first instruction on, not while the system
keeps it from running. The notification thread's fallback, which wakes an idle
secondary when CPU0 is late for a device, arms the watch, so a device wake that
stalls is still recorded, later and shorter; the case stays open for a guest of
one vCPU, a suspended guest, a carrier woken by an IPI from one that goes idle
again within that time, and a stall shorter than the fallback needs. A carrier
that stays blocked past its limit is looked at once and then left alone, and one
that is due, starved, and runs before the watch has collected its wake-up is not
seen.

The watch reads no guest memory, takes no lock a carrier takes, suspends
nothing and calls no framework from its own thread, which the kernel-thread
fence can hold; it waits in a system call of `libsystem_kernel` alone. The
host's state is observed on a serial queue of system threads: the system's
notices of a change of the thermal state or Low Power Mode and the
memory-pressure source publish it and wake the watch to record the change, and
the main queue is asked, and the thermal source read for the core count, every
250 ms only while a stall is open or being confirmed. A hold of the kernel's
threads is not recorded as a stall. Andarium adds the previous and current
records, the stalls in words and the guest's `/proc` counters to its bug
report.

The network slot holds the guest's user network in numbers: DNS queries
answered, refused by the system (policy denied), timed out or failed; DHCP
discovers, requests and acknowledgements; TCP connection attempts and how many
were established, unreachable (`connect()` failed at once), reset (refused,
timed out on the host after 75 s, or failed after it started) or unanswered
(the rest), not counting DNS over TCP to the name server or the gateway (the
provider counts them, [`darwin/README.md`](../darwin/README.md)). They are the
share since the session began, rewritten at each checkpoint and at the end, and
hold no address, port or name. The slot also holds `denials`, how many denials
of the network by iOS began during the session (the application's notice goes up
for each; runtime API 36, `set_network_notice`), and the one in force.

A guest that restarts itself boots again in the same process, because iOS can
neither re-exec nor respawn an application. The macOS launchers
(`darwin/app/macos`) make the machine once (the emulator handle, the SLiRP
provider, the GPU service with its display) and make the services and command
line of each boot afresh; after a restart they reset the SLiRP link, ask the GPU
service for its next connection and run the handle again, and the exit status
is the last guest's. A guest that does not boot is not restarted for ever
(`emulator/src/restart_loop_guard.hpp`): five restarts in a row within 15 s of
their boot end the loop, and so do eight in a row that came within 15 s or, for
an Android guest whose init output the console showed (`init_observed`), before
the boot had finished (`boot_completed`), however long it ran. The higher limit
leaves Android's own boot-loop rescue its turn. A restart after a finished boot
that lasted 15 s or more never counts, so a reboot suite is not limited; a
Linux guest, or an Android console that shows nothing of init, keeps the 15 s
rule alone. A stop request ends the loop at once. Android's boot control block
is part of the loop: every restart reads the misc partition again (see the
[Android boot contract](../android/lineage-24/README.md)). The Kernarium runtime
runs the same loop inside its one `run` call
(`Kernarium/Runtime/KernariumRestartLoop.hpp`, `kernarium-restart-loop`): a stop
that arrives as the guest restarts wins, the next boot waits while the guest is
paused or from the moment the application asks for its GPU requests to be held
(`set_gpu_hold`, before the hold's camera wait, whether or not the gate has
closed yet) until it gives them back (and again once the boot is built, so a hold
that arrives meanwhile also keeps the guest from running), and a restart is
reported twice so the application can re-apply what belongs to a kernel. After
Android powers off cleanly the runtime releases the guest through
`uml_emulator_release_guest` and returns success only once another run can
start. An application that can be backgrounded holds the guest's GPU requests (`set_gpu_hold`) before it quiesces
and releases them before it resumes. On an Android guest the hold is preceded by
the camera's end of the guest's running video: the camera device ends it as the
platform's notice of the departure would and the hold waits (two seconds at most,
inside the edge's budget, never added to it) until the guest has stopped its
video streams, so a recording that runs ends before the guest's GPU requests
stop (`uml::camera::BackgroundVideo`, the camera device's README). A caller
that has a deadline passes what it leaves (`set_gpu_hold` of 3 or more, the
milliseconds): the camera wait then takes what is left after the hold's own wait
of one second for a request that is running, and nothing when less is left. The
deadline bounds the camera wait and not that second, which the drain always has.
The hold of a caller with none (Andarium, at the edge) is held three
seconds after the edge at the latest, the wait and then the drain; the main path
of a kept-running job, which waits for the screen to go off first, gets what its
four-second edge leaves. The bound that holds that job's GPU 1.5 s after the edge
takes the plain hold without the camera's wait (`set_gpu_hold` with 2), so the gate
is closed at 1.5 s after the edge and drained 2.5 s after it at the latest; a bound
that fires while the other caller waits for the camera holds at once and ends that
wait, and two holds that meet wait for a request that is running once.

The kernel's whole-file disk locks pass through the host's guest-file registry
(`platform/guest_files`). On iOS, iPadOS, the Simulator and in Designed for
iPad the registry keeps them itself and locks no host file, because iOS
terminates a process that holds a file lock when it suspends it, and a guest
that refuses or outlasts its suspend is still running then; macOS locks the
files. Once the guest confirms system suspension and its host files are
flushed, quiescing holds every thread that can run the guest and releases the
disk locks. The hold keeps the unlocked disks unwritten; a hold that cannot be
taken leaves the locks in place. Resume and stop reacquire every lock before
restoring guest RAM, releasing a thread or waking the guest, and verify the
disk's identity, size, links and modification time, and on macOS its change
time (an iOS-family application's own metadata writes move it); contention, a
replaced descriptor or changed storage ends the process without resuming the
guest.

The guest binds the management console as a Unix datagram socket under a
per-run `uml-kern<pid>` directory (mode 0700) that the host passes as `uml_dir=`.
The run removes it when it returns and on every terminating signal it observes;
the next run removes the directories of runs that have exited. Every such path
must fit Darwin's `sun_path` and is refused, not truncated, when it does not
(`guest-host-residue`).

### Statistics

An application that shows a running guest's load reads it from the host, and
nothing runs between reads: there is no sampler, timer or counter of its own.
`uml_emulator_get_statistics()` sums the user and system time of the process's
`uml-vcpu-N-carrier` threads and reports the guest RAM the host holds.
`uml_emulator_get_guest_memory()` asks the kernel through the management
console's `mem` command; like every console request it ends a suspend, so it is
sent only under the handle's lifecycle mutex (taken with `try_lock`) and only
while the guest is not quiesced. The macOS launchers print the same figures with
`--stats=<ms>`. The runtime's reading also carries the display's count of
refused graphics allocations and the device memory budget with its usage, and
a separate reading lists the budget's holders by guest process (the device
memory budget in [`graphics.md`](graphics.md)).

### Hibernation

On a physical iOS device, where guest RAM is wired and so neither compressible
nor reclaimable, a quiesce that entered system suspension also hibernates the
guest's RAM (`runtime/guest_hibernation.*`), so that the paused application is
small enough for the system to keep. `--guest-hibernation=auto|off|on` selects
it; `on` forces it on any host, which is how tests reach the same code. The
order is the contract:

- **Hold.** Every thread that can touch guest RAM is Mach-suspended first: the
  host's `uml-*` services and every thread the kernel creates
  (`runtime/kernel_threads`). A thread is held only where it holds none of the
  allocator's, C library's or runtime's locks; otherwise it is let go and tried
  again. If every thread is not held within a bounded time, nothing is held or
  written and the guest stays wired and resident.
- **Write.** The held pages go to an unlinked image in the application's
  `$TMPDIR` in hashed chunks, each taken off the footprint once written. The
  file holds no lock, because iOS terminates an application that holds a file
  lock during suspension.
- **Restore.** `uml_emulator_resume()` restores and verifies every chunk,
  releases the threads, and only then wakes the guest; `uml_emulator_request_stop()`
  restores first, since a held thread cannot see a stop.
- **Failure.** A restore that cannot bring every page back verified wakes
  nothing and ends the process with a diagnostic, so the next launch boots the
  guest again and Kernarium reports the restart notice.

A quiesce whose guest did not confirm its suspend, a guest without a console,
a resident quiesce, and a guest kept running (Picture in Picture, the job) never
hibernate. The first hold stands: a hibernated guest is not made resident by a
later quiesce, nor a resident one hibernated. A confirmed system suspension holds the same threads on every host whether or not
the RAM is written, because the disk locks leave with it (see
[guest lifecycle](#guest-lifecycle)).

## Tests and structural audit

Test suites and workflows are in [`testing.md`](testing.md).
`architecture-audit` checks the CMake File API graph, dependency cycles,
implementation includes, source ownership, and test registration. Only a
darwin/ source's owner compiles it: no other CMake file and no Xcode project
names one, and Xcode, not CMake, compiles the sources of Andarium/ and
GuestHost/. No host source under darwin/, Kernarium/, Andarium/ or GuestHost/
names x18 outside a comment, because Darwin clears that platform register when it
interrupts a thread (`arm64-emitted-registers` and
`x86-64-jit-emitted-registers` hold emitted code to the same rule). The audit
does not prove performance, generated code shape, a kernel boot, or guest
behavior.

The host/kernel ABI source is [`../kernel/common/abi`](../kernel/common/abi/).
Version, fingerprint, generated-copy, layout, and import checks fail closed on
drift.
