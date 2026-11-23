# x86-64 gadget engine

The engine runs x86-64 userspace, long64 and compat32, as data-only streams of
pointers to handwritten arm64 gadgets. It is the x86-64 product's default
engine and the only one on every other product that runs x86 code. This file
records the contracts that are not obvious from the code; the design credit for
both gadget engines is [`NOTICE.md`](../../arm64/gadgets/NOTICE.md).

## Ownership

| Path | Role |
| --- | --- |
| `engine.S`, `engine/*.inc` | The gadgets. `engine/region.inc` places every group. |
| `engine/generate_baked.py` | Writes the register-baked scalar bank at build time: `baked_scalar.inc`, and `baked_tail.inc` for the families behind the last group. |
| `engine/fit_slots.py` | Reports the groups that no longer fit their slot. |
| `native_builder.cpp`, `native_baked.hpp`, `native_entry.hpp` | Turn decoded instructions into streams. |
| `executor.cpp`, `program.cpp`, `translation_body.hpp`, `translation_space.*` | The translation cache, its sharing and its budget. |
| `native_memory.cpp` | The C++ side of the software TLB. |
| `exception_checks.*` | Checked execution of the exceptions a guest enables. |
| `x87_math.*` | Bounded software primitives for x87 formats. |

The target is `uml_x86_64_gadgets`. The guest CPU state, decoder, CPUID,
software TLB entry, code-page registry, split-operand lock, segment model and
SIMD floating-point model are not the engine's own: they are
`uml_x86_64_target` in the parent directory, which every x86 engine links.
Gadget streams are data. Executable memory has one owner,
`darwin/cpu/jit_memory/executable_memory.cpp`, and this engine never allocates
or publishes code
([architecture](../../../../docs/architecture.md#cpu-execution-and-scheduling)).

## Engine selection

The gadget engine is selected by name and is the default. On macOS
`--execution-engine jit` sends long64 and compat32 tasks to the
[x86-64 JIT](../jit/README.md) instead; the runtime chooses per dispatch, so
neither engine depends on the other.

## Region layout

The engine's code is a pinned region, a bank and a cold part, each a run of
fixed slots from the table in `engine/region.inc`
([architecture](../../../../docs/architecture.md#cpu-execution-and-scheduling)
has the contract). Private labels take the Mach-O assembler-temporary prefix
`L`; a named local symbol ahead of a target fixes an addend the assembler
rejects.

- A group (the common entry code, the veneer island, one baked family or one
  `engine/*.inc` file) that changes size within its slot moves no other group.
- A group that outgrows its slot fails assembly at `.org Lx86_bank_end_<group>`
  or `.org Lx86_cold_end_<group>`, and every later group fails too.
  `engine/fit_slots.py --build-dir <tree>` lists each group that no longer fits
  with the slot it needs: its phase and new size plus 1 KiB, rounded up to a
  128-byte line. Setting those rows' slots moves later groups by whole lines
  and keeps their phase, so the commit says which slots grew and needs no pad
  control ([optimization history](../../../../docs/optimization-history.md)).
- A phase never changes. A new group takes a new row with phase 0, in the
  order `engine.S` emits it: a family the generator writes to
  `baked_scalar.inc` after the last of those; a new `engine/*.inc` file's after
  the last of those, in front of the tail families, which then move by whole
  lines and keep their phase; and a family the generator writes to
  `baked_tail.inc` after the last tail group, `baked_compare_branch` being the
  first. The tail families end the table, so a new one moves no other bank
  group; the cold part starts on the first page after the bank's slots and
  moves by whole pages.
- A call to a helper outside the engine is a `helper_call`, and a new helper
  is appended to the island's list in `engine/common.inc`. The helper may
  overwrite x0 to x17, so no gadget keeps a value in x2 to x17 across the call
  (`x86-64-gadget-helper-clobbers`).

## Gadget banks

The ordinary bank decodes its operands from stream words and covers every shape
the decoder produces. The register-baked bank has every register offset as an
immediate in its entry; `engine/generate_baked.py` enumerates it,
`native_baked.hpp` declares the tables, and their enumeration order must change
together. The builder takes a baked entry for every 64-bit-mode instruction the
bank enumerates that has no segment override, address-size override or LOCK
prefix, and the ordinary bank for everything else.
`UML_X86_64_GADGET_BAKED=0` keeps the ordinary bank for every instruction, for
comparison. The descriptor bit `UML_X86_64_MEMORY_PLAIN_ADDRESS` is only ever
an optimization: a descriptor that leaves it clear takes the general address
tail and resolves the same address.

## Stream and block contracts

- An instruction whose gadget can fault, trap or leave carries its position
  (its address less the block's first address, `program_begin`) in ten bits,
  not its address. A cold exit rebuilds the RIP with `instruction_rip`, so a
  block ends before the first instruction 1024 bytes or more past its start. A
  baked PUSH or POP of a register carries nothing: `execute` recovers the
  instruction from the stream pointer on a fault.
- A block ends at an unconditional transfer or at the 64-instruction fetch
  window, not at its first conditional branch. A cached link names its target
  by the target's first entry and its stream past that entry, one aligned pair
  that a single paired store replaces, so a follow branches on the pair alone.
  In front of the stream a pair names sit the target's instruction count, its
  guest bounds and the first entry (`link_header` in `program.hpp`): a body's
  own, or an unlinked record's, whose entry is `link_exit`.
  `x86-64-gadget-link-layout` holds the layout.
- In long64 code with the baked bank on, a register CMP or TEST against an
  immediate or a register at 32 or 64 bits and the conditional branch right
  after it are one baked entry when the branch takes its inline form (its
  fall-through follows it and its target has a link), for every condition but
  parity and, after TEST, the four that read the carry (B, AE, BE, A). The
  branch then has no stream word, and a stream built without links keeps the
  two apart.
- A dispatch's instruction budget bounds how long it runs, not where a block
  stops: the block that crosses it runs to its end. Only a single step and the
  instruction a POPF or IRET setting TF leaves before its trap ask for an exact
  budget, and that replays the block from its saved source, never from guest
  memory.
- A repeated string instruction retires once however many elements it moves.
  It yields between elements after `UML_X86_64_STRING_REPEAT_BUDGET` of work
  with RIP still naming it, and the dispatch charges the same work to its
  budget. A step is one iteration. A repeated CMPS or SCAS commits its flags
  when it finishes.
- RF is state only: `execute` clears it as it enters the engine and no gadget
  reads or clears it. The runtime reports RF set for a fault exit and the live
  flags for a trap, as hardware does.
- The deferred-flag record (`DeferredFlagOperation`, `CpuState::carry_slot`) is
  read by assembly and C++ materializers that must agree. A producer that
  leaves host NZCV lets the next SETcc, CMOVcc or Jcc read it, and the deferred
  record is written as well, but for one case: in long64 with the baked bank
  on, a register-only ADD, SUB, AND, OR or XOR at 32 or 64 bits whose next
  instruction in the block is a register-only ADD, SUB, AND, OR, XOR, CMP or
  TEST writes neither, since that instruction writes the whole record before
  anything can read it.

## Guest-visible model

- System instructions behave as at CPL 3 on Linux with UMIP: the privileged
  ones raise `#GP` before they touch memory, and SGDT, SIDT, SMSW, SLDT and STR
  store what Linux's UMIP emulation stores. RDPID, RDTSCP's ECX and the per-CPU
  segment limit report processor 0 of node 0. CMPXCHG16B raises `#GP(0)` off
  16-byte alignment, locked or not.
- Both x86 engines share one segmentation model
  ([`segments.hpp`](../segments.hpp)): Linux's GDT, the i386 TLS entries and the
  task's `modify_ldt()` LDT. Segment loads, far transfers, an IRET that changes
  CS or SS, and LAR, LSL, VERR, VERW, SLDT and STR leave the engine unretired
  for `execute_segment_instruction`. The execution mode follows CS, code
  segments are flat, and 16-bit segments are refused, so Win16 and DOS code do
  not run. In compatibility mode FS and GS accesses check their limit, and a
  task whose ES, SS or DS is not flat writable data steps one instruction at a
  time; long-mode code pays for none of it.
- An instruction whose store faults has changed no flag, x87 status bit,
  MXCSR flag, register or stack pointer, apart from the elements a repeated
  string instruction had stored. A gadget commits its results after the
  store, and one whose helper raises status first keeps what it would change
  and puts it back at the fault exit. An exit for the runtime's exact segment
  check, which runs the instruction again, has changed none of it either. The
  `--retirement` rows of the corpus runner hold it.
- The engine runs only on hosts with FEAT_LSE2, which the runtime checks once.
  A locked operand inside one 16-byte granule keeps the exclusive loop; one
  across a granule or a page runs under the process-wide `split_operand_lock`
  (`../split_operand_lock.hpp`), which is atomic only against other operands on
  that path. A split CMPXCHG whose compare fails writes nothing back, because
  another vCPU's plain store may have landed since the read; it checks write
  access to each page instead.
- Every gadget that touches guest memory leaves through a `dmb ish`, because
  x86 promises total store order and full-fence LOCK and XCHG. `translate`,
  `stack_fast` and the bulk-helper macros mark their gadget
  (`x86_marks_guest_memory`); the mark is textual, so a new guest-memory path
  that bypasses them must mark itself.
- A translation may miss and a miss is a call that does not save host vector
  registers, so a gadget may not hold a guest value in one across its own
  translation. This is also why vector result forwarding takes register forms
  only. The corpus CTest `x86-64-instruction-corpus-clobbered-calls` holds it:
  every call into the address space leaves garbage in the caller-saved
  registers.
- The software TLB is direct-mapped and indexed by the low
  `UML_X86_64_TLB_INDEX_BITS` bits of the page number
  (`../layout/assembly_offsets.h`); `native_tlb_index` in
  `../software_tlb.hpp` and the three assembly copies of the index
  (`translate` and `translate_plain` in `engine/common.inc`, and the baked
  entries in `engine/generate_baked.py`) all read that constant. A
  translation compares the page of its access's last byte, and an index that
  never puts neighbouring pages in one entry is what makes every access that
  crosses a page miss and take its crossing path.
- x22 (`_tlb` in `engine/common.inc`) holds the run's software-TLB base:
  `uml_x86_64_gadget_enter` loads it from the context, the host frame gives
  the caller its own back, and no gadget writes it
  (`x86-64-gadget-tlb-register`).
- A stack access first re-checks the entry that the run's last stack
  translation used (`stack_fast`): PUSH, POP, CALL and RET, and the stack twin
  that the baked tables name in place of every fused [rsp + displacement]
  entry. A run starts at the entry of RSP's page. A hit needs the entry's exact
  tag, so it is that page's own entry and retires with it, and a store hits
  only an entry with no code bit, which a page holding code and a frame held
  at another address both set, while the run's `write_code` is clear, so it
  keeps the overlap test and skips the publication test. A twin hits only an
  access aligned to its size, which cannot cross a page. A crossing or
  misaligned access or another page takes the ordinary translation, and only
  the stack instructions' misses move the entry.

## Floating-point environment

A dispatch runs in the guest's rounding mode: entry maps MXCSR's rounding field
onto FPCR's and every MXCSR write republishes it. SSE, AVX and FMA arithmetic
return x86's values where the host's differ (NaN selection, DAZ and FTZ, the
estimate instructions), and MXCSR's exception flags are sticky through the
host's cumulative FPSR flags. Three flags stay inexact, as
`x86-64-masked-flags` names them: the denormal operand while DAZ is clear,
which no host flag shows, and two the host raises and x86 does not, the
underflow of a product, quotient, FMA, narrowing or dot-product step that
rounds up to the smallest normal and the invalid operation of an FMA of
infinity times zero plus a quiet NaN. x87 arithmetic computes in SoftFloat
helpers and raises no host flag; the transcendentals follow an Intel core, in
FEX's `cephes/X87Math.cpp`, which the JIT shares, and do not reproduce its
microcode rounding noise. The model of the exact SIMD flags is
[`../fp_model.hpp`](../fp_model.hpp).

`uml_x86_64_gadget_leave` restores round-to-nearest by clearing FPCR's mode
field. [`gadget_host_frame.inc`](../../gadget_host_frame.inc) lays out the entry
frame for both gadget engines. No gadget keeps a value in x18: Darwin clears
that platform register when it interrupts the thread, and `architecture-audit`
rejects one.

## Exceptions the guest enables

x86 raises `#XM` for a SIMD floating-point exception MXCSR unmasks, `#MF` at
the next waiting x87 instruction after an unmasked one, and `#AC` for a
misaligned access while RFLAGS.AC is set. A guest that enables none runs
unchecked. Otherwise a gadget leaves before it stores anything, with
`UML_X86_64_EVENT_EXCEPTION_CHECK` and the stream position, and the runtime
runs that instruction again alone and checked. When RFLAGS.AC, MXCSR unmasking
underflow, precision or a denormal operand, or a pending unmasked x87
exception makes every such instruction a candidate, the runtime dispatches
through an executor whose blocks end before each one (a check head that no link
enters, run by `execute_checked`). An instruction that could change what a run
checks, such as POPF setting AC or LDMXCSR and FLDCW unmasking, retires and
leaves the engine, so linked code never runs past it unchecked. Code under
these settings runs much slower than unchecked code.

Known deviations from x86 that remain:

- The faulting address of a partly mapped FXSAVE, XSAVE, XSAVEOPT or XRSTOR
  area is the first inaccessible byte or the operand's base, where an Intel
  core reports the last byte of the extent first.
- Neither engine raises the `#GP` x86 raises for a misaligned operand of an
  aligned SSE form such as MOVAPS, MOVNTPS or a legacy SSE 16-byte memory
  source.
- The gadget engine checks only an instruction's first access for `#AC`; the
  JIT checks every one.

## Code pages and cached translations

- [`../code_page_registry.hpp`](../code_page_registry.hpp) keeps, per
  address-space generation, the set of pages in which any executor holds a
  translation. A store publishes a code change at its own page only when that
  page is in that set (a store through a writable alias also publishes at the
  other holders of its frame), which keeps publication precise under
  READ_IMPLIES_EXEC. A participant with another TLB layout, such as the x86-64
  JIT, joins with a function that retires its own entry for a page. A page
  enters the set before any translation built from it is handed out, and every
  participating executor's write-side TLB entry for it is retired before the
  registration returns. A registration waits at most one second for a
  participant that is still inside the engine, and says so once; the page then
  stays unsettled for as long as that participant stays inside, and the gadget
  engine builds the translation of an unsettled page again at every dispatch
  instead of caching it.
- The memory layer accounts the writer locations and the holders of each frame
  ([`docs/architecture.md`](../../../../docs/architecture.md#x86-64-frame-accounting)).
  A translator that registers a frame retires the write translations of the
  writer locations the registration names.
- A translation is built and validated only from a frame its generation holds:
  the executor registers the frame at the page's location before it reads a
  byte, and caches nothing from a page whose registration did not settle: an
  unsettled, failed or refused one gives a transient program, one instruction
  when no holder stands, never cached and rebuilt at the next dispatch (a
  refused first page, one that is not a code page or whose generation is
  gone, runs that one instruction and no more). Every body records the generation's
  flush serial with its page stamps, so a flush of the generation makes a
  body compare its source and register again though no stamp moved.
  The executor remembers the last settled registration of a page and skips
  the address-space call while the generation's mappings are unchanged, its
  flush serial is the one it was taken at, and the frame's change count has
  not moved. A holder added to the frame, and its writers becoming unknown,
  move the count; an unsettled, failed, refused or global registration is
  never remembered ([`../registration_memo.hpp`](../registration_memo.hpp)).
- A write fill is tagged when its frame has a holder at another location,
  whether or not the page it fills is executable, and a store through that
  entry publishes at every holder: the holders in this generation cool their
  blocks, those in another generation are published in theirs. A store that
  writes, through such an alias, into the block that is running leaves the
  block after the store, as a store at the block's own address does, whatever
  instruction stored: an ordinary or baked store, PUSH and CALL, a store across
  a page, a vector, x87 or bulk store, a string store, ENTER, an extended-state
  save and the segment model's SLDT and STR. A helper that stores and
  publishes itself records the store's reach in the run, and the instruction's
  epilogue leaves on it.
- Every store to a page that holds cached code owes a publication of its own,
  and the program performs it before its next store: another vCPU may
  revalidate the page between two stores, so a program that patches a page
  store by store pays one publication per store.

### Keeping a cached translation

A cached program is reused while the guest bytes it was translated from are
still there. The stamp of each page it covers, which any code-change
publication on that page moves, and the program's saved source bytes, compared
whenever a stamp has moved, answer that. A stamp certifies only the bytes read
after it was taken, so a stamp is always taken before the source it stands for,
and a new translation reads its block once. A writer stores the bytes, then
moves the page stamp, then moves the code-change sequence; a reader observes
the sequence before it looks. A store still in flight reaches the vCPU at its
next code-change checkpoint (a system call, CPUID or IRET), which leaves the
native run so the runtime revalidates what it selects next. A moved mapping
sequence or a raised execution interrupt abandons a validation. A code-change
range of length 0 is the whole address space, and a change the ring cannot
describe is treated alike: every body's link records are reset, since no stamp
guards a link.

### Sharing and budget

- Executors that run one address space share its translations, unless
  `DARWIN_UML_GADGET_SHARED_BODIES=0` keeps them private. A shared space is
  made for an address-space generation and the exceptions its blocks are built
  for, and an executor built for other exceptions never shares with it.
- A body is unpublished when a slot lets go of it but stays in limbo until every
  executor seen inside translated code has left, which `run_x86_gadgets`
  announces with `begin_run` and `end_run`. A shared space is never swept: it
  is closed and replaced once its arenas hold as much freed space as live
  bodies, and dies with its last executor.
- Each executor lets go of the address spaces the MMU reports destroyed at the
  start of every run (`Executor::leave_gone_generations`); the kernel never
  reuses a generation number.
- Each address space charges an account in the gadget engines' process-wide
  byte budget (`cpu/gadget_cache_budget.hpp`,
  [`darwin/README.md`](../../../README.md)); a run's first block tells the
  account the address space was entered, and an account asked to shed is let go
  of at the executor's next run.

`UML_GADGET_CACHE_CENSUS=ON` builds record what each cache holds in the arm64
engine's format
([`tests/perf/README.md`](../../../../tests/perf/README.md#cache-census)).

## Build and test

```sh
cmake --workflow --preset check-unit
```

Focused tests: `ctest -R '^x86-64-gadget-'` (cache, link layout, sharing,
bootstrap, pinned region), `x86-64-masked-flags` and `x86-64-sse-exactness`.
The instruction corpus, which is the acceptance gate, and the FEX campaign are
in [`docs/testing.md`](../../../../docs/testing.md#x86-instruction-conformance).
