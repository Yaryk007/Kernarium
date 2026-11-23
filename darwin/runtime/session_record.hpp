// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "guest_statistics.hpp"
#include "platform/camera_presentation.hpp"
#include "platform/camera_still_failure.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace uml::runtime {

/*
 * The record of how the process's guest session ended, for the next launch.
 *
 * A launch that names a record file (--session-record=) has the run write
 * "running" into it before the kernel starts, and every way the session can
 * end overwrites that with the end: the run's own return, a guest kernel
 * panic or other kernel crash (darwin_uml_abort), an exit the kernel drives
 * (darwin_uml_exit), a hibernated guest whose RAM cannot be restored, a resume
 * that finds a guest disk changed (failed), and any exit() of the process
 * while the guest runs. A record still "running" at the next launch therefore
 * says that something outside the process ended it, without letting it run
 * another instruction: a SIGKILL from the system, such as a memory-limit end,
 * or from the person.
 *
 * The kernel's console and diagnostic output goes through write(), which the
 * loader resolves to kernel_write(): the record keeps the last few KiB of it,
 * so a crash's record carries the panic message and the lines before it. A
 * fault on a host thread that owns no vCPU does not reach the kernel's fault
 * path: the kernel's handler reports it on stderr and lets the fault end the
 * process through the default disposition, which writes the crash report.
 * kernel_write records that end as host_fault when it sees the report. The
 * same output tells whether an Android guest finished booting
 * (console_boot_completed) and whether init's output reached the record at all
 * (console_init_observed), which the emulator reports as the run's end and an
 * application's restart loop reads. The output is read where it is written to
 * stdout, stderr and the descriptors the `con` and `ssl` arguments name with
 * fd:, so a console on anything else shows none of it. For a guest whose
 * console reports its boot (set_session_boot_facts) the state also says "boot
 * completed", "boot unfinished" (init's output was seen, the boot did not
 * finish) or "boot unobserved" (neither was seen), "restarts N" and the boot's
 * "ramdisk stock", "ramdisk root" or "ramdisk recovery". While the
 * boot has not completed the checkpoint thread rewrites the state with the
 * output so far whenever the output grew, so a process that is killed
 * mid-boot leaves where the boot had got; an end that the guest or the person
 * chose (a power-off, a restart, a stop) quotes the output too. The output a
 * record quotes is that of its own boot: what an earlier boot of the process
 * printed is not in it. An end is the last write of the state: a checkpoint
 * whose write lands after it, however late, writes the end's record again.
 * Each state is one write of the whole state area, padded with newlines that
 * the readers ignore, and the file is cut to its size once, at the session's
 * begin: a reader never sees one state mixed with another.
 *
 * Every memory_checkpoint_interval_ms while a session runs, a thread of the
 * record's own writes the process's footprint, its peak and what its memory
 * limit leaves into a fixed slot at the start of the file, so a record that
 * is still "running" says how close to its limit the process was. With a
 * terms reader (set_memory_terms_reader) the slot also holds the footprint's
 * large terms (MemoryTerms) now and at the session's highest checkpoint, and
 * their history: every checkpoint of the last memory_history_recent, and one
 * in memory_history_older_stride before those, memory_history_older of them.
 * Reading malloc's term stalls every thread that allocates meanwhile, so a
 * checkpoint asks for it only when memory_malloc_reading_due says so and
 * otherwise repeats the last reading with its age. The other terms are read
 * every time, and so is the footprint less them ("other"), which follows
 * malloc and what no term names between readings.
 *
 * With a run figures reader (set_run_figures_reader) each history row also says
 * how the run went in the time it covers (RunFigures): the scanout flushes
 * and the longest gap between two (frame rate and worst frame), the host's
 * thermal state and memory pressure, the gadget cache's reliefs, shed
 * accounts, renewals and largest account, the CPU time of the process and of
 * the vCPU carriers, and from the task's power information the share of that
 * time on performance cores and the energy, and on macOS the cycles per CPU
 * second ("-" on iOS, which has no published source for them). A row
 * of the recent history covers the time since the checkpoint before it; a row
 * of the older history covers the memory_history_older_stride checkpoints it
 * stands for, so that nothing between two older rows goes unseen. A figure
 * whose source has nothing to say is "-". The reader runs at the checkpoint
 * alone: nothing runs per frame but the device's count of its flushes.
 *
 * Everything that runs at an end only formats into static storage and makes
 * system calls: it takes no lock and allocates nothing, so a panic, a signal
 * handler or a process whose other threads are suspended can run it.
 *
 * The file is text of a fixed layout and of a size it never exceeds
 * (session_record_max_bytes), whatever the session does: the memory slot, the
 * stall slot, the network slot, the storage slot, the camera slot, then the state with the
 * kernel output it quotes. Each part is rewritten in place by its own writer,
 * so none grows with the session's length. The stall slot is the stall
 * watch's (runtime/stall_record.hpp). The network slot holds the counts of the
 * guest's user network (set_network_counts_reader) and whether the application
 * showed its notice that iOS denies it the network (set_network_notice):
 * numbers and words only, never an address, a port or a name. The storage slot
 * holds how many times the guest's storage was settled (note_guest_settle) and
 * how many of those the guest did not confirm, and the last settle's kind,
 * outcome and time; it stays blank until the first one. The camera slot holds
 * how many stills of the guest's cameras did not end in a picture and why for
 * the last few (note_camera_still_failure): the camera, the stage, the sizes
 * asked for, resolved and encoded, the photo output's orientation compensation
 * and the framework's error as a domain word and a code, never a name or a
 * message; it stays blank until the first one. The first line names
 * the layout: "uml-session 8" has the camera slot between the storage slot and
 * the state; "uml-session 7" has the storage slot between the network slot
 * and the state; "uml-session 6" has the history rows described above in a
 * memory slot of 8 KiB; "uml-session 5" has the network slot between the
 * stall slot and the state and shorter rows in a 4 KiB memory slot;
 * "uml-session 4" has the stall slot between the memory slot and the state;
 * the earlier layouts (1 to 3) have the state where the stall slot is now.
 */
