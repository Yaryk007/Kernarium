// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KERNARIUM_RUNTIME_H
#define KERNARIUM_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#define KERNARIUM_RUNTIME_API_VERSION 46U

#if defined(__OBJC__)
@class MTKView;
typedef MTKView* KernariumMetalView;
#else
typedef void* KernariumMetalView;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* What the framework's embedded kernel boots. One framework carries exactly
 * one flavour, because the kernel ELF is fixed in its signed carrier. */
enum {
    KERNARIUM_RUNTIME_GUEST_LINUX = 0,
    KERNARIUM_RUNTIME_GUEST_ANDROID = 1,
};

/* The send_touch coordinates are absolute positions in the guest's
 * touchscreen range, 0 through this value on both axes, wherever the display
 * is and however it is sized. The host maps its view onto that range, so the
 * range never has to follow a mode change. Each simultaneous host contact
 * keeps a slot in 0..<TOUCH_CONTACTS until it ends. A release affects only
 * that slot; invalid slots and out-of-range coordinates are ignored.
 *
 * A contact the host did not end itself, such as a touch the system takes
 * over, ends with cancel_touch instead of a release. The guest sees the
 * contact turn into a palm and then lift, which Android reports to
 * applications as ACTION_CANCEL (or a cancelled pointer-up while other
 * fingers stay down) and Linux readers as a cancelled touch, so no tap or
 * click follows. The contact stays at the last position it reported, a
 * slot that holds no contact is ignored, and the slot is free for a new
 * contact at once. Android's recovery menu still treats such a still tap as
 * a selection. */
enum {
    KERNARIUM_RUNTIME_TOUCH_MAXIMUM = 32767,
    KERNARIUM_RUNTIME_TOUCH_CONTACTS = 10,
};

enum {
    KERNARIUM_RUNTIME_ANDROID_BOOT_AUTO = 0,
    KERNARIUM_RUNTIME_ANDROID_BOOT_NORMAL = 1,
    KERNARIUM_RUNTIME_ANDROID_BOOT_RECOVERY = 2,
};

/* The bits of kernarium_runtime_boot.android_host_features (runtime API 31).
 * Each is one feature of the host, and the runtime's table of them
 * (KernariumHostFeatures.hpp) names the kernel argument it becomes. */
enum {
    /* The device has haptics (a Taptic Engine) and the application plays the
     * guest's vibrator on it. The guest then finds a vibrator; without the bit
     * it finds none. */
    KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS = 1u << 0,
    /* The application gives the guest the device's orientation (runtime API
     * 37), which it follows where the device's interface does not turn. The
     * guest then lists a device-orientation sensor, which Android's window
     * manager follows in place of the accelerometer; without the bit it lists
     * none and judges the accelerometer, as an image without the sensor does. */
    KERNARIUM_RUNTIME_ANDROID_HOST_ORIENTATION = 1u << 1,
    /* The application gives the guest the device's magnetic field (runtime API
     * 38). The guest then lists a magnetometer and declares the compass
     * feature, from which Android derives the rotation vector and the
     * orientation that navigation applications take their heading from;
     * without the bit it lists none and declares none. */
    KERNARIUM_RUNTIME_ANDROID_HOST_COMPASS = 1u << 2,
    /* The application asks for a 120 Hz display (runtime API 44). The runtime
     * decides once at boot, against the boot view's own screen (at least 100
     * frames a second) and the boot size (the EDID must carry the 120 Hz
     * timing), and on a Mac or Vision Pro always refuses. Granted, it
     * advertises one 120 Hz mode in place of the 60 Hz one and paces its
     * presents to the guest's rate, and the guest then runs the 120 Hz
     * configuration of Android's frame scheduler; refused, or without the bit,
     * the display and the guest are what they have always been. The
     * statistics' display_refresh_hz says what the run was granted. */
    KERNARIUM_RUNTIME_ANDROID_HOST_HIGH_REFRESH = 1u << 3,
};

/* What the camera does for an Android guest this run (runtime API 37), the
 * value of kernarium_runtime_boot.android_camera_mode. It is fixed at the
 * start of the run and nothing changes it until the guest powers off. */
enum {
    /* The guest finds the host's cameras and every frame is black: the guest
     * reports Android's hardware camera privacy switch as on (a lens cover),
     * the host opens no capture session, and the system's camera indicator
     * cannot light. This is also what zero, an application that states
     * nothing, gets. */
    KERNARIUM_RUNTIME_ANDROID_CAMERA_BLOCKED = 0,
    /* The guest's camera apps see the host's cameras. A capture session opens
     * only while a guest app has a camera open. The application asks the
     * system for access first (the runtime never does) and states this only
     * when the system has granted it: a session whose access is refused later
     * delivers black frames as an interruption does. */
    KERNARIUM_RUNTIME_ANDROID_CAMERA_LIVE = 1,
};

