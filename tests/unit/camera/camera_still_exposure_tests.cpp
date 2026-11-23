// SPDX-License-Identifier: GPL-2.0-or-later
// The exposure a still reports, from a photo's metadata. The metadata is a
// literal dictionary: nothing here opens a camera.
#include "photo_metadata.hpp"
#include "require.hpp"
#include "still_exposure.hpp"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

using namespace uml::platform;
using uml::testing::require;

namespace {

// An owned CoreFoundation object, released with the scope.
template <class Ref>
class Owned {
public:
    explicit Owned(Ref ref) : ref_(ref) {}
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;
    ~Owned() { if (ref_ != nullptr) CFRelease(ref_); }
    [[nodiscard]] Ref get() const { return ref_; }
private:
    Ref ref_;
};

Owned<CFNumberRef> real(double value) { return Owned<CFNumberRef>(CFNumberCreate(nullptr, kCFNumberDoubleType, &value)); }
Owned<CFNumberRef> integer(std::int64_t value) { return Owned<CFNumberRef>(CFNumberCreate(nullptr, kCFNumberSInt64Type, &value)); }

using Entries = std::vector<std::pair<CFStringRef, CFTypeRef>>;

Owned<CFDictionaryRef> dictionary(const Entries& entries)
{
    std::vector<const void*> keys, values;
    for (const auto& [key, value] : entries) {
        keys.push_back(key);
        values.push_back(value);
    }
    return Owned<CFDictionaryRef>(CFDictionaryCreate(nullptr, keys.data(), values.data(), CFIndex(keys.size()),
                                                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
}

Owned<CFArrayRef> array(std::initializer_list<CFTypeRef> values)
{
    std::vector<const void*> items(values.begin(), values.end());
    return Owned<CFArrayRef>(CFArrayCreate(nullptr, items.data(), CFIndex(items.size()), &kCFTypeArrayCallBacks));
}

// What a device says of itself when a photo is delivered: a live preview at
// 1/30 s and ISO 400, an f/1.5 lens, a converged exposure and a lens that
// is a zoomed 2x.
CameraExposure device()
{
    CameraExposure exposure;
    exposure.exposureNanoseconds = 33'333'333;
    exposure.frameDurationNanoseconds = 33'333'333;
    exposure.iso = 400;
    exposure.whiteBalanceGains = {1.9f, 1.0f, 1.6f};
    exposure.colorTemperature = 5200;
    exposure.ae = CameraControlState::converged;
    exposure.af = CameraControlState::locked;
    exposure.awb = CameraControlState::converged;
    exposure.zoomRatio = 2.0f;
    exposure.activeCamera = 3;
    exposure.aperture = 1.5f;
    return exposure;
}

void aTypicalPhotoReportsItsOwnExposure()
{
    PhotoReadings readings;
    readings.exposureSeconds = 1.0 / 120.0;
    readings.iso = 64;
    readings.fNumber = 1.78;
    readings.focalLengthMillimetres = 6.86;
    readings.flash = 1;
    const auto exposure = stillExposureOf(device(), readings, std::nullopt, {});
    require(exposure.exposureNanoseconds == 8'333'333, "the photo's exposure time, not the preview's");
    require(exposure.iso == 64);
    require(std::fabs(exposure.aperture - 1.78f) < 1e-6f, "the photo's f-number");
    require(std::fabs(exposure.focalLengthMillimetres - 6.86f) < 1e-6f);
    require(exposure.flashFired, "Exif Flash bit 0 is set");
    // What the device states of itself is the still's.
    require(exposure.ae == CameraControlState::converged && exposure.af == CameraControlState::locked);
    require(exposure.zoomRatio == 2.0f && exposure.activeCamera == 3 && exposure.colorTemperature == 5200);
    require(exposure.whiteBalanceGains[0] == 1.9f && exposure.frameDurationNanoseconds == 33'333'333);
    // A JPEG has no neutral point or noise profile of its own.
    for (const float value : exposure.neutralPoint) require(value == 0.0f);
    for (const float value : exposure.noiseProfile) require(value == 0.0f);
}

void aMissingReadingIsTheLensesFixedValueOrNoneNeverThePreviews()
{
    const auto exposure = stillExposureOf(device(), {}, std::nullopt, {});
    require(exposure.exposureNanoseconds == 0, "the preview's 1/30 s is not the photo's");
    require(exposure.iso == 0, "the preview's ISO is not the photo's");
    require(exposure.focalLengthMillimetres == 0.0f, "none from the photo: the camera device states the lens's fixed focal length");
    require(exposure.aperture == 1.5f, "the lens's fixed aperture when the photo names none");
    require(!exposure.flashFired);
    // A lens that does not state its aperture has none.
    auto unknown = device();
    unknown.aperture = 0.0f;
    require(stillExposureOf(unknown, {}, std::nullopt, {}).aperture == 0.0f);
    unknown.aperture = std::numeric_limits<float>::quiet_NaN();
    require(stillExposureOf(unknown, {}, std::nullopt, {}).aperture == 0.0f);
}

void aReadingThatCannotBeAMeasurementIsAbsent()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    for (const double bad : {0.0, -1.0, nan, infinity, -infinity, 1e300}) {
        PhotoReadings readings;
        readings.exposureSeconds = bad;
        readings.iso = bad;
        readings.fNumber = bad;
        readings.focalLengthMillimetres = bad;
        const auto exposure = stillExposureOf(device(), readings, std::nullopt, {});
        require(exposure.exposureNanoseconds == 0, "no exposure time");
        require(exposure.iso == 0, "no sensitivity");
        require(exposure.aperture == 1.5f, "the lens's fixed aperture, since the photo's f-number is not one");
        require(exposure.focalLengthMillimetres == 0.0f);
    }
    // The longest exposure and the sensitivities at the ends of the range are read.
    PhotoReadings extreme;
    extreme.exposureSeconds = 3600.0;
    extreme.iso = 1'000'000.0;
    auto exposure = stillExposureOf(device(), extreme, std::nullopt, {});
    require(exposure.exposureNanoseconds == 3'600'000'000'000ull && exposure.iso == 1'000'000u, "the ends of the range");
    extreme.iso = 1.0;
    require(stillExposureOf(device(), extreme, std::nullopt, {}).iso == 1u);
    // A sensitivity outside 1 to 1,000,000 is no measurement: the guest's HAL
    // holds it as a signed 32-bit number, and no sensor has less than ISO 1.
    for (const double outside : {0.5, 0.999, 1'000'001.0, 4.0e9, 2.2e9}) {
        extreme.iso = outside;
        require(stillExposureOf(device(), extreme, std::nullopt, {}).iso == 0, "an ISO outside 1 to 1,000,000 is none");
    }
    extreme.exposureSeconds = 3601.0;
    require(stillExposureOf(device(), extreme, std::nullopt, {}).exposureNanoseconds == 0, "over an hour is none");
}

void theFlashIsTheMetadatasWordThenThePlatformsDecision()
{
    PhotoReadings fired, notFired, silent;
    fired.flash = 0x19;     // fired, compulsory, red-eye off
    notFired.flash = 0x10;  // compulsory off: did not fire
    require(stillExposureOf(device(), fired, false, {}).flashFired, "the metadata says it fired");
    require(!stillExposureOf(device(), notFired, true, {}).flashFired, "the metadata says it did not, whatever was planned");
    require(stillExposureOf(device(), silent, true, {}).flashFired, "no word in the metadata: the resolved settings decide");
    require(!stillExposureOf(device(), silent, false, {}).flashFired);
    require(!stillExposureOf(device(), silent, std::nullopt, {}).flashFired, "no word anywhere: not fired");

    // An Exif Flash field is 16 bits: a negative or wider number is not one,
    // says nothing, and leaves the decision to the resolved settings.
    PhotoReadings negative, wide, last, zero;
    negative.flash = -1;
    wide.flash = 0x10001;
    last.flash = 0xffff;
    zero.flash = 0;
    require(!stillExposureOf(device(), negative, false, {}).flashFired && stillExposureOf(device(), negative, true, {}).flashFired,
            "a negative Flash value says nothing");
    require(!stillExposureOf(device(), wide, false, {}).flashFired && stillExposureOf(device(), wide, true, {}).flashFired,
            "nor does a Flash value wider than 16 bits");
    require(stillExposureOf(device(), last, false, {}).flashFired, "the widest Flash field is read");
    require(!stillExposureOf(device(), zero, true, {}).flashFired, "0 is the metadata's word that it did not fire");
}

void aBayerStillCarriesItsOwnNeutralAndNoise()
{
    DngExposureData dng;
    dng.neutralPoint = std::array<float, 3> {0.5f, 1.0f, 0.75f};
    dng.noiseProfile = std::array<float, 8> {1e-3f, 2e-6f, 2e-3f, 3e-6f, 2e-3f, 3e-6f, 4e-3f, 5e-6f};
    const auto exposure = stillExposureOf(device(), {}, std::nullopt, dng);
    require(exposure.neutralPoint == *dng.neutralPoint && exposure.noiseProfile == *dng.noiseProfile);
    // A DNG that states one and not the other leaves the other absent.
    dng.noiseProfile.reset();
    const auto half = stillExposureOf(device(), {}, std::nullopt, dng);
    require(half.neutralPoint[0] == 0.5f);
    for (const float value : half.noiseProfile) require(value == 0.0f, "no noise profile is not a flat one");
}

void theDictionaryGivesItsReadings()
{
    const auto exposureTime = real(1.0 / 250.0), fNumber = real(2.2), focal = real(5.1), iso100 = real(100), iso200 = real(200);
    const auto flash = integer(16);
    const auto isos = array({iso100.get(), iso200.get()});
    const auto exif = dictionary({{kCGImagePropertyExifExposureTime, exposureTime.get()},
                                  {kCGImagePropertyExifISOSpeedRatings, isos.get()},
                                  {kCGImagePropertyExifFNumber, fNumber.get()},
                                  {kCGImagePropertyExifFocalLength, focal.get()},
                                  {kCGImagePropertyExifFlash, flash.get()}});
    const auto metadata = dictionary({{kCGImagePropertyExifDictionary, exif.get()}});
    const auto readings = photoReadingsOf(metadata.get());
    require(readings.exposureSeconds && std::fabs(*readings.exposureSeconds - 0.004) < 1e-12);
    require(readings.iso && *readings.iso == 100.0, "the first of the ratings");
    require(readings.fNumber && std::fabs(*readings.fNumber - 2.2) < 1e-12);
    require(readings.focalLengthMillimetres && std::fabs(*readings.focalLengthMillimetres - 5.1) < 1e-12);
    require(readings.flash && *readings.flash == 16);
    const auto exposure = stillExposureOf(device(), readings, std::nullopt, {});
    require(exposure.exposureNanoseconds == 4'000'000 && exposure.iso == 100 && !exposure.flashFired);
}

void aDictionaryThatLacksKeysOrHoldsOtherTypesGivesNothing()
{
    require(!photoReadingsOf(nullptr).exposureSeconds);
    const auto empty = dictionary({});
    const auto none = photoReadingsOf(empty.get());
    require(!none.exposureSeconds && !none.iso && !none.fNumber && !none.focalLengthMillimetres && !none.flash);

    // An {Exif} dictionary with no keys, with strings and an empty rating list.
    const auto text = Owned<CFStringRef>(CFStringCreateWithCString(nullptr, "1/60", kCFStringEncodingUTF8));
    const auto noRatings = array({});
    const auto exif = dictionary({{kCGImagePropertyExifExposureTime, text.get()},
                                  {kCGImagePropertyExifISOSpeedRatings, noRatings.get()},
                                  {kCGImagePropertyExifFNumber, text.get()}});
    const auto metadata = dictionary({{kCGImagePropertyExifDictionary, exif.get()}});
    const auto odd = photoReadingsOf(metadata.get());
    require(!odd.exposureSeconds && !odd.iso && !odd.fNumber && !odd.flash, "a string is not a number, an empty list has no rating");

    // A Flash number that is not an integer is no Exif Flash.
    const auto fraction = real(1.5);
    const auto fractional = dictionary({{kCGImagePropertyExifFlash, fraction.get()}});
    const auto fractionalMetadata = dictionary({{kCGImagePropertyExifDictionary, fractional.get()}});
    require(!photoReadingsOf(fractionalMetadata.get()).flash, "a non-integer Flash is none");
    const auto negativeFlash = integer(-1);
    const auto negativeExif = dictionary({{kCGImagePropertyExifFlash, negativeFlash.get()}});
    const auto negativeMetadata = dictionary({{kCGImagePropertyExifDictionary, negativeExif.get()}});
    const auto passed = photoReadingsOf(negativeMetadata.get()).flash;
    require(passed && *passed == -1, "a negative integer is passed on for stillExposureOf to judge");

    // {Exif} that is not a dictionary.
    const auto wrong = dictionary({{kCGImagePropertyExifDictionary, text.get()}});
    require(!photoReadingsOf(wrong.get()).exposureSeconds);
}

void zeroAndNonFiniteNumbersInTheDictionaryEndAsAbsent()
{
    const auto zero = real(0.0), negative = real(-0.5), nan = real(std::numeric_limits<double>::quiet_NaN());
    const auto infinity = real(std::numeric_limits<double>::infinity());
    for (const auto* bad : {&zero, &negative, &nan, &infinity}) {
        const auto isos = array({bad->get()});
        const auto exif = dictionary({{kCGImagePropertyExifExposureTime, bad->get()},
                                      {kCGImagePropertyExifISOSpeedRatings, isos.get()},
                                      {kCGImagePropertyExifFNumber, bad->get()},
                                      {kCGImagePropertyExifFocalLength, bad->get()}});
        const auto metadata = dictionary({{kCGImagePropertyExifDictionary, exif.get()}});
        const auto exposure = stillExposureOf(device(), photoReadingsOf(metadata.get()), std::nullopt, {});
        require(exposure.exposureNanoseconds == 0 && exposure.iso == 0 && exposure.focalLengthMillimetres == 0.0f,
            "the dictionary's bad numbers are not measurements");
        require(exposure.aperture == 1.5f);
    }
}

// A two-by-two JPEG that carries `properties`, made by ImageIO in memory.
std::vector<std::uint8_t> encodedWith(CFDictionaryRef properties)
{
    const auto space = Owned<CGColorSpaceRef>(CGColorSpaceCreateDeviceGray());
    std::uint8_t pixels[4] = {0, 85, 170, 255};
    const auto context = Owned<CGContextRef>(
        CGBitmapContextCreate(pixels, 2, 2, 8, 2, space.get(), kCGImageAlphaNone));
    const auto image = Owned<CGImageRef>(CGBitmapContextCreateImage(context.get()));
    const auto data = Owned<CFMutableDataRef>(CFDataCreateMutable(nullptr, 0));
    const auto destination = Owned<CGImageDestinationRef>(
        CGImageDestinationCreateWithData(data.get(), CFSTR("public.jpeg"), 1, nullptr));
    CGImageDestinationAddImage(destination.get(), image.get(), properties);
    require(CGImageDestinationFinalize(destination.get()), "ImageIO writes the JPEG");
    const auto* bytes = CFDataGetBytePtr(data.get());
    return {bytes, bytes + CFDataGetLength(data.get())};
}

void anEncodedPhotoGivesItsExif()
{
    const auto exposureTime = real(0.008), fNumber = real(1.8), focal = real(6.0), iso = integer(200);
    const auto flash = integer(1);
    const auto isos = array({iso.get()});
    const auto exif = dictionary({{kCGImagePropertyExifExposureTime, exposureTime.get()},
                                  {kCGImagePropertyExifISOSpeedRatings, isos.get()},
                                  {kCGImagePropertyExifFNumber, fNumber.get()},
                                  {kCGImagePropertyExifFocalLength, focal.get()},
                                  {kCGImagePropertyExifFlash, flash.get()}});
    const auto properties = dictionary({{kCGImagePropertyExifDictionary, exif.get()}});
    const auto readings = photoReadingsOfImage(encodedWith(properties.get()));
    require(readings.exposureSeconds && std::fabs(*readings.exposureSeconds - 0.008) < 1e-6);
    require(readings.iso && *readings.iso == 200.0);
    require(readings.fNumber && std::fabs(*readings.fNumber - 1.8) < 1e-3);
    require(readings.focalLengthMillimetres && std::fabs(*readings.focalLengthMillimetres - 6.0) < 1e-3);
    require(readings.flash && (*readings.flash & 1) == 1);
    const auto exposure = stillExposureOf(device(), readings, std::nullopt, {});
    require(exposure.exposureNanoseconds == 8'000'000 && exposure.iso == 200 && exposure.flashFired);
}

void anImageWithoutExifAndBytesWithoutAnImageGiveNothing()
{
    const auto bare = encodedWith(nullptr);
    const auto readings = photoReadingsOfImage(bare);
    require(!readings.exposureSeconds && !readings.iso && !readings.fNumber && !readings.flash);
    const std::vector<std::uint8_t> noise(64, 0x5a);
    require(!photoReadingsOfImage(noise).exposureSeconds, "not an image");
    require(!photoReadingsOfImage({}).exposureSeconds, "no bytes");
}

} // namespace

int main()
{
    aTypicalPhotoReportsItsOwnExposure();
    aMissingReadingIsTheLensesFixedValueOrNoneNeverThePreviews();
    aReadingThatCannotBeAMeasurementIsAbsent();
    theFlashIsTheMetadatasWordThenThePlatformsDecision();
    aBayerStillCarriesItsOwnNeutralAndNoise();
    theDictionaryGivesItsReadings();
    aDictionaryThatLacksKeysOrHoldsOtherTypesGivesNothing();
    zeroAndNonFiniteNumbersInTheDictionaryEndAsAbsent();
    anEncodedPhotoGivesItsExif();
    anImageWithoutExifAndBytesWithoutAnImageGiveNothing();
    return 0;
}