inline constexpr unsigned memory_checkpoint_interval_ms = 2000;
/* The bytes of the memory slot at the start of the file, of the stall slot
 * after it, of the network slot after that, of the storage slot, of the camera
 * slot, and where the session's state begins. */
inline constexpr std::size_t session_memory_slot_bytes = 8192;
inline constexpr std::size_t session_stall_slot_bytes = 4096;
inline constexpr std::size_t session_network_slot_bytes = 512;
inline constexpr std::size_t session_storage_slot_bytes = 256;
inline constexpr std::size_t session_camera_slot_bytes = 1024;
inline constexpr std::size_t session_network_offset =
	session_memory_slot_bytes + session_stall_slot_bytes;
inline constexpr std::size_t session_storage_offset =
	session_network_offset + session_network_slot_bytes;
inline constexpr std::size_t session_camera_offset =
	session_storage_offset + session_storage_slot_bytes;
inline constexpr std::size_t session_state_offset =
	session_camera_offset + session_camera_slot_bytes;
/* The kernel output the state quotes, and the most the state ever takes: its
 * lines, a message and that output, which is the boot's own. Once the ring has
 * dropped output of that boot the state quotes it from its first complete
 * line. What identifies the installation is masked wherever the state quotes
 * it: the serial number, the adb key and the time zone that the session's
 * kernel arguments state (by their assignments and by value) and any hardware
 * address in its colon spelling.
 * The values are matched on the text as the record stores it, after what it
 * drops or replaces, and on word boundaries: a longer word that holds one is
 * kept. */
inline constexpr std::size_t session_kernel_output_bytes = 4096;
inline constexpr std::size_t session_state_max_bytes =
	session_kernel_output_bytes + 1024;