/* A boot request. Linux guests supply rootfs_path and overlay_path. Android
 * guests supply the release directory (guest/system.img, the read-only system
 * disk; guest/partition-layout.json; boot/kernel-command-line.txt), the path of
 * the instance's writable data disk, and the desired boot mode. The runtime
 * creates a data disk that is absent, sized android_data_gib GiB or the
 * release's default when that is 0, and grows an existing one to
 * android_data_gib GiB when that is larger; it never shrinks one. The guest
 * has exactly these two disks. The kernel is carried by the runtime.
 *
 * The host GPU's budget is B = max(L - A - G - E - S, 352 MiB), evaluated
 * once at launch (see the architecture document): L is the application's
 * memory limit, G the guest's memory, and E the gadget cache plus the other
 * memory the guest side of the process uses. Without a limit B is G. The
 * request states the three launch parameters: A, the application's own
 * expected ceiling; E_rest, the guest side's memory beyond the gadget cache;
 * and S, the headroom left unused. Zero, which is what the application states
 * until their values are chosen, adds nothing to the sum.
 *
 * An Android guest authorizes one adb host key for this boot without asking
 * its user: android_adb_public_key is the key as adb writes it, base64 of the
 * RSAPublicKey record without the trailing comment, 1 through 1024 characters
 * of A-Z a-z 0-9 + / =, or null for none. The runtime reads it only from a
 * request whose struct_size covers it and fails the run with
 * -UML_EMULATOR_INVALID_ARGUMENT when it is anything else. The guest's
 * kernel command line, which also carries the release's own and the launch's
 * arguments, holds 4096 bytes; a run whose command line is longer fails
 * with a message that says so. A Linux guest ignores the key.
 *
 * An Android guest also takes the host's time zone for this boot:
 * android_time_zone is its IANA name (for example Asia/Tokyo), 1 through 64
 * characters of A-Z a-z 0-9 _ + - and / in slash-separated components that
 * start with a letter or digit, or null for none. The runtime reads it only
 * from a request whose struct_size covers it, passes a valid name as
 * androidboot.kernarium.timezone, and drops anything else with a line on
 * stderr: a time zone never fails a boot. Whether the guest knows the name,
 * and whether it follows it, is the guest's to decide. A Linux guest ignores
 * it.
 *
 * session_record_path names the file the runtime records how the guest's
 * session ended in (darwin/runtime/session_record.hpp): "running" from the
 * boot on, then the end, a guest kernel panic with its message included. The
 * caller reads it at its next launch, before it boots again. Null keeps no
 * record; the runtime reads it only from a request whose struct_size covers
 * it.
 *
 * use_jit asks for the JIT engine in place of the gadget engine for this boot
 * (runtime API 30), on a runtime that has one and has been through
 * prepare_jit for this request's guest memory and vCPU count with a result of
 * KERNARIUM_RUNTIME_JIT_READY. A boot that asks for it when that is not so
 * runs on gadgets, with a line on stderr: an engine never fails a boot. The
 * runtime reads it only from a request whose struct_size covers it.
 *
 * android_host_features says what the host has that an Android guest is to
 * find (runtime API 31): a mask of the KERNARIUM_RUNTIME_ANDROID_HOST_ bits
 * below, zero for none. The runtime turns each bit it knows into the one kernel
 * argument the guest's image reads for it, which the release's command-line
 * file cannot set, and ignores a bit it does not know with a line on stderr: a
 * feature never fails a boot. The runtime reads it only from a request whose
 * struct_size covers it; a Linux guest ignores it.
 *
 * android_camera_mode and android_camera_calibration_path (runtime API 37)
 * give an Android guest the host's cameras. The runtime discovers them, which
 * needs no permission, composes the camera layout that describes them
 * (darwin/device/camera/README.md) as androidboot.product.vendor.sku, which the
 * release's command-line file cannot set, and makes the camera device after
 * the GNSS receiver. A host with no camera gets no device and the layout
 * "none". The mode is one of the KERNARIUM_RUNTIME_ANDROID_CAMERA_ values; an
 * unknown value runs as blocked, with a line on stderr, because a camera never
 * fails a boot. The calibration path is a directory, the one the colour data a
 * Bayer still teaches is kept in (per lens, outside the guest's disks), or null
 * to keep it in memory. The runtime reads both only from a request whose
 * struct_size covers them; a Linux guest ignores them.
 *
 * android_serial_number and android_ethernet_mac (runtime API 42) state the
 * installation's identity for a run that must have fixed values, which a test
 * or a measurement wants. An Android guest otherwise has an identity of its
 * own: the runtime keeps a random serial number and a random eth0 address in
 * identity.json beside the data disk, made at the first start of a data disk
 * (and of an installation that has none), and gives them to the guest. A
 * request that states either value gets that one value, and the file is
 * neither read nor written. The other stays what the release and the user
 * network have without an identity: a request that states only the address
 * gets the release's own fallback serial number, and one that states only the
 * serial number gets the user network's default eth0 address. The serial is 6 through 20 characters of A-Z a-z
 * 0-9 other than "unknown"; the address is the text aa:bb:cc:dd:ee:ff of a
 * unicast address that is locally administered and none of 02:00:00:00:00:00,
 * 52:54:00:12:34:56 or 02:15:b2:00:00:00. A value that is anything else fails
 * the run with -UML_EMULATOR_INVALID_ARGUMENT. The host's own messages never
 * contain either value; what the guest itself reports (an Android bug report,
 * the kernel's echo of its command line) carries them as on any Android device.
 * The runtime reads them only from a request whose struct_size covers them; a
 * Linux guest ignores them.
 *
 * android_aot_disabled (runtime API 45) keeps the ahead-of-time component out
 * of an Android launch: a nonzero value leaves out the release identity that
 * the runtime states to the emulator, without which the component never runs,
 * so the guest runs on the execution engine alone. Zero, and a request whose
 * struct_size does not cover the field, state the release as before, and the
 * emulator then decides as it always has (the component runs only where the
 * build has it and the release is the one it was made from). The word is read
 * at each boot request, and a guest that restarts itself inside a run keeps the
 * one its run was asked with. A Linux guest ignores it.
 *
 * android_root (runtime API 46) asks for the root boot: a nonzero value starts
 * the guest, on every boot and restart of the run, with the Magisk-patched
 * ramdisk that the release carries beside its system disk
 * (darwin/device/block/README.md, "Boot selection"). Zero, and a request whose
 * struct_size does not cover the field, boot the release's own ramdisk, so
 * Kernarium is unchanged. The runtime never takes a ramdisk from any other
 * file: a request for root against a release that carries none fails the run
 * with -UML_EMULATOR_INVALID_ARGUMENT, and recovery keeps its own ramdisk. A
 * Linux guest ignores the field. */
typedef struct kernarium_runtime_boot {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t dots_per_inch;
    uint32_t guest_memory_mib;
    uint32_t guest_processor_count;
    uint32_t android_boot_mode;
    uint32_t auto_resize_guest;
    const char* rootfs_path;
    const char* overlay_path;
    const char* android_data_disk_path;
    const char* android_release_path;
    uint32_t android_data_gib;
    uint32_t gpu_budget_application_mib;
    uint32_t gpu_budget_engine_mib;
    uint32_t gpu_budget_headroom_mib;
    const char* android_adb_public_key;
    const char* android_time_zone;
    const char* session_record_path;
    uint32_t use_jit;
    uint32_t android_host_features;
    uint32_t android_camera_mode;
    const char* android_camera_calibration_path;
    const char* android_serial_number;
    const char* android_ethernet_mac;
    uint32_t android_aot_disabled;
    uint32_t android_root;
} kernarium_runtime_boot;

/* What a launch's host GPU budget comes to, as the runtime evaluates it for
 * the boot request it receives. */
typedef struct kernarium_runtime_gpu_budget {
    uint32_t struct_size;
    /* Nonzero when L - A - G - E - S is below the budget's 352 MiB floor: the
     * process is likely to be ended, the guest boots anyway with the budget
     * clamped to the floor, and the application is to say so. */
    uint32_t short_of_floor;
    /* B. */
    uint64_t budget_bytes;
    /* L, zero when the host sets no limit. */
    uint64_t process_limit_bytes;
    /* L - A - G - E - S, zero when the sum passes L. */
    uint64_t available_bytes;
    uint64_t guest_memory_bytes;
    /* E: the gadget cache the run will set plus E_rest. */
    uint64_t engine_bytes;
    uint64_t application_bytes;
    uint64_t headroom_bytes;
} kernarium_runtime_gpu_budget;

/* Boots the guest and returns when it is over. A guest that restarts itself is
 * booted again inside this call, on the same view, disk and network, so the
 * call returns only when the guest powers off, the application stops it, a run
 * fails, or the restart-loop guard ends the loop. It returns the guest's own
 * status (0 for a power-off or a stop), a negated uml_emulator error constant
 * for a run or start-up that failed, KERNARIUM_RUNTIME_RESULT_MEMORY_REFUSED
 * or KERNARIUM_RUNTIME_RESULT_RESTART_LOOP.
 * For Android, 0 means the guest powered off cleanly and all its resources have
 * been released: another run may start in this process. A host stop or an
 * unreported shutdown returns an error. */
typedef int (*kernarium_runtime_run_function)(
    KernariumMetalView view,
    const kernarium_runtime_boot* boot);

enum {
    /* The host could not give the guest what its size needs, the address space
     * for its kernel window and heap or the memory limit for its RAM, and
     * nothing was started: the run ended before its kernel began. The size can
     * be lowered and the run tried again. The runtime returns it for a run
     * that the library marked, on its handle, as refused for that reason
     * (emulator/src/start_refusal.hpp), and for no other failure: the
     * library's -UML_EMULATOR_OUT_OF_MEMORY also comes from a failed
     * allocation of its own, and is returned as it is. A guest's own exit
     * status is returned as it is too, so a guest whose user writes exactly
     * this value as its status (root, through /proc/exitcode) returns it
     * without any refusal. It is outside the range of the negated
     * uml_emulator errors (-1 through -4). */
    KERNARIUM_RUNTIME_RESULT_MEMORY_REFUSED = -101,
    /* The guest restarted itself five times in a row within 15 seconds of its
     * boot, or, for Android, eight times in a row each within 15 seconds or
     * before its boot had finished (init processed sys.boot_completed=1,
     * started recovery or apexd marked the boot completed, on a console that
     * showed init's output), so the runtime stopped booting it. A restart
     * after a finished boot that lasted 15 seconds or more never counts. It is
     * outside the range of the negated uml_emulator errors (-1 through -4). */
    KERNARIUM_RUNTIME_RESULT_RESTART_LOOP = -100,
};

/* Evaluates the host GPU's budget for a guest of `guest_memory_mib` and
 * `guest_processor_count` vCPUs with the launch parameters of
 * kernarium_runtime_boot, as a run with that request will (the application's
 * memory limit and the gadget cache, whose cap follows the vCPU count,
 * included), and fills `plan`, whose struct_size the caller sets. The
 * application calls it before run to warn the user when `short_of_floor` is
 * set. Returns 0, or a negative value for a null or short `plan`, a guest
 * memory of zero or a vCPU count of zero. */
