# Darwin UML host runtime

The arm64 Apple host that boots User-Mode Linux kernels: platform
capabilities, guest address spaces, the CPU engines, the devices, and the
products that compose them. [`../docs/architecture.md`](../docs/architecture.md)
owns the dependency direction and the runtime contracts that cross modules.

## Ownership

```text
api/       installed C API and ABI identity
include/   host copies of the host/Linux bridge and host-only guest contracts
platform/  Apple capabilities, VM, files, signals, sockets, and the hardware
           video encoder and decoder
memory/    guest address spaces, software MMU, and TLBs
cpu/       gadgets, JIT, executable memory, shared decode, and neutral
           engine selection
runtime/   guest-entry composition
device/    block, console, GPU, input, audio, video, camera, vsock, GNSS, and
           network providers, and the vhost-user transport they share
emulator/  options and product composition
app/macos/ standalone process adapter
app/initramfs/ host tool that writes a guest's per-run initramfs
```

Targets and source lists belong to these directories; `CMakeLists.txt` here
only composes them. Subsystem contracts are in the README beside the code:
[platform](platform/README.md), the CPU engines
([arm64 gadgets](cpu/arm64/gadgets/README.md),
[arm64 JIT](cpu/arm64/jit/README.md),
[AArch32](cpu/arm64/aarch32/README.md),
[x86_64 gadgets](cpu/x86_64/gadgets/README.md),
[x86_64 JIT](cpu/x86_64/jit/README.md)), and the devices
([audio](device/audio/README.md), [block](device/block/README.md),
[video](device/video/README.md), [camera](device/camera/README.md),
[vsock](device/vsock/README.md), [GNSS](device/gnss/README.md)).

## Products and defaults