/* The size the file has from a session's begin on: the state's area is always
 * written whole. */
inline constexpr std::size_t session_record_max_bytes =
	session_state_offset + session_state_max_bytes;
inline constexpr unsigned memory_history_recent = 24;
inline constexpr unsigned memory_history_older = 24;
inline constexpr unsigned memory_history_older_stride = 15;

/* A checkpoint reads malloc's term at a session's first checkpoint and when
 * the footprint passes the highest one a reading was taken at by
 * memory_malloc_peak_step_bytes: readings gather where the footprint climbs,
 * as at boot and while a game loads, a steady footprint takes none, and a
 * footprint that climbs to the limit has a reading from at most one step
 * below its top. */
inline constexpr std::uint64_t memory_malloc_peak_step_bytes =
	std::uint64_t{64} << 20;

/* The rule above; `read_peak` is the highest footprint a reading was taken
 * at. */
[[nodiscard]] constexpr bool memory_malloc_reading_due(bool read_before,
	std::uint64_t footprint, std::uint64_t read_peak) noexcept
{
	return !read_before ||
		footprint >= read_peak + memory_malloc_peak_step_bytes;
}

/* What the checkpoint asks for the footprint's terms, malloc's included when
 * `read_malloc`; any thread. */
using MemoryTermsReader = MemoryTerms (*)(bool read_malloc) noexcept;

/* The reader the memory checkpoints use from now on; null for none, which
 * leaves the slot with the footprint alone. */
void set_memory_terms_reader(MemoryTermsReader reader) noexcept;

/* What the checkpoint asks for the figures of its history rows; any thread.
 * `session_begins` is true for the reading that opens a session, which the
 * rows are measured from. */
using RunFiguresReader = RunFigures (*)(bool session_begins) noexcept;

/* The reader the checkpoints use from now on; null for none, which leaves
 * every figure of a row that it adds to the terms as "-". */
void set_run_figures_reader(RunFiguresReader reader) noexcept;

/* The counts of the guest's user network at one moment, each since the
 * provider was made (device/network/slirp/network_counters.hpp says what each
 * one counts). The record writes the difference between the reading at the
 * session's start and the reading at each checkpoint and at its end. */
struct SessionNetworkCounts final {
	std::uint64_t dns_answered{};
	std::uint64_t dns_policy_denied{};
	std::uint64_t dns_timed_out{};
	std::uint64_t dns_failed{};
	std::uint64_t dhcp_discovers{};
	std::uint64_t dhcp_requests{};
	std::uint64_t dhcp_acks{};
	std::uint64_t tcp_attempts{};
	std::uint64_t tcp_established{};
	std::uint64_t tcp_unreachable{};
	std::uint64_t tcp_reset{};
};

/* What the checkpoints and the end ask for the network's counts: false when
 * the guest has no network. Any thread, a signal handler included: it takes no
 * lock and allocates nothing. */
using SessionNetworkReader = bool (*)(SessionNetworkCounts &) noexcept;

/* The reader the sessions use from now on; null for none, which leaves the
 * network slot without counts. */
void set_network_counts_reader(SessionNetworkReader reader) noexcept;

/* Which iOS denial the application's notice is up for, as the application
 * states it (runtime API 36): none, or cellular or Wi-Fi access denied. The
 * record keeps how many times a denial began during the session, the session's
 * start included when one was under way, and the one in force. Any thread. */
enum class NetworkNotice : std::uint8_t {
	none,
	cellular_denied,
	wifi_denied,
};

void set_network_notice(NetworkNotice notice) noexcept;

/* What a call that settles the guest's storage ended with, for the storage
 * slot. `kind` is the call; `storage` is what the guest confirmed of its own
 * writeback, and `suspension` whether the call put it to sleep. */
enum class SettleKind : std::uint8_t {
	sync,
	quiesce,
	quiesce_resident,
};