typedef int (*kernarium_runtime_plan_gpu_budget_function)(
    uint32_t guest_memory_mib,
    uint32_t guest_processor_count,
    uint32_t gpu_budget_application_mib,
    uint32_t gpu_budget_engine_mib,
    uint32_t gpu_budget_headroom_mib,
    kernarium_runtime_gpu_budget* plan);

/* What prepare_jit leaves, as its return value. */
enum {
    /* The JIT's executable memory is prepared for this guest memory and vCPU
     * count, and the debugger, if one made it, has detached. A boot that sets
     * use_jit runs on the JIT. */
    KERNARIUM_RUNTIME_JIT_READY = 0,
    /* This runtime has no JIT: a build for the Simulator or a host that cannot
     * make executable memory. */
    KERNARIUM_RUNTIME_JIT_UNSUPPORTED = 1,
    /* No debugger is attached to the process, or the one that was left
     * before it served the request, so none prepared the memory. A boot
     * runs on gadgets. */
    KERNARIUM_RUNTIME_JIT_NO_DEBUGGER = 2,
    /* The memory could not be prepared: the debugger did not answer, or the
     * pages did not run code. A boot runs on gadgets, and the process does
     * not try again. */
    KERNARIUM_RUNTIME_JIT_FAILED = 3,
    /* An earlier start of this process prepared the memory for another guest
     * memory or vCPU count, and the debugger has left. A boot runs on
     * gadgets until the application is opened from the debugger again. */
    KERNARIUM_RUNTIME_JIT_OTHER_SETTINGS = 4,
    /* `enable` was zero: the runtime asked an attached debugger to detach
     * and prepared nothing. */
    KERNARIUM_RUNTIME_JIT_DISABLED = 5,
};
/* Prepares the JIT for a guest of `guest_memory_mib` and `guest_processor_count`
 * vCPUs when `enable` is nonzero (runtime API 30), and returns a
 * KERNARIUM_RUNTIME_JIT_ value. On an iOS device the JIT's executable memory
 * comes from a region a debugger the user attached to the process blesses once
 * per process; the debugger then detaches. A zero `enable` only tells an
 * attached debugger to detach, so a process that will not use the JIT does
 * not stay under it. Call it before run, with the memory and vCPUs the run
 * will state, and away from the main thread: it blocks for the seconds the
 * debugger takes. It may be called long before the run, such as when the
 * application opens: the prepared memory then stays wired until the process
 * ends, and a run that does not use it carries it beside its own cache. */
typedef int (*kernarium_runtime_prepare_jit_function)(
    int enable,
    uint32_t guest_memory_mib,
    uint32_t guest_processor_count);

typedef void (*kernarium_runtime_request_stop_function)(void);

/* What the guest confirmed about the writeback of its own storage when sync,
 * quiesce or quiesce_resident returned, as uml_emulator_guest_storage states
 * it. Only CONFIRMED and NO_GUEST mean that nothing the host cannot see was
 * left dirty in the guest when the call returned: a caller says the guest's
 * storage is settled only for those two. The zero value claims nothing. */
enum {
    /* Nothing was reported: the call did not write the report, as when it
     * returned an error, so a report the caller zeroed still holds this. */
    KERNARIUM_RUNTIME_GUEST_STORAGE_NOT_REPORTED = 0,
    /* The guest ran sync(2) to its return and said so, in this call: it
     * answered the sync request, or its suspend reply stated that the
     * suspend's own sync had returned. A suspend reply that says nothing of
     * it, as an older kernel's, confirms nothing: the guest syncs on the way to
     * suspend only while /sys/power/sync_on_suspend is set. */
    KERNARIUM_RUNTIME_GUEST_STORAGE_CONFIRMED = 1,
    /* No guest was running, so there was nothing to settle. */
    KERNARIUM_RUNTIME_GUEST_STORAGE_NO_GUEST = 2,
    /* The guest was asked and did not confirm in time, or the host could not
     * ask because another lifecycle call or console request was busy. */
    KERNARIUM_RUNTIME_GUEST_STORAGE_UNCONFIRMED = 3,
    /* The guest refused the request, or its reply was no confirmation. */
    KERNARIUM_RUNTIME_GUEST_STORAGE_REFUSED = 4,
    /* A guest was running and its management console could not be reached. */
    KERNARIUM_RUNTIME_GUEST_STORAGE_UNREACHABLE = 5,
};
/* Whether the call put the guest to sleep. */
enum {
    /* The call does not suspend (sync), or nothing was asked. */
    KERNARIUM_RUNTIME_GUEST_SUSPENSION_NOT_ASKED = 0,
    /* The guest replied that it entered system suspend. */
    KERNARIUM_RUNTIME_GUEST_SUSPENSION_SUSPENDED = 1,
    /* The guest refused, or did not confirm that it slept: it may run. */
    KERNARIUM_RUNTIME_GUEST_SUSPENSION_NOT_SUSPENDED = 2,
};
/* What sync, quiesce and quiesce_resident report about the guest (runtime API
 * 43). The call writes it only when it returns 0 or more. */
typedef struct kernarium_runtime_settle {
    /* Set by the caller to sizeof the structure. */
    uint32_t struct_size;
    uint32_t reserved;
    /* A KERNARIUM_RUNTIME_GUEST_STORAGE_ value. */
    uint32_t guest_storage;
    /* A KERNARIUM_RUNTIME_GUEST_SUSPENSION_ value. */
    uint32_t guest_suspension;
} kernarium_runtime_settle;

/* Has the guest write its filesystems back, then flushes the guest's writable
 * host files. Safe to call from any thread while a guest runs, and reports the
 * number of files that could not be flushed: it says nothing of the guest.
 * `settle` (nullable; the caller sets struct_size) reports what the guest
 * confirmed. The call waits for the guest at most 3.5 s, and for a lifecycle
 * call in progress at most 1 s of that, whoever else is in the runtime; the
 * host files are flushed all the same, and the storage is UNCONFIRMED when the
 * guest did not reply in time. It fills `settle` with NO_GUEST when no guest
 * runs. Returns -EINVAL for a struct_size smaller than the structure, with
 * `settle` untouched, as for the calls below. */
typedef int (*kernarium_runtime_sync_function)(kernarium_runtime_settle* settle);
/* Leaves the guest safely abandonable: has it write its filesystems back,
 * trims its cold page cache once, enters full system suspend, then flushes
 * the host's own files. Pair it with resume. `settle` (nullable) reports
 * what the guest confirmed, as for sync, and whether it sleeps. The storage
 * is confirmed by the guest's reply to a writeback request of this call, or by
 * a suspend reply that states the suspend's own sync returned. A quiesce
 * waits for a lifecycle call in progress, which is itself bounded; each wait
 * for the guest is bounded: the first writeback 2 s,
 * the trim 3 s, the suspend 10 s (4 s more to abort one that does not
 * answer) and a final writeback after a suspend that failed 1.5 s. Returns
 * -EINVAL for a struct_size smaller than the structure, with `settle`
 * untouched. */
typedef int (*kernarium_runtime_quiesce_function)(kernarium_runtime_settle* settle);
/* quiesce for a host that must not write what it is short of: the same
 * order, result, report and struct_size rule, with the guest's RAM kept in the
 * process and no hibernation image written where a quiesce would have written
 * one. */
typedef int (*kernarium_runtime_quiesce_resident_function)(kernarium_runtime_settle* settle);
typedef void (*kernarium_runtime_resume_function)(void);
typedef void (*kernarium_runtime_send_touch_function)(
    uint32_t slot,
    uint32_t x,
    uint32_t y,
    int pressed);
/* Withdraws the contact in `slot`, as the comment above the touch constants
 * describes: a cancel in place of the release send_touch would give it. */
typedef void (*kernarium_runtime_cancel_touch_function)(uint32_t slot);
typedef void (*kernarium_runtime_send_key_function)(
    uint16_t code,
    int pressed);
typedef void (*kernarium_runtime_console_output_callback)(
    void* context,
    const void* buffer,
    size_t length);
