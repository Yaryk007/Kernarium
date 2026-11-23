// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumHostFeatures.hpp"
#include "motion_protocol.hpp"
#include "android_boot.hpp"
#include "named_tests.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace kernarium::hostfeatures;
using uml::testing::require;
using uml::testing::requireThrows;

constexpr std::string_view bootDevices =
    "androidboot.boot_devices=uml-blkdev.0,uml-blkdev.1";

void writeText(const std::filesystem::path& path, const std::string& text)
{
    std::FILE* file = std::fopen(path.c_str(), "wb");
    require(file != nullptr);
    require(std::fwrite(text.data(), 1, text.size(), file) == text.size());
    require(std::fclose(file) == 0);
}

void everyBitIsOneBitOfItsOwn()
{
    require(distinctRows(features));
    std::uint32_t seen = 0;
    for (const Feature& feature : features) {
        require(singleBit(feature.bit), std::string {feature.name});
        require((seen & feature.bit) == 0, std::string {feature.name});
        seen |= feature.bit;
    }
    require(seen == knownBits());
    /* The enum of the public header is what the applications state. */
    require(KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS == 1U);
    require((knownBits() & KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS) != 0U);
    require(KERNARIUM_RUNTIME_ANDROID_HOST_ORIENTATION == 2U);
    require((knownBits() & KERNARIUM_RUNTIME_ANDROID_HOST_ORIENTATION) != 0U);
    require(KERNARIUM_RUNTIME_ANDROID_HOST_COMPASS == 4U);
    require((knownBits() & KERNARIUM_RUNTIME_ANDROID_HOST_COMPASS) != 0U);
    require(KERNARIUM_RUNTIME_ANDROID_HOST_HIGH_REFRESH == 8U);
    require((knownBits() & KERNARIUM_RUNTIME_ANDROID_HOST_HIGH_REFRESH) != 0U);
    require(knownBits() == 15U);
}

void aCollisionBetweenTwoRowsIsDetected()
{
    const Feature first {"first", 1U << 0, "androidboot.first=", "on"};
    const Feature second {"second", 1U << 1, "androidboot.second=", "on"};
    require(distinctRows(std::array {first, second}));

    /* The same bit twice would turn one feature into the other. */
    Feature sameBit = second;
    sameBit.bit = first.bit;
    require(!distinctRows(std::array {first, sameBit}));
    /* So would the same name or the same argument. */
    Feature sameName = second;
    sameName.name = first.name;
    require(!distinctRows(std::array {first, sameName}));
    Feature samePrefix = second;
    samePrefix.argumentPrefix = first.argumentPrefix;
    require(!distinctRows(std::array {first, samePrefix}));

    /* A row is one bit, with an argument of the form name=value. */
    Feature noBit = second;
    noBit.bit = 0;
    require(!distinctRows(std::array {first, noBit}));
    Feature twoBits = second;
    twoBits.bit = 0b110;
    require(!distinctRows(std::array {first, twoBits}));
    Feature noEquals = second;
    noEquals.argumentPrefix = "androidboot.second";
    require(!distinctRows(std::array {first, noEquals}));
    Feature noValue = second;
    noValue.argumentValue = "";
    require(!distinctRows(std::array {first, noValue}));
    Feature noName = second;
    noName.name = "";
    require(!distinctRows(std::array {first, noName}));
}

void theHapticsBitBecomesTheSkuArgument()
{
    require(kernelArguments(KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS) ==
            std::vector<std::string> {
                "androidboot.product.hardware.sku=haptics"});
    /* A device without haptics passes nothing. */
    require(kernelArguments(0).empty());
}