enum class GuestStorage : std::uint8_t {
	/* The zero value claims nothing: no call reported. A record never holds
	 * it. */
	not_reported,
	/* The guest ran sync(2) to its return and said so. */
	confirmed,
	/* No guest was running: there was nothing to settle. */
	no_guest,
	/* The guest was asked and did not confirm in time, or only started the
	 * writeback, or the host could not ask. */
	unconfirmed,
	/* The guest refused the request. */
	refused,
	/* A guest was running and its console could not be reached. */
	unreachable,
};

enum class SettleSuspension : std::uint8_t {
	not_asked,
	suspended,
	not_suspended,
};

/* Whether the guest's storage counts as settled: it confirmed its writeback,
 * or there was no guest. Anything else leaves dirty pages the host cannot see
 * in a guest that may still run. */
[[nodiscard]] constexpr bool guest_storage_settled(GuestStorage storage)
	noexcept
{
	return storage == GuestStorage::confirmed ||
		storage == GuestStorage::no_guest;
}

/* Records one settle in the storage slot of the session that is open: a count
 * of all of them and of those that left the storage unsettled, and the last
 * one's kind, outcome and time. Nothing without an open session. Any thread
 * but a signal handler: it may wait for another writer of the slot. */
void note_guest_settle(SettleKind kind, GuestStorage storage,
	SettleSuspension suspension) noexcept;

/* How many of the last stills that did not end in a picture the camera slot
 * names, the newest of them. */
inline constexpr unsigned session_camera_failures_kept = 8;

/* Records a still of the guest's cameras that did not end in a picture in the
 * camera slot of the session that is open: a count of all of them, and the
 * last session_camera_failures_kept with their time (the ones that do not fit
 * the slot, oldest first, are left out). The camera device reports one through
 * its owner for each. Nothing without an open session. Any thread but a signal
 * handler: it may wait for another writer of the slot. */
void note_camera_still_failure(
	const uml::platform::CameraStillFailure &failure) noexcept;

/* The cameras the camera slot keeps a note of: the guest's numbers 0 to 7 (the
 * camera device offers at most eight). */
inline constexpr unsigned session_cameras_kept = 8;

/* Records how the platform presented one of the guest's cameras in the camera
 * slot of the session that is open: the camera's whole note replaces its
 * earlier one, and the slot keeps the newest note of each camera
 * (note_camera_still_failure's rules for the rest). The camera device reports
 * each change through its owner. Nothing without an open session, and nothing
 * for a camera number the slot has no room for. */
void note_camera_presentation(
	const uml::platform::CameraPresentation &presentation) noexcept;

enum class SessionEnd : std::uint8_t {
	running,
	powered_off,
	restarted,
	stopped,
	failed,
	guest_panic,
	kernel_abort,
	kernel_exit,
	hibernation_restore_failed,
	process_exit,
	host_fault,
};

/* The record's word for `end`. */
[[nodiscard]] std::string_view session_end_name(SessionEnd end) noexcept;

/* The descriptors that kernel arguments name as console output (con, conN,
 * ssl or sslN set to fd:IN,fd:OUT or fd:BOTH). */
[[nodiscard]] std::size_t console_output_descriptors(
	std::span<const std::string> kernel_arguments, std::span<int> descriptors)
	noexcept;

/* Opens the record at `path` for the session that is about to boot and
 * writes "running"; the console output the kernel writes to the descriptors
 * the arguments name is kept from here on. An empty path keeps no record.
 * Returns false, after one log line, when the file cannot be opened. */
bool begin_session_record(std::string_view path,
	std::span<const std::string> kernel_arguments) noexcept;

/* Ends the session with `end` and `status`, the run's result or the exit
 * status. The first end of a session wins; a later one changes nothing. */
void end_session_record(SessionEnd end, int status,
	std::string_view message = {}) noexcept;