typedef void (*kernarium_runtime_set_console_output_callback_function)(
    kernarium_runtime_console_output_callback callback,
    void* context);
/* Writes to the guest's console. */
typedef int (*kernarium_runtime_send_console_input_function)(
    const void* buffer,
    size_t length);
/* Holds (nonzero) or releases (zero) every GPU request the guest makes, so
 * none reaches the host GPU while it is held. A request that arrives waits in
 * its virtqueue, through any suspend of the guest, and runs in order after the
 * release. Work already in the renderer carries on. The application holds
 * before it enters the background, where the host GPU is revoked within
 * seconds, and releases when it returns. A hold of 1 on an Android guest first
 * ends the guest's running camera video and waits, two seconds at most, until
 * the guest has stopped it, so a recording that runs ends before the requests
 * stop; with no video running it waits for nothing. A hold of 3 or more is a
 * hold of 1 from a caller that has a deadline: the value is the milliseconds
 * the deadline leaves, and the wait takes what is left of them after the one
 * second the hold may spend on a request that is running, two seconds at most;
 * the deadline does not shorten that second.
 * A hold of 2 takes the hold at once, with no such wait (the application's own
 * bound, when the edge is already late), and ends a wait of a hold that is
 * pending. A hold asked for keeps a guest that restarts from booting until the
 * requests are given back, from the moment of the call. Returns 1 when the
 * setting is in effect and no request was still running after one second, 0
 * when one was (the hold is in effect regardless; a hold that finds the gate
 * held already answers what the hold that closed it found), and -1 when no
 * guest is running: the setting is kept and applies to the guest's GPU once it starts.
 * Callers serialize their holds and releases: a release that overtakes a hold
 * still waiting for the camera is undone by that hold's close. */
typedef int (*kernarium_runtime_set_gpu_hold_function)(int held);
/* Whether the guest's display is on: 1 while it has its primary scanout, 0
 * while it has none, as an Android screen that is off has not, and -1 when no
 * guest is running or its GPU has not started. A request the guest sent behind
 * a held GPU has not run, so the answer then is the one the guest's last
 * request that ran left; the application asks before it holds. */
typedef int (*kernarium_runtime_guest_display_active_function)(void);
/* Receives one copy of the guest's display as a CVPixelBufferRef: an
 * IOSurface-backed 32BGRA buffer from a pool, turned the way the user turned
 * the display, no wider than the width asked for. The buffer is borrowed for
 * the call; a receiver that keeps it retains it. It runs on a runtime thread
 * and must return promptly. */
typedef void (*kernarium_runtime_frame_sink)(void* context, void* pixel_buffer);
/* Mirrors the guest's display to `sink`, for a view of it that the
 * application shows while its own window cannot, such as Picture in Picture.
 * At most `frames_per_second` frames arrive each second, and a static display
 * arrives again about once a second. Nothing is rendered while no sink is set.
 * A null sink stops the mirror, and returns only once no call to the previous
 * sink is running or will start, so its context may be released; the sink must
 * not call this. Returns 0, or a negative value for a width or rate of zero.
 * The setting applies to the guest's display whenever one exists. */
typedef int (*kernarium_runtime_set_frame_sink_function)(
    kernarium_runtime_frame_sink sink,
    void* context,
    uint32_t maximum_width,
    uint32_t frames_per_second);
/* Moves the guest's vCPU threads to (nonzero) or from (zero) the system's
 * background scheduling policy, which runs them at low priority, prefers the
 * efficiency cores, and throttles their I/O. The application turns it on while
 * the guest is kept running behind a window the user is not looking at, so the
 * guest keeps its clock and its work moving without a foreground's power draw,
 * and off when the application returns. It applies to the vCPU threads that
 * exist when it is called, and leaves the host's other threads alone. Returns
 * the number of threads changed, or a negative value if the threads could not
 * be listed. */
typedef int (*kernarium_runtime_set_background_throttle_function)(int enabled);
/* Turns the guest's periodic page-cache trimming on (nonzero) or off, as
 * uml_emulator_set_page_cache_trimming does, and returns its result. A guest
 * boots with it off, as a guest in front of its user should be; the
 * application turns it on only while the guest is kept running in the
 * background. Quiescing trims once by itself. It may wait for the guest's
 * console, so call it away from the main thread. */
typedef int (*kernarium_runtime_set_page_cache_trimming_function)(int enabled);
/* What set_microphone leaves, as its return value. */
enum {
    /* The Microphone setting is off. The guest records silence. */
    KERNARIUM_RUNTIME_MICROPHONE_OFF = 0,
    /* On, but the system has not been asked for access: the guest records
     * silence until the application has asked, and the application, not the
     * runtime, is the one that asks. */
    KERNARIUM_RUNTIME_MICROPHONE_UNASKED = 1,
    /* On, but the system refuses access. The guest records silence. */
    KERNARIUM_RUNTIME_MICROPHONE_DENIED = 2,
    /* On and permitted: a guest that records hears the microphone. */
    KERNARIUM_RUNTIME_MICROPHONE_ON = 3,
};
/* Turns the guest's microphone on (nonzero) or off (zero) and returns the
 * resulting KERNARIUM_RUNTIME_MICROPHONE_ value. It is off until the
 * application turns it on, and while it is off, or the system has not granted
 * access, a guest that records gets silence and nothing opens the microphone.
 * The runtime never asks the system for access and never shows a permission
 * prompt: the application asks, as the direct result of the user turning the
 * setting on, and then calls this. Calling it again with a nonzero value after
 * the answer, or when the application becomes active, lets a guest that is
 * recording pick the microphone up. It may be called whether or not a guest
 * runs; a guest that starts later follows it. */
typedef int (*kernarium_runtime_set_microphone_function)(int enabled);

/* The host-side figures of the running guest, for an application that shows
 * statistics. Nothing in the runtime measures between calls: a reading is the
 * system's own accounting of the guest's vCPU threads, the display's frame
 * counters and the host's record of the guest RAM it holds, taken when the
 * call is made. */
typedef struct kernarium_runtime_statistics {
    /* Set by the caller to sizeof the structure. */
    uint32_t struct_size;
    /* The guest's vCPUs that have come up. */
    uint32_t processor_count;
    /* How many times this launch's guest has restarted: 0 for the first boot.
     * Two readings with different values span two guests, and the CPU time of
     * the second starts again from zero. */
    uint32_t boot_index;
    /* As guest_display_active: 1 on, 0 off, -1 unknown. */
    int32_t display_active;
    /* When the CPU time was read, in nanoseconds of the steady clock, which
     * does not count the time the device sleeps. */
    uint64_t timestamp_ns;
    /* The user and system time the guest's vCPU threads have used in this
     * boot, in nanoseconds. Two readings of one boot differ by the time the
     * vCPUs spent between them: divided by the elapsed timestamps and
     * processor_count it is the share of the guest's processors that
     * computed. */
    uint64_t processor_time_ns;
    /* Frames that reached the screen, and frames the guest offered, since the
     * display was made. The display outlives a restart, so neither starts
     * again; both are 0 while there is no display. */
    uint64_t presented_frames;
    uint64_t offered_frames;
    /* The guest's RAM, and the part of it the host holds now. Both are 0
     * before the guest's kernel has claimed its RAM. */
    uint64_t memory_bytes;
    uint64_t memory_held_bytes;
    /* How many times the host told the guest that its GPU had no room for a
     * graphics allocation, since the display was made, so a restart does not
     * reset it. Only an answer the guest received counts, after the host has
     * freed what it could and tried again; a Venus (Vulkan) allocation the
     * host refused is not counted. A rise means a guest application may have
     * lost its graphics state; 0 while there is no display. */
    uint64_t gpu_memory_refusals;
    /* The host GPU's device memory budget, B, and what the process holds
     * against it, in bytes. The usage is the count the budget refuses an
     * allocation by: the larger of Metal's allocation less the host memory the
     * drivers only import and the graphics footprint, plus the reservations
     * in flight. It includes memory the host's drivers keep for reuse, so it
     * is above what the guest's own allocations add up to. Both are 0 while
     * there is no budget: no GPU yet, or a runtime that gives its guest none. */
    uint64_t gpu_memory_budget_bytes;
    uint64_t gpu_memory_used_bytes;
    /* The refresh rate the host granted the run's display (runtime API 44):
     * 120 when the run was given the 120 Hz mode, 60 for the 60 Hz display, and
     * 0 while there is no display. It is decided once, before the guest boots
     * (ANDROID_HOST_HIGH_REFRESH), and the guest's own mode follows it only
     * while a display size can state it. The application learns from it
     * whether a run has the 120 Hz display, whatever it asked. */
    uint32_t display_refresh_hz;
    uint32_t reserved;
} kernarium_runtime_statistics;
/* Fills `statistics` (the caller sets struct_size) with the running guest's
 * host-side figures. Returns 0, -ENODEV with no guest running (before its
 * kernel starts, between two boots, after it ends), or -EINVAL for a null or
 * short structure. It reads counters, about 0.1 ms, and never waits for the
 * guest; any thread. */