void theOrientationBitBecomesTheArgumentTheSubHalReads()
{
    /* The guest's motion sub-HAL reads ro.boot.kernarium.orientation
     * (motion_protocol.hpp), which Android's init imports from this kernel
     * argument. */
    require(kernelArguments(KERNARIUM_RUNTIME_ANDROID_HOST_ORIENTATION) ==
            std::vector<std::string> {"androidboot.kernarium.orientation=1"});
    /* init makes "ro.boot.<name>" of "androidboot.<name>". */
    const std::string property {andarium::motion::orientation_property};
    require(property.starts_with("ro.boot."));
    require("androidboot." + property.substr(std::string_view {"ro.boot."}.size()) +
                "=" ==
            kernelArguments(KERNARIUM_RUNTIME_ANDROID_HOST_ORIENTATION).front().substr(
                0, std::string {"androidboot.kernarium.orientation="}.size()));
    require(kernelArguments(KERNARIUM_RUNTIME_ANDROID_HOST_ORIENTATION |
                            KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS) ==
            std::vector<std::string> {
                "androidboot.product.hardware.sku=haptics",
                "androidboot.kernarium.orientation=1"});
}

void theCompassBitBecomesTheArgumentTheSubHalWillRead()
{
    require(kernelArguments(KERNARIUM_RUNTIME_ANDROID_HOST_COMPASS) ==
            std::vector<std::string> {"androidboot.kernarium.compass=1"});
    /* init makes "ro.boot.kernarium.compass" of this argument, the property
     * the guest's sub-HAL and the compass feature are keyed by. */
    const std::string argument =
        kernelArguments(KERNARIUM_RUNTIME_ANDROID_HOST_COMPASS).front();
    require(argument.starts_with("androidboot."));
    const std::string name = argument.substr(
        std::string_view {"androidboot."}.size(),
        argument.find('=') - std::string_view {"androidboot."}.size());
    require("ro.boot." + name == "ro.boot.kernarium.compass");
    /* The three features are independent and come in table order. */
    require(kernelArguments(KERNARIUM_RUNTIME_ANDROID_HOST_COMPASS |
                            KERNARIUM_RUNTIME_ANDROID_HOST_ORIENTATION |
                            KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS) ==
            std::vector<std::string> {
                "androidboot.product.hardware.sku=haptics",
                "androidboot.kernarium.orientation=1",
                "androidboot.kernarium.compass=1"});
    require(kernelArguments(KERNARIUM_RUNTIME_ANDROID_HOST_COMPASS |
                            KERNARIUM_RUNTIME_ANDROID_HOST_ORIENTATION) ==
            std::vector<std::string> {"androidboot.kernarium.orientation=1",
                                      "androidboot.kernarium.compass=1"});
    /* Neither of the other bits makes the compass argument. */
    for (const std::uint32_t bit : {KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS,
                                    KERNARIUM_RUNTIME_ANDROID_HOST_ORIENTATION}) {
        for (const std::string& other : kernelArguments(bit))
            require(!other.starts_with("androidboot.kernarium.compass="));
    }
}

void theHighRefreshBitBecomesTheArgumentTheGuestsSchedulerWillRead()
{
    require(kernelArguments(KERNARIUM_RUNTIME_ANDROID_HOST_HIGH_REFRESH) ==
            std::vector<std::string> {"androidboot.kernarium.high_refresh=1"});
    /* init makes "ro.boot.kernarium.high_refresh" of this argument, the
     * property the guest's 120 Hz configuration is keyed by. */
    const std::string argument =
        kernelArguments(KERNARIUM_RUNTIME_ANDROID_HOST_HIGH_REFRESH).front();
    require(argument.starts_with("androidboot."));
    const std::string name = argument.substr(
        std::string_view {"androidboot."}.size(),
        argument.find('=') - std::string_view {"androidboot."}.size());
    require("ro.boot." + name == "ro.boot.kernarium.high_refresh");
    /* The other bits never make it, and without the bit no argument is
     * composed: a 60 Hz host's command line is what it was. */
    for (const std::uint32_t bit : {KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS,
                                    KERNARIUM_RUNTIME_ANDROID_HOST_ORIENTATION,
                                    KERNARIUM_RUNTIME_ANDROID_HOST_COMPASS}) {
        for (const std::string& other : kernelArguments(bit))
            require(!other.starts_with("androidboot.kernarium.high_refresh="));
    }
    require(kernelArguments(0).empty());
    require(kernelArguments(KERNARIUM_RUNTIME_ANDROID_HOST_HIGH_REFRESH |
                            KERNARIUM_RUNTIME_ANDROID_HOST_COMPASS) ==
            std::vector<std::string> {"androidboot.kernarium.compass=1",
                                      "androidboot.kernarium.high_refresh=1"});
}

