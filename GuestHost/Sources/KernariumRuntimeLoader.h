// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KERNARIUM_RUNTIME_LOADER_H
#define KERNARIUM_RUNTIME_LOADER_H

#import <Foundation/Foundation.h>
#import <MetalKit/MetalKit.h>
#include <stddef.h>
#include <stdint.h>

#include "../../Kernarium/Runtime/KernariumRuntime.h"

NS_ASSUME_NONNULL_BEGIN

typedef void (^KernariumConsoleOutputHandler)(NSData* data);
/* A restart event of the guest, delivered on the main queue: `event` is a
 * KERNARIUM_RUNTIME_RESTART_ value and `restartCount` the number of times the
 * guest has restarted. */
typedef void (^KernariumRestartHandler)(
    uint32_t event,
    uint32_t restartCount);
/* The guest powered its GNSS receiver on (`powered` is true) or off, delivered
 * on the main queue. */
typedef void (^KernariumLocationReceiverHandler)(BOOL powered);
/* What the host is doing for the guest's cameras (a
 * KERNARIUM_RUNTIME_CAMERA_STATE_ value), delivered on the main queue in the
 * order the states happened. */
typedef void (^KernariumCameraStateHandler)(int state);
/* A level the system's memory pressure reached the host at (a
 * KERNARIUM_RUNTIME_MEMORY_PRESSURE_ value) and what the host does about it
 * (a KERNARIUM_RUNTIME_MEMORY_RESPONSE_ value), delivered on the main queue in
 * the order the levels arrived. */
typedef void (^KernariumMemoryPressureHandler)(int level, int response);
/* The guest's keyboard lights, an application's pointer capture or the guest's
 * registration of its keyboard or pointer changed (a
 * KERNARIUM_RUNTIME_INPUT_STATUS_ value, and whether it is on), delivered on
 * the main queue. */
typedef void (^KernariumInputStatusHandler)(int status, BOOL on);