typedef int (*kernarium_runtime_read_statistics_function)(
    kernarium_runtime_statistics* statistics);

/* How many holders one graphics memory reading carries, and the room for a
 * holder's command name (the guest kernel's, at most 15 characters). */
#define KERNARIUM_RUNTIME_GRAPHICS_HOLDERS 8U
#define KERNARIUM_RUNTIME_GRAPHICS_NAME_BYTES 16U

/* One guest process (or one context the host cannot tell the process of) and
 * the host GPU memory its graphics contexts hold. */
typedef struct kernarium_runtime_graphics_holder {
    /* The guest process, as its kernel numbers it; 0 for a context whose
     * creator the host cannot tell, which a guest kernel without the process
     * naming gives for every context. */
    uint32_t pid;
    /* How many of the process's contexts exist. */
    uint32_t context_count;
    /* The bytes it holds. A buffer two processes share is counted in full for
     * each. */
    uint64_t bytes;
    /* Milliseconds since the process's most recently active context last ran a
     * command. */
    uint64_t idle_ms;
    /* The process's command name, or an unnamed context's debug name,
     * truncated and null-terminated. */
    char name[KERNARIUM_RUNTIME_GRAPHICS_NAME_BYTES];
} kernarium_runtime_graphics_holder;

/* The guest's graphics memory budget and who holds it, for an application
 * that frees graphics memory by ending the guest app that holds it. The
 * caller sets the first three fields; the rest is the answer. */
typedef struct kernarium_runtime_graphics_memory {
    /* Set by the caller to sizeof the structure. */
    uint32_t struct_size;
    /* Only holders quiet for at least this many milliseconds, and holding at
     * least minimum_holder_bytes, are listed. */
    uint32_t minimum_idle_ms;
    uint64_t minimum_holder_bytes;
    /* How many entries of `holders` are filled. */
    uint32_t holder_count;
    /* 1 while the guest's own low-memory killer asks the host for the budget's
     * holders and ends them itself (runtime API 29); an application that ends
     * holders on its own stands down meanwhile. 0 otherwise. */
    uint32_t guest_reclaims;
    /* The device memory budget, and what the process holds against it. The
     * host's drivers keep memory they could reuse, so usage_bytes is not what
     * the guest holds. 0 while no renderer has a budget. */
    uint64_t budget_bytes;
    uint64_t usage_bytes;
    /* What the guest's contexts hold of it, named or not, every buffer once:
     * the part of usage_bytes the host can credit to a guest context. */
    uint64_t context_bytes;
    /* As kernarium_runtime_statistics.gpu_memory_refusals, from the same
     * instant. */
    uint64_t memory_refusals;
    /* Largest first. */
    kernarium_runtime_graphics_holder holders[KERNARIUM_RUNTIME_GRAPHICS_HOLDERS];
} kernarium_runtime_graphics_memory;
/* Fills `memory` (the caller sets struct_size and the two filters) with the
 * running guest's graphics memory. Returns 0, -ENODEV with no guest running
 * or before its GPU starts, or -EINVAL for a null or short structure. It reads
 * counters, about 0.1 ms, and never waits for the guest; any thread. */
typedef int (*kernarium_runtime_read_graphics_memory_function)(
    kernarium_runtime_graphics_memory* memory);

typedef struct kernarium_runtime_guest_memory {
    /* Set by the caller to sizeof the structure. */
    uint32_t struct_size;
    uint32_t reserved;
    /* The guest kernel's own total and available memory (MemTotal and
     * MemAvailable), in bytes. Its memory in use is total_bytes minus
     * available_bytes. */
    uint64_t total_bytes;
    uint64_t available_bytes;
} kernarium_runtime_guest_memory;
/* Asks the guest kernel for its own memory figures through its management
 * console. Returns 0; -ENODEV with no guest running; -EAGAIN, with nothing
 * sent, when asking could wake a guest that sleeps (the application quiesced
 * it, a lifecycle call such as quiesce or sync is in progress, or its display
 * is off, as an Android guest's is before it suspends itself); -ENOTSUP for a
 * kernel without the request; -ETIMEDOUT with no answer in 500 ms; or -EINVAL
 * for a null or short structure. It leaves `memory` alone unless it returns 0.
 * It blocks up to 500 ms, and a quiesce, resume or sync called meanwhile waits
 * for it: call it away from the main thread. */
typedef int (*kernarium_runtime_read_guest_memory_function)(
    kernarium_runtime_guest_memory* memory);

/* What a restart event reports. */
enum {
    /* The guest asked to restart and the old guest is gone: its devices are
     * stopped and the runtime is preparing the next boot, which waits while
     * the application holds the guest's GPU requests or has quiesced the guest
     * (the new guest's GPU probe would otherwise time out and its boot would
     * run without 3D). */
    KERNARIUM_RUNTIME_RESTART_BEGAN = 0,
    /* The restarted guest's management console answers, so what belongs to a
     * kernel, the vCPU throttle and the page-cache trimming, can be applied to
     * the new one: it boots with neither. If the console has not answered
     * within two minutes the event is sent anyway. It is not sent when the
     * guest ends first. */
    KERNARIUM_RUNTIME_RESTART_BOOTED = 1,
};
/* `restart_count` is the number of times this launch's guest has restarted,
 * 1 for the first restart. An event follows BEGAN with the same count, or the
 * guest ends. It runs on a runtime thread and must return promptly. */
typedef void (*kernarium_runtime_restart_callback)(
    void* context,
    uint32_t event,
    uint32_t restart_count);
/* Sets, or with a null callback clears, the one receiver of restart events.
 * Once it returns, no call to the previous callback is running or will start,
 * so its context may be released; the callback must not call this. */
typedef void (*kernarium_runtime_set_restart_callback_function)(
    kernarium_runtime_restart_callback callback,
    void* context);

/* Opens a stream to the guest's vsock port `port` (an Android guest's adbd
 * listens on 5555) and returns a connected SOCK_STREAM descriptor that the
 * caller owns and closes, or a negated errno: -ENODEV when no guest runs,
 * -ECONNREFUSED when nothing listens on the port (yet), -ETIMEDOUT when the
 * guest did not answer within timeout_ms (a guest without the vsock driver, or
 * one the host quiesced), -EMFILE when 32 connections are open, -ECONNRESET
 * when the guest stops or restarts first, and -EINVAL for a port that cannot
 * be named. A guest that suspended itself is woken first, as network traffic
 * wakes it. The stream ends when the guest closes it, stops or restarts, and
 * no other process or guest application can reach it. Wait for the descriptor
 * with poll or kqueue and read what they report: the guest's half-close
 * arrives as shutdown(SHUT_WR) on the peer end, which a blocking read can miss
 * (docs/architecture.md, "Local stream sockets"). It blocks: call it away from
 * the main thread. */
typedef int (*kernarium_runtime_connect_vsock_function)(
    uint32_t port,
    uint32_t timeout_ms);

/* The host's location, offered to an Android guest as a GNSS receiver
 * (darwin/device/gnss/README.md). A flag says which optional field is valid;
 * a field without its flag is left out of the guest's stream, never written
 * as zero. */
