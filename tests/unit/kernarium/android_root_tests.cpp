// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumAndroidRoot.hpp"
#include "named_tests.hpp"
#include "require.hpp"

#include <cstddef>
#include <cstdint>

namespace {

using namespace kernarium::androidroot;
using uml::android::RootMode;
using uml::testing::require;

/* A request of the current layout that states `root`. */
kernarium_runtime_boot request(std::uint32_t root)
{
    kernarium_runtime_boot boot {};
    boot.struct_size = static_cast<std::uint32_t>(sizeof(boot));
    boot.android_root = root;
    return boot;
}

void aRequestThatStatesNothingBootsTheReleasesOwnRamdisk()
{
    require(rootMode(request(0U)) == RootMode::off);
    /* A zeroed request, as Kernarium makes one, is today's launch. */
    kernarium_runtime_boot zeroed {};
    zeroed.struct_size = static_cast<std::uint32_t>(sizeof(zeroed));
    require(rootMode(zeroed) == RootMode::off);
}

void anyNonzeroWordAsksForTheRootBoot()
{
    require(rootMode(request(1U)) == RootMode::on);
    require(rootMode(request(2U)) == RootMode::on);
    require(rootMode(request(0xffffffffU)) == RootMode::on);
}

void aRequestThatEndsBeforeTheWordNeverStatesIt()
{
    /* An older application's request is shorter than the word, and the bytes
     * past its struct_size are not its own: whatever they hold is not read. */
    kernarium_runtime_boot boot = request(1U);
    boot.struct_size = static_cast<std::uint32_t>(bootSizeBeforeRoot);
    require(rootMode(boot) == RootMode::off);
    boot.struct_size = static_cast<std::uint32_t>(
        bootSizeBeforeRoot + sizeof(boot.android_root) - 1U);
    require(rootMode(boot) == RootMode::off);
    boot.struct_size = static_cast<std::uint32_t>(
        bootSizeBeforeRoot + sizeof(boot.android_root));
    require(rootMode(boot) == RootMode::on);
}

void theWordIsTheLastMemberAndTheApiIsTheOneThatAddedIt()
{
    /* A later field is a later API: nothing may follow the word unnoticed,
     * because every request that ends before it would then state it too. */
    require(bootSizeBeforeRoot + sizeof(std::uint32_t) <=
            sizeof(kernarium_runtime_boot));
    require(sizeof(kernarium_runtime_boot) - bootSizeBeforeRoot <=
            sizeof(void*));
    require(KERNARIUM_RUNTIME_API_VERSION >= 46U);
}

} // namespace

int main()
{
    static constexpr uml::testing::NamedTest tests[] = {
        {"a-request-that-states-nothing-boots-the-releases-own-ramdisk",
         aRequestThatStatesNothingBootsTheReleasesOwnRamdisk},
        {"any-nonzero-word-asks-for-the-root-boot",
         anyNonzeroWordAsksForTheRootBoot},
        {"a-request-that-ends-before-the-word-never-states-it",
         aRequestThatEndsBeforeTheWordNeverStatesIt},
        {"the-word-is-the-last-member-and-the-api-is-the-one-that-added-it",
         theWordIsTheLastMemberAndTheApiIsTheOneThatAddedIt},
    };
    return uml::testing::runNamedTests(tests);
}