#ifdef __cplusplus
extern "C" {
#endif

/* Selects and loads one bundled runtime for the lifetime of the process. The
 * flavour names exactly one embedded framework; every runtime has 4 KiB
 * guest pages. */
int kernarium_runtime_loader_select(uint32_t guest_flavor);

int kernarium_runtime_loader_run(
    MTKView* view,
    const kernarium_runtime_boot* boot);

void kernarium_runtime_loader_request_stop(void);
/* The calls that write the guest's storage back. Each returns the number of
 * host files that could not be flushed, or what the runtime returns: -EINVAL for
 * a `settle` whose struct_size is smaller than the structure, which is then left
 * untouched, in the loader as in the runtime. With no runtime loaded it returns
 * -1 and reports NO_GUEST, the one return below zero that carries a report: no
 * guest runs, so nothing is settled and nothing unsettled. `settle` (nullable)
 * reports what the guest confirmed; see kernarium_runtime_sync_function. */
/* Has the guest write its storage back and flushes the guest's writable host
 * files. */
int kernarium_runtime_loader_sync(kernarium_runtime_settle* _Nullable settle);
/* Leaves the guest safely abandonable. Pair it with
 * kernarium_runtime_loader_resume. */
int kernarium_runtime_loader_quiesce(kernarium_runtime_settle* _Nullable settle);
/* quiesce that keeps the guest's RAM in the process, where a quiesce would
 * hibernate it: for a host short of the space the image takes. Pair it with
 * kernarium_runtime_loader_resume. */
int kernarium_runtime_loader_quiesce_resident(kernarium_runtime_settle* _Nullable settle);
void kernarium_runtime_loader_resume(void);
/* Holds (1, or 2 to hold at once) or releases (zero) the guest's GPU requests; see
 * kernarium_runtime_set_gpu_hold_function. Returns the runtime's answer, or -1
 * when no runtime is loaded. */
int kernarium_runtime_loader_set_gpu_hold(int held);
/* Whether the guest's display is on; see
 * kernarium_runtime_guest_display_active_function. Returns the runtime's
 * answer, or -1 when no runtime is loaded. */
int kernarium_runtime_loader_guest_display_active(void);
/* The running guest's host-side figures; see
 * kernarium_runtime_read_statistics_function. Returns the runtime's answer, or
 * -ENODEV when no runtime is loaded. */
int kernarium_runtime_loader_read_statistics(
    kernarium_runtime_statistics* statistics);
/* The guest's graphics memory budget and who holds it; see
 * kernarium_runtime_read_graphics_memory_function. Returns the runtime's
 * answer, or -ENODEV when no runtime is loaded. */
int kernarium_runtime_loader_read_graphics_memory(
    kernarium_runtime_graphics_memory* memory);
/* The guest kernel's own memory figures; see
 * kernarium_runtime_read_guest_memory_function. Returns the runtime's answer,
 * or -ENODEV when no runtime is loaded. Call it away from the main thread: it
 * may wait up to 500 ms. */
int kernarium_runtime_loader_read_guest_memory(
    kernarium_runtime_guest_memory* memory);
/* Moves the guest's vCPU threads to (nonzero) or from (zero) the system's
 * background policy; see kernarium_runtime_set_background_throttle_function.
 * Returns the number of threads changed, or -1 when no runtime is loaded. */
int kernarium_runtime_loader_set_background_throttle(int enabled);
/* Mirrors the guest's display to `sink`, or stops the mirror for a null sink;
 * see kernarium_runtime_set_frame_sink_function. Returns the runtime's answer,
 * or -1 when no runtime is loaded. */
int kernarium_runtime_loader_set_frame_sink(
    _Nullable kernarium_runtime_frame_sink sink,
    void* _Nullable context,
    uint32_t maximum_width,
    uint32_t frames_per_second);
/* Turns periodic guest page-cache trimming on (nonzero) only while the guest
 * is kept running in the background, and off again on return to the
 * foreground. Returns 0 once the guest has applied it, a negative value
 * otherwise, -1 when no runtime is loaded. Call it away from the main thread:
 * it may wait for the guest's console. */
int kernarium_runtime_loader_set_page_cache_trimming(int enabled);
/* Evaluates the host GPU's budget for a launch with these parameters, as the
 * selected runtime's run will, and fills `plan`; see
 * kernarium_runtime_plan_gpu_budget_function. Returns the runtime's answer, or
 * -1 when no runtime is loaded. */
int kernarium_runtime_loader_plan_gpu_budget(
    uint32_t guest_memory_mib,
    uint32_t guest_processor_count,
    uint32_t application_mib,
    uint32_t engine_mib,
    uint32_t headroom_mib,
    kernarium_runtime_gpu_budget* plan);
/* A, E_rest and S of the host GPU's budget in MiB: the launch parameters
 * (runtime/guest_memory_budget.hpp). */
typedef struct kernarium_launch_memory_set {
    uint32_t application_mib;
    uint32_t engine_mib;
    uint32_t headroom_mib;
} kernarium_launch_memory_set;

/* The conservative parameters: the ones the default guest RAM and the host
 * GPU's budget of a launch are measured with, and so the ones a launch states
 * in its boot request unless it states its own. The loader answers this and
 * the functions below itself, from the same header the runtime's budget comes
 * from: the runtime is loaded once, for the flavour a launch chooses, so the
 * settings cannot ask it. */
kernarium_launch_memory_set kernarium_runtime_loader_launch_memory_set(void);
/* The parameters the most guest RAM a person may choose is measured with: they
 * count none of A, E_rest and S. Only the maximum uses them. */
kernarium_launch_memory_set kernarium_runtime_loader_maximum_memory_set(void);
/* The most guest RAM, in MiB, this host admits whatever the budget says (C of
 * runtime/guest_memory_budget.hpp): the RAM the runtime's native kernel window
 * holds for this process, 9152 on an iOS device or the Simulator and 1 TiB in
 * a macOS window, and no more than the host's own RAM, on the 64 MiB steps; in
 * an application signed without Extended Virtual Addressing, no more than the
 * address space the system leaves it holds a window of. Zero where that holds
 * none of even the smallest guest: nothing is admitted. The settings' ruler
 * ends here at most. */
uint32_t kernarium_runtime_loader_guest_memory_ceiling_mib(void);
/* 1 when a run starts a guest of `memory_mib` MiB on this host, 0 when it is
 * refused: the sizes from the range's minimum through the ceiling above in
 * its steps, none where the ceiling is zero. Every size the settings offer is
 * one. The runtime's own test is the same without the address space of an
 * application the system limits, whose reservation of the window is the check
 * there. */
uint32_t kernarium_runtime_loader_guest_memory_admitted(uint32_t memory_mib);
/* The shape of that test, for the applications' own restatements of it to be
 * checked against: the least size, the step and the top of any host's range
 * (no ceiling is above it). */
typedef struct kernarium_guest_memory_range {
    uint32_t minimum_mib;
    uint32_t step_mib;
    uint32_t largest_mib;
} kernarium_guest_memory_range;
kernarium_guest_memory_range kernarium_runtime_loader_guest_memory_range(void);
/* The most guest RAM, in MiB, the settings offer at `guest_processor_count`
 * vCPUs with `parameters` (zero vCPUs count as one): the largest multiple of 64
 * from 512 through the ceiling above that leaves the host GPU its 352 MiB floor
 * under the memory limit the system sets this process. The ceiling where the
 * system sets none, the Mac included; 512 where even that leaves less than the
 * floor. The settings pass the maximum set. */
uint32_t kernarium_runtime_loader_maximum_guest_memory_mib(
    uint32_t guest_processor_count,
    kernarium_launch_memory_set parameters);
/* The same for a memory limit the caller states in bytes, zero for none. */
uint32_t kernarium_runtime_loader_maximum_guest_memory_mib_for_limit(
    uint64_t limit_bytes,
    uint32_t guest_processor_count,
    kernarium_launch_memory_set parameters);
/* The guest RAM, in MiB, a new guest gets at `guest_processor_count` vCPUs
 * (zero counts as one): the conservative size below, and where the system's
 * limit lets a guest have 4096 MiB at one vCPU also no more than the largest
 * multiple of 512 that leaves the host GPU a budget of 992 MiB with
 * `parameters`, the launch set. Never more than 4096, whatever is offered;
 * 4096 where the system sets no limit. A guest with more is likely to run
 * short of graphics memory. */
uint32_t kernarium_runtime_loader_default_guest_memory_mib(
    uint32_t guest_processor_count,
    kernarium_launch_memory_set parameters);
/* The same for a memory limit the caller states in bytes, zero for none. */
uint32_t kernarium_runtime_loader_default_guest_memory_mib_for_limit(
    uint64_t limit_bytes,
    uint32_t guest_processor_count,
    kernarium_launch_memory_set parameters);
/* Nonzero where the default above is the size the host GPU's 992 MiB budget
 * rule lowered, below the conservative size: a guest with more has less than
 * that for the GPU from the next multiple of 512, which the applications warn
 * about. Zero where the default is the conservative size, and then it says
 * nothing of the GPU's budget. The same choice makes the default. */
uint32_t kernarium_runtime_loader_default_guest_memory_holds_gpu_budget(
    uint32_t guest_processor_count,
    kernarium_launch_memory_set parameters);
/* The same for a memory limit the caller states in bytes, zero for none. */
uint32_t kernarium_runtime_loader_default_guest_memory_holds_gpu_budget_for_limit(
    uint64_t limit_bytes,
    uint32_t guest_processor_count,
    kernarium_launch_memory_set parameters);
/* The most guest RAM, in MiB, before the system is likely to end the process
 * at `guest_processor_count` vCPUs: the most guest RAM with `parameters`, the
 * launch set, taken down to a multiple of 512, at least 1536, and never above
 * the maximum the settings offer. The maximum where the system sets no limit. A
 * guest with more is likely to be ended by the system. */
uint32_t kernarium_runtime_loader_conservative_guest_memory_mib(
    uint32_t guest_processor_count,
    kernarium_launch_memory_set parameters);
/* The same for a memory limit the caller states in bytes, zero for none. */
uint32_t kernarium_runtime_loader_conservative_guest_memory_mib_for_limit(
    uint64_t limit_bytes,
    uint32_t guest_processor_count,
    kernarium_launch_memory_set parameters);
/* The host GPU's budget B, in MiB, a launch of `guest_memory_mib` of guest RAM
 * at `guest_processor_count` vCPUs gets with `parameters` (zero vCPUs count as
 * one) under the memory limit the system sets this process: the evaluation the
 * run itself makes (kernarium_runtime_plan_gpu_budget_function), rounded down.
 * The guest RAM where the system sets no limit. */
uint32_t kernarium_runtime_loader_gpu_budget_mib(
    uint32_t guest_memory_mib,
    uint32_t guest_processor_count,
    kernarium_launch_memory_set parameters);
/* The same for a memory limit the caller states in bytes, zero for none. */
uint32_t kernarium_runtime_loader_gpu_budget_mib_for_limit(
    uint64_t limit_bytes,
    uint32_t guest_memory_mib,
    uint32_t guest_processor_count,
    kernarium_launch_memory_set parameters);
/* The most host-side memory, in MiB, that a live camera session holds out of
 * the GPU budget B of `gpu_budget_mib` MiB: the camera device's ceiling for
 * the tier that budget gets (uml::camera::cameraHostMemoryReserveMiB,
 * darwin/device/camera/camera_tiers.hpp), which bounds the layouts of the
 * system cameras (a development synthetic source can hold more). It needs no
 * runtime; it is what the application plans with where the runtime cannot say
 * the exact figure (kernarium_runtime_loader_plan_camera_reserve_mib). */
uint32_t kernarium_runtime_loader_camera_reserve_mib(uint32_t gpu_budget_mib);
/* The host-side memory, in MiB, that a live camera session of this host holds
 * out of the GPU budget B of `gpu_budget_mib` MiB, as the runtime composes it
 * (kernarium_runtime_plan_camera_reserve_function): the layout of the cameras it
 * discovers and the colour data in `camera_calibration_path` (null for none)
 * decides how much. While the cameras are live the GPU's pool is B less this
 * figure, taken after B is clamped to its floor; blocked cameras or none take
 * nothing, and a host without a camera answers 0. Discovery asks for no
 * permission and opens no session. The runtime discovers the cameras for the
 * first question and keeps them with the answers composed from them;
 * `rediscover` (not 0) discovers again and composes every answer again, which
 * a caller sets on the first of the questions it asks together. The figure is
 * the next boot's while the camera source, the cameras and the calibration data
 * are unchanged since it was composed. `*exact`, when
 * not null, is set to 1 for the layout's own figure and to 0 where the figure is
 * the ceiling, because no runtime is loaded or it could not discover the
 * cameras. Any thread. */
uint32_t kernarium_runtime_loader_plan_camera_reserve_mib(
    uint32_t gpu_budget_mib,
    const char* _Nullable camera_calibration_path,
    uint32_t rediscover,
    uint32_t* _Nullable exact);
/* The vCPU count the applications default to and offer at most on a device with
 * `physical_processor_count` physical cores (zero counts as one): every core,
 * and never more than the count the memory budget is measured for
 * (application_vcpu_limit, runtime/guest_memory_budget.hpp). The loader answers
 * it too, for the same reason. */
uint32_t kernarium_runtime_loader_application_vcpu_limit(
    uint32_t physical_processor_count);
/* Prepares the JIT for a guest of `guest_memory_mib` and `guest_processor_count`
 * vCPUs when `enable` is nonzero, or tells an attached debugger to detach when
 * it is zero; see kernarium_runtime_prepare_jit_function. Returns the runtime's
 * KERNARIUM_RUNTIME_JIT_ answer, or KERNARIUM_RUNTIME_JIT_UNSUPPORTED when no
 * runtime is loaded. It blocks for the seconds a debugger takes, so call it
 * away from the main thread. */
int kernarium_runtime_loader_prepare_jit(
    int enable,
    uint32_t guest_memory_mib,
    uint32_t guest_processor_count);
/* Whether a debugger is attached to this process, which the application knows
 * before any runtime is loaded. */
int kernarium_runtime_loader_debugger_attached(void);
/* Turns the guest's microphone on (nonzero) or off (zero), as
 * kernarium_runtime_set_microphone_function does, and returns the resulting
 * KERNARIUM_RUNTIME_MICROPHONE_ value, or -1 when no runtime is loaded. It
 * never asks the system for access; the caller asks first. */
int kernarium_runtime_loader_set_microphone(int enabled);
void kernarium_runtime_loader_send_touch(
    uint32_t slot,
    uint32_t x,
    uint32_t y,
    int pressed);
/* Withdraws the contact in `slot` instead of releasing it, as
 * kernarium_runtime_cancel_touch_function does. */
void kernarium_runtime_loader_cancel_touch(uint32_t slot);
void kernarium_runtime_loader_send_key(uint16_t code, int pressed);
void kernarium_runtime_loader_set_console_output_handler(
    KernariumConsoleOutputHandler _Nullable handler);
/* Receives the guest's restart events (see
 * kernarium_runtime_set_restart_callback_function), or none for a nil handler.
 * It may be set before a runtime is selected. */
void kernarium_runtime_loader_set_restart_handler(
    KernariumRestartHandler _Nullable handler);
/* Receives the guest's receiver-power events (see
 * kernarium_runtime_set_location_receiver_callback_function), or none for a nil
 * handler. It may be set before a runtime is selected. */
void kernarium_runtime_loader_set_location_receiver_handler(
    KernariumLocationReceiverHandler _Nullable handler);
/* Gives the guest's GNSS receiver the host's newest fix, or with a null `fix`
 * drops it; see kernarium_runtime_report_location_function. Returns 0, or a
 * negated errno: -ENODEV when no runtime is loaded or no guest with a receiver
 * runs, -EINVAL for a fix the runtime refuses. */
int kernarium_runtime_loader_report_location(
    const kernarium_runtime_location_fix* _Nullable fix);
/* The Android guest's hardware keyboard, mouse and pointer
 * (kernarium_runtime_set_input_connected_function and the entries after it).
 * Each returns 0, or a negated errno: -ENODEV when no runtime is loaded or no
 * guest with the devices runs, -EINVAL for an argument the runtime refuses. */
int kernarium_runtime_loader_set_input_connected(int device, BOOL connected);
int kernarium_runtime_loader_send_keyboard_usage(uint32_t usage, BOOL pressed);
int kernarium_runtime_loader_send_pointer(
    uint32_t x,
    uint32_t y,
    uint32_t buttons,
    BOOL inRange);
int kernarium_runtime_loader_send_mouse(
    int32_t dx,
    int32_t dy,
    uint32_t buttons,
    int32_t wheel,
    int32_t horizontalWheel);
/* Receives the keyboard lights and the pointer capture (see
 * kernarium_runtime_set_input_status_callback_function), or none for a nil
 * handler. It may be set before a runtime is selected. */
void kernarium_runtime_loader_set_input_status_handler(
    KernariumInputStatusHandler _Nullable handler);
/* A button of a game controller went down: wakes a guest that suspended
 * itself, as a key down does; see kernarium_runtime_wake_for_input_function.
 * It does nothing when no runtime is loaded or the runtime has no such entry.
 * It may be called from any thread. */
void kernarium_runtime_loader_wake_for_input(void);
/* Tells the runtime which denial of the network the application's notice is up
 * for, for the session record; see kernarium_runtime_set_network_notice_function.
 * Returns 0, or a negated errno: -ENODEV when no runtime is loaded, -EINVAL for
 * a value the runtime does not know. */
int kernarium_runtime_loader_set_network_notice(int notice);
/* Receives the camera state (see
 * kernarium_runtime_set_camera_state_callback_function), or none for a nil
 * handler. It may be set before a runtime is selected. */
void kernarium_runtime_loader_set_camera_state_handler(
    KernariumCameraStateHandler _Nullable handler);
/* Tells the camera how far the host turns the guest's picture; see
 * kernarium_runtime_set_camera_rotation_function. Returns 0, or a negated
 * errno: -ENODEV when no runtime is loaded, -EINVAL for a value the runtime
 * refuses. */
int kernarium_runtime_loader_set_camera_rotation(uint32_t degrees);
/* Receives the levels of the system's memory pressure and the host's response
 * (see kernarium_runtime_set_memory_pressure_callback_function), or none for a
 * nil handler. It may be set before a runtime is selected. Each report is
 * queued to the main queue as one block that holds the handler of its time, so
 * the number of reports waiting is not bounded while the main queue is busy, and
 * setting or clearing the handler does not cancel the ones already queued: they
 * reach the handler they were queued for. */
void kernarium_runtime_loader_set_memory_pressure_handler(
    KernariumMemoryPressureHandler _Nullable handler);
/* Whether the selected runtime is an automation build and the launch asked it,
 * through the environment, to start the background job with the guest. Always
 * zero for a release runtime, which cannot be asked, and when none is loaded. */
int kernarium_runtime_loader_automation_job_at_start(void);
/* The milliseconds after the application enters the background at which an
 * automation launch asked for the guest's GPU to be held, or zero for the
 * edge itself. Always zero for a release runtime, which cannot be asked, and
 * when none is loaded. */
int kernarium_runtime_loader_automation_hold_delay_ms(void);
int kernarium_runtime_loader_send_console_input(
    const void* buffer,
    size_t length);
/* Opens a stream to the guest's vsock port; see
 * kernarium_runtime_connect_vsock_function. Returns a connected descriptor
 * the caller owns, or a negated errno: -ENODEV when no runtime is loaded or no
 * guest runs. It blocks for up to `timeout_ms`, so call it away from the main
 * thread. */
int kernarium_runtime_loader_connect_vsock(
    uint32_t port,
    uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

NS_ASSUME_NONNULL_END

#endif