enum {
    /* The host knows the position only roughly (iOS's reduced accuracy). */
    KERNARIUM_RUNTIME_LOCATION_APPROXIMATE = 1U << 0,
    KERNARIUM_RUNTIME_LOCATION_HAS_ALTITUDE = 1U << 1,
    KERNARIUM_RUNTIME_LOCATION_HAS_ELLIPSOIDAL_ALTITUDE = 1U << 2,
    KERNARIUM_RUNTIME_LOCATION_HAS_HORIZONTAL_ACCURACY = 1U << 3,
    KERNARIUM_RUNTIME_LOCATION_HAS_VERTICAL_ACCURACY = 1U << 4,
    KERNARIUM_RUNTIME_LOCATION_HAS_SPEED = 1U << 5,
    KERNARIUM_RUNTIME_LOCATION_HAS_SPEED_ACCURACY = 1U << 6,
    KERNARIUM_RUNTIME_LOCATION_HAS_COURSE = 1U << 7,
    KERNARIUM_RUNTIME_LOCATION_HAS_COURSE_ACCURACY = 1U << 8,
};
typedef struct kernarium_runtime_location_fix {
    /* sizeof of this struct, set by the caller. */
    uint32_t struct_size;
    uint32_t flags;
    /* The fix's own time: UTC in nanoseconds since 1970-01-01. */
    int64_t utc_ns;
    /* WGS 84, in degrees. */
    double latitude;
    double longitude;
    /* Meters above sea level, and above the WGS 84 ellipsoid. */
    double altitude;
    double ellipsoidal_altitude;
    /* Meters of radius for the horizontal accuracy, and of the vertical one. */
    double horizontal_accuracy;
    double vertical_accuracy;
    /* Meters per second, and its accuracy. */
    double speed;
    double speed_accuracy;
    /* Degrees clockwise from true north, and its accuracy. */
    double course;
    double course_accuracy;
} kernarium_runtime_location_fix;

/* `powered` is nonzero when the guest powers its GNSS receiver (an Android
 * client wants a position) and zero when it stops, or when the guest ends or
 * restarts with the receiver on. It runs on a runtime thread and must return
 * promptly. */
typedef void (*kernarium_runtime_location_receiver_callback)(
    void* context,
    int powered);
/* Sets, or with a null callback clears, the one receiver of the guest's
 * receiver-power events. A receiver that is already powered is reported to a
 * new callback at once. Once it returns, no call to the previous callback is
 * running or will start, so its context may be released; the callback must
 * not call this. A runtime without a GNSS receiver keeps the entry and never
 * calls back. */
typedef void (*kernarium_runtime_set_location_receiver_callback_function)(
    kernarium_runtime_location_receiver_callback callback,
    void* context);

/* Gives the guest's receiver the host's newest fix, which the receiver
 * repeats for ten seconds (an approximate fix for twenty minutes) and then
 * reports as no fix, and reports to the guest at once. A null `fix` drops the
 * held fix: the guest sees no fix until the next one. Returns 0, -ENODEV when
 * no guest with a receiver runs, or -EINVAL for a `struct_size` that does not
 * cover the struct, a time or a position outside its range. It may be called
 * from any thread with the receiver on or off. */
typedef int (*kernarium_runtime_report_location_function)(
    const kernarium_runtime_location_fix* fix);

/* The hardware keyboard, mouse and pointer of an Android guest
 * (darwin/device/input/README.md). The guest has the three devices from boot,
 * unplugged: it sees no keyboard, mouse or cursor until the application plugs
 * them, and unplugging takes them away again with every key and button
 * released. What the application last asked of each device holds for the run:
 * a guest that restarts itself has the devices plugged as the application left
 * them, and a plug asked for between two boots is kept for the next. A plug
 * that is asked for before the guest's driver has probed takes effect when it
 * has. The guest registers each device once its driver has set it up, and the
 * runtime says so with KERNARIUM_RUNTIME_INPUT_STATUS_KEYBOARD_REGISTERED and
 * KERNARIUM_RUNTIME_INPUT_STATUS_POINTER_REGISTERED (the pointer is registered
 * while the mouse and the absolute pointer both are), and says they are not
 * when the guest restarts and when the run ends. An Android guest whose kernel
 * lacks the hot-plug topic never registers them: a plug changes nothing of
 * what it sees, and input sent to them is dropped. An application that takes
 * something from the guest for a device, such as the system pointer's clicks
 * for the mouse, does so only while that device is registered. The Linux
 * runtime keeps the entries and answers -ENODEV, as does a runtime with no
 * guest running.
 *
 * Input for a device that is not plugged, or a key the keyboard has no code
 * for, is dropped and returns 0, and input sent between two boots returns
 * -ENODEV; every call may come from any thread. */
enum {
    /* The keyboard. */
    KERNARIUM_RUNTIME_INPUT_KEYBOARD = 1,
    /* The mouse, which is the relative mouse and the absolute pointer together:
     * Android sees one mouse, with its own cursor. */
    KERNARIUM_RUNTIME_INPUT_MOUSE = 2,
};

/* The bits of the buttons of send_pointer and send_mouse, in the order of
 * UIKit's button numbers. Back and Forward are Android's navigation buttons. */
enum {
    KERNARIUM_RUNTIME_BUTTON_PRIMARY = 1U << 0,
    KERNARIUM_RUNTIME_BUTTON_SECONDARY = 1U << 1,
    KERNARIUM_RUNTIME_BUTTON_MIDDLE = 1U << 2,
    KERNARIUM_RUNTIME_BUTTON_BACK = 1U << 3,
    KERNARIUM_RUNTIME_BUTTON_FORWARD = 1U << 4,
};

/* One wheel detent, in the high-resolution units of send_mouse's wheels. */
enum {
    KERNARIUM_RUNTIME_WHEEL_DETENT = 120,
};

/* Plugs (`connected` nonzero) or unplugs one of the KERNARIUM_RUNTIME_INPUT_
 * devices; plugging a plugged device or unplugging an unplugged one changes
 * nothing. Returns 0, -EINVAL for another device, or -ENODEV when no guest
 * run is in progress. */
typedef int (*kernarium_runtime_set_input_connected_function)(
    int device,
    int connected);

/* A key of the keyboard, by its position: the usage on the HID Keyboard/Keypad
 * page (0x04 is A, 0xe3 the left Command key), which the runtime turns into
 * the Linux key code a USB keyboard with that usage would send. Android's own
 * key layout turns it into a character. The keyboard has no auto-repeat, as
 * Android repeats keys itself. A key going down wakes a guest that suspended
 * itself, at most once a second. Returns 0 or -ENODEV. */
typedef int (*kernarium_runtime_send_keyboard_usage_function)(
    uint32_t usage,
    int pressed);

/* The pointer at (x, y), in the touchscreen's range 0 through
 * KERNARIUM_RUNTIME_TOUCH_MAXIMUM on both axes (see send_touch) over the guest's
 * display, with the buttons that are down, which are the
 * KERNARIUM_RUNTIME_BUTTON_ bits (any other bit is ignored). `in_range` zero
 * releases the buttons and ends the pointer's contact, and Android's cursor
 * stays where it was. A position in range before the guest has a display is
 * dropped. A button going down wakes a guest that suspended itself; a pointer
 * that only moves does not. Returns 0, -EINVAL for a coordinate out of range,
 * or -ENODEV. */
typedef int (*kernarium_runtime_send_pointer_function)(
    uint32_t x,
    uint32_t y,
    uint32_t buttons,
    int in_range);

/* The mouse's relative motion in guest pixels (what an application that
 * captured the pointer reads), the buttons that are down (the
 * KERNARIUM_RUNTIME_BUTTON_ bits, any other ignored), and the vertical and
 * horizontal wheels in 1/120 of a detent, positive up and to the right. A
 * button going down or a wheel taking a whole step wakes a guest that
 * suspended itself; motion does not. Returns 0 or -ENODEV. */
