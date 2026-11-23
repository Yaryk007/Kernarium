// SPDX-License-Identifier: GPL-2.0-or-later

// The installation identity an Android guest is given: how a boot request
// states values it must have, and how the runtime resolves the rest from the
// file beside the data disk. Every value here is synthetic.

#include "KernariumIdentity.hpp"
#include "named_tests.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {

using namespace kernarium::identity;
namespace android = uml::android;
namespace fs = std::filesystem;
using uml::testing::require;

kernarium_runtime_boot fullBoot()
{
    kernarium_runtime_boot boot {};
    boot.struct_size = sizeof(boot);
    return boot;
}

void aRequestThatStatesNothingStatesNoValue()
{
    auto boot = fullBoot();
    auto request = readRequest(boot);
    require(!request.stated.serial && !request.stated.ethernetMac && !request.invalid);

    // An application built before the fields states nothing, whatever follows
    // its struct in memory.
    static const char serial[] = "TESTSERIAL01";
    static const char mac[] = "02:aa:bb:cc:dd:ee";
    boot.android_serial_number = serial;
    boot.android_ethernet_mac = mac;
    boot.struct_size = bootSizeBeforeSerial;
    request = readRequest(boot);
    require(!request.stated.serial && !request.stated.ethernetMac && !request.invalid);
    boot.struct_size = bootSizeBeforeEthernetMac;
    request = readRequest(boot);
    require(request.stated.serial == "TESTSERIAL01", "the serial alone");
    require(!request.stated.ethernetMac && !request.invalid, "and not the address");
    boot.struct_size = sizeof(boot);
    request = readRequest(boot);
    require(request.stated.serial == "TESTSERIAL01");
    require((request.stated.ethernetMac
             == android::MacAddress { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee }));
    require(!request.invalid);
}

void aValueThatIsNotOneIsInvalidAndNeverGuessed()
{
    for (const char* bad : { "", "abc", "unknown", "UNKNOWN", "abc def1",
             "abcdefghij0123456789a", "umlandroid-17" }) {
        auto boot = fullBoot();
        boot.android_serial_number = bad;
        const auto request = readRequest(boot);
        require(request.invalid && !request.stated.serial, bad);
    }
    for (const char* bad : { "", "02:aa:bb:cc:dd", "02-aa-bb-cc-dd-ee",
             "03:aa:bb:cc:dd:ee", "00:aa:bb:cc:dd:ee", "02:00:00:00:00:00",
             "52:54:00:12:34:56", "02:15:b2:00:00:00", "zz:aa:bb:cc:dd:ee",
             "02:aa:bb:cc:dd:ee:ff" }) {
        auto boot = fullBoot();
        boot.android_ethernet_mac = bad;
        const auto request = readRequest(boot);
        require(request.invalid && !request.stated.ethernetMac, bad);
    }
    // A string the caller did not end is never read past its longest value.
    const std::string unterminated(4096, 'a');
    auto boot = fullBoot();
    boot.android_serial_number = unterminated.c_str();
    require(readRequest(boot).invalid);
    boot = fullBoot();
    boot.android_ethernet_mac = unterminated.c_str();
    require(readRequest(boot).invalid);
}

/* A run as the runtime makes it (prepareInstallation without the disk, whose
 * layout this test has none of): the lock first, then the resolution of the
 * request identityRequest makes. */
uml::android::ResolvedIdentity resolveLocked(
    const Request& request, const fs::path& dataDisk, bool created)
{
    auto asked = identityRequest(request, dataDisk);
    asked.diskCreated = created;
    const auto held = request.stated.serial || request.stated.ethernetMac
        ? std::optional<android::InstallationLock> {}
        : android::acquireInstallationLock(asked.file);
    asked.lock = held ? &*held : nullptr;
    return android::resolveIdentity(asked);
}

void theFileBesideTheDataDiskIsTheInstallationsIdentity()
{
    const uml::testing::ScratchSubdirectory scratch { "uml-kernarium-identity.XXXXXX" };
    const auto first = scratch.path() / "first";
    const auto second = scratch.path() / "second";
    fs::create_directory(first);
    fs::create_directory(second);
    const Request nothing;

    // The first start of an installation makes the file and the next keeps it.
    const auto made = resolveLocked(nothing, first / "data.img", true);
    require(made.outcome == android::IdentityOutcome::created);
    require(made.identity.serial && made.identity.ethernetMac);
    require(fs::exists(first / "identity.json"));
    const auto kept = resolveLocked(nothing, first / "data.img", false);
    require(kept.outcome == android::IdentityOutcome::kept && kept.identity == made.identity);

    // An installation that has none (one that predates the identity) gets one
    // on its first start after the update, and keeps it.
    const auto lazily = resolveLocked(nothing, second / "data.img", false);
    require(lazily.outcome == android::IdentityOutcome::created);
    require(resolveLocked(nothing, second / "data.img", false).identity == lazily.identity);
    // Two installations differ.
    require(lazily.identity.serial != made.identity.serial);
    require(lazily.identity.ethernetMac != made.identity.ethernetMac);

    // A data disk this start made is a new installation.
    const auto anew = resolveLocked(nothing, first / "data.img", true);
    require(anew.identity.serial != made.identity.serial);
    require(resolveLocked(nothing, first / "data.img", false).identity == anew.identity);

    // What a request states is used as given and leaves the file alone.
    Request stated;
    stated.stated.serial = "TESTSERIAL01";
    const auto given = resolveLocked(stated, first / "data.img", true);
    require(given.outcome == android::IdentityOutcome::stated);
    require(given.identity.serial == "TESTSERIAL01" && !given.identity.ethernetMac);
    require(resolveLocked(nothing, first / "data.img", false).identity == anew.identity);
    fs::remove(first / "identity.json");
    require(resolveLocked(stated, first / "data.img", false).outcome == android::IdentityOutcome::stated);
    require(!fs::exists(first / "identity.json"), "a stated value made a file");
    // The request names the file beside the data disk and carries what it states.
    const auto asked = identityRequest(stated, first / "data.img");
    require(asked.file == first / "identity.json" && asked.stated.serial == stated.stated.serial);
    require(identityRequest(nothing, first / "data.img").stated == android::InstallationIdentity {});

    // A run that could not take the lock reads and writes nothing: values it
    // does not keep.
    auto direct = identityRequest(nothing, first / "data.img");
    require(android::resolveIdentity(direct).outcome == android::IdentityOutcome::unsaved);
    require(!fs::exists(first / "identity.json"), "an unlocked run made the file");
    const auto missing = scratch.path() / "missing" / "data.img";
    require(!android::acquireInstallationLock(identityRequest(nothing, missing).file),
            "a lock was taken in a directory that is not there");
}

} // namespace

int main()
{
    static constexpr uml::testing::NamedTest tests[] = {
        { "a-request-that-states-nothing-states-no-value", aRequestThatStatesNothingStatesNoValue },
        { "a-value-that-is-not-one-is-invalid-and-never-guessed", aValueThatIsNotOneIsInvalidAndNeverGuessed },
        { "the-file-beside-the-data-disk-is-the-installations-identity", theFileBesideTheDataDiskIsTheInstallationsIdentity },
    };
    return uml::testing::runNamedTests(tests);
}
