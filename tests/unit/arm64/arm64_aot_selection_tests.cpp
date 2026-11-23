// SPDX-License-Identifier: GPL-2.0-or-later
/* Whether the ahead-of-time component is on is decided by each run of a
 * process (a restart decides again), so each case runs in a process of its
 * own: the parent starts this program
 * again with the case named and the environment the case states, and the
 * child answers with its exit status. The component is on by default for the
 * release its blob was made from; the environment turns it off, a launch's
 * own word (--aot=) beats the environment, and no word runs a blob with a
 * release it was not made from. A session's end (the clean power-off whose
 * guest is released) lets the next session in the process choose again. */
#include "cpu/aot_selection.hpp"
#include "cpu/arm64/aot/aot_runtime.hpp"
#include "cpu/arm64/gadgets/executor.hpp"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern char **environ;

namespace {

namespace aot = uml::cpu::arm64::aot;
using uml::cpu::AotRelease;
using uml::cpu::AotSelection;

#define require(condition) \
	do { \
		if (!(condition)) { \
			std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, \
				#condition); \
			return 1; \
		} \
	} while (false)

struct Case final {
	const char *name;
	const char *environment; // DARWIN_UML_AOT, or nullptr for unset
	int (*run)();
};

/* The release the framework's blob was made from: what a launch of that
 * release states. */
AotRelease same_release()
{
	const auto release = aot::embedded_release();
	return release ? *release : AotRelease{};
}

AotRelease other_id()
{
	auto release = same_release();
	release.id = "another-release";
	return release;
}

AotRelease other_system()
{
	auto release = same_release();
	release.system_sha256[31] ^= 1U;
	return release;
}

AotRelease other_manifest()
{
	auto release = same_release();
	release.manifest_sha256[0] ^= 1U;
	return release;
}

int the_blob_names_a_release()
{
	const auto release = aot::embedded_release();
	require(release.has_value());
	require(!release->id.empty());
	return 0;
}

int same_release_is_on()
{
	const auto release = same_release();
	require(aot::select(AotSelection::automatic, &release));
	require(aot::enabled());
	return 0;
}

int environment_turns_it_off()
{
	const auto release = same_release();
	require(aot::select(AotSelection::automatic, &release));
	require(!aot::enabled());
	return 0;
}

int environment_one_is_on()
{
	const auto release = same_release();
	require(aot::select(AotSelection::automatic, &release));
	require(aot::enabled());
	return 0;
}

int other_values_are_the_default()
{
	const auto release = same_release();
	require(aot::select(AotSelection::automatic, &release));
	require(aot::enabled());
	return 0;
}

int launch_off_beats_the_default()
{
	const auto release = same_release();
	require(aot::select(AotSelection::off, &release));
	require(!aot::enabled());
	return 0;
}

int launch_on_beats_the_environment()
{
	const auto release = same_release();
	require(aot::select(AotSelection::on, &release));
	require(aot::enabled());
	return 0;
}

/* Another release, or the same ID with another manifest or system partition,
 * is not the system the blob was made from: off by default, and a launch
 * that insists on the component does not start. Nothing is latched by the
 * refusal. */
template<AotRelease (*Release)()>
int a_release_the_blob_does_not_fit()
{
	const auto release = Release();
	require(aot::select(AotSelection::automatic, &release));
	require(!aot::enabled());
	return 0;
}

template<AotRelease (*Release)()>
int a_launch_that_insists_on_another_release_fails()
{
	const auto release = Release();
	require(!aot::select(AotSelection::on, &release));
	/* The refusal decided nothing: the launch that follows is its own. */
	const auto same = same_release();
	require(aot::select(AotSelection::automatic, &same));
	require(aot::enabled());
	return 0;
}

/* A guest that is not an Android release states none. It gets no
 * ahead-of-time code: the answer is off before any executor exists, and an
 * executor made after it holds no state of the component (no tables, which a
 * binder allocates on its first lookup). */
int a_guest_with_no_release_pays_nothing()
{
	require(aot::select(AotSelection::automatic, nullptr));
	require(!aot::enabled());
	uml::cpu::arm64::gadgets::GadgetExecutor executor;
	require(!executor.aot_selected());
	return 0;
}

int a_guest_with_no_release_refuses_on()
{
	require(!aot::select(AotSelection::on, nullptr));
	require(aot::select(AotSelection::off, nullptr));
	require(!aot::enabled());
	return 0;
}

int a_release_launch_is_on_for_an_executor()
{
	const auto release = same_release();
	require(aot::select(AotSelection::automatic, &release));
	uml::cpu::arm64::gadgets::GadgetExecutor executor;
	require(executor.aot_selected());
	return 0;
}

/* A process whose launch selected nothing never enters the component (a
 * diagnostics build, which tests drive without a launch, answers with the
 * environment instead). */
int an_undecided_process_is_off()
{
#if UML_AOT_DIAGNOSTICS
	return 0;
#else
	require(!aot::enabled());
	return 0;
#endif
}

/* A run that starts again in the process decides again. The executors of a
 * run read the answer once, when they are made, so the first run's executor
 * keeps what it was made with and the next run's is made with the new
 * answer. */
int a_restart_with_the_same_release_stays_on()
{
	const auto release = same_release();
	require(aot::select(AotSelection::automatic, &release));
	uml::cpu::arm64::gadgets::GadgetExecutor first;
	require(first.aot_selected());
	require(aot::select(AotSelection::on, &release));
	require(aot::select(AotSelection::automatic, &release));
	uml::cpu::arm64::gadgets::GadgetExecutor second;
	require(second.aot_selected());
	return 0;
}

/* The release changed under a launch that began on the component: under
 * `automatic` the next run is not a failure, it runs on the engine, and none
 * of its executors enters the blob. */
template<AotRelease (*Release)()>
int a_restart_with_another_release_runs_on_the_engine()
{
	const auto release = same_release();
	require(aot::select(AotSelection::automatic, &release));
	uml::cpu::arm64::gadgets::GadgetExecutor first;
	require(first.aot_selected());
	const auto other = Release();
	require(aot::select(AotSelection::automatic, &other));
	require(!aot::enabled());
	uml::cpu::arm64::gadgets::GadgetExecutor second;
	require(!second.aot_selected());
	/* Nor does the run after it enter it before its release fits again. */
	require(aot::select(AotSelection::automatic, &other));
	uml::cpu::arm64::gadgets::GadgetExecutor third;
	require(!third.aot_selected());
	return 0;
}

/* A launch that insists on the component still does not start on a release
 * the blob does not fit, whatever the process ran before. */
int a_restart_that_insists_on_another_release_fails()
{
	const auto release = same_release();
	require(aot::select(AotSelection::on, &release));
	const auto other = other_system();
	require(!aot::select(AotSelection::on, &other));
	/* The refusal decided nothing: the process still runs it for this one. */
	require(aot::enabled());
	require(aot::select(AotSelection::on, &release));
	return 0;
}

/* A process that ran without the component does not turn it on later: a run
 * under `automatic` that the blob fits again runs on the engine, and one that
 * insists on it does not start. */
int a_restart_after_a_mismatch_stays_on_the_engine()
{
	const auto release = same_release();
	const auto other = other_manifest();
	require(aot::select(AotSelection::automatic, &other));
	require(!aot::enabled());
	require(aot::select(AotSelection::automatic, &release));
	require(!aot::enabled());
	uml::cpu::arm64::gadgets::GadgetExecutor executor;
	require(!executor.aot_selected());
	require(!aot::select(AotSelection::on, &release));
	require(!aot::enabled());
	return 0;
}

/* A launch's own `off` is per run too. */
int a_restart_that_says_off_runs_on_the_engine()
{
	const auto release = same_release();
	require(aot::select(AotSelection::on, &release));
	require(aot::select(AotSelection::off, &release));
	require(!aot::enabled());
	require(aot::select(AotSelection::automatic, &release));
	require(!aot::enabled());
	return 0;
}

int a_late_on_is_refused_after_off()
{
	const auto release = same_release();
	require(aot::select(AotSelection::off, &release));
	require(!aot::enabled());
	require(!aot::select(AotSelection::on, &release));
	require(aot::select(AotSelection::off, &release));
	return 0;
}

/* A process that ran without the component stays off for the rest of that
 * session, a restart included, but a session ends with Android's clean
 * power-off and the release of its guest (the loader then reuses the process
 * for the next start). That end gives the next session its own choice: a
 * person who turned the component off and then on again gets it at the next
 * start, in the same process. */
int a_new_session_after_an_off_start_can_select_the_component()
{
	/* The off start: a launch that states no release (the person's choice). */
	require(aot::select(AotSelection::automatic, nullptr));
	require(!aot::enabled());
	uml::cpu::arm64::gadgets::GadgetExecutor off;
	require(!off.aot_selected());
	/* A restart inside the session decides the same. */
	const auto release = same_release();
	require(aot::select(AotSelection::automatic, &release));
	require(!aot::enabled());
	require(!aot::select(AotSelection::on, &release));
	/* The session ends; the next one states the release. */
	aot::end_session();
	require(aot::select(AotSelection::automatic, &release));
	require(aot::enabled());
	uml::cpu::arm64::gadgets::GadgetExecutor on;
	require(on.aot_selected());
	return 0;
}

int a_new_session_after_an_off_start_can_insist_on_the_component()
{
	require(aot::select(AotSelection::automatic, nullptr));
	aot::end_session();
	const auto release = same_release();
	require(aot::select(AotSelection::on, &release));
	require(aot::enabled());
	return 0;
}

/* The other way: the component ran, the person turned it off, and the next
 * session runs on the engine; turned on again, the one after has it. */
int a_new_session_after_an_on_start_can_be_off_and_on_again()
{
	const auto release = same_release();
	require(aot::select(AotSelection::automatic, &release));
	require(aot::enabled());
	aot::end_session();
	require(aot::select(AotSelection::automatic, nullptr));
	require(!aot::enabled());
	uml::cpu::arm64::gadgets::GadgetExecutor off;
	require(!off.aot_selected());
	aot::end_session();
	require(aot::select(AotSelection::automatic, &release));
	require(aot::enabled());
	uml::cpu::arm64::gadgets::GadgetExecutor on;
	require(on.aot_selected());
	return 0;
}

/* A new session takes the component only for the release it was made from,
 * after an off start as at any boot: the release is validated at every boot,
 * the session's end only lets the choice be made again. */
template<AotRelease (*Release)()>
int a_new_session_after_an_off_start_still_checks_the_release()
{
	require(aot::select(AotSelection::automatic, nullptr));
	aot::end_session();
	const auto other = Release();
	require(!aot::select(AotSelection::on, &other));
	require(aot::select(AotSelection::automatic, &other));
	require(!aot::enabled());
	uml::cpu::arm64::gadgets::GadgetExecutor mismatched;
	require(!mismatched.aot_selected());
	/* That session ran without it, and so stays: a restart does not turn it
	 * on. The session after it can. */
	const auto same = same_release();
	require(aot::select(AotSelection::automatic, &same));
	require(!aot::enabled());
	require(!aot::select(AotSelection::on, &same));
	aot::end_session();
	require(aot::select(AotSelection::on, &same));
	require(aot::enabled());
	return 0;
}

const Case cases[]{
	{"blob_names_a_release", nullptr, the_blob_names_a_release},
	{"same_release_is_on", nullptr, same_release_is_on},
	{"environment_turns_it_off", "0", environment_turns_it_off},
	{"environment_one_is_on", "1", environment_one_is_on},
	{"other_values_are_the_default", "yes", other_values_are_the_default},
	{"launch_off", nullptr, launch_off_beats_the_default},
	{"launch_on", "0", launch_on_beats_the_environment},
	{"other_id_is_off", nullptr, a_release_the_blob_does_not_fit<other_id>},
	{"other_system_is_off", nullptr,
		a_release_the_blob_does_not_fit<other_system>},
	{"other_manifest_is_off", nullptr,
		a_release_the_blob_does_not_fit<other_manifest>},
	{"other_id_fails_on", nullptr,
		a_launch_that_insists_on_another_release_fails<other_id>},
	{"other_system_fails_on", nullptr,
		a_launch_that_insists_on_another_release_fails<other_system>},
	{"other_manifest_fails_on", nullptr,
		a_launch_that_insists_on_another_release_fails<other_manifest>},
	{"no_release_pays_nothing", nullptr, a_guest_with_no_release_pays_nothing},
	{"no_release_refuses_on", nullptr, a_guest_with_no_release_refuses_on},
	{"release_launch_selects_executor", nullptr,
		a_release_launch_is_on_for_an_executor},
	{"undecided_is_off", nullptr, an_undecided_process_is_off},
	{"restart_same_release_stays_on", nullptr,
		a_restart_with_the_same_release_stays_on},
	{"restart_other_id_runs_on_the_engine", nullptr,
		a_restart_with_another_release_runs_on_the_engine<other_id>},
	{"restart_other_system_runs_on_the_engine", nullptr,
		a_restart_with_another_release_runs_on_the_engine<other_system>},
	{"restart_other_manifest_runs_on_the_engine", nullptr,
		a_restart_with_another_release_runs_on_the_engine<other_manifest>},
	{"restart_insisting_on_another_release_fails", nullptr,
		a_restart_that_insists_on_another_release_fails},
	{"restart_after_a_mismatch_stays_on_the_engine", nullptr,
		a_restart_after_a_mismatch_stays_on_the_engine},
	{"restart_that_says_off_runs_on_the_engine", nullptr,
		a_restart_that_says_off_runs_on_the_engine},
	{"late_on_after_off", "1", a_late_on_is_refused_after_off},
	{"new_session_after_off_selects", nullptr,
		a_new_session_after_an_off_start_can_select_the_component},
	{"new_session_after_off_insists", nullptr,
		a_new_session_after_an_off_start_can_insist_on_the_component},
	{"new_session_after_on_can_be_off_and_on_again", nullptr,
		a_new_session_after_an_on_start_can_be_off_and_on_again},
	{"new_session_after_off_checks_other_id", nullptr,
		a_new_session_after_an_off_start_still_checks_the_release<other_id>},
	{"new_session_after_off_checks_other_system", nullptr,
		a_new_session_after_an_off_start_still_checks_the_release<
			other_system>},
	{"new_session_after_off_checks_other_manifest", nullptr,
		a_new_session_after_an_off_start_still_checks_the_release<
			other_manifest>},
};

int run_child(const Case &test)
{
	return test.run();
}

int run_parent(const char *program)
{
	int failures = 0;
	for (const auto &test : cases) {
		std::vector<std::string> environment;
		for (char **entry = environ; *entry != nullptr; ++entry)
			if (std::strncmp(*entry, "DARWIN_UML_AOT", 14) != 0)
				environment.emplace_back(*entry);
		if (test.environment != nullptr)
			environment.push_back(
				std::string{"DARWIN_UML_AOT="} + test.environment);
		std::vector<char *> envp;
		for (auto &entry : environment)
			envp.push_back(entry.data());
		envp.push_back(nullptr);
		std::string program_copy{program};
		std::string case_name{test.name};
		char *argv[]{program_copy.data(), case_name.data(), nullptr};
		pid_t child{};
		if (posix_spawn(&child, program, nullptr, nullptr, argv,
			    envp.data()) != 0) {
			std::fprintf(stderr, "%s: cannot start the case\n", test.name);
			++failures;
			continue;
		}
		int status{};
		if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
		    WEXITSTATUS(status) != 0) {
			std::fprintf(stderr, "%s: failed\n", test.name);
			++failures;
		}
	}
	if (failures == 0)
		std::printf("the component is on by default for the release it was "
			    "made from, the environment turns it off, a launch's "
			    "word wins, and no other release gets it\n");
	return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc == 2) {
		for (const auto &test : cases)
			if (std::strcmp(argv[1], test.name) == 0)
				return run_child(test);
		return 2;
	}
	return run_parent(argv[0]);
}