The standalone macOS application supports the arm64-userspace Linux kernel and
the separate x86_64-userspace kernel, both with 4 KiB guest pages, and the
Android composition. `cmake --preset macos-x86_64` selects the x86_64 artifact
and host gadget backend; the other presets select arm64 userspace. The launcher
defaults for `mem=` and `ncpus=` are in `app/macos`; `ncpus=` follows the
[applications' vCPU limit](../docs/architecture.md#memory-and-executable-code),
and an explicit kernel argument overrides either. The x86_64 product defaults to
`cgroup.memory=nokmem`: kernel-object charging is outside that compatibility
slice, and an explicit `cgroup.memory=` overrides it.

Each macOS executable embeds an Info.plist (`app/macos/Info.plist.in`) that
names its bundle identifier, by which Metal places the process's shader cache
([Metal shader cache](../docs/graphics.md#metal-shader-cache)). Privacy
permissions are requested under the code-signing identifier `uml-darwin` and
attributed to the terminal or application that launched the emulator. Signing
the executable again with `codesign` binds the property list and makes the
bundle identifier its code-signing identifier.

The emulator library chooses no guest init and configures no guest network; the
image owns both. Each Linux product embeds the boot base archive for its
userspace architecture, and unless the command line names `init=` or `initrd=`
the launcher boots it with a per-run initramfs (an explicit `init=/bin/sh`
starts a raw shell on the image). The
[boot ramdisk contract](../tools/rootfs/boot-ramdisk/README.md) owns the
archive, its parameters and the host-side `uml-initramfs` tool.

The Android macOS composition is a separate product: it consumes a compatible
Android kernel artifact, accepts a rootfs directory and one directly writable
GPT image without comparing rootfs release identity, and never searches for the
Linux desktop image. Kernarium's runtime frameworks and defaults are in
[`../Kernarium/README.md`](../Kernarium/README.md).

## Runtime, memory, and kernel loading

Handle and run ownership, the vCPU carriers and host signal ownership, the
native kernel window, guest RAM commitment, accepted-artifact sharing, and the
signed kernel loader are owned by the architecture: see
[runtime ownership](../docs/architecture.md#runtime-ownership),
[memory and executable code](../docs/architecture.md#memory-and-executable-code),
and [CPU execution and scheduling](../docs/architecture.md#cpu-execution-and-scheduling).

## CPU engines

Engine selection, executable-memory ownership, the JIT's semantic slow path
and the x86_64 userspace profile are in the
[architecture](../docs/architecture.md#cpu-execution-and-scheduling); each
engine's lowering is in the README beside it. The cache contracts the engines
share:

- **Gadget translation cache.** A cache slot indexes a translation held in its
  address space's arena, tagged by the id the cache gives each resident address
  space, so retiring an address space drops its state as a unit. Slot geometry
  is one build-time setting for both ISAs
  (`cpu/gadget_cache_geometry.hpp`). A key with no compact tag form (an address
  at or above the guest address limit) is built unlinked, run once and never
  stored, so it leaves no link record or indirect site in its space.
- **Shared bodies.** Executors running one address space share its bodies, link
  records and indirect sites unless `DARWIN_UML_GADGET_SHARED_BODIES=0`; a code
  change one vCPU publishes reaches the others at their next block transition.
  The machinery is `cpu/gadget_shared_space.hpp`, `gadget_body_arena.hpp` and
  `gadget_generation_window.hpp`.
- **Bounded growth.** A shared space is renewed once its arena is mostly freed,
  a private cache sweeps link state nothing names, an address space the kernel
  destroyed is let go of at the executor's next entry, and a process-wide byte
  budget (`cpu/gadget_cache_budget.hpp`) caps the open address spaces. The
  storage of one it asked to shed goes with the last holder to let go, so the
  cache is above the budget by the shed spaces in flight. An arm64 holder
  running such a space ends its run at its next return from translated code and
  lets go of it by emptying its slots, which tell the space nothing; an x86-64
  holder, and an idle one of either, lets go when it next runs. An executor
  keeps eight address spaces and admits a ninth by retiring the least recently
  current one, unless another executor holds it and some executor entered it in
  the last 175 ms: retiring it would free the bodies the other is running. The
  oldest destroyed or idle one (nobody entered it in those 175 ms) goes instead,
  and when none is, the one the executor names the fewest slots of. An address
  space only this executor holds is never spared. Past the budget, an address
  space that alone holds more than fifteen sixteenths of the budget (the line),
  or more than seven eighths of it (the target) while no executor enters it, is
  asked to shed, and then, only while the cache is above the target, the others:
  those no executor entered for the last 175 ms first, then the less active by
  band and the smaller first, so the address space the guest runs most goes
  last. A running one between the target and the line is spared while others
  are left, and they stop at the line, which leaves the open accounts a
  sixteenth of the budget of headroom for the growth of any of them until the
  next relief; past the line it is asked whole, and its program stalls while
  its threads translate it again. The emulator sets the budget before the
  first executor, from guest memory and the process memory limit or
  `--gadget-cache-budget=`; it is also the gadget-cache term of the host GPU's
  budget
  ([architecture](../docs/architecture.md#memory-and-executable-code)).
- **Code writes.** In both arm64 engines, code another vCPU or another process
  changed and synchronized takes effect at this vCPU's next context
  synchronization point, including through a shared frame under another
  address space: a per-frame holder word on the mapped backing names the
  address spaces that fetched the frame, and the synchronization (or a
  `ptrace` or `process_vm_writev()` write, which names the frame it wrote)
  publishes to them
  ([architecture](../docs/architecture.md#translation-caches-and-guest-debugging)).
  A kernel write into a guest-code page by `uaccess` reaches arm64 code only
  through the guest's own `IC IVAU` or `cacheflush()`, as on hardware that
  advertises neither DIC nor IDC; x86 code sees such a write into a page-cache
  frame, which the x86_64 kernel names to the host
  (`flush_dcache_folio()`). `DARWIN_UML_CODE_HOLDER_COUNTERS=<path>`
  writes the mechanism's counters once a second.
- **Forwarding and diagnostics.** Scalar and vector result forwarding are
  template-owned and can be disabled per program through `BuildOptions`.
  `UML_GADGET_EXECUTION_HISTOGRAM` is a default-off diagnostic build whose
  streams measure instruction mix, never speed.
- **JIT.** Private and shared translation caches derive from one process
  budget (`cpu/arm64/jit/jit_cache_geometry.hpp`). Links live in
  executor-private writable data outside executable mappings and are scoped to
  an address space. The semantic slow path performs each atomic, exclusive and
  ordered access as one host access, so it is single-copy atomic against native
  code on other vCPUs and host threads.

## Devices

The vhost-user transport and the Apple graphics stack are in the
[architecture](../docs/architecture.md#devices-and-graphics) and
[`../docs/graphics.md`](../docs/graphics.md). The Venus route's host side fails
closed on an invalid memory type, range, ownership, acknowledgement, or
reconnect state. The devices Kernarium attaches and its display geometry are
listed in [`../Kernarium/README.md`](../Kernarium/README.md#devices-and-display).

### User network

The SLiRP provider owns the user network. libslirp comes from the maintained
series in [`../dependencies/README.md`](../dependencies/README.md), whose
patches provide the behaviors the `slirp-network-*` tests check.

- Host forwards belong to the provider and survive provider reopen; an
  unavailable host port is reported without removing the guest's network
  interface. Filesystem socket paths are removed only by their owning guards or
  the common termination cleanup.
- A forward queues up to the host's `SOMAXCONN` pending connections; the
  advertised receive window never exceeds the free buffer; a connection whose
  guest end stops answering is closed; a host connection that dies in both
  directions is reset toward the guest, also while the guest is not reading
  what SLiRP holds for it, and never aborts; a connect the host stack refuses
  is answered with ICMP net-unreachable (port 0 excepted); ICMP echo is
  forwarded as a router would.
- The guest's hardware address is the provider's default until the launcher sets
  one (`--net-mac`, `uml_slirp_net_set_mac`), and the provider's ARP, DHCP and
  DNS answers follow the guest's own address (`slirp-network-guest-mac`).
- The provider answers the guest's name server itself, through the host's system
  resolver, and SLiRP's own DNS proxy is off: the proxy would send queries from
  the application to a local-network router, which needs the Local Network
  permission, and the guest follows the device's own resolver configuration.
  The wire rules are `network-dns-rules`. While the host has no network path
  (the platform's path monitor, which the provider makes at its first open,
  keeps for its life and makes again at an open if the system gave it none), a
  name the system cannot find is answered SERVFAIL, which a guest retries, and
  counted as failed; answers that carry a record of the asked type stand, and
  the next report of a path ends it (`slirp-network-dns`,
  `platform-host-network-path`).
- When the path monitor reports that an interface carries the path and the
  host no longer has the address a guest TCP connection's host socket is bound
  to (Wi-Fi gone, the path on cellular), the guest gets a reset on exactly
  those connections once an interface has carried the path for 8 s since the
  host's addresses last changed (a report with a changed address set starts
  the 8 s over) and the address was missing at both ends of that time;
  otherwise a connection the guest writes on hangs until the host stack gives
  up, and one only the peer writes on never ends. An outage no interface
  carries resets nothing and starts the 8 s over once the path is back; an
  address first found missing when they end gets 8 s of its own. An address
  that returns, host forwards, UDP, DNS and ICMP are left alone, and the hold
  of a pending report is the only timer (`slirp-network-address-reset`,
  `network-address-reset-rules`).
- The provider counts what became of the guest's DNS queries (answered, policy
  denied, timed out, failed), its DHCP exchange and its TCP connection attempts,
  as numbers only, and the runtime writes them into the session record
  (`network-counters`, `slirp-network-counters`). An attempt is a SYN that
  SLiRP connects for; DNS over TCP to the name server or the gateway, which the
  guest forward answers itself, is not one. Each attempt is classed by what the
  guest was told: established (a SYN-ACK); unreachable (an ICMP net or host
  unreachable, which SLiRP sends when `connect()` fails at once with anything
  but `ECONNREFUSED`); reset (a reset: `ECONNREFUSED` at once, a connect that
  failed after it had started, whose errno SLiRP has dropped, or SLiRP's own
  75 s timer on a connect nothing answered); unanswered (the rest: still under
  way, given up by the guest, or pushed out of the 64-entry table of waiting
  attempts). A connect the system denies lands in unreachable or reset, by how
  `connect()` fails.
- Guest IPv4 multicast is dropped on the virtual link. The Local Network
  permission is asked for only when the guest itself reaches the local network.
- The guest cannot reach the machine the application runs on. SLiRP does not
  map the gateway or the broadcast address to the host's loopback
  (`disable_host_loopback`), and the provider refuses a guest's TCP connect
  attempts and UDP datagrams to any address a host interface holds, its
  broadcast address and `127.0.0.1` included: a TCP reset or an ICMP port
  unreachable at once, whether or not anything listens, and silence for the
  other protocols. The provider checks the host's current addresses when the
  guest starts a flow: it lists them when a connect attempt or a datagram to
  an address that is not one of SLiRP's own services finds the last listing a
  second old, never otherwise, so a quiet guest costs no wake-up and no system
  call, and the network does not open without a first listing. A listing that
  fails is tried again by the next such frame. Once the last good listing is
  four seconds old, every address counts as the host's until a listing
  succeeds (the limited broadcast address, which carries DHCP, stays outside
  it): each connect attempt, UDP datagram and ICMP packet that the table
  judges is refused, those of flows already open included, and only the
  segments of TCP connections already open pass. The virtual network is no
  exception: SLiRP connects a flow to every address of it but the gateway as
  given, so an address of it that a host interface holds is the host's and is
  refused like any other. Only what SLiRP and the provider serve at their own
  addresses is theirs even then: ICMP to the gateway or the name server, which
  the virtual network answers or drops itself and never sends to a host
  socket, DNS at either, and DHCP at the gateway. The rule is about the host's
  own addresses, not about what the host can route to: a destination the host
  reaches over its network is reached as before. An address the host answers
  that no interface holds, such as a service a VPN client runs inside its
  tunnel, is not covered. What the guest reaches through the gateway is SLiRP's
  own (DHCP, ICMP echo, the name server's DNS); other machines on the local
  network are reached as before. A host forward is the one listener the guest
  shares with the host, and it leads into the guest, also when the host's own
  client comes through one of the host's addresses (TCP; a UDP forward used that
  way from a non-loopback address of the host stays unreachable). A test that
  needs a host listener names a guest forward to a Unix socket, which only the
  provider's test build offers (`UML_TEST_SLIRP_GUEST_FORWARD`, read by
  `uml-darwin-network-test`, which no preset installs or packages); no product
  links that build (`network-host-isolation-rules`,
  `slirp-network-host-isolation`, `guest-slirp-host-isolation`).
- The guest network driver has no transmit flow control, so a guest transmit
  into a full ring waits briefly for the SLiRP loop (`guest-slirp-network-transfer`).

## Guest storage and lifecycle

Sync, quiesce, resume, and the management console are in the
[architecture](../docs/architecture.md#guest-lifecycle). Disk capacity is an
application concern: the runtime validates that a GPT matches its file and
boots it without rewriting either, and the
[Android static disk contract](../android/lineage-24/README.md#static-disk-and-boot-contract)
owns how an image and its filesystem grow. When Kernarium syncs, quiesces, and
resumes is in [`../Kernarium/README.md`](../Kernarium/README.md#lifecycle).

## API and validation

The installed API is under `api/include/uml/`; its ABI identity is independent
of the host/kernel bridge under `../kernel/common/abi`. Follow the ABI README
for declaration changes.

Use the workflows in [`../docs/testing.md`](../docs/testing.md). The
architecture audit verifies source ownership, dependency direction,
executable-memory ownership, test registration, and platform isolation. Guest,
package, graphics, and performance behavior require their separate workflows.