typedef int (*kernarium_runtime_send_mouse_function)(
    int32_t dx,
    int32_t dy,
    uint32_t buttons,
    int32_t wheel,
    int32_t horizontal_wheel);

/* What the guest tells the host of the keyboard and the mouse: the lock lights
 * of its keyboard, an application capturing or releasing the pointer, and
 * whether it has registered the devices. */
enum {
    KERNARIUM_RUNTIME_INPUT_STATUS_NUM_LOCK = 1,
    KERNARIUM_RUNTIME_INPUT_STATUS_CAPS_LOCK = 2,
    KERNARIUM_RUNTIME_INPUT_STATUS_SCROLL_LOCK = 3,
    KERNARIUM_RUNTIME_INPUT_STATUS_POINTER_CAPTURE = 4,
    /* The guest registered the keyboard (on) or no longer has it (off). Off
     * also when the guest restarts and when the run ends. */
    KERNARIUM_RUNTIME_INPUT_STATUS_KEYBOARD_REGISTERED = 5,
    /* The guest registered the mouse and the absolute pointer, both (on), or
     * lost either (off). Off also when the guest restarts and when the run
     * ends. */
    KERNARIUM_RUNTIME_INPUT_STATUS_POINTER_REGISTERED = 6,
};

/* `on` is nonzero when the light is lit, the pointer is captured or the device
 * is registered. It runs on a runtime thread and must return promptly. */
typedef void (*kernarium_runtime_input_status_callback)(
    void* context,
    int status,
    int on);
/* Sets, or with a null callback clears, the one receiver of the input status
 * events. Once it returns, no call to the previous callback is running or will
 * start, so its context may be released; the callback must not call this. A
 * runtime without the devices keeps the entry and never calls back. */
typedef void (*kernarium_runtime_set_input_status_callback_function)(
    kernarium_runtime_input_status_callback callback,
    void* context);

/* A button of a game controller went down: wakes a guest that suspended
 * itself, as a key down does through send_key, and does nothing to one that is
 * running. It shares the keyboard's limit of one wake a second, never wakes
 * for an axis or a release, and may be called from any thread. A runtime with
 * no guest ignores it. */
typedef void (*kernarium_runtime_wake_for_input_function)(void);

/* Which denial of the network the application's notice is up for (runtime
 * API 36). The application watches the system's network path; when iOS denies
 * Andarium cellular or Wi-Fi access it shows a notice, and tells the runtime,
 * which keeps it in the session record (how many denials began during the
 * session and the one in force) beside the counts of the guest's user network.
 * The notice stays reported after the person dismisses it. It is a note for
 * bug reports: nothing in the guest's network follows it. */
enum {
    KERNARIUM_RUNTIME_NETWORK_NOTICE_NONE = 0,
    KERNARIUM_RUNTIME_NETWORK_NOTICE_CELLULAR_DENIED = 1,
    KERNARIUM_RUNTIME_NETWORK_NOTICE_WIFI_DENIED = 2,
};

/* Sets the denial above; the same value again changes nothing. It may be called
 * at any time, before a guest runs and between two boots included: the next
 * session's record starts with the denial in force. Returns 0, or -EINVAL for
 * another value. Any thread. */
typedef int (*kernarium_runtime_set_network_notice_function)(int notice);

/* What the host is doing for the cameras of the running guest, as the
 * callback reports it (runtime API 37; BLOCKED_IN_USE is API 39). */
enum {
    /* No guest app has a camera open: no capture session exists and the
     * system's camera indicator is off. Also the state of a guest that
     * ended. */
    KERNARIUM_RUNTIME_CAMERA_STATE_IDLE = 0,
    /* A guest app has a camera open and frames flow. */
    KERNARIUM_RUNTIME_CAMERA_STATE_STREAMING = 1,
    /* The capture cannot run for a reason that ends by itself (another app
     * or a call has the camera, the system has not granted access); the guest
     * gets black frames meanwhile. */
    KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED = 2,
    /* The same, because other apps share the screen (iPad Split View, Slide
     * Over or a window the system does not let capture). */
    KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED_BY_MULTITASKING = 3,
    /* The same, because the system is under pressure (heat). */
    KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED_BY_SYSTEM_PRESSURE = 4,
    /* The guest runs blocked (android_camera_mode) and a guest app has a
     * camera open: no capture session exists and the system's camera
     * indicator stays off, the guest gets black frames, and the application
     * can say the camera is off. Reported when the app opens a camera and
     * again at each later open, IDLE in between; a live guest never reports
     * it. A runtime before API 39 reports a blocked guest as IDLE. */
    KERNARIUM_RUNTIME_CAMERA_STATE_BLOCKED_IN_USE = 5,
};

/* `state` is a KERNARIUM_RUNTIME_CAMERA_STATE_ value. It runs on a runtime
 * thread and must return promptly. */
typedef void (*kernarium_runtime_camera_state_callback)(
    void* context,
    int state);
/* Sets, or with a null callback clears, the one receiver of the camera state
 * (runtime API 37). The state in force is reported to a new callback at once.
 * A state is reported when it changes, and IDLE when the guest ends or
 * restarts with a camera open. Once it returns, no call to the previous
 * callback is running or will start, so its context may be released; the
 * callback must not call this. A runtime without a camera keeps the entry and
 * never calls back. */
typedef void (*kernarium_runtime_set_camera_state_callback_function)(
    kernarium_runtime_camera_state_callback callback,
    void* context);

/* Tells the cameras how the device body is turned (runtime API 37): the
 * clockwise degrees from portrait, 0, 90, 180 or 270, as the device's user
 * sees it. It matters when the host owns the rotation (an iPad window): the
 * guest's display stays upright at rotation 0 while the device, which carries
 * every built-in sensor, turns under it, so the sensors' mounting turns with
 * it. Each boot reads it once and states every camera's SENSOR_ORIENTATION
 * from it (a back camera's turned the same way, a front camera's the other
 * way, an external camera's not at all); a later change turns the picture by
 * the difference, as ROTATE_AND_CROP's automatic setting does. A guest whose
 * display the guest rotates itself (an iPhone) keeps 0. Zero until the
 * application says otherwise. Returns 0, or -EINVAL for another
 * value. Any thread, with or without a guest. */
typedef int (*kernarium_runtime_set_camera_rotation_function)(
    uint32_t degrees);

/* How a camera reserve was reached (runtime API 40). */
enum {
    /* The reserve is the layout's own: the runtime discovered the host's
     * cameras (which asks the system for no permission and opens no capture
     * session), read the colour data the calibration directory holds, composed
     * the layout it would run and took the memory that layout lets a live
     * session hold. A host with no camera the image can describe holds none:
     * the reserve is 0, and exact. */
    KERNARIUM_RUNTIME_CAMERA_RESERVE_EXACT = 0,
    /* The cameras could not be discovered: the reserve is the tier's ceiling,
     * which bounds the layouts of the system cameras at the budget's tier (a
     * development synthetic source can hold more). */
    KERNARIUM_RUNTIME_CAMERA_RESERVE_CEILING = 1,
};

/* What the caller of kernarium_runtime_plan_camera_reserve_function asks. */
enum {
    /* Discover the host's cameras again before answering, and compose every
     * answer again: the runtime keeps the discovered source and the answers
     * composed from it (calibration files included) until a call asks for
     * this, so an application that asks for each of many budgets sets this on
     * the first and not on the rest. */
    KERNARIUM_RUNTIME_CAMERA_PLAN_DISCOVER_AGAIN = 1,
};

/* What kernarium_runtime_plan_camera_reserve_function takes and answers. The
 * caller sets struct_size and flags. */
typedef struct kernarium_runtime_camera_reserve {
    uint32_t struct_size;
    /* KERNARIUM_RUNTIME_CAMERA_PLAN_ flags. */
    uint32_t flags;
    /* The host memory, in whole MiB, that a live camera session holds out of
     * the GPU budget. */
    uint32_t reserve_mib;
    /* A KERNARIUM_RUNTIME_CAMERA_RESERVE_ value. */
    uint32_t basis;
} kernarium_runtime_camera_reserve;