/* The guest kernel crashed and the process is about to abort: records a
 * guest_panic with the kernel's panic message when its output carries one,
 * otherwise a kernel_abort, and names the calling thread after the panic, so
 * that the crash report, which shows thread names, says why. */
void record_kernel_crash() noexcept;

/* Writes the memory slot, the network slot and, for a guest whose boot is
 * unfinished (set_session_boot_facts), the state with the output so far now;
 * the checkpoint thread calls it. */
void checkpoint_session_memory() noexcept;

/* What the host that runs the guest knows of its boots, for the state the
 * record keeps: whether the guest's console reports a finished boot (an
 * Android guest's does, a Linux desktop guest's does not) and how many times
 * the guest restarted in this process before the session that begins. Set it
 * before each run; any thread. For a guest that reports its boot the state
 * says "boot completed", "boot unfinished" or "boot unobserved" and "restarts
 * N"; for the others it says none and the checkpoints leave the state alone. */
void set_session_boot_facts(bool console_reports_completion,
	unsigned restarts) noexcept;

/* Which ramdisk a boot starts the guest with, as the state says it: the
 * release's own, the root image's (Magisk patched in) or recovery's. */
enum class BootRamdisk : std::uint8_t {
	unreported,
	stock,
	root,
	recovery,
};

/* The ramdisk of the boot that begins, once the host has chosen it (after
 * set_session_boot_facts, which forgets the one before). For a guest that
 * reports its boot the state then also says "ramdisk stock", "ramdisk root"
 * or "ramdisk recovery" on the line after "restarts N", which is how an
 * application learns from the record alone whether the boot that ended was
 * the root ramdisk's, whatever it asked for and whatever the guest restarted
 * into. A boot whose ramdisk was never set, and a guest that does not
 * report its boot, say none. Any thread. */
void set_session_boot_ramdisk(BootRamdisk ramdisk) noexcept;

/* Whether a session is writing to a record file. */
[[nodiscard]] bool session_record_open() noexcept;

/* Replaces the stall slot with `slot`, which must be exactly
 * session_stall_slot_bytes of text; anything else changes nothing. Any
 * thread; it takes no lock, allocates nothing and does nothing without an
 * open session. */
void write_session_stall_slot(std::string_view slot) noexcept;

/* The loader's resolution of the kernel's write import. */
ssize_t kernel_write(int descriptor, const void *buffer, std::size_t length)
	noexcept;

/* kernel_write for the import named `name` (write, with or without the
 * leading underscore the Darwin SDK's asm labels add), else nullptr. */
[[nodiscard]] void *kernel_output_import(const char *name) noexcept;

/* Whether the kernel's console output, since the session began, said that an
 * Android guest finished booting (console_boot_signal.hpp). It is false for a
 * guest whose console never says so. Any thread. */
[[nodiscard]] bool console_boot_completed() noexcept;

/* Whether init's output has reached the record since the session began: its
 * first line, or a line that ends the boot. False for a console the record
 * does not read, and for a guest that is not Android. A boot that is neither
 * completed nor init-observed is unobserved, not known to be unfinished. Any
 * thread. */
[[nodiscard]] bool console_init_observed() noexcept;

/* What the record holds of the kernel's output since the session began: the
 * last bytes, oldest first, copied into `buffer`. For tests. */
std::size_t kernel_output_tail(std::span<char> buffer) noexcept;

/* The message of the last "Darwin UML panic: " line in `output`, or empty. */
[[nodiscard]] std::string_view panic_message(std::string_view output) noexcept;

/* The registers of the last host-fault report in `output` (the text after
 * "Darwin host fault: " on its "signal=" line), or empty. */
[[nodiscard]] std::string_view host_fault_message(std::string_view output)
	noexcept;

/* Forgets the session: points the record's descriptor at /dev/null and drops
 * the kept output. For tests. */
void reset_session_record() noexcept;

} // namespace uml::runtime