void everyKnownBitMakesOneArgumentInTableOrder()
{
    const auto arguments = kernelArguments(knownBits());
    require(arguments.size() == std::size(features));
    for (std::size_t row = 0; row < arguments.size(); ++row) {
        require(arguments[row] ==
                std::string {features[row].argumentPrefix} +
                    std::string {features[row].argumentValue});
    }
    /* A bit alone makes its own argument and no other. */
    for (const Feature& feature : features) {
        const auto alone = kernelArguments(feature.bit);
        require(alone.size() == 1);
        require(alone.front().starts_with(feature.argumentPrefix));
    }
}

void bitsTheRuntimeHasNoRowForAreReportedAndMakeNothing()
{
    require(unknownBits(0) == 0);
    require(unknownBits(knownBits()) == 0);
    constexpr std::uint32_t beyond = 1U << 31;
    require(unknownBits(beyond | KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS) ==
            beyond);
    require(kernelArguments(beyond).empty());
    /* The unknown part does not change what the known part makes. */
    require(kernelArguments(beyond | KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS) ==
            kernelArguments(KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS));
}

void aReleaseCommandLineCannotSetAFeatureArgument()
{
    const uml::testing::ScratchSubdirectory scratch {"host-features-XXXXXX"};
    const auto path = scratch.path() / "command-line.txt";
    const auto prefixes = launcherPrefixes();
    require(prefixes.size() == std::size(features));

    writeText(path, "quiet " + std::string {bootDevices} + "\n");
    require(uml::android::readKernelCommandLine(path, prefixes) ==
            std::vector<std::string> {"quiet", std::string {bootDevices}});
    for (const Feature& feature : features) {
        const std::array<std::string_view, 2> values {
            feature.argumentValue, "other"};
        for (const std::string_view value : values) {
            writeText(path, "quiet " + std::string {feature.argumentPrefix} +
                                std::string {value} + " " +
                                std::string {bootDevices} + "\n");
            requireThrows<std::exception>([&] {
                (void)uml::android::readKernelCommandLine(path, prefixes);
            });
        }
    }
}

void theSkuPrefixIsTheOneTheGuestImportsFromTheOdmAxis()
{
    /* Android reads ro.boot.product.hardware.sku for the ODM manifest of the
     * SKU, and Build.ODM_SKU shows the value to applications. */
    require(features[0].name == "haptics");
    require(features[0].argumentPrefix ==
            "androidboot.product.hardware.sku=");
    require(features[0].argumentValue == "haptics");
}

} // namespace

int main()
{
    static constexpr uml::testing::NamedTest tests[] = {
        {"every-bit-is-one-bit-of-its-own", everyBitIsOneBitOfItsOwn},
        {"a-collision-between-two-rows-is-detected",
         aCollisionBetweenTwoRowsIsDetected},
        {"the-haptics-bit-becomes-the-sku-argument",
         theHapticsBitBecomesTheSkuArgument},
        {"the-orientation-bit-becomes-the-argument-the-sub-hal-reads",
         theOrientationBitBecomesTheArgumentTheSubHalReads},
        {"the-compass-bit-becomes-the-argument-the-sub-hal-will-read",
         theCompassBitBecomesTheArgumentTheSubHalWillRead},
        {"the-high-refresh-bit-becomes-the-argument-the-guests-scheduler-will-read",
         theHighRefreshBitBecomesTheArgumentTheGuestsSchedulerWillRead},
        {"every-known-bit-makes-one-argument-in-table-order",
         everyKnownBitMakesOneArgumentInTableOrder},
        {"bits-the-runtime-has-no-row-for-are-reported-and-make-nothing",
         bitsTheRuntimeHasNoRowForAreReportedAndMakeNothing},
        {"a-release-command-line-cannot-set-a-feature-argument",
         aReleaseCommandLineCannotSetAFeatureArgument},
        {"the-sku-prefix-is-the-one-the-guest-imports-from-the-odm-axis",
         theSkuPrefixIsTheOneTheGuestImportsFromTheOdmAxis},
    };
    return uml::testing::runNamedTests(tests);
}
