// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumAotRelease.hpp"
#include "named_tests.hpp"
#include "require.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace {

using namespace kernarium::aot;
using uml::testing::require;

/* A request of the current layout that states `disabled`. */
kernarium_runtime_boot request(std::uint32_t disabled)
{
    kernarium_runtime_boot boot {};
    boot.struct_size = static_cast<std::uint32_t>(sizeof(boot));
    boot.android_aot_disabled = disabled;
    return boot;
}

void aRequestThatStatesNothingKeepsTheComponentIn()
{
    require(!aotDisabled(request(0U)));
    /* A zeroed request, as Kernarium makes one, is today's launch. */
    kernarium_runtime_boot zeroed {};
    zeroed.struct_size = static_cast<std::uint32_t>(sizeof(zeroed));
    require(!aotDisabled(zeroed));
}

void anyNonzeroWordKeepsTheComponentOut()
{
    require(aotDisabled(request(1U)));
    require(aotDisabled(request(2U)));
    require(aotDisabled(request(0xffffffffU)));
}

void aRequestThatEndsBeforeTheWordNeverStatesIt()
{
    /* An older application's request is shorter than the word, and the bytes
     * past its struct_size are not its own: whatever they hold is not read. */
    kernarium_runtime_boot boot = request(1U);
    boot.struct_size = static_cast<std::uint32_t>(bootSizeBeforeAotDisabled);
    require(!aotDisabled(boot));
    boot.struct_size = static_cast<std::uint32_t>(
        bootSizeBeforeAotDisabled + sizeof(boot.android_aot_disabled) - 1U);
    require(!aotDisabled(boot));
    boot.struct_size = static_cast<std::uint32_t>(
        bootSizeBeforeAotDisabled + sizeof(boot.android_aot_disabled));
    require(aotDisabled(boot));
}

void theWordIsFollowedByTheRootWordAndTheApiIsTheOneThatAddedIt()
{
    /* A later field is a later API: nothing may come between the word and
     * the next one unnoticed, because every request that ends before it
     * would then state it too. The root word (API 46) follows it directly;
     * what follows that one is android_root_tests's to check. */
    require(bootSizeBeforeAotDisabled +
                sizeof(kernarium_runtime_boot::android_aot_disabled) ==
            offsetof(kernarium_runtime_boot, android_root));
    require(KERNARIUM_RUNTIME_API_VERSION >= 45U);
}

/* The source of a release's identity, which counts the times it is read. */
struct FakeRelease {
    std::optional<uml::android::ReleaseIdentity> identity;
    mutable int reads {};

    std::optional<uml::android::ReleaseIdentity> releaseIdentity() const noexcept
    {
        ++reads;
        return identity;
    }
};

FakeRelease matchingRelease()
{
    return {uml::android::ReleaseIdentity {
                "20261115-los24-r28", std::string(64, 'a'), std::string(64, '0')},
            0};
}

const std::string statedMatchingRelease =
    "--aot-release=20261115-los24-r28," + std::string(64, 'a') + "," +
    std::string(64, '0');

void aLaunchThatStatesNothingGivesTheEmulatorTheRelease()
{
    /* The arguments the runtime produces for a request of the current layout
     * that states the word 0, and for a zeroed one. */
    const FakeRelease release = matchingRelease();
    require(releaseArgument(aotDisabled(request(0U)), release) ==
            statedMatchingRelease);
    require(release.reads == 1);
}

void aLaunchThatStatesTheWordGivesTheEmulatorNoRelease()
{
    /* Whatever the release is, a set word leaves it out, and does not read it. */
    const FakeRelease release = matchingRelease();
    require(releaseArgument(aotDisabled(request(1U)), release).empty());
    require(releaseArgument(aotDisabled(request(0xffffffffU)), release).empty());
    require(release.reads == 0);
}

void anOlderRequestStillGivesTheEmulatorTheRelease()
{
    /* An application that predates the word has no way to keep the component
     * out, and its start boots with the release as before. */
    kernarium_runtime_boot boot = request(1U);
    boot.struct_size = static_cast<std::uint32_t>(bootSizeBeforeAotDisabled);
    const FakeRelease release = matchingRelease();
    require(releaseArgument(aotDisabled(boot), release) ==
            statedMatchingRelease);
}

void aReleaseWithoutAnIdentityGivesTheEmulatorNone()
{
    /* A release that says nothing it can be identified by gets no argument
     * whichever way the request goes. */
    const FakeRelease none {std::nullopt, 0};
    require(releaseArgument(aotDisabled(request(0U)), none).empty());
    FakeRelease malformed = matchingRelease();
    malformed.identity->systemSha256 = "not a digest";
    require(releaseArgument(aotDisabled(request(0U)), malformed).empty());
}

} // namespace

int main()
{
    static constexpr uml::testing::NamedTest tests[] = {
        {"a-request-that-states-nothing-keeps-the-component-in",
         aRequestThatStatesNothingKeepsTheComponentIn},
        {"any-nonzero-word-keeps-the-component-out",
         anyNonzeroWordKeepsTheComponentOut},
        {"a-request-that-ends-before-the-word-never-states-it",
         aRequestThatEndsBeforeTheWordNeverStatesIt},
        {"the-word-is-followed-by-the-root-word-and-the-api-is-the-one-that-added-it",
         theWordIsFollowedByTheRootWordAndTheApiIsTheOneThatAddedIt},
        {"a-launch-that-states-nothing-gives-the-emulator-the-release",
         aLaunchThatStatesNothingGivesTheEmulatorTheRelease},
        {"a-launch-that-states-the-word-gives-the-emulator-no-release",
         aLaunchThatStatesTheWordGivesTheEmulatorNoRelease},
        {"an-older-request-still-gives-the-emulator-the-release",
         anOlderRequestStillGivesTheEmulatorTheRelease},
        {"a-release-without-an-identity-gives-the-emulator-none",
         aReleaseWithoutAnIdentityGivesTheEmulatorNone},
    };
    return uml::testing::runNamedTests(tests);
}