/* The host memory a live camera session of this host would hold out of the GPU
 * budget of `gpu_budget_mib` MiB (runtime API 40): the figure the next boot with
 * android_camera_mode LIVE and the same calibration directory takes out of the
 * pool it gives the GPU, while the camera source, the cameras it presents and
 * the calibration data are as they were when the answer was composed. The
 * application asks before the boot, to show the graphics memory the guest would
 * have while Camera is on. The runtime composes the layout the boot would run
 * (the cameras it discovers and the colour data in `camera_calibration_path`,
 * null for none), at the tier the budget gets, and answers what that layout lets
 * a session hold. It keeps the discovered source and the answers composed from
 * it: the first call discovers, and `flags` DISCOVER_AGAIN discovers again and
 * composes every answer again (a boot discovers for its own run). Nothing is
 * opened and no permission is asked. A host without a camera, and a runtime
 * without cameras (a Linux guest), answer 0. Returns 0, or a negative value for
 * a null or short `reserve`; any thread, with or without a guest. */
typedef int (*kernarium_runtime_plan_camera_reserve_function)(
    uint32_t gpu_budget_mib,
    const char* camera_calibration_path,
    kernarium_runtime_camera_reserve* reserve);

/* The level the system's memory pressure source reported to the host (runtime
 * API 41). At a critical level the host turns on the guest's page-cache
 * trimmer and frees the host's graphics caches; a warning and the normal level
 * turn that trimmer off again and do nothing else (docs/architecture.md,
 * "Memory pressure"). */
enum {
    KERNARIUM_RUNTIME_MEMORY_PRESSURE_NORMAL = 0,
    KERNARIUM_RUNTIME_MEMORY_PRESSURE_WARNING = 1,
    KERNARIUM_RUNTIME_MEMORY_PRESSURE_CRITICAL = 2,
};

/* What the host does about that level. */
enum {
    /* A warning or the normal level: no response starts (the guest's trimmer
     * that a critical level turned on is turned off). */
    KERNARIUM_RUNTIME_MEMORY_RESPONSE_NONE = 0,
    /* A critical level: the host starts a response now, which turns on the
     * guest's gradual page-cache trimmer (one short message) and frees the
     * host's graphics caches. */
    KERNARIUM_RUNTIME_MEMORY_RESPONSE_STARTED = 1,
    /* Reserved: never reported, as a warning starts no response. */
    KERNARIUM_RUNTIME_MEMORY_RESPONSE_WITHHELD = 2,
    /* A response is already running, and this level starts none. The host
     * handles the notifications one at a time, each after the response of the
     * one before, so this is rare. */
    KERNARIUM_RUNTIME_MEMORY_RESPONSE_UNDER_WAY = 3,
};

/* `level` is a KERNARIUM_RUNTIME_MEMORY_PRESSURE_ value and `response` a
 * KERNARIUM_RUNTIME_MEMORY_RESPONSE_ value. It is called once for each
 * notification the system delivers while a guest runs, the repeats and the
 * return to NORMAL included (also the host's own return to NORMAL after a long
 * quiet, for a system that never delivers it), as soon as the host has decided
 * what it does and
 * before it starts the work for that notification, in the order delivered.
 * The host handles the notifications one at a
 * time: a level raised during a response is reported after the response, and
 * one that the system itself merged with others is reported as the most
 * serious of them. It carries no figures. It runs on a runtime thread and must
 * return promptly. */
typedef void (*kernarium_runtime_memory_pressure_callback)(
    void* context,
    int level,
    int response);
/* Sets, or with a null callback clears, the one receiver of the reports
 * (runtime API 41). It is an event and not a state: a callback set while the
 * pressure is in force hears the next level, and a guest that ends or restarts
 * during the pressure reports no NORMAL, so the application takes the end of
 * the run as the end of it. It may be set before a guest runs. Once it
 * returns, no call to the previous callback is running or will start, so its
 * context may be released; the callback must not call this. */
typedef void (*kernarium_runtime_set_memory_pressure_callback_function)(
    kernarium_runtime_memory_pressure_callback callback,
    void* context);

/* The application resolves only this versioned getter after loading exactly
 * one runtime framework. The table is immutable and remains valid until
 * process exit. The selected framework owns one host runtime and its
 * matching code-signed kernel carrier. */
typedef struct kernarium_runtime_api {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t guest_page_size;
    uint32_t guest_flavor;
    kernarium_runtime_run_function run;
    kernarium_runtime_request_stop_function request_stop;
    kernarium_runtime_sync_function sync;
    kernarium_runtime_quiesce_function quiesce;
    kernarium_runtime_resume_function resume;
    kernarium_runtime_send_touch_function send_touch;
    kernarium_runtime_send_key_function send_key;
    kernarium_runtime_set_console_output_callback_function
        set_console_output_callback;
    kernarium_runtime_send_console_input_function send_console_input;
    kernarium_runtime_set_gpu_hold_function set_gpu_hold;
    kernarium_runtime_set_frame_sink_function set_frame_sink;
    kernarium_runtime_set_background_throttle_function set_background_throttle;
    kernarium_runtime_set_page_cache_trimming_function
        set_page_cache_trimming;
    kernarium_runtime_set_microphone_function set_microphone;
    kernarium_runtime_set_restart_callback_function set_restart_callback;
    kernarium_runtime_guest_display_active_function guest_display_active;
    kernarium_runtime_plan_gpu_budget_function plan_gpu_budget;
    kernarium_runtime_connect_vsock_function connect_vsock;
    kernarium_runtime_read_statistics_function read_statistics;
    kernarium_runtime_read_guest_memory_function read_guest_memory;
    kernarium_runtime_cancel_touch_function cancel_touch;
    kernarium_runtime_quiesce_resident_function quiesce_resident;
    kernarium_runtime_read_graphics_memory_function read_graphics_memory;
    kernarium_runtime_prepare_jit_function prepare_jit;
    kernarium_runtime_set_location_receiver_callback_function
        set_location_receiver_callback;
    kernarium_runtime_report_location_function report_location;
    kernarium_runtime_set_input_connected_function set_input_connected;
    kernarium_runtime_send_keyboard_usage_function send_keyboard_usage;
    kernarium_runtime_send_pointer_function send_pointer;
    kernarium_runtime_send_mouse_function send_mouse;
    kernarium_runtime_set_input_status_callback_function
        set_input_status_callback;
    kernarium_runtime_wake_for_input_function wake_for_input;
    kernarium_runtime_set_network_notice_function set_network_notice;
    kernarium_runtime_set_camera_state_callback_function
        set_camera_state_callback;
    kernarium_runtime_set_camera_rotation_function set_camera_rotation;
    kernarium_runtime_plan_camera_reserve_function plan_camera_reserve;
    kernarium_runtime_set_memory_pressure_callback_function
        set_memory_pressure_callback;
} kernarium_runtime_api;

#if defined(__GNUC__)
#define KERNARIUM_RUNTIME_EXPORT __attribute__((visibility("default")))
#else
#define KERNARIUM_RUNTIME_EXPORT
#endif

KERNARIUM_RUNTIME_EXPORT
const kernarium_runtime_api* kernarium_runtime_get_api_v28(void);

/* An automation-only extension, outside the versioned table: only a runtime
 * built for automation exports this symbol, and the application looks it up
 * without requiring it. It returns nonzero when the launch asked, through the
 * environment, for the background job to start as soon as the guest runs, and
 * zero otherwise. The setting's name is in the automation runtimes alone, so a
 * release runtime cannot be asked. */
KERNARIUM_RUNTIME_EXPORT
int kernarium_runtime_automation_job_at_start(void);

/* Another automation-only extension, found the same way: how many
 * milliseconds after the application enters the background the launch asked
 * for the guest's GPU to be held, instead of at once, so a measurement can
 * find how long the host GPU stays usable there. Zero means no delay, and a
 * runtime built for release has no such symbol. */
KERNARIUM_RUNTIME_EXPORT
int kernarium_runtime_automation_hold_delay_ms(void);

#ifdef __cplusplus
}
#endif

#endif
