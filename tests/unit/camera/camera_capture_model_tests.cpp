// SPDX-License-Identifier: GPL-2.0-or-later
// The anchor model of photo_dimensions.hpp: the one format a camera runs in,
// what it offers, the rates it takes, and the session's transactions. Every
// reviewer counter-example of the earlier choosers is a named test built from
// the reviewer's table, and the invariants hold over generated tables (the seed
// is in a failure's message).
#include "front_tables.hpp"
#include "photo_dimensions.hpp"
#include "require.hpp"
#include "video_stabilization.hpp"

#include <Block.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace uml::platform;
using uml::testing::require;

namespace {

using Sizes = std::vector<CameraStillSize>;
using Spans = std::vector<CameraRateSpan>;

VideoFormatFacts fmt(std::uint32_t width, std::uint32_t height, Sizes photos, Spans rates = {{1, 30}}, bool full = true,
                     bool multi = false)
{
    VideoFormatFacts facts;
    facts.width = width;
    facts.height = height;
    facts.frameRates = std::move(rates);
    facts.photoSizes = std::move(photos);
    facts.fullRange = full;
    facts.multiCamera = multi;
    return facts;
}

constexpr std::pair<std::uint32_t, std::uint32_t> standardSizes[] = {
    {176, 144}, {320, 240}, {640, 480}, {1280, 720}, {1920, 1080}, {3840, 2160},
};

// The sizes a camera's video is cut from: the standard sizes and the native sizes
// of its formats (candidateVideoSizes in the device).
std::vector<FrameSize> candidatesFor(const std::vector<VideoFormatFacts>& formats)
{
    std::vector<FrameSize> sizes;
    for (const auto& [width, height] : standardSizes) sizes.push_back({width, height});
    for (const auto& format : formats)
        if (std::find(sizes.begin(), sizes.end(), FrameSize {format.width, format.height}) == sizes.end())
            sizes.push_back({format.width, format.height});
    return sizes;
}

// The capture buffer the reserve counts, by hand: rows padded to 256 bytes, so a
// 1920-wide row is 2048 bytes.
constexpr std::uint32_t rowAlignment = 256;
constexpr std::uint64_t smallFrameBound = 2048ull * 1440 * 3 / 2;
constexpr std::uint64_t largeFrameBound = 3840ull * 2880 * 3 / 2;

// The small tier: 1080p video, 12 MP stills (12.7 million pixels), and the
// frame of 1920x1440 at most.
OfferRules smallRules(const std::vector<VideoFormatFacts>& formats)
{
    OfferRules rules;
    rules.limits = {12'700'000, smallFrameBound, rowAlignment};
    rules.maximumVideoWidth = 1920;
    rules.maximumVideoHeight = 1080;
    rules.candidateSizes = candidatesFor(formats);
    return rules;
}

OfferRules largeRules(const std::vector<VideoFormatFacts>& formats)
{
    OfferRules rules;
    rules.limits = {50'000'000, largeFrameBound, rowAlignment};
    rules.maximumVideoWidth = 3840;
    rules.maximumVideoHeight = 2160;
    rules.candidateSizes = candidatesFor(formats);
    return rules;
}

// ---------------------------------------------------------------------------
// Rates
// ---------------------------------------------------------------------------

void aSpansWholeRatesSnapToTheNearWholeNumber()
{
    const auto whole = [](double low, double high) { return wholeRatesOf({low, high}); };
    require(whole(29.97, 29.97) == std::optional<WholeRates>(WholeRates {30, 30}), "29.97 is 30");
    require(whole(30.00003, 30.00003) == std::optional<WholeRates>(WholeRates {30, 30}), "30.00003 is 30");
    require(whole(23.976, 23.976) == std::optional<WholeRates>(WholeRates {24, 24}), "23.976 is 24");
    require(whole(14.99, 14.99) == std::optional<WholeRates>(WholeRates {15, 15}), "14.99 is 15");
    require(whole(1, 30) == std::optional<WholeRates>(WholeRates {1, 30}), "a plain range keeps its ends");
    require(whole(29.97, 59.94) == std::optional<WholeRates>(WholeRates {30, 60}), "both ends snap");
    require(whole(0.5, 30) == std::optional<WholeRates>(WholeRates {1, 30}), "a range from below one starts at one");
    require(whole(1.0005, 30) == std::optional<WholeRates>(WholeRates {1, 30}), "a hair above one is one");
    require(whole(7.5, 7.9) == std::nullopt, "a span between two whole numbers holds none");
    require(whole(7.5, 8.2) == std::optional<WholeRates>(WholeRates {8, 8}), "but one that spans a whole number holds it");
    require(whole(29.9, 29.9) == std::nullopt, "29.9 is not 30: it is a third of a percent away");
    require(whole(30, 20) == std::nullopt, "an inverted span holds none");
    require(whole(std::nan(""), 30) == std::nullopt && whole(1, std::nan("")) == std::nullopt, "no number, no rate");
    require(whole(0, 0) == std::nullopt, "a span of nothing");
    require(whole(1, 1e12)->maximum == 4'000'000'000u, "an absurd rate is bounded");
}

void aRateAHairOutsideASpanIsProgrammedFromTheSpansOwnDuration()
{
    const auto facts = fmt(1920, 1080, {}, {{29.97, 29.97}, {1, 15}, {30.00003, 30.00003}});
    const auto up = rateSettingFor(facts, 30);
    require(up && up->span == 0 && up->duration == FrameDuration::spanShortest,
            "30 on a 29.97 span is the span's shortest duration");
    require(std::fabs(programmedFrameDuration(facts.frameRates[0], up->duration, 30) - 1.0 / 29.97) < 1e-12, "which is 1 / 29.97");
    const auto down = rateSettingFor(fmt(1920, 1080, {}, {{30.00003, 30.00003}}), 30);
    require(down && down->span == 0 && down->duration == FrameDuration::spanLongest,
            "30 on a 30.00003 span is the span's longest duration");
    const auto inside = rateSettingFor(facts, 10);
    require(inside && inside->span == 1 && inside->duration == FrameDuration::reciprocal, "a rate inside a span is its reciprocal");
    require(!rateSettingFor(facts, 20), "no span holds 20");
    require(!rateSettingFor(fmt(1920, 1080, {}, {}), 30), "a format with no span takes no rate");
    // An exact span is preferred to a snapped one.
    const auto both = rateSettingFor(fmt(1920, 1080, {}, {{1, 29.97}, {30, 60}}), 30);
    require(both && both->span == 1 && both->duration == FrameDuration::reciprocal, "the span that holds 30 exactly wins");
}

void theOfferedRatesAreWholeRatesUpToTheCap()
{
    const auto rates = [](Spans spans, std::uint32_t cap = 30) { return offeredRatesOf(fmt(1920, 1080, {}, std::move(spans)), cap); };
    require(rates({{1, 60}}) == std::vector<WholeRates> {{1, 30}}, "a range is clipped at the cap");
    require(rates({{1, 10}, {50, 60}}) == std::vector<WholeRates> {{1, 10}}, "a span above the cap is not offered");
    require(rates({{24, 24}, {1, 15}, {15, 24}}) == std::vector<WholeRates> {{1, 24}}, "overlapping and adjacent ranges merge");
    require(rates({{29.97, 29.97}, {23.976, 23.976}, {14.99, 14.99}}) == std::vector<WholeRates> {{15, 15}, {24, 24}, {30, 30}},
            "near-integer rates are whole rates");
    require(rates({{60, 60}, {120, 120}}) == std::vector<WholeRates> {{60, 60}}, "a format with whole rates only above the cap offers its slowest");
    require(rates({{7.5, 7.9}}).empty(), "a format with no whole rate offers none");
    require(rates({{1, 60}}, 0) == std::vector<WholeRates> {{1, 60}}, "a cap of zero is none");
}

void aPressureCapChoosesTheLargestOfferedRateBelowIt()
{
    const std::vector<WholeRates> offered {{1, 10}, {24, 24}, {30, 30}};
    require(throttledRate(offered, 30, 0) == 30, "no cap");
    require(throttledRate(offered, 30, 20) == 10, "20 is not offered: the largest below it");
    require(throttledRate(offered, 30, 24) == 24, "an offered rate");
    require(throttledRate(offered, 15, 0) == 10, "a request between two ranges");
    require(throttledRate(offered, 30, 15) == 10, "a cap of 15");
    require(throttledRate(offered, 60, 0) == 30, "above every range");
    const std::vector<WholeRates> fast {{120, 120}};
    require(throttledRate(fast, 30, 15) == 120, "nothing offered below the cap: the slowest offered");
    require(throttledRate({}, 30, 0) == 0, "nothing offered, nothing to run");
    require(throttledRate(offered, 30, 99) == 30, "a cap above the request does nothing");
}

// ---------------------------------------------------------------------------
// Photos
// ---------------------------------------------------------------------------

void aFormatsPhotoMaximumIsTheLargestSizeWithinTheCap()
{
    const auto phone = fmt(4032, 3024, {{4032, 3024}, {8064, 6048}, {2016, 1512}});
    require(photoMaximum(phone, 12'700'000) == std::optional<CameraStillSize>(CameraStillSize {4032, 3024}),
            "a 48 MP size is not the maximum under a 12 MP cap");
    require(photoMaximum(phone, 0) == std::optional<CameraStillSize>(CameraStillSize {8064, 6048}), "without a cap it is");
    require(!photoMaximum(fmt(1920, 1080, {{8064, 6048}}), 12'700'000), "no size within the cap, no maximum");
    require(!photoMaximum(fmt(1920, 1080, {}), 0) && !photoMaximum(fmt(1920, 1080, {{0, 0}, {4032, 0}}), 0), "no usable size");
    // Equal areas order by width.
    require(photoMaximum(fmt(1, 1, {{3000, 2000}, {2000, 3000}}), 0) == std::optional<CameraStillSize>(CameraStillSize {3000, 2000}),
            "the wider of two sizes of one area");
}

void theStillsAFormatMakesAreWithinTheMaximumInBothDimensions()
{
    const auto format = fmt(4032, 3024, {{4032, 3024}, {4224, 2376}, {1920, 1080}, {1920, 1080}, {0, 0}});
    const auto stills = stillsOf(format, 12'700'000);
    require(stills == Sizes({{4032, 3024}, {1920, 1080}}),
            "4224x2376 is wider than the maximum, the duplicate and the empty size are not sizes");
    require(formatMakesPhoto(format, {4032, 3024}, 12'700'000) && formatMakesPhoto(format, {1920, 1080}, 12'700'000), "made");
    require(!formatMakesPhoto(format, {4224, 2376}, 12'700'000), "listed, but wider than the output's maximum");
    require(!formatMakesPhoto(format, {2016, 1512}, 12'700'000), "not listed");
    require(stillsOf(fmt(1, 1, {}), 0).empty(), "a format listing none makes none");
}

void theSizesAnyFormatMakesAreTheUnionLargestFirst()
{
    const std::vector<VideoFormatFacts> formats {fmt(1920, 1440, {{1920, 1440}, {4032, 3024}}), fmt(1920, 1080, {{1920, 1080}, {2016, 1134}})};
    const auto all = stillsMadeByAnyFormat(formats, 12'700'000);
    require(all == Sizes({{4032, 3024}, {1920, 1440}, {2016, 1134}, {1920, 1080}}), "the union, by area then width");
}

void theOutputSizesFollowTheAnchor()
{
    const auto none = outputSizesFor(fmt(1920, 1080, {}), 1920, 1080, 12'700'000);
    require(!none.frame && !none.photoMaximum, "the format's own size and no photo size to set");
    const auto scaled = outputSizesFor(fmt(1920, 1080, {{1920, 1080}}), 640, 480, 12'700'000);
    require(scaled.frame == std::optional<FrameSize>(FrameSize {864, 486}) && scaled.photoMaximum == CameraStillSize({1920, 1080}),
            "864x486 and the format's largest");
    require(!outputSizesFor(fmt(1920, 1080, {}), 0, 0, 0).frame, "a camera with no stream has no video output to size");
    const auto phone = outputSizesFor(fmt(4032, 3024, {{4032, 3024}, {8064, 6048}}), 1920, 1080, 12'700'000);
    require(phone.photoMaximum == CameraStillSize({4032, 3024}), "the output is capped at 12 MP: no 48 MP pipeline is prepared");
}

// ---------------------------------------------------------------------------
// The anchor: each counter-example the reviewers built, as a table
// ---------------------------------------------------------------------------

struct Chosen {
    std::size_t index {};
    CaptureOffer offer;
};
Chosen anchorOf(const std::vector<VideoFormatFacts>& formats, bool multiOnly = false, bool large = false)
{
    const OfferRules rules = large ? largeRules(formats) : smallRules(formats);
    const auto index = chooseAnchor(formats, rules, multiOnly);
    require(index.has_value(), "the table has an anchor");
    return {*index, offerOf(formats[*index], rules)};
}

const std::vector<VideoFormatFacts> tabletFormats {
    fmt(1440, 1080, {{1440, 1080}, {2016, 1512}}, {{1, 30}}, true, true),
    fmt(1920, 1440, {{1920, 1440}, {4032, 3024}}, {{1, 30}}, true, true),
    fmt(4032, 3024, {{4032, 3024}, {4224, 2376}}, {{1, 30}}, true, false),
    fmt(1920, 1080, {{1920, 1080}, {2016, 1134}}, {{1, 60}}, true, true),
};

// R1, the tablet fallback (recheck3): a stream of every size runs in 1920x1440 at
// every rate, and the 12 MP photo and the 1920x1440 one are made in every run.
// 4224x2376, which only the 4032x3024 format lists, is not offered.
void theTabletRunsInTheFormatThatMakesTheLargestStillAtTheSmallestCost()
{
    const auto chosen = anchorOf(tabletFormats);
    require(chosen.index == 1, "1920x1440: K2 ties it with the 4032x3024 format, K4 takes the smaller");
    require(chosen.offer.stills == Sizes({{4032, 3024}, {1920, 1440}}), "4224x2376 is never offered");
    require(chosen.offer.videoSizes.front() == FrameSize({1920, 1080}) && chosen.offer.box == FrameSize({1920, 1080}), "video up to 1080p");
    require(chosen.offer.rates == std::vector<WholeRates> {{1, 30}}, "the anchor's rates, to the cap");
    const auto multi = anchorOf(tabletFormats, true);
    require(multi.index == 1, "the same among the formats a multi-camera session takes");
}

// R1 (recheck3, interior rate): 4K lists 12 MP at 1-10 and 50-60 fps only.
void aFormatThatHasNoRateWithinTheCapLosesToOneThatDoes()
{
    const std::vector<VideoFormatFacts> formats {fmt(1920, 1080, {{1920, 1080}}, {{1, 60}}),
                                                 fmt(3840, 2160, {{4032, 3024}}, {{1, 10}, {50, 60}})};
    const auto chosen = anchorOf(formats);
    require(chosen.index == 0, "30 fps beats 10 (K1 before K2)");
    require(chosen.offer.stills == Sizes({{1920, 1080}}) && chosen.offer.rates == std::vector<WholeRates> {{1, 30}},
            "its stills and its rates to the cap");
}

// R1 (recheck4, combined request): 1600x900 and 1440x1080 make 12 MP, 1600x1200
// only 1080p. A stream of 1600x900 and one of 1440x1080 together asked for
// 1600x1080 in the old model, a size no format made 12 MP in.
void everyAdmittedStreamSetFitsInsideTheAnchor()
{
    const std::vector<VideoFormatFacts> formats {fmt(1600, 900, {{4032, 3024}}), fmt(1440, 1080, {{4032, 3024}}), fmt(1600, 1200, {{1920, 1080}})};
    const auto chosen = anchorOf(formats);
    require(chosen.index == 1, "1440x1080: its box is larger than 1600x900's (K3)");
    require(chosen.offer.stills == Sizes({{4032, 3024}}), "12 MP");
    for (const auto& size : chosen.offer.videoSizes)
        require(size.width <= 1440 && size.height <= 1080, "1600x900 is not offered, so 1600x1080 cannot be asked for");
}

// R1 (recheck4, under-offer): 1920x1440 at exactly 30 fps makes 12 MP; the
// 1080p format at 1-30 makes 1080p only. Every offered run can use the first.
void aFormatAtExactlyTheCapStillOffersItsPhotos()
{
    const std::vector<VideoFormatFacts> formats {fmt(1920, 1440, {{4032, 3024}}, {{30, 30}}), fmt(1920, 1080, {{1920, 1080}}, {{1, 30}})};
    const auto chosen = anchorOf(formats);
    require(chosen.index == 0 && chosen.offer.stills == Sizes({{4032, 3024}}), "12 MP is offered");
    require(chosen.offer.rates == std::vector<WholeRates> {{30, 30}} && chosen.offer.videoSizes.front() == FrameSize({1920, 1080}),
            "at 30 fps and up to 1080p: the rates below 30 are given up");
}

// R4 (a) (recheck4): a small format makes 12 MP and a larger one lists 12 and
// 48 MP. The camera runs only the anchor, and the photo output is capped.
void aCameraNeverRunsInAFormatLargerThanItsAnchor()
{
    const std::vector<VideoFormatFacts> formats {fmt(4032, 3024, {{4032, 3024}, {8064, 6048}}), fmt(1920, 1440, {{4032, 3024}})};
    const auto chosen = anchorOf(formats);
    require(chosen.index == 1, "K4 takes the smaller format: both make 12 MP under the cap");
    require(photoMaximum(formats[chosen.index], 12'700'000) == CameraStillSize({4032, 3024}), "and its output is capped at 12 MP");
    require(photoMaximum(formats[0], 12'700'000) == CameraStillSize({4032, 3024}), "so even the larger format prepares no 48 MP pipeline");
}

// N1 (recheck4, only fast): no format has a rate within the cap. Nothing is
// clipped away: the camera keeps its video and its photos.
void aCameraWithOnlyFastFormatsKeepsAModeAndItsPhotos()
{
    const std::vector<VideoFormatFacts> formats {fmt(640, 480, {{1920, 1080}}, {{60, 60}}), fmt(1920, 1080, {{4032, 3024}}, {{120, 120}})};
    const auto chosen = anchorOf(formats);
    require(chosen.index == 1, "K1 ties; K2 takes the format that makes 12 MP");
    require(chosen.offer.stills == Sizes({{4032, 3024}}) && !chosen.offer.videoSizes.empty() &&
                chosen.offer.rates == std::vector<WholeRates> {{120, 120}},
            "its slowest rate is offered alone");
}

// N1 (recheck4, lost 30): 1080p at 1-60 and 1920x1440 at 1-30. The 1440 format
// is the anchor; native 1080p stays offered.
void aFormatThatTakesTheTierSizeKeepsItsModesWhenAnotherWins()
{
    const std::vector<VideoFormatFacts> formats {fmt(1920, 1080, {{1920, 1080}}, {{1, 60}}), fmt(1920, 1440, {{4032, 3024}}, {{1, 30}})};
    const auto chosen = anchorOf(formats);
    require(chosen.index == 1 && chosen.offer.stills == Sizes({{4032, 3024}}), "12 MP wins");
    require(chosen.offer.videoSizes.front() == FrameSize({1920, 1080}) && chosen.offer.rates == std::vector<WholeRates> {{1, 30}},
            "1080p at 1-30 stays: the camera keeps a mode");
}

// N2 (recheck4): 1080p at exactly 60 and 1920x1920 at 1-30, both 12 MP. Every
// size offered starts, because the box was built from the anchor's own frame.
void everyOfferedSizeStartsInTheAnchor()
{
    const std::vector<VideoFormatFacts> formats {fmt(1920, 1080, {{4032, 3024}}, {{60, 60}}), fmt(1920, 1920, {{4032, 3024}}, {{1, 30}})};
    const auto chosen = anchorOf(formats);
    require(chosen.index == 1, "the format that has a rate within the cap");
    require(chosen.offer.box == FrameSize({1280, 720}), "a 1080p frame from 1:1 would be 1920x1920, over the bound");
    for (const auto& size : chosen.offer.videoSizes)
        require(deliveredFrameBytes(formats[1], size.width, size.height, rowAlignment) <= smallFrameBound, "every offered size fits the frame bound");
    require(deliveredFrameBytes(formats[1], chosen.offer.box.width, chosen.offer.box.height, rowAlignment) <= smallFrameBound, "so does the box");
}

// R2 (recheck5): a 1802x1534 format delivers 1802x1534 to every size it is asked for
// (its reduced aspect ratio, 901:767, has no smaller even multiple). Its pixels,
// 4,146,402 bytes tightly packed, are under the old bound of 4,147,200; in
// capture buffers whose rows are padded to 256 bytes it is 4,712,448, over the
// 4,423,680 the reserve counts. The offer is bounded by the reserve's figure, so
// the format offers no size and is not an anchor, and no camera that has only it
// is offered.
void aFrameThatFitsTightlyButNotInTheReservedBufferIsNotOffered()
{
    const VideoFormatFacts odd = fmt(1802, 1534, {{4032, 3024}});
    require(deliveredFrameSize(1802, 1534, 1280, 720) == FrameSize({1802, 1534}), "1280x720 is delivered as the whole format");
    require(deliveredFrameBytes(odd, 1280, 720, 0) == 1802ull * 1534 * 3 / 2 && deliveredFrameBytes(odd, 1280, 720, 0) < 4'147'200,
            "tightly packed it is under 1920x1440's bytes");
    require(deliveredFrameBytes(odd, 1280, 720, rowAlignment) == 4'712'448, "in 256-byte rows it is 4,712,448");
    require(deliveredFrameBytes(odd, 1280, 720, rowAlignment) > smallFrameBound, "which is over the reserved buffer");
    const std::vector<VideoFormatFacts> alone {odd};
    const OfferRules rules = smallRules(alone);
    require(offerOf(odd, rules).videoSizes.empty(), "no size of it is offered");
    require(!anchorKeyOf(odd, rules) && !chooseAnchor(alone, rules, false), "so it is no anchor");
    // With the bound of tightly packed rows (the old figure), it was admitted.
    OfferRules tight = rules;
    tight.limits.frameRowAlignment = 0;
    require(!offerOf(odd, tight).videoSizes.empty(), "the same format was offered when rows were counted tightly");
    // Beside a format that fits, the camera runs in that one.
    const std::vector<VideoFormatFacts> two {odd, fmt(1920, 1440, {{4032, 3024}})};
    require(anchorOf(two).index == 1, "the format that fits is the anchor");
}

// The Mac's external camera lists near-integer rates.
void aCameraThatListsNearIntegerRatesIsOfferedWholeRates()
{
    const std::vector<VideoFormatFacts> formats {fmt(1920, 1080, {{1920, 1080}}, {{29.97, 29.97}, {23.976, 23.976}, {20, 20}, {14.99, 14.99}}),
                                                 fmt(1280, 720, {{1280, 720}}, {{29.97, 29.97}})};
    const auto chosen = anchorOf(formats);
    require(chosen.index == 0 && chosen.offer.rates == std::vector<WholeRates> {{15, 15}, {20, 20}, {24, 24}, {30, 30}},
            "30 24 20 15: the Mac's rate goes from 20 to 30");
    const auto setting = rateSettingFor(formats[0], 30);
    require(setting && setting->duration == FrameDuration::spanShortest, "30 is programmed as the span's own shortest duration");
}

void theLargeTierOffersWhatItsLimitsHold()
{
    const std::vector<VideoFormatFacts> formats {fmt(3840, 2160, {{8064, 6048}, {4032, 3024}}, {{1, 60}}), fmt(1920, 1440, {{4032, 3024}}, {{1, 30}})};
    const auto chosen = anchorOf(formats, false, true);
    require(chosen.index == 0 && chosen.offer.stills.front() == CameraStillSize({8064, 6048}), "48 MP in the large tier");
    require(chosen.offer.videoSizes.front() == FrameSize({3840, 2160}), "4K video");
}

void aFormatThatCarriesNoStreamIsNotAnAnchor()
{
    require(!chooseAnchor({}, smallRules({}), false), "no formats, no anchor");
    const std::vector<VideoFormatFacts> noRate {fmt(1920, 1080, {{1920, 1080}}, {{7.5, 7.9}})};
    require(!chooseAnchor(noRate, smallRules(noRate), false), "no whole rate, no anchor");
    const std::vector<VideoFormatFacts> noSize {fmt(8, 8, {{1920, 1080}}, {{1, 30}})};
    require(!chooseAnchor(noSize, smallRules(noSize), false), "no video size in its box, no anchor");
    const std::vector<VideoFormatFacts> neverUsable {fmt(1920, 1080, {{1920, 1080}}, {{7.5, 7.9}}), fmt(1280, 720, {}, {{1, 30}})};
    const auto chosen = chooseAnchor(neverUsable, smallRules(neverUsable), false);
    require(chosen && *chosen == 1, "a format that lists no photo can still be the anchor");
    const std::vector<VideoFormatFacts> single {fmt(1920, 1080, {{1920, 1080}}, {{1, 30}}, true, false)};
    require(!chooseAnchor(single, smallRules(single), true), "no multi-camera format, no multi-camera anchor");
}

void theFrameBoundKeepsAnOversizedFormatOutOfTheBox()
{
    // A 1:1 format delivers a 1080p frame as 1920x1920 (5,529,600 bytes): over the bound.
    const auto square = fmt(1920, 1920, {{4032, 3024}}, {{1, 30}});
    const std::vector<VideoFormatFacts> formats {square};
    const auto offer = offerOf(square, smallRules(formats));
    require(offer.box == FrameSize({1280, 720}), "the box ends at 1280x720");
    for (const auto& size : offer.videoSizes) require(size.width <= 1280 && size.height <= 720, "no larger size is offered");
}

void aTiesBreakByCostThenLowRatesThenFullRangeThenIndex()
{
    const Sizes photos {{4032, 3024}};
    std::vector<VideoFormatFacts> formats {fmt(4032, 3024, photos), fmt(1920, 1440, photos)};
    require(anchorOf(formats).index == 1, "the smaller frame");
    formats = {fmt(1920, 1440, photos, {{5, 30}}), fmt(1920, 1440, photos, {{1, 30}})};
    require(anchorOf(formats).index == 1, "the lower minimum rate");
    formats = {fmt(1920, 1440, photos, {{1, 30}}, false), fmt(1920, 1440, photos, {{1, 30}}, true)};
    require(anchorOf(formats).index == 1, "the full-range format");
    formats = {fmt(1920, 1440, photos), fmt(1920, 1440, photos)};
    require(anchorOf(formats).index == 0, "the lower index");
}

void theKeyOrderIsRateThenStillThenVideo()
{
    const Sizes twelve {{4032, 3024}};
    // A 12 MP format that reaches only 15 fps loses to a 30 fps one (K1 before K2).
    std::vector<VideoFormatFacts> formats {fmt(1920, 1440, twelve, {{1, 15}}), fmt(1920, 1080, {{1920, 1080}}, {{1, 30}})};
    require(anchorOf(formats).index == 1, "the format that reaches 30");
    // Equal rate, a larger still wins over a larger video box (K2 before K3).
    formats = {fmt(1280, 720, twelve), fmt(1920, 1080, {{1920, 1080}})};
    require(anchorOf(formats).index == 0, "12 MP photos over 1080p video");
    // Equal rate and still: the larger video box (K3 before K4).
    formats = {fmt(1280, 720, twelve), fmt(1920, 1080, twelve)};
    require(anchorOf(formats).index == 1, "the larger box");
}

void aCameraKeepsConcurrencyOnlyWhenItsMultiCameraFormatsOfferAsMuch()
{
    const Sizes twelve {{4032, 3024}};
    std::vector<VideoFormatFacts> formats {fmt(1920, 1440, twelve, {{1, 30}}, true, true), fmt(1920, 1080, {{1920, 1080}})};
    require(keepsConcurrency(formats, smallRules(formats)), "the best format is multi-camera");
    formats = {fmt(1920, 1440, twelve, {{1, 30}}, true, false), fmt(1920, 1080, {{1920, 1080}}, {{1, 30}}, true, true)};
    require(!keepsConcurrency(formats, smallRules(formats)), "the multi-camera formats make a smaller photo");
    formats = {fmt(1920, 1440, twelve, {{1, 30}}, true, false), fmt(1920, 1440, twelve, {{1, 30}}, true, true)};
    require(keepsConcurrency(formats, smallRules(formats)), "a multi-camera format that offers as much");
    formats = {fmt(1920, 1440, twelve, {{1, 30}}, true, false)};
    require(!keepsConcurrency(formats, smallRules(formats)), "no multi-camera format at all");
    require(!keepsConcurrency({}, smallRules({})), "no formats");
}

void aCameraJoinsAMultiCameraSessionOnlyWhenEveryAnchorIsAFormatItTakes()
{
    const bool yes[] {true, true};
    const bool one[] {true, false};
    require(mayJoin(false, false, {}), "the first input joins whatever it is");
    require(mayJoin(true, true, yes), "every anchor is a multi-camera format");
    require(!mayJoin(true, false, yes), "this anchor is not");
    require(!mayJoin(true, true, one), "another input's is not");
    require(mayJoin(true, true, std::span<const bool>()), "peers that cannot be named count as none to check");
}

// ---------------------------------------------------------------------------
// The session's transactions: a fake device that records what was added and what
// was removed
// ---------------------------------------------------------------------------

// A device as the platform models it (the rules AVCaptureDevice.h states): the active format with the frame durations
// it holds, which a long manual exposure may make longer at the maximum than the rate asks for, and a manual exposure.
// A maximum duration shorter than a custom exposure shortens the exposure to it; a minimum above the maximum, or a
// maximum below the minimum, is refused; a format brings its own default durations; and adding the input to a session
// resets the durations (the fake session does that).
struct FakeDevice {
    DeviceTiming timing;
    std::vector<std::string> writes;
    bool refuseLock {}, refuseFormat {}, refuseExposure {};
    int locks {}, unlocks {};
    FakeDevice()
    {
        timing.format = 7;
        timing.minimum = timing.maximum = {1, 30};
    }
    [[nodiscard]] static CameraTime fps(std::int32_t rate) { return {1, rate}; }
    void durations(CameraTime minimum, CameraTime maximum)
    {
        timing.minimum = minimum;
        timing.maximum = maximum;
    }
    void customExposure(CameraTime duration, float iso) { timing.exposure = {true, duration, iso}; }
    // The platform shortens a longer custom exposure to a shorter maximum.
    void clamp()
    {
        if (timing.exposure.custom && timing.exposure.duration.seconds() > timing.maximum.seconds()) timing.exposure.duration = timing.maximum;
    }
    // What an activation does to the device: the format, then the minimum, and the maximum unless it is refused
    // between them (`middle` false).
    bool program(std::uint32_t format, CameraTime rate, bool middle)
    {
        timing.format = format;
        timing.minimum = rate;
        if (!middle) return false;
        timing.maximum = rate;
        clamp();
        return true;
    }
    TimingSteps steps()
    {
        TimingSteps s;
        s.lock = [this] {
            ++locks;
            return !refuseLock;
        };
        s.unlock = [this] { ++unlocks; };
        s.read = [this] { return timing; };
        s.setFormat = [this](std::uint32_t format) {
            if (refuseFormat) return false;
            timing.format = format;
            timing.minimum = timing.maximum = {1, 30};
            writes.push_back("format");
            return true;
        };
        s.setMinimum = [this](CameraTime time) {
            if (time.seconds() > timing.maximum.seconds()) return false;
            timing.minimum = time;
            writes.push_back("minimum");
            return true;
        };
        s.setMaximum = [this](CameraTime time) {
            if (time.seconds() < timing.minimum.seconds()) return false;
            timing.maximum = time;
            clamp();
            writes.push_back("maximum");
            return true;
        };
        s.setCustomExposure = [this](CameraTime duration, float iso) {
            if (refuseExposure) return false;
            timing.exposure = {true, duration, iso};
            writes.push_back("exposure");
            return true;
        };
        return s;
    }
};

struct FakeSession {
    bool input {}, video {}, photo {};
    std::vector<std::string> log;
    int begins {}, commits {};
    bool refuseInput {}, refuseActivate {}, refuseVideo {}, refusePhoto {}, overBudget {};
    // The glue programs the new video connection (the stabilization) inside the configuration.
    bool programsVideo {};
    // The device whose timing a refused change must not lose.
    FakeDevice device;
    DeviceTiming saved;
    bool restored {true};
    // Something the device does by itself while the call runs (an automatic exposure follows the scene).
    std::function<void(FakeDevice&)> meanwhile;

    AdmitSteps steps(const AdmitPlan& plan)
    {
        AdmitSteps s;
        s.begin = [this] { ++begins; log.push_back("begin"); };
        s.commit = [this] { ++commits; log.push_back("commit"); };
        s.addInput = [this] {
            log.push_back("addInput");
            if (refuseInput) return false;
            input = true;
            // Joining the session resets the device's durations to its format's.
            device.timing.minimum = device.timing.maximum = {1, 30};
            return true;
        };
        s.snapshotTiming = [this] {
            log.push_back("snapshot");
            saved = device.timing;
        };
        s.activate = [this] {
            log.push_back("activate");
            if (meanwhile) meanwhile(device);
            // The minimum is set first; a refusal can come between the two.
            return device.program(1, FakeDevice::fps(15), !refuseActivate);
        };
        s.addVideo = [this] {
            log.push_back("addVideo");
            if (refuseVideo) return false;
            video = true;
            return true;
        };
        if (programsVideo) s.programVideo = [this] { log.push_back("programVideo"); };
        s.addPhoto = [this] {
            log.push_back("addPhoto");
            if (refusePhoto) return false;
            photo = true;
            return true;
        };
        s.overBudget = [this] { return overBudget; };
        s.removeVideo = [this] {
            log.push_back("removeVideo");
            video = false;
        };
        s.removePhoto = [this] {
            log.push_back("removePhoto");
            photo = false;
        };
        s.removeInput = [this] {
            log.push_back("removeInput");
            input = false;
        };
        s.restoreTiming = [this] {
            log.push_back("restoreTiming");
            restored = putBackTiming(saved, device.steps());
        };
        (void)plan;
        return s;
    }
};

// R3 (recheck4): a still is in flight (the camera has its input and a photo
// output, no video) and a stream is asked for and refused. Only the video
// output the call added is removed.
void aRefusedStreamRemovesOnlyTheOutputItAdded()
{
    FakeSession session;
    session.input = session.photo = true;
    session.refuseVideo = true;
    const AdmitPlan plan {.addInput = false, .activate = true, .addVideo = true, .addPhoto = false};
    require(admit(plan, session.steps(plan)) == Admission::videoRefused, "refused");
    require(session.input && session.photo && !session.video, "the input and the photo output of the still in flight stay");
    require(session.log == std::vector<std::string> {"begin", "snapshot", "activate", "addVideo", "removeVideo", "restoreTiming", "commit"},
            "begin, the durations taken, the rate, the refused video output, its removal, the durations back, commit: no removal of the photo "
            "output or the input");
    require(session.begins == session.commits, "one configuration, closed");
}

void aRefusedStreamBeforeItsInputLeavesTheSessionAsItWas()
{
    FakeSession session;
    session.refuseVideo = true;
    const AdmitPlan plan {.addInput = true, .activate = true, .addVideo = true, .addPhoto = false};
    require(admit(plan, session.steps(plan)) == Admission::videoRefused, "refused");
    require(!session.input && !session.video && !session.photo, "the input this call added left again");
    require(session.log == std::vector<std::string> {"begin", "snapshot", "addInput", "activate", "addVideo", "removeVideo", "removeInput",
                                                      "restoreTiming", "commit"},
            "the timing is taken before the input is added, and put back after it has left");
}

// The new video connection is programmed (the stabilization the guest asks for) inside the admission's configuration, after
// the output is added and before the commit: a session that runs already, to which a lens is added, starts the output at
// the commit, and a session that starts later delivers from its first frame. A video output that is refused, or that was in
// the session already, is not programmed.
void aNewVideoConnectionIsProgrammedBeforeTheConfigurationCommits()
{
    FakeSession fresh;
    fresh.programsVideo = true;
    const AdmitPlan plan {.addInput = true, .activate = true, .addVideo = true, .addPhoto = false};
    require(admit(plan, fresh.steps(plan)) == Admission::admitted, "admitted");
    require(fresh.log == std::vector<std::string> {"begin", "snapshot", "addInput", "activate", "addVideo", "programVideo", "commit"},
            "programmed after the output is added and before the commit");
    FakeSession refused;
    refused.programsVideo = true;
    refused.refuseVideo = true;
    require(admit(plan, refused.steps(plan)) == Admission::videoRefused, "refused");
    require(std::count(refused.log.begin(), refused.log.end(), "programVideo") == 0, "an output that was refused is not programmed");
    FakeSession present;
    present.programsVideo = true;
    present.input = present.video = true;
    const AdmitPlan still {.addInput = false, .activate = false, .addVideo = false, .addPhoto = true};
    require(admit(still, present.steps(still)) == Admission::admitted, "a still joins");
    require(std::count(present.log.begin(), present.log.end(), "programVideo") == 0, "a video connection already in the session is left");
}

void aRefusedInputOrFormatAddsNothingElse()
{
    FakeSession refused;
    refused.refuseInput = true;
    const AdmitPlan plan {.addInput = true, .activate = true, .addVideo = true, .addPhoto = true};
    require(admit(plan, refused.steps(plan)) == Admission::inputRefused &&
                refused.log == std::vector<std::string> {"begin", "snapshot", "addInput", "restoreTiming", "commit"},
            "a refused input stops the transaction before anything else runs, and leaves the device as it was");
    FakeSession format;
    format.refuseActivate = true;
    require(admit(plan, format.steps(plan)) == Admission::formatRefused && !format.input && !format.video && !format.photo,
            "a refused format takes the input it added out");
    require(format.log == std::vector<std::string> {"begin", "snapshot", "addInput", "activate", "removeInput", "restoreTiming", "commit"},
            "and nothing else");
    FakeSession kept;
    kept.input = true;
    kept.refuseActivate = true;
    const AdmitPlan existing {.addInput = false, .activate = true, .addVideo = true, .addPhoto = false};
    require(admit(existing, kept.steps(existing)) == Admission::formatRefused && kept.input &&
                kept.log == std::vector<std::string> {"begin", "snapshot", "activate", "restoreTiming", "commit"},
            "a camera that was in the session stays in it");
}

void aRefusedPhotoOutputRemovesTheVideoAndInputItAdded()
{
    FakeSession session;
    session.refusePhoto = true;
    const AdmitPlan plan {.addInput = true, .activate = true, .addVideo = true, .addPhoto = true};
    require(admit(plan, session.steps(plan)) == Admission::photoRefused, "refused");
    require(!session.input && !session.video && !session.photo, "everything this call added left");
    require(session.log == std::vector<std::string> {"begin", "snapshot", "addInput", "activate", "addVideo", "addPhoto", "removePhoto", "removeVideo",
                                                      "removeInput", "restoreTiming", "commit"},
            "the newest first, inside the one configuration");
    // A photo output refused for a camera that streams leaves the stream alone.
    FakeSession streaming;
    streaming.input = streaming.video = true;
    streaming.refusePhoto = true;
    const AdmitPlan still {.addInput = false, .activate = false, .addVideo = false, .addPhoto = true};
    require(admit(still, streaming.steps(still)) == Admission::photoRefused && streaming.input && streaming.video,
            "a refused still leaves the stream as it was");
    require(streaming.log == std::vector<std::string> {"begin", "addPhoto", "removePhoto", "commit"},
            "and, as it changes no durations, takes and puts back none");
}

void aSessionOverItsBudgetRemovesWhatTheJoinAdded()
{
    FakeSession session;
    session.overBudget = true;
    const AdmitPlan plan {.addInput = true, .activate = true, .addVideo = true, .addPhoto = false};
    require(admit(plan, session.steps(plan)) == Admission::overBudget, "refused after the commit");
    require(!session.input && !session.video, "the join is undone");
    require(session.begins == 2 && session.commits == 2, "inside a configuration of its own");
    require(session.log == std::vector<std::string> {"begin", "snapshot", "addInput", "activate", "addVideo", "commit", "begin", "removeVideo",
                                                      "removeInput", "restoreTiming", "commit"},
            "the first configuration commits, the second undoes it");
    FakeSession admitted;
    require(admit(plan, admitted.steps(plan)) == Admission::admitted && admitted.input && admitted.video && admitted.begins == 1,
            "a join within budget stays");
}

// R4 (recheck5): a still-only camera is in the session at 1/30 s, a long manual exposure having made the maximum 1/10 s,
// and a 15 fps stream is asked for. Whatever refuses it (the video output, the photo output, a hardware budget, or the
// device partway through its durations), the device holds the very durations it held before, not the 1/15 s the
// request programmed and not the 1/30 s a rate would recompute.
void aRefusedStreamOnACameraInTheSessionPutsItsDurationsBack()
{
    const auto before = [] {
        FakeDevice device;
        device.durations(FakeDevice::fps(30), FakeDevice::fps(10));
        return device.timing;
    }();
    const AdmitPlan plan {.addInput = false, .activate = true, .addVideo = true, .addPhoto = false};
    const auto session = [&] {
        FakeSession fake;
        fake.input = true;
        fake.device.timing = before;
        return fake;
    };
    auto video = session();
    video.photo = true;
    video.refuseVideo = true;
    require(admit(plan, video.steps(plan)) == Admission::videoRefused && video.device.timing == before, "a refused video output");
    auto photo = session();
    photo.refusePhoto = true;
    const AdmitPlan both {.addInput = false, .activate = true, .addVideo = true, .addPhoto = true};
    require(admit(both, photo.steps(both)) == Admission::photoRefused && photo.device.timing == before && photo.input && !photo.video,
            "a refused photo output, the video output it added removed");
    auto budget = session();
    budget.overBudget = true;
    require(admit(plan, budget.steps(plan)) == Admission::overBudget && budget.device.timing == before && budget.input && !budget.video,
            "a session over its hardware budget");
    auto partway = session();
    partway.refuseActivate = true;
    require(admit(plan, partway.steps(plan)) == Admission::formatRefused && partway.device.timing == before &&
                partway.device.timing.minimum != FakeDevice::fps(15),
            "a device that took the minimum duration and refused the maximum");
    // A join that succeeds keeps what it programmed.
    auto admitted = session();
    require(admit(plan, admitted.steps(plan)) == Admission::admitted && admitted.device.timing.minimum == FakeDevice::fps(15) &&
                admitted.device.timing.maximum == FakeDevice::fps(15),
            "a stream that is admitted runs at its rate");
}

// R4 (recheck6): the same camera has a custom exposure of 1/10 s (ISO 400). Shortening the maximum duration to 1/15 s
// shortens the exposure with it, so the durations put back do not put the exposure back: it is part of what a refusal
// restores, including the refusal that comes after the configuration is committed (a hardware budget). The fake
// device clamps as the platform does, so a restore that put back only the pair fails here.
void aRefusedStreamPutsTheManualExposureBackToo()
{
    FakeDevice start;
    start.durations(FakeDevice::fps(30), FakeDevice::fps(10));
    start.customExposure(FakeDevice::fps(10), 400.0f);
    const DeviceTiming before = start.timing;
    const AdmitPlan plan {.addInput = false, .activate = true, .addVideo = true, .addPhoto = false};
    const AdmitPlan both {.addInput = false, .activate = true, .addVideo = true, .addPhoto = true};
    const auto session = [&] {
        FakeSession fake;
        fake.input = true;
        fake.device.timing = before;
        return fake;
    };
    auto admitted = session();
    require(admit(plan, admitted.steps(plan)) == Admission::admitted && admitted.device.timing.exposure.duration == FakeDevice::fps(15),
            "the platform does shorten the exposure to the new maximum (the fake models it)");
    auto video = session();
    video.refuseVideo = true;
    require(admit(plan, video.steps(plan)) == Admission::videoRefused && video.device.timing == before && video.restored,
            "a refused video output: durations and exposure");
    auto photo = session();
    photo.refusePhoto = true;
    require(admit(both, photo.steps(both)) == Admission::photoRefused && photo.device.timing == before, "a refused photo output");
    auto budget = session();
    budget.overBudget = true;
    require(admit(plan, budget.steps(plan)) == Admission::overBudget && budget.device.timing == before,
            "the committed configuration over its budget: the exposure comes back after the commit");
    auto partway = session();
    partway.refuseActivate = true;
    require(admit(plan, partway.steps(plan)) == Admission::formatRefused && partway.device.timing == before,
            "a refusal between the minimum and the maximum");
    // A device that was not in a custom exposure has none to put back, and none is written.
    FakeSession automatic;
    automatic.input = true;
    automatic.device.durations(FakeDevice::fps(30), FakeDevice::fps(10));
    automatic.refuseVideo = true;
    automatic.device.timing.exposure = {false, {1, 50}, 100.0f};
    // An automatic exposure follows the scene while the call runs: what it reads then is not the device's to be put back,
    // and writing it would switch the device to a custom exposure.
    automatic.meanwhile = [](FakeDevice& device) { device.timing.exposure = {false, {1, 60}, 160.0f}; };
    require(admit(plan, automatic.steps(plan)) == Admission::videoRefused && !automatic.device.timing.exposure.custom &&
                automatic.device.timing.exposure.duration == CameraTime {1, 60} &&
                std::find(automatic.device.writes.begin(), automatic.device.writes.end(), "exposure") == automatic.device.writes.end(),
            "an automatic exposure is left alone: it is never switched to a custom one");
    // The ISO is put back as it was held, not as a default.
    FakeSession iso;
    iso.input = true;
    iso.device.durations(FakeDevice::fps(30), FakeDevice::fps(10));
    iso.device.customExposure({1, 8}, 1250.0f);
    iso.device.timing.maximum = {1, 8};
    iso.refuseVideo = true;
    const auto isoBefore = iso.device.timing;
    require(admit(plan, iso.steps(plan)) == Admission::videoRefused && iso.device.timing == isoBefore && iso.device.timing.exposure.iso == 1250.0f,
            "mode, duration and ISO as held");
}

// R4 (recheck6): the device is in another format, at its own durations, before its first join. Adding the input resets
// the durations, so the snapshot is taken before the input; the camera is then put in its anchor, and a refusal leaves
// the device as it was before the join, in its format too, once the input has gone.
void aRefusedFirstJoinPutsTheDeviceBackAsItWasBeforeTheJoin()
{
    FakeDevice start;
    start.durations(FakeDevice::fps(24), FakeDevice::fps(12));
    const DeviceTiming before = start.timing;
    require(before.format != 1, "the device is in another format than the anchor's");
    const AdmitPlan plan {.addInput = true, .activate = true, .addVideo = true, .addPhoto = true};
    const auto first = [&] {
        FakeSession fake;
        fake.device.timing = before;
        return fake;
    };
    auto video = first();
    video.refuseVideo = true;
    require(admit(plan, video.steps(plan)) == Admission::videoRefused && video.device.timing == before && video.restored && !video.input,
            "a refused video output: the format and the durations from before the join, which the input had reset");
    auto photo = first();
    photo.refusePhoto = true;
    require(admit(plan, photo.steps(plan)) == Admission::photoRefused && photo.device.timing == before, "a refused photo output");
    auto budget = first();
    budget.overBudget = true;
    require(admit(plan, budget.steps(plan)) == Admission::overBudget && budget.device.timing == before, "over the budget");
    auto format = first();
    format.refuseActivate = true;
    require(admit(plan, format.steps(plan)) == Admission::formatRefused && format.device.timing == before, "a refused anchor");
    auto input = first();
    input.refuseInput = true;
    require(admit(plan, input.steps(plan)) == Admission::inputRefused && input.device.timing == before, "a refused input");
    // A device that cannot be set back says so: nothing is claimed that was not done.
    auto stuck = first();
    stuck.refuseVideo = true;
    stuck.device.refuseFormat = true;
    require(admit(plan, stuck.steps(plan)) == Admission::videoRefused && !stuck.restored && stuck.device.timing.format == 1,
            "a refused format restore is reported");
}

// R4 (recheck6): putBackTiming writes in the order that never breaks the platform's rule that the minimum does not
// exceed the maximum, writes the exposure last and only when it is needed, and stops at the first refusal.
void putBackTimingOrdersItsWritesAndStopsAtARefusal()
{
    FakeDevice slow;  // the held minimum is above the maximum the device has now: the maximum goes first
    slow.durations(FakeDevice::fps(60), FakeDevice::fps(60));
    DeviceTiming slower = slow.timing;
    slower.minimum = FakeDevice::fps(30);
    slower.maximum = FakeDevice::fps(10);
    require(putBackTiming(slower, slow.steps()) && slow.timing == slower && slow.writes == std::vector<std::string> {"maximum", "minimum"},
            "a minimum above the current maximum waits for the maximum to move");
    FakeDevice fast;  // the held minimum is within the maximum the device has now: the minimum goes first
    fast.durations(FakeDevice::fps(30), FakeDevice::fps(10));
    DeviceTiming faster = fast.timing;
    faster.minimum = FakeDevice::fps(60);
    faster.maximum = FakeDevice::fps(60);
    require(putBackTiming(faster, fast.steps()) && fast.timing == faster && fast.writes == std::vector<std::string> {"minimum", "maximum"},
            "a minimum within the current maximum is written first, so the maximum never goes below it");
    FakeDevice unchanged;
    require(putBackTiming(unchanged.timing, unchanged.steps()) && unchanged.locks == 1 && unchanged.unlocks == 1,
            "locked once and unlocked once");
    FakeDevice locked;
    locked.refuseLock = true;
    DeviceTiming other = locked.timing;
    other.minimum = FakeDevice::fps(15);
    require(!putBackTiming(other, locked.steps()) && locked.writes.empty() && locked.unlocks == 0, "a device that will not lock is not written");
    FakeDevice exposure;
    exposure.durations(FakeDevice::fps(30), FakeDevice::fps(10));
    exposure.customExposure(FakeDevice::fps(10), 100.0f);
    DeviceTiming held = exposure.timing;
    exposure.timing.maximum = FakeDevice::fps(15);
    exposure.clamp();
    exposure.refuseExposure = true;
    require(!putBackTiming(held, exposure.steps()) && exposure.unlocks == 1 && exposure.timing.maximum == held.maximum,
            "a refused exposure is false, after the durations, and the device is unlocked");
    FakeDevice refusedFormat;
    DeviceTiming otherFormat = refusedFormat.timing;
    otherFormat.format = 3;
    refusedFormat.refuseFormat = true;
    require(!putBackTiming(otherFormat, refusedFormat.steps()) && refusedFormat.writes.empty() && refusedFormat.unlocks == 1,
            "nothing after a refused format is written");
}

struct FakeStream {
    FakeDevice device;
    DeviceTiming saved;
    int size {640};
    bool refuseDurations {}, refuseSize {};
    std::vector<std::string> log;
    FakeStream()
    {
        device.durations(FakeDevice::fps(30), FakeDevice::fps(10));
        device.customExposure(FakeDevice::fps(10), 400.0f);
    }
    ResizeSteps steps(bool rate, bool dimension)
    {
        ResizeSteps s;
        s.begin = [this] { log.push_back("begin"); };
        s.commit = [this] { log.push_back("commit"); };
        if (rate)
            s.snapshotTiming = [this] {
                log.push_back("snapshot");
                saved = device.timing;
            };
        if (rate)
            s.setDurations = [this] {
                log.push_back("durations");
                device.timing.minimum = FakeDevice::fps(15);
                if (refuseDurations) return false;
                device.timing.maximum = FakeDevice::fps(15);
                device.clamp();
                return true;
            };
        if (dimension)
            s.setVideoSize = [this] {
                log.push_back("size");
                if (refuseSize) return false;
                size = 1280;
                return true;
            };
        s.restoreTiming = [this] {
            log.push_back("restore");
            (void)putBackTiming(saved, device.steps());
        };
        return s;
    }
};

void aResizeSetsTheDurationsThenTheSizeAndRestoresTheOldTimingOnARefusal()
{
    const FakeStream fresh;
    const DeviceTiming before = fresh.device.timing;  // a long manual exposure made the maximum longer than the rate asks
    FakeStream ok;
    require(resizeStream(ok.steps(true, true)) == Resizing::resized && ok.device.timing.minimum == FakeDevice::fps(15) &&
                ok.device.timing.maximum == FakeDevice::fps(15) && ok.size == 1280,
            "both change");
    require(ok.log == std::vector<std::string> {"begin", "snapshot", "durations", "size", "commit"}, "the durations first");
    FakeStream sizeRefused;
    sizeRefused.refuseSize = true;
    require(resizeStream(sizeRefused.steps(true, true)) == Resizing::sizeRefused && sizeRefused.device.timing == before,
            "a refused size puts the device's own timing back, exposure included, not what a rate would program");
    require(sizeRefused.log == std::vector<std::string> {"begin", "snapshot", "durations", "size", "restore", "commit"}, "in the same configuration");
    FakeStream durationsRefused;
    durationsRefused.refuseDurations = true;
    require(resizeStream(durationsRefused.steps(true, true)) == Resizing::durationsRefused && durationsRefused.size == 640 &&
                durationsRefused.device.timing == before &&
                durationsRefused.log == std::vector<std::string> {"begin", "snapshot", "durations", "restore", "commit"},
            "a refused rate leaves the size alone, and the timing as it was after the minimum had changed");
    FakeStream sizeOnly;
    sizeOnly.refuseSize = true;
    require(resizeStream(sizeOnly.steps(false, true)) == Resizing::sizeRefused &&
                sizeOnly.log == std::vector<std::string> {"begin", "size", "commit"},
            "a resize that does not change the rate has no timing to restore");
    FakeStream rateOnly;
    require(resizeStream(rateOnly.steps(true, false)) == Resizing::resized && rateOnly.size == 640, "a rate alone");
}

// R4 (recheck7): what the backend remembers of a camera's stream and settings. A change the device refused is undone
// with restoreTiming, and when that is refused too the device holds something between the old and the new: an
// identical request must not be answered from the cache, and the settings must not be skipped as unchanged. The fake
// backend mirrors the glue's start, resize and apply around the same StreamCache and the same transactions.
struct FakeBackend {
    FakeSession session;
    FakeDevice& device = session.device;
    StreamCache cache;
    // The settings: the manual exposure the guest asked for, and the one last written to the device.
    CameraTime asked {1, 10};
    CameraTime appliedExposure {1, 10};
    bool applied {};
    bool refuseSize {};
    DeviceTiming saved;
    FakeBackend()
    {
        // 640x480 at 15 fps running, a 1/10 s manual exposure having made the maximum duration 1/10 s.
        device.timing.format = 1;
        device.durations(FakeDevice::fps(15), FakeDevice::fps(10));
        device.customExposure(FakeDevice::fps(10), 100.0f);
        cache.video = true;
        cache.width = 640;
        cache.height = 480;
        cache.frameRate = 15;
        applied = true;
    }
    // What the connection holds of the stabilization and what was written to it, through the production apply
    // (applyVideoStabilization) and the production order of an apply (applyCameraSettings), as the glue calls them.
    VideoStabilizationHeld stabilization;
    StabilizationWish wish;
    std::vector<VideoStabilization> stabilizationWrites;
    std::vector<VideoStabilizationState> stabilizationNotes;
    struct Port final : VideoStabilizationPort {
        Port(VideoStabilizationSupport supported, std::vector<VideoStabilization>& written) : supported_(supported), written_(written) {}
        VideoStabilizationSupport support() override { return supported_; }
        void write(VideoStabilization mode) override { written_.push_back(mode); }
        VideoStabilizationSupport supported_;
        std::vector<VideoStabilization>& written_;
    };
    void putStabilization(bool wanted, VideoStabilizationSupport support)
    {
        Port port(support, stabilizationWrites);
        const VideoStabilizationStep step = applyVideoStabilization(port, wanted, stabilization);
        if (step.note) stabilizationNotes.push_back(step.state);
    }
    // A fresh settings cache holds the defaults, which are the settings the guest asks for in video mode (stabilization on).
    void applyAll(bool wanted, VideoStabilizationSupport support, CameraTime exposure, bool force)
    {
        asked = exposure;
        (void)applyCameraSettings(
            cache, wish, wanted, force, applied && appliedExposure == exposure, [&] { putStabilization(wanted, support); },
            [&] { write(exposure); });
    }
    void apply(CameraTime exposure, bool force)
    {
        asked = exposure;
        StabilizationWish ignored;
        (void)applyCameraSettings(
            cache, ignored, false, force, applied && appliedExposure == exposure, [] {}, [&] { write(exposure); });
    }
    void write(CameraTime exposure)
    {
        auto steps = device.steps();
        if (!steps.lock()) return;
        (void)steps.setCustomExposure(exposure, 100.0f);
        steps.unlock();
        appliedExposure = exposure;
        applied = true;
    }
    // A lock the device refuses fails the write, as the platform's activate does.
    bool activateRate(std::uint32_t rate)
    {
        auto steps = device.steps();
        if (!steps.lock()) return false;
        device.writes.push_back("durations");
        const bool ok = device.program(1, FakeDevice::fps(std::int32_t(rate)), true);
        steps.unlock();
        return ok;
    }
    bool resize(std::uint32_t width, std::uint32_t height, std::uint32_t rate)
    {
        const bool rateChanges = cache.rateChanges(rate), sizeChanges = cache.sizeChanges(width, height);
        ResizeSteps s;
        s.begin = [] {};
        s.commit = [] {};
        s.snapshotTiming = [this] { saved = device.timing; };
        if (rateChanges) s.setDurations = [&] { return activateRate(rate); };
        if (sizeChanges)
            s.setVideoSize = [&] {
                // The device that took the larger stream's durations will not be locked again.
                if (refuseSize) device.refuseLock = true;
                return !refuseSize;
            };
        s.restoreTiming = [this] {
            if (!putBackTiming(saved, device.steps())) cache.restoreRefused();
        };
        if (resizeStream(s) != Resizing::resized) return false;
        cache.width = width;
        cache.height = height;
        cache.frameRate = rate;
        if (cache.timingWritten() && applied) apply(appliedExposure, true);
        return true;
    }
    bool admitStream(std::uint32_t width, std::uint32_t height, std::uint32_t rate)
    {
        const AdmitPlan plan {.addInput = true, .activate = true, .addVideo = true, .addPhoto = false};
        auto steps = session.steps(plan);
        const auto base = steps.restoreTiming;
        steps.restoreTiming = [&] {
            base();
            if (!session.restored) cache.restoreRefused();
        };
        if (admit(plan, steps) != Admission::admitted) return false;
        const bool stale = plan.activate && cache.timingWritten();
        cache.video = true;
        cache.width = width;
        cache.height = height;
        cache.frameRate = rate;
        if (stale && applied) apply(appliedExposure, true);
        return true;
    }
    bool start(std::uint32_t width, std::uint32_t height, std::uint32_t rate)
    {
        switch (cache.pathFor(width, height, rate)) {
        case StreamCache::Path::cached: return true;
        case StreamCache::Path::resize: return resize(width, height, rate);
        case StreamCache::Path::admit: return admitStream(width, height, rate);
        }
        return false;
    }
    [[nodiscard]] int written(const std::string& what) const
    {
        return int(std::count(device.writes.begin(), device.writes.end(), what));
    }
};

// Setting both durations to the rate's: after a refused change the device may hold the durations of a faster stream
// (a maximum of 1/30 s), and the minimum set to 1/15 s first would exceed it.
void theDurationThatMovesOutOfTheOthersWayIsWrittenFirst()
{
    const CameraTime fast {1, 30}, slow {1, 15};
    require(maximumFirst(fast, slow), "a longer duration than the maximum: the maximum goes first");
    require(!maximumFirst(slow, fast) && !maximumFirst(fast, fast), "a shorter or equal one: the minimum goes first");
    // Replayed on the device model, which refuses a minimum above the maximum.
    FakeDevice device;
    device.durations(fast, fast);
    auto steps = device.steps();
    const auto setBoth = [&](CameraTime duration) {
        if (maximumFirst(device.timing.maximum, duration)) return steps.setMaximum(duration) && steps.setMinimum(duration);
        return steps.setMinimum(duration) && steps.setMaximum(duration);
    };
    require(setBoth(slow) && device.timing.minimum == slow && device.timing.maximum == slow, "30 fps to 15 fps");
    require(setBoth(fast) && device.timing.minimum == fast && device.timing.maximum == fast, "and back");
}

void aRestoreThatIsRefusedLeavesNothingInTheCacheTrue()
{
    // The reviewer's sequence: a 640x480/15 stream with a 1/10 s manual exposure; a 1280x720/30 stream whose output
    // refuses the size, and whose restore cannot take the device's lock; the larger stream is withdrawn.
    FakeBackend backend;
    backend.refuseSize = true;
    require(!backend.start(1280, 720, 30), "the larger stream is refused");
    require(backend.cache.unrestored, "the restore was refused: the cache says so");
    require(backend.device.timing.maximum == FakeDevice::fps(30) && backend.device.timing.exposure.duration == FakeDevice::fps(30),
            "the device holds the larger stream's durations, and its exposure was shortened with them");
    backend.refuseSize = false;
    backend.device.refuseLock = false;
    backend.device.writes.clear();
    require(backend.cache.pathFor(640, 480, 15) == StreamCache::Path::resize,
            "the old request is not answered from the cache: its fields match but the device does not");
    require(backend.start(640, 480, 15), "the old request goes through");
    require(backend.written("durations") == 1 && backend.device.timing.minimum == FakeDevice::fps(15) &&
                backend.device.timing.maximum == FakeDevice::fps(15),
            "the durations are written again");
    require(backend.written("exposure") == 1 && backend.device.timing.exposure == (ManualExposure {true, FakeDevice::fps(10), 100.0f}),
            "and the manual exposure, which the unchanged settings would not have written");
    require(!backend.cache.unrestored && backend.cache.pathFor(640, 480, 15) == StreamCache::Path::cached,
            "after it, the cache stands for what the device holds");
    backend.device.writes.clear();
    require(backend.start(640, 480, 15) && backend.device.writes.empty(), "and an identical request writes nothing");
}

// The guest's request for stabilization in video mode equals the settings a new connection's cache holds, so an apply
// skipped for unchanged settings left the picture unstabilized until the first setting changed (a tap), when it was
// stabilized at once and the view jumped. The stabilization is decided before that exit.
void aNewConnectionIsStabilizedByAnApplyThatLeavesTheOtherSettingsAlone()
{
    FakeBackend backend;
    const VideoStabilizationSupport standard {true, false};
    backend.applyAll(true, standard, backend.appliedExposure, false);
    require(backend.stabilizationWrites == std::vector<VideoStabilization> {VideoStabilization::standard},
            "the first apply stabilizes the connection although its settings are the ones applied");
    require(backend.written("exposure") == 0, "and writes none of the other settings");
    require(backend.stabilizationNotes == std::vector<VideoStabilizationState> {VideoStabilizationState::standard},
            "the record is told once");
    // The first tap changes a setting: the apply that follows carries the same wish.
    backend.applyAll(true, standard, CameraTime {1, 20}, false);
    require(backend.stabilizationWrites.size() == 1 && backend.written("exposure") == 1,
            "a later apply writes the changed setting and leaves the stabilization alone: the view does not jump");
    require(backend.stabilizationNotes.size() == 1, "and says nothing");
}

// The apply that leaves the other settings alone is also what tells the platform that the guest wants the stabilization:
// the admission of a connection that follows programs it from that wish, before the connection can deliver. (A camera
// whose guest asks for the defaults, as video mode does, never writes a setting.)
void anApplyThatLeavesTheSettingsAloneStillStatesTheWish()
{
    FakeBackend backend;
    require(!backend.wish.atAdmission(), "before any apply the admission asks nothing");
    backend.applyAll(true, {}, backend.appliedExposure, false);
    require(backend.written("exposure") == 0, "the apply wrote none of the settings");
    require(backend.wish.atAdmission(), "and the admission now programs the stabilization the guest asked for");
    backend.applyAll(false, {}, backend.appliedExposure, false);
    require(!backend.wish.atAdmission(), "a guest that stops asking is no longer programmed");
}

void aFormatThatSupportsNoModeLeavesTheConnectionAloneAndSaysSoOnce()
{
    FakeBackend backend;
    backend.applyAll(true, {}, backend.appliedExposure, false);
    backend.applyAll(true, {}, CameraTime {1, 20}, false);
    require(backend.stabilizationWrites.empty() && backend.stabilizationNotes == std::vector<VideoStabilizationState> {VideoStabilizationState::unsupported},
            "nothing is written and the record says the format supports none, once");
}

// The record's photo line states the compensation read after the admission committed the session's configuration, of the
// output the admission added: the note is made as the admission returns, before the camera's entry holds that output, so
// the reading is taken from the added output and not from the entry's. The facts keep the reading taken as the output was
// attached where the admission was undone or added no output.
struct FakePhotoFacts {
    CameraFact compensationSupported {};
    CameraFact compensationEnabled {};
    CameraReading reading {};
    int connection {};
};
struct FakePhotoOutput {
    CameraFact supported;
    CameraFact enabled;
};

void theRecordStatesTheReadingTakenAfterTheAdmissionCommitted()
{
    FakePhotoFacts attached;
    attached.compensationSupported = CameraFact::no;
    attached.compensationEnabled = CameraFact::no;
    attached.reading = CameraReading::beforeCommit;
    attached.connection = 7;
    FakePhotoOutput added {CameraFact::yes, CameraFact::yes};
    FakePhotoOutput other {CameraFact::no, CameraFact::unknown};
    std::vector<const FakePhotoOutput*> read;
    const auto reader = [&](const FakePhotoOutput* output) {
        read.push_back(output);
        FakePhotoFacts facts;
        facts.compensationSupported = output->supported;
        facts.compensationEnabled = output->enabled;
        facts.reading = CameraReading::afterCommit;
        return facts;
    };
    auto committed = committedPhotoFacts<FakePhotoFacts>(attached, &added, true, reader);
    require(committed && committed->reading == CameraReading::afterCommit && committed->compensationSupported == CameraFact::yes &&
                committed->compensationEnabled == CameraFact::yes && committed->connection == 7,
            "an admission that added the photo output records what the added output reads after its commit, and keeps the rest");
    require(read == std::vector<const FakePhotoOutput*> {&added}, "the output it reads is the one the admission added, no other");
    read.clear();
    committed = committedPhotoFacts<FakePhotoFacts>(attached, &added, false, reader);
    require(read.empty() && committed && committed->reading == CameraReading::beforeCommit && committed->compensationSupported == CameraFact::no,
            "one that was refused and undone keeps the reading taken as the output was attached");
    committed = committedPhotoFacts<FakePhotoFacts>(attached, static_cast<const FakePhotoOutput*>(nullptr), true, reader);
    require(read.empty() && committed && committed->reading == CameraReading::beforeCommit,
            "one that added no output has no later reading to take");
    committed = committedPhotoFacts<FakePhotoFacts>(std::nullopt, &other, true, reader);
    require(read.empty() && !committed, "and none attached records nothing");
}

void aRequestAfterARefusedRestoreFailsWhileTheDeviceCannotBeWritten()
{
    FakeBackend backend;
    backend.refuseSize = true;
    require(!backend.start(1280, 720, 30), "refused");
    backend.refuseSize = false;
    // The device still will not lock: no cached success, and nothing is claimed.
    backend.device.writes.clear();
    require(!backend.start(640, 480, 15), "the old request fails, it is not answered from the cache");
    require(!backend.start(640, 480, 15) && backend.cache.unrestored, "and again");
    require(backend.device.writes.empty(), "no write reached the device");
    backend.device.refuseLock = false;
    require(backend.start(640, 480, 15) && !backend.cache.unrestored, "once the device can be written it is");
}

void settingsAreWrittenAgainAfterARefusedRestoreEvenWhenUnchanged()
{
    FakeBackend backend;
    backend.refuseSize = true;
    require(!backend.start(1280, 720, 30), "refused");
    backend.device.refuseLock = false;
    backend.device.writes.clear();
    // The device's caller applies the settings it has always had.
    backend.apply({1, 10}, false);
    require(backend.written("exposure") == 1 && backend.device.timing.exposure.duration == FakeDevice::fps(10),
            "unchanged settings are not skipped when what the device holds is not known");
    FakeBackend known;
    known.apply({1, 10}, false);
    require(known.written("exposure") == 0, "and are, when it is");
    known.apply({1, 10}, true);
    require(known.written("exposure") == 1, "a forced apply always writes");
}

void aRefusedFirstJoinWithARefusedRestoreIsWrittenAgainByTheNextJoin()
{
    FakeBackend backend;
    backend.cache = StreamCache {};
    backend.session.refuseVideo = true;
    backend.session.meanwhile = [](FakeDevice& device) { device.refuseLock = true; };
    require(!backend.start(640, 480, 15) && backend.cache.unrestored && !backend.session.restored,
            "the join is refused, and the device was not put back");
    require(!backend.cache.video, "no stream is remembered");
    backend.session.refuseVideo = false;
    backend.session.meanwhile = nullptr;
    backend.device.refuseLock = false;
    backend.device.writes.clear();
    require(backend.start(640, 480, 15) && !backend.cache.unrestored, "the next join succeeds");
    require(backend.written("exposure") == 1, "and writes the settings again, the format not having changed");
}

void theStreamCacheSaysWhatIsKnown()
{
    StreamCache cache;
    require(cache.pathFor(640, 480, 15) == StreamCache::Path::admit, "no stream: it is made");
    cache.video = true;
    cache.width = 640;
    cache.height = 480;
    cache.frameRate = 15;
    require(cache.pathFor(640, 480, 15) == StreamCache::Path::cached && !cache.rateChanges(15) && !cache.sizeChanges(640, 480),
            "the same request is answered from the cache");
    require(cache.pathFor(640, 480, 30) == StreamCache::Path::resize && cache.rateChanges(30) && !cache.sizeChanges(640, 480), "a rate");
    require(cache.pathFor(1280, 720, 15) == StreamCache::Path::resize && !cache.rateChanges(15) && cache.sizeChanges(1280, 720), "a size");
    require(cache.mayLeaveSettings(false, true) && !cache.mayLeaveSettings(true, true) && !cache.mayLeaveSettings(false, false),
            "settings are left when unchanged and not forced");
    require(!cache.timingWritten(), "nothing was unknown");
    cache.restoreRefused();
    require(cache.pathFor(640, 480, 15) == StreamCache::Path::resize && cache.rateChanges(15) && cache.sizeChanges(640, 480) &&
                !cache.mayLeaveSettings(false, true),
            "a refused restore makes everything written again");
    require(cache.timingWritten() && !cache.unrestored && cache.pathFor(640, 480, 15) == StreamCache::Path::cached, "until it has been");
    require(!cache.timingWritten(), "reported once");
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

// R5 (recheck5): a fake device that records the order of what recovery does.
struct FakeRecovery {
    std::vector<std::string> log;
    std::set<std::size_t> moved;    // the cameras the system moved out of their anchors
    std::set<std::size_t> refused;  // the cameras whose repair the device refuses
    // The cameras whose repair puts the format back and then fails an output: they are in their anchor afterwards.
    std::set<std::size_t> partial;
    RecoverySteps steps()
    {
        RecoverySteps s;
        s.outOfAnchor = [this](std::size_t camera) { return moved.contains(camera); };
        s.repair = [this](std::size_t camera) {
            log.push_back("repair " + std::to_string(camera));
            if (partial.contains(camera)) {
                moved.erase(camera);
                return false;
            }
            if (refused.contains(camera)) return false;
            moved.erase(camera);
            return true;
        };
        s.announce = [this](Recovery outcome) { log.push_back(outcome == Recovery::resumed ? "resumed" : "failed"); };
        return s;
    }
};

void everyCameraIsRepairedBeforeTheSessionIsToldItIsBack()
{
    const std::size_t cameras[] {0, 1, 2};
    FakeRecovery none;
    require(recoverAnchors(cameras, none.steps()) == Recovery::resumed && none.log == std::vector<std::string> {"resumed"},
            "cameras in their anchors need no repair, and the session is back");
    FakeRecovery two;
    two.moved = {0, 2};
    require(recoverAnchors(cameras, two.steps()) == Recovery::resumed &&
                two.log == std::vector<std::string> {"repair 0", "repair 2", "resumed"},
            "every camera out of its anchor is put back, then the session is told");
    FakeRecovery empty;
    require(recoverAnchors({}, empty.steps()) == Recovery::resumed && empty.log == std::vector<std::string> {"resumed"}, "no camera, no repair");
}

void aRefusedRepairIsAFailureAndNothingIsAnnouncedAsBack()
{
    const std::size_t cameras[] {0, 1, 2};
    FakeRecovery first;
    first.moved = {0, 1};
    first.refused = {0};
    require(recoverAnchors(cameras, first.steps()) == Recovery::failed && first.log == std::vector<std::string> {"repair 0", "failed"},
            "the first refusal ends the recovery: failed, with no resumed");
    require(first.moved.contains(1), "no further repair is attempted in a session that is being closed");
    FakeRecovery later;
    later.moved = {0, 2};
    later.refused = {2};
    require(recoverAnchors(cameras, later.steps()) == Recovery::failed &&
                later.log == std::vector<std::string> {"repair 0", "repair 2", "failed"},
            "a refusal after a success is a failure too, and no camera is announced as back");
    for (const auto& entry : {first.log, later.log})
        require(std::find(entry.begin(), entry.end(), "resumed") == entry.end(), "never resumed");
}

// R5 (recheck6): the end of an interruption and the restart after an error are both recoveries and both may be asked
// for. A repair that puts a device's format back and then fails an output leaves it in its anchor, so a second recovery
// finds nothing out of anchor on that camera and, run again, announces the session back with the output unrepaired. The
// latch keeps the failure for the session: nothing is repaired or announced after it.
void aFailedRecoveryIsFinalForItsSession()
{
    const std::size_t cameras[] {0, 1};
    FakeRecovery fake;
    fake.moved = {0, 1};
    fake.partial = {0};
    RecoveryLatch latch;
    require(!latch.failed(), "nothing has failed yet");
    require(latch.recover(cameras, fake.steps()) == Recovery::failed && fake.log == std::vector<std::string> {"repair 0", "failed"} && latch.failed(),
            "the first recovery fails after a partial repair");
    require(!fake.moved.contains(0) && fake.moved.contains(1), "camera 0 is in its anchor again, camera 1 is not");
    fake.log.clear();
    require(latch.recover(cameras, fake.steps()) == Recovery::failed && fake.log.empty(),
            "a second notification repairs nothing and announces nothing, so no `resumed` follows the failure");
    // What it prevents: the same two calls without the latch announce the session back.
    FakeRecovery unlatched;
    unlatched.moved = {0, 1};
    unlatched.partial = {0};
    (void)recoverAnchors(cameras, unlatched.steps());
    unlatched.log.clear();
    require(recoverAnchors(cameras, unlatched.steps()) == Recovery::resumed && unlatched.log == std::vector<std::string> {"repair 1", "resumed"},
            "without it the second recovery announces resumed with camera 0's output unrepaired");
    // A recovery that succeeds is not latched, and a new session has its own latch.
    FakeRecovery good;
    good.moved = {1};
    RecoveryLatch fresh;
    require(fresh.recover(cameras, good.steps()) == Recovery::resumed && !fresh.failed() &&
                fresh.recover(cameras, good.steps()) == Recovery::resumed,
            "a recovery that succeeds leaves the session free to recover again");
    RecoveryLatch next;
    FakeRecovery again;
    again.moved = {0};
    require(next.recover(cameras, again.steps()) == Recovery::resumed, "a new session's latch is open");
}

// R5 (recheck6): the capture's frames carry their capture time; one made before the repair is discarded when it reaches
// the callback after it, within a window that a disagreeing clock cannot stretch into every frame.
// A mutex that says when a callback reaches for it: the callback has read the boundary by then, since it reads it first.
struct SpyMutex {
    std::mutex held;
    std::atomic<bool> waiting {false};
    void lock()
    {
        waiting = true;
        held.lock();
    }
    void unlock() { held.unlock(); }
};

bool waitUntil(const std::atomic<bool>& flag)
{
    for (int tries = 0; tries < 500 && !flag; ++tries) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return flag;
}

constexpr std::int64_t second = 1'000'000'000;

// R5 (recheck7): a sample is already inside the output callback, waiting for the lock the announcement of the repair
// holds, when the repair completes; it is delivered after it, however long it waited. The reviewer's probe held it
// for 2.3 s; a window of two seconds let it through, which is why the boundary is a generation read at entry.
void aSampleThatWasInsideTheCallbackWhenTheRepairCompletedIsRejected()
{
    // The sample's timestamp is before the repair's, and, as it would be on a clock that runs ahead of the one the repair
    // was read on, after it: the generation rejects it either way.
    for (const std::int64_t waited : {second / 10, 3 * second, 120 * second}) for (const std::int64_t ahead : {std::int64_t(0), waited + 10 * second}) {
        RepairBoundary boundary;
        SpyMutex mutex;
        int delivered = 0;
        mutex.lock();
        mutex.waiting = false;
        std::thread callback([&] { (void)deliverSample(boundary, mutex, 1000 * second + ahead, [&] { ++delivered; }); });
        require(waitUntil(mutex.waiting), "the callback is waiting for the lock");
        // The repair completes `waited` after the sample was captured, with the lock, and resumed is announced.
        boundary.repaired(1000 * second + waited);
        mutex.unlock();
        callback.join();
        require(delivered == 0, "the sample that was in the callback before the repair is not delivered after it");
    }
}

void aSampleWhoseCallbackBeganAfterTheRepairIsJudgedByItsCaptureTime()
{
    RepairBoundary boundary;
    std::mutex mutex;
    int delivered = 0;
    const auto deliver = [&](std::optional<std::int64_t> capturedAt) {
        const int before = delivered;
        (void)deliverSample(boundary, mutex, capturedAt, [&] { ++delivered; });
        return delivered != before;
    };
    boundary.repaired(500 * second);
    require(!deliver(499 * second), "captured before the repair: rejected");
    require(!deliver(500 * second - 1), "just before it");
    require(deliver(500 * second), "at it: kept, the device was in its anchor");
    require(deliver(501 * second), "after it");
    require(!deliver(500 * second - 30 * second), "captured thirty seconds before the repair: rejected, there is no age beyond which it is kept");
    require(!deliver(500 * second - 3600 * second), "nor an hour");
    require(!deliver(-5 * second), "nor a sample before the clock's own start");
}

void aFreshFrameAfterTheRepairPassesAndAStreamIsNotMuted()
{
    RepairBoundary boundary;
    std::mutex mutex;
    int delivered = 0;
    boundary.repaired(100 * second);
    for (int frame = 0; frame < 30 * 60; ++frame)
        (void)deliverSample(boundary, mutex, 100 * second + frame * (second / 30), [&] { ++delivered; });
    require(delivered == 30 * 60, "every frame captured after the repair reaches the guest");
}

// The boundary compares what it is given with what it is given; it does not know a host clock. A capture clock that is
// a fixed offset from the host's (ahead or behind, by any amount) therefore changes nothing: the repair is read on the
// capture's clock, so are the samples.
void aCaptureClockThatDiffersFromTheHostsChangesNothing()
{
    for (const std::int64_t offset : {-3600 * second, -10 * second, std::int64_t(-1), std::int64_t(1), 10 * second, 3600 * second}) {
        const auto onCapture = [offset](std::int64_t host) { return host + offset; };
        RepairBoundary boundary;
        std::mutex mutex;
        int stale = 0, fresh = 0;
        // Frames in flight at the repair, captured over the 5 s before it (host time 100 s), and the stream after it.
        boundary.repaired(onCapture(100 * second));
        for (int frame = 0; frame < 150; ++frame)
            (void)deliverSample(boundary, mutex, onCapture(95 * second + frame * (second / 30)), [&] { ++stale; });
        for (int frame = 0; frame < 30 * 20; ++frame)
            (void)deliverSample(boundary, mutex, onCapture(100 * second + frame * (second / 30)), [&] { ++fresh; });
        require(stale == 0, "nothing captured before the repair is delivered, whatever the offset");
        require(fresh == 30 * 20, "and nothing after it is withheld: the stream is not muted");
    }
}

void aSampleWithoutATimestampOrARepairWithoutAClockIsJudgedByTheGenerationAlone()
{
    RepairBoundary boundary;
    std::mutex mutex;
    int delivered = 0;
    const auto deliver = [&](std::optional<std::int64_t> capturedAt) {
        const int before = delivered;
        (void)deliverSample(boundary, mutex, capturedAt, [&] { ++delivered; });
        return delivered != before;
    };
    require(deliver(std::nullopt) && deliver(5 * second), "before any repair, everything passes");
    boundary.repaired(std::nullopt);  // the capture's clock could not be read
    require(deliver(5 * second) && deliver(std::nullopt), "after a repair without a clock reading, a callback that began after it passes");
    boundary.repaired(50 * second);
    require(deliver(std::nullopt), "a sample without a timestamp is not rejected by one");
    boundary.repaired(std::nullopt);
    require(deliver(40 * second), "and a later repair without a reading does not keep the time of an earlier one");
    SpyMutex spy;
    RepairBoundary later;
    int count = 0;
    spy.lock();
    spy.waiting = false;
    std::thread callback([&] { (void)deliverSample(later, spy, std::nullopt, [&] { ++count; }); });
    require(waitUntil(spy.waiting), "waiting");
    later.repaired(std::nullopt);
    spy.unlock();
    callback.join();
    require(count == 0, "but the one that was inside the callback before the repair is rejected, with or without either");
}

void everyRepairDrawsANewBoundary()
{
    RepairBoundary boundary;
    SpyMutex mutex;
    int delivered = 0;
    boundary.repaired(10 * second);
    mutex.lock();
    mutex.waiting = false;
    std::thread callback([&] { (void)deliverSample(boundary, mutex, 20 * second, [&] { ++delivered; }); });
    require(waitUntil(mutex.waiting), "waiting");
    boundary.repaired(30 * second);  // a second recovery
    mutex.unlock();
    callback.join();
    require(delivered == 0, "a sample that was captured after the first repair, and is inside a callback when the second completes, is rejected");
}

void aSampleIsDeliveredWhileTheLockIsHeld()
{
    RepairBoundary boundary;
    std::mutex mutex;
    bool held = false;
    require(deliverSample(boundary, mutex, std::nullopt, [&] { held = !mutex.try_lock(); if (!held) mutex.unlock(); }),
            "delivered");
    require(held, "under the lock the announcement of a repair also takes, so that none is delivered across it");
}

// ---------------------------------------------------------------------------
// Pressure
// ---------------------------------------------------------------------------

// R1 (recheck5): the pressure observer's handler and the work it queues for the session's queue hold what they use.
// The session's queue is drained when the session ends, but a notification that arrives during the drain queues its
// work behind it, to run after the session, its anchors and its running cameras are gone. This is the glue's shape in
// the glue's own medium, Objective-C blocks: the owner `session` (RunningCamera::watch), the observer's `handler` and
// the queued work. `begin` takes the watch by value, so the blocks copy it from a local; a handler that named a
// reference parameter (the old `const std::shared_ptr<const Anchor>&`, which was the session's element) would read
// freed memory when the late work runs, which AddressSanitizer reports.
struct PressureRig {
    using Handler = void (^)(std::uint32_t);
    using Work = void (^)();
    std::unique_ptr<std::shared_ptr<PressureWatch>> session;
    std::vector<Work> queue;                 // the session's queue
    std::vector<ProgrammedRate> programmed;  // what reached the device
    Handler handler {};

    explicit PressureRig(const VideoFormatFacts& facts)
        : session(std::make_unique<std::shared_ptr<PressureWatch>>(std::make_shared<PressureWatch>(facts, offeredRatesOf(facts, videoRateCap))))
    {
        (*session)->request(30);
        begin(*session);
    }
    ~PressureRig()
    {
        drain();
        if (handler) Block_release(handler);
    }
    PressureRig(const PressureRig&) = delete;
    PressureRig& operator=(const PressureRig&) = delete;

    void begin(std::shared_ptr<PressureWatch> watch)
    {
        PressureRig* const self = this;
        handler = Block_copy(^(std::uint32_t cap) {
            if (!watch->pressure(cap)) return;
            self->queue.push_back(Block_copy(^{
                if (const auto rate = watch->programmed()) self->programmed.push_back(*rate);
            }));
        });
    }
    void drain()
    {
        auto work = std::move(queue);
        queue.clear();
        for (auto item : work) {
            item();
            Block_release(item);
        }
    }
};

void aPressureNotificationIsAppliedWhileTheStreamRunsAndNeverAfterIt()
{
    const VideoFormatFacts facts = fmt(1920, 1440, {{4032, 3024}}, {{1, 60}});
    {
        PressureRig rig(facts);
        rig.handler(20);
        rig.drain();
        require(rig.programmed.size() == 1 && rig.programmed.back().rate == 20, "serious pressure caps the stream at 20 fps");
        rig.handler(20);
        require(rig.queue.empty(), "the same cap again changes nothing and queues nothing");
        rig.handler(15);
        rig.handler(0);
        rig.drain();
        require(rig.programmed.size() == 3 && rig.programmed[1].rate == 30 && rig.programmed[2].rate == 30,
                "each change is programmed from the cap the work finds when it runs: both found the pressure gone");
        rig.handler(15);
        rig.drain();
        require(rig.programmed.size() == 4 && rig.programmed.back().rate == 15, "and 15 fps under critical pressure");
    }
    {
        // The notification that arrives during the session's final drain: its work runs after the session is gone.
        PressureRig rig(facts);
        rig.handler(15);
        require(rig.queue.size() == 1, "the work is queued behind the session's drain");
        (*rig.session)->end();  // stop() ends the watches inside the queue...
        rig.session->reset();    // ...and the session lets go of its own (the anchors_ element, null from now on)
        rig.drain();             // the late work runs now: through a reference to that element it would find null
        rig.session.reset();     // and the session's storage goes
        require(rig.programmed.empty(), "it programs nothing: the stream is over");
        rig.handler(20);
        require(rig.queue.empty(), "and a notification after the end queues nothing");
    }
    {
        // Ended before a cap change that was already queued, with the session still there.
        PressureRig rig(facts);
        rig.handler(20);
        (*rig.session)->end();
        require(!(*rig.session)->live() && (*rig.session)->cap() == 0 && !(*rig.session)->programmed().has_value(), "an ended watch programs nothing");
        rig.drain();
        require(rig.programmed.empty(), "so the queued work does nothing");
    }
}

// ---------------------------------------------------------------------------
// Refusals
// ---------------------------------------------------------------------------

void aRefusalIsLoggedOncePerCameraAndReasonUntilTheCameraChanges()
{
    RefusalLog log;
    require(log.first(0, Refusal::stillNotMade), "the first refusal is logged");
    for (int again = 0; again < 100; ++again) require(!log.first(0, Refusal::stillNotMade), "the same refusal is not");
    require(log.first(0, Refusal::overBudget), "another reason is");
    require(log.first(1, Refusal::stillNotMade), "another camera is");
    log.reset(0);
    require(log.first(0, Refusal::stillNotMade) && log.first(0, Refusal::overBudget), "a reset camera logs again");
    require(!log.first(1, Refusal::stillNotMade), "and another camera is not reset with it");
    for (const auto reason : {Refusal::noAnchor, Refusal::formatChanged, Refusal::notJoinable, Refusal::inputRefused, Refusal::outputRefused,
                              Refusal::overBudget, Refusal::stillNotMade, Refusal::rawUnavailable, Refusal::raised, Refusal::notRestored,
                              Refusal::ratioMoved, Refusal::turnNotHeld, Refusal::stillsNotTurned, Refusal::ratioFailed,
                              Refusal::ratioTimedOut, Refusal::stillNotTurned})
        require(!refusalName(reason).empty() && refusalName(reason) != "unknown", "every reason has a name");
}

// The photo callback's thread logs a still that failed while the session's queue resets the camera (a stream stops, a still
// is captured): one log, two threads. Under a thread sanitizer this is a data race on the map without the log's lock.
void theRefusalLogIsUsedFromTwoThreads()
{
    RefusalLog log;
    std::atomic<bool> stop {false};
    std::thread queue([&] {
        for (std::size_t round = 0; !stop.load(); ++round) {
            if (round % 16 == 15) log.clear();
            else log.reset(round % 4);
        }
    });
    std::size_t logged = 0;
    for (std::size_t round = 0; round < 20000; ++round) {
        logged += log.first(round % 4, Refusal::stillNotTurned) ? 1 : 0;
        logged += log.first(round % 4, Refusal::stillNotMade) ? 1 : 0;
    }
    stop = true;
    queue.join();
    require(logged > 0, "a refusal is logged while the camera is reset from another thread");
    log.clear();
    require(log.first(0, Refusal::stillNotTurned) && !log.first(0, Refusal::stillNotTurned), "a cleared log logs again, once");
}

// ---------------------------------------------------------------------------
// Generated tables
// ---------------------------------------------------------------------------

struct Rng {
    std::uint64_t state;
    std::uint64_t next()
    {
        std::uint64_t z = (state += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    std::uint32_t below(std::uint32_t n) { return n == 0 ? 0 : std::uint32_t(next() % n); }
    bool chance(std::uint32_t percent) { return below(100) < percent; }
    template<typename T> const T& pick(const std::vector<T>& values) { return values[below(std::uint32_t(values.size()))]; }
};

std::vector<VideoFormatFacts> generateTable(Rng& rng)
{
    static const std::vector<std::pair<std::uint32_t, std::uint32_t>> bases {
        {160, 120}, {320, 240}, {640, 480}, {1280, 720}, {1440, 1080}, {1600, 900}, {1600, 1200}, {1920, 1080}, {1920, 1440}, {1920, 1920},
        {2016, 1512}, {2560, 1440}, {3840, 2160}, {4032, 3024}, {1088, 1088}, {1536, 1536}, {3000, 2000}, {1921, 1081}, {4224, 2376}};
    static const std::vector<CameraStillSize> photoSizes {{4032, 3024}, {4224, 2376}, {8064, 6048}, {2016, 1512}, {1920, 1080}, {1280, 720},
                                                          {3088, 2316}, {640, 480}, {5712, 4284}, {1920, 1440}};
    static const std::vector<std::uint32_t> rates {10, 15, 24, 25, 30, 60, 120, 240};
    std::vector<VideoFormatFacts> formats;
    const std::uint32_t count = 1 + rng.below(12);
    for (std::uint32_t index = 0; index < count; ++index) {
        VideoFormatFacts format;
        if (rng.chance(25)) {
            const std::uint32_t width = 160 + 2 * rng.below(1936);
            static const std::vector<std::pair<std::uint32_t, std::uint32_t>> aspects {{1, 1}, {16, 9}, {4, 3}, {3, 2}, {5, 4}, {21, 9}};
            const auto aspect = rng.pick(aspects);
            format.width = width;
            format.height = std::max<std::uint32_t>(16, width * aspect.second / aspect.first);
        } else {
            const auto size = rng.pick(bases);
            format.width = size.first;
            format.height = size.second;
        }
        const std::uint32_t kind = rng.below(7);
        const std::uint32_t k = rng.pick(rates);
        switch (kind) {
        case 0: format.frameRates = {{1, double(k)}}; break;
        case 1: format.frameRates = {{double(k), double(k)}}; break;
        case 2: format.frameRates = {{1, 10}, {50, 60}}; break;
        case 3: format.frameRates = {{k * 1000.0 / 1001.0, k * 1000.0 / 1001.0}}; break;
        case 4: format.frameRates = {{k + 3e-5, k + 3e-5}}; break;
        case 5: format.frameRates = {{1, double(k)}, {double(k) + 1, 2.0 * k}}; break;
        default: format.frameRates = rng.chance(10) ? Spans {{7.5, 7.9}} : Spans {{double(k), double(k) * 2}}; break;
        }
        const std::uint32_t photos = rng.below(4);
        for (std::uint32_t photo = 0; photo < photos; ++photo) format.photoSizes.push_back(rng.pick(photoSizes));
        format.fullRange = rng.chance(60);
        format.multiCamera = rng.chance(50);
        formats.push_back(std::move(format));
    }
    return formats;
}

// The rules, restated from the model as a person would read it, to compare with
// the implementation. A whole rate is a number the span holds, or is within
// 0.15 % of.
bool refWholeRate(const CameraRateSpan& span, std::uint32_t n)
{
    const double tolerance = 0.0015 * n;
    return (n >= span.minimum && n <= span.maximum) || std::fabs(n - span.minimum) <= tolerance || std::fabs(n - span.maximum) <= tolerance;
}

std::vector<std::uint32_t> refRates(const VideoFormatFacts& format)
{
    std::vector<std::uint32_t> rates;
    for (std::uint32_t n = 1; n <= 600; ++n)
        if (std::any_of(format.frameRates.begin(), format.frameRates.end(), [&](const CameraRateSpan& span) { return refWholeRate(span, n); }))
            rates.push_back(n);
    return rates;
}

Sizes refStills(const VideoFormatFacts& format, std::uint64_t cap)
{
    Sizes within;
    for (const auto& size : format.photoSizes)
        if (size.width != 0 && size.height != 0 && (cap == 0 || std::uint64_t(size.width) * size.height <= cap)) within.push_back(size);
    if (within.empty()) return {};
    const auto largest = *std::max_element(within.begin(), within.end(), [](const CameraStillSize& a, const CameraStillSize& b) {
        const auto areaA = std::uint64_t(a.width) * a.height, areaB = std::uint64_t(b.width) * b.height;
        return areaA != areaB ? areaA < areaB : a.width < b.width;
    });
    Sizes stills;
    for (const auto& size : within)
        if (size.width <= largest.width && size.height <= largest.height && std::find(stills.begin(), stills.end(), size) == stills.end())
            stills.push_back(size);
    return stills;
}

// What the platform delivers for a stream, from first principles: the smallest even multiple of the format's
// reduced aspect ratio (found by counting up) that covers the request, else the format's own size.
FrameSize refDelivered(const VideoFormatFacts& format, std::uint32_t width, std::uint32_t height)
{
    const std::uint32_t common = std::gcd(format.width, format.height);
    const std::uint32_t ratioWidth = format.width / common, ratioHeight = format.height / common;
    for (std::uint32_t multiple = 1; multiple < common; ++multiple) {
        const std::uint32_t w = multiple * ratioWidth, h = multiple * ratioHeight;
        if (w % 2 != 0 || h % 2 != 0) continue;
        if (w >= width && h >= height) return {w, h};
    }
    return {format.width, format.height};
}

// The buffer the reserve counts for a frame, from first principles: every row padded up to a multiple of 256
// bytes, one luma plane and a half-height chroma plane.
std::uint64_t refBufferBytes(FrameSize frame)
{
    std::uint64_t pitch = 0;
    while (pitch < frame.width) pitch += rowAlignment;
    return pitch * frame.height + pitch * ((frame.height + 1) / 2);
}

using RefKey = std::tuple<bool, std::uint32_t, std::uint64_t, std::uint32_t, std::uint64_t, std::int64_t, std::int64_t, bool>;

// The key of §1, computed from the facts by the brute-force rules above.
std::optional<RefKey> refKey(const VideoFormatFacts& format, const OfferRules& rules)
{
    const auto rates = refRates(format);
    if (rates.empty()) return std::nullopt;
    std::vector<FrameSize> kept;
    std::uint32_t boxWidth = 0, boxHeight = 0;
    auto candidates = rules.candidateSizes;
    std::sort(candidates.begin(), candidates.end(), [](const FrameSize& a, const FrameSize& b) {
        const auto areaA = std::uint64_t(a.width) * a.height, areaB = std::uint64_t(b.width) * b.height;
        return areaA != areaB ? areaA > areaB : a.width > b.width;
    });
    for (const auto& size : candidates) {
        if (size.width < 16 || size.height < 16 || size.width % 2 != 0 || size.height % 2 != 0) continue;
        if (size.width > rules.maximumVideoWidth || size.height > rules.maximumVideoHeight) continue;
        if (size.width > format.width || size.height > format.height) continue;
        if (std::find(kept.begin(), kept.end(), size) != kept.end()) continue;
        const auto w = std::max(boxWidth, size.width), h = std::max(boxHeight, size.height);
        if (refBufferBytes(refDelivered(format, w, h)) > rules.limits.maximumFrameBytes) continue;
        kept.push_back(size);
        boxWidth = w;
        boxHeight = h;
    }
    if (kept.empty()) return std::nullopt;
    const auto cap = rules.rateCap;
    std::uint32_t best = 0;
    for (const auto n : rates)
        if (n <= cap) best = std::max(best, n);
    const auto stills = refStills(format, rules.limits.maximumStillPixels);
    std::uint64_t stillArea = 0;
    std::uint32_t stillWidth = 0;
    for (const auto& size : stills) {
        const auto area = std::uint64_t(size.width) * size.height;
        if (area > stillArea || (area == stillArea && size.width > stillWidth)) {
            stillArea = area;
            stillWidth = size.width;
        }
    }
    return RefKey {best != 0, best, stillArea, stillWidth, std::uint64_t(boxWidth) * boxHeight, -std::int64_t(std::uint64_t(format.width) * format.height),
                   -std::int64_t(rates.front()), format.fullRange};
}

void everyGeneratedTableHoldsTheInvariants()
{
    constexpr std::uint64_t seed = 20261113;
    constexpr int tables = 20000;
    Rng rng {seed};
    std::uint64_t anchors = 0, withoutAnchor = 0, multiAnchors = 0, requestsChecked = 0;
    for (int table = 0; table < tables; ++table) {
        const std::string where = " (seed " + std::to_string(seed) + ", table " + std::to_string(table) + ")";
        const auto formats = generateTable(rng);
        for (const bool large : {false, true}) {
            const OfferRules rules = large ? largeRules(formats) : smallRules(formats);
            const std::uint64_t bound = rules.limits.maximumFrameBytes;
            const std::uint64_t cap = rules.limits.maximumStillPixels;
            for (const bool multiOnly : {false, true}) {
                const auto index = chooseAnchor(formats, rules, multiOnly);
                // P6: the anchor is the best candidate by the key of the model, the lowest index among equals.
                std::optional<std::size_t> expected;
                std::optional<RefKey> expectedKey;
                for (std::size_t i = 0; i < formats.size(); ++i) {
                    if (multiOnly && !formats[i].multiCamera) continue;
                    const auto key = refKey(formats[i], rules);
                    if (key && (!expectedKey || *key > *expectedKey)) {
                        expected = i;
                        expectedKey = key;
                    }
                }
                require(index == expected, "P6: the anchor is the best-ranked candidate" + where);
                // P5: a second call, and the table in another order, give the same key.
                require(chooseAnchor(formats, rules, multiOnly) == index, "P5: determinism" + where);
                if (formats.size() > 1) {
                    auto shuffled = formats;
                    for (std::size_t i = shuffled.size() - 1; i > 0; --i) std::swap(shuffled[i], shuffled[rng.below(std::uint32_t(i + 1))]);
                    const auto other = chooseAnchor(shuffled, rules, multiOnly);
                    require(other.has_value() == index.has_value(), "P5: the same candidates in another order" + where);
                    if (index) require(refKey(shuffled[*other], rules) == refKey(formats[*index], rules), "P5: an anchor of an equal key" + where);
                }
                if (!index) {
                    // P4: no candidate offers no camera.
                    ++withoutAnchor;
                    continue;
                }
                ++anchors;
                multiAnchors += multiOnly;
                const auto& anchor = formats[*index];
                require(!multiOnly || anchor.multiCamera, "P7: a multi-camera anchor is a multi-camera format" + where);
                const auto offer = offerOf(anchor, rules);
                // P4: an offered camera has a video size and a rate.
                require(!offer.videoSizes.empty() && !offer.rates.empty(), "P4: an anchor has a video mode and a rate" + where);
                // P2: every offered still is made, within the cap.
                for (const auto& still : offer.stills) {
                    require(formatMakesPhoto(anchor, still, cap), "P2: every offered still is made" + where);
                    require(cap == 0 || std::uint64_t(still.width) * still.height <= cap, "P2: within the cap" + where);
                }
                require(offer.stills == refStills(anchor, cap) || [&] {
                    auto a = offer.stills, b = refStills(anchor, cap);
                    std::sort(a.begin(), a.end(), [](auto x, auto y) { return std::tie(x.width, x.height) < std::tie(y.width, y.height); });
                    std::sort(b.begin(), b.end(), [](auto x, auto y) { return std::tie(x.width, x.height) < std::tie(y.width, y.height); });
                    return a == b;
                }(), "P2: the stills are those the model lists" + where);
                // P3: the delivered frame at the box is within the bound, and grows with the request.
                require(bound == 0 || refBufferBytes(refDelivered(anchor, offer.box.width, offer.box.height)) <= bound,
                        "P3: the box's capture buffer is within the reserved one" + where);
                require(deliveredFrameBytes(anchor, offer.box.width, offer.box.height, rowAlignment) ==
                            refBufferBytes(refDelivered(anchor, offer.box.width, offer.box.height)),
                        "P3: the model's byte count is the buffer's" + where);
                // P1: every admitted run (a request inside the box, at an offered rate) runs in the anchor.
                std::vector<std::uint32_t> offered;
                for (const auto& range : offer.rates)
                    for (const auto rate : {range.minimum, range.maximum, range.minimum + (range.maximum - range.minimum) / 2}) offered.push_back(rate);
                for (const auto rate : offered) {
                    const auto setting = rateSettingFor(anchor, rate);
                    require(setting.has_value(), "P1: every offered rate can be programmed" + where);
                    if (setting) {
                        const auto& span = anchor.frameRates[setting->span];
                        const double duration = programmedFrameDuration(span, setting->duration, rate);
                        // P8: the duration lies in the span and is within 0.15 % of the nominal one.
                        require(duration >= 1.0 / span.maximum - 1e-12 && duration <= 1.0 / span.minimum + 1e-12, "P8: the duration is the span's to give" + where);
                        require(std::fabs(duration * rate - 1.0) <= 0.0015 + 1e-9, "P8: within 0.15 % of 1 / rate" + where);
                    }
                }
                for (int draw = 0; draw < 4; ++draw) {
                    // A request of one to three streams, each inside some offered size.
                    std::uint32_t w = 0, h = 0;
                    for (std::uint32_t stream = 0, n = 1 + rng.below(3); stream < n; ++stream) {
                        const auto& size = offer.videoSizes[rng.below(std::uint32_t(offer.videoSizes.size()))];
                        const std::uint32_t sw = std::max<std::uint32_t>(16, (size.width - rng.below(size.width / 2 + 1)) & ~1u);
                        const std::uint32_t sh = std::max<std::uint32_t>(16, (size.height - rng.below(size.height / 2 + 1)) & ~1u);
                        w = std::max(w, rng.chance(50) ? size.width : std::min(sw, size.width));
                        h = std::max(h, rng.chance(50) ? size.height : std::min(sh, size.height));
                    }
                    ++requestsChecked;
                    require(w <= anchor.width && h <= anchor.height, "P1: the anchor covers the request" + where);
                    require(bound == 0 || refBufferBytes(refDelivered(anchor, w, h)) <= bound,
                            "P1/P3: the request's capture buffer is within the reserved one" + where);
                    require(deliveredFrameSize(anchor.width, anchor.height, w, h) == refDelivered(anchor, w, h), "P1: the delivered frame" + where);
                }
                // P3: the delivered frame grows with the request in each dimension.
                for (int draw = 0; draw < 6; ++draw) {
                    const std::uint32_t w = 16 + 2 * rng.below(anchor.width / 2), h = 16 + 2 * rng.below(anchor.height / 2);
                    const auto base = deliveredFrameBytes(anchor, w, h, rowAlignment);
                    require(deliveredFrameBytes(anchor, std::min(anchor.width, w + 2 * (1 + rng.below(40))), h, rowAlignment) >= base, "P3: grows with the width" + where);
                    require(deliveredFrameBytes(anchor, w, std::min(anchor.height, h + 2 * (1 + rng.below(40))), rowAlignment) >= base, "P3: grows with the height" + where);
                }
                // The rates, against the brute-force whole rates.
                const auto whole = refRates(anchor);
                std::vector<std::uint32_t> inOffer;
                for (const auto& range : offer.rates)
                    for (std::uint32_t rate = range.minimum; rate <= range.maximum; ++rate) inOffer.push_back(rate);
                std::vector<std::uint32_t> expectedOffer;
                for (const auto rate : whole)
                    if (rate <= rules.rateCap) expectedOffer.push_back(rate);
                if (expectedOffer.empty()) expectedOffer.push_back(whole.front());
                require(inOffer == expectedOffer, "the offered rates are the whole rates up to the cap, or the slowest alone" + where);
            }
        }
    }
    require(anchors > 10000 && withoutAnchor > 100 && multiAnchors > 1000 && requestsChecked > 40000, "the tables exercise both outcomes and every path");
}

} // namespace

// ---------------------------------------------------------------------------
// The front camera's still limit
// ---------------------------------------------------------------------------

std::vector<VideoFormatFacts> factsOfRows(const std::vector<CameraCaptureFormat>& rows)
{
    return std::vector<VideoFormatFacts>(rows.begin(), rows.end());
}

// What the limit changes of a format is its photo sizes and nothing else.
bool sameButForThePhotoSizes(const VideoFormatFacts& a, const VideoFormatFacts& b)
{
    return a.width == b.width && a.height == b.height && a.frameRates == b.frameRates && a.fullRange == b.fullRange &&
        a.multiCamera == b.multiCamera;
}

std::uint64_t areaOfSize(CameraStillSize size) { return std::uint64_t(size.width) * size.height; }

// Decision 1: the limit is a pixel count, about 3 MP, so that one value gives the same result on every table: the 4:3
// 2016x1512 and the 16:9 2304x1296 and the 4:3 2048x1536 are inside, whatever lens lists them, and the sizes above (the
// 3088x2316 of the iPad mini's front, a 12 MP front camera's 4032x3024, 4K's 3840x2160) are not.
void theFrontStillLimitIsAboutThreeMegapixelsOnAnyTable()
{
    const auto listed = fmt(1, 1, {{2016, 1512}, {2304, 1296}, {2048, 1536}, {1920, 1440}, {1920, 1080}, {640, 480}, {2560, 1440}, {3088, 2316},
                                   {4032, 3024}, {3840, 2160}, {5712, 4284}, {8064, 6048}});
    const auto limited = limitedStillFormats(CameraPosition::front, std::vector<VideoFormatFacts> {listed});
    require(limited.size() == 1 && limited[0].photoSizes == Sizes({{2016, 1512}, {2304, 1296}, {2048, 1536}, {1920, 1440}, {1920, 1080}, {640, 480}}),
            "the sizes up to 3.15 MP stay, in their order; 2560x1440 (3.69 MP) and everything larger go");
    require(frontStillMaximumPixels >= areaOfSize({2048, 1536}) && frontStillMaximumPixels < areaOfSize({2560, 1440}),
            "the limit is between the 3 MP sizes and the next ones up");
    for (const auto position : {CameraPosition::back, CameraPosition::external}) {
        const auto same = limitedStillFormats(position, std::vector<VideoFormatFacts> {listed});
        require(same.size() == 1 && same[0] == listed, "no other camera is limited here: the back cameras' limit is the tier's");
    }
    require(limitedStillFormats(CameraPosition::front, {}).empty(), "no formats, no formats");
}

// The tables the tests model, with the camera taken for a front one: the iPad mini 6's front camera as the Apple
// backend lists it (copied from a device), the tablet and the phone the other tests use, and a laptop's.
void theFrontStillLimitChangesOnlyThePhotoSizesOfTheTablesTheTestsModel()
{
    const auto mini = factsOfRows(uml::testing::miniFrontFormats());
    require(mini.size() == 32, "the iPad mini's front table");
    const auto limitedMini = limitedStillFormats(CameraPosition::front, mini);
    require(limitedMini.size() == mini.size());
    for (std::size_t index = 0; index < mini.size(); ++index) {
        require(sameButForThePhotoSizes(mini[index], limitedMini[index]), "only the photo sizes are limited");
        const bool lists3088 = std::find(mini[index].photoSizes.begin(), mini[index].photoSizes.end(), CameraStillSize {3088, 2316}) !=
            mini[index].photoSizes.end();
        require(lists3088 ? limitedMini[index].photoSizes == Sizes({{2016, 1512}}) : limitedMini[index].photoSizes == mini[index].photoSizes,
                "the two formats that list 3088x2316 keep 2016x1512; every other format's list is as listed");
    }
    // The same rule on the tables of the other tests.
    const auto tablet = limitedStillFormats(CameraPosition::front, tabletFormats);
    require(tablet[0].photoSizes == tabletFormats[0].photoSizes && tablet[1].photoSizes == Sizes({{1920, 1440}}) && tablet[2].photoSizes.empty() &&
                tablet[3].photoSizes == tabletFormats[3].photoSizes,
            "the tablet's table: its 12 MP photos go, a format that lists only larger sizes lists none while another does");
    const auto phone = limitedStillFormats(CameraPosition::front, std::vector<VideoFormatFacts> {fmt(4032, 3024, {{4032, 3024}, {8064, 6048}, {2016, 1512}})});
    require(phone[0].photoSizes == Sizes({{2016, 1512}}), "a phone's: the binned size stays");
    const auto laptop = limitedStillFormats(CameraPosition::front, std::vector<VideoFormatFacts> {fmt(1920, 1080, {{1920, 1080}}, {{1, 30}}, false, false)});
    require(laptop[0].photoSizes == Sizes({{1920, 1080}}), "a laptop's 1080p camera is as it was");
}

// The limit is a ceiling with no exception: when no format of a front camera lists a size within it, every format lists
// none, and so the camera is offered no native still (Android derives its JPEG sizes from the offered video) and its
// photo output has no maximum to prepare. A format that lists no usable size lists none either.
void aFrontCameraNeverKeepsASizeAboveTheLimit()
{
    const auto big = limitedStillFormats(CameraPosition::front, std::vector<VideoFormatFacts> {fmt(3840, 2160, {{8064, 6048}, {4032, 3024}, {3840, 2160}}),
                                                                                            fmt(1920, 1080, {{4032, 3024}, {0, 0}})});
    require(big.size() == 2 && big[0].photoSizes.empty() && big[1].photoSizes.empty(), "nothing inside the limit anywhere: every format lists none");
    for (const auto& format : big) {
        require(!photoMaximum(format, 12'700'000) && stillsOf(format, 12'700'000).empty(), "so there is no output maximum and no offered still");
        require(!photoMaximum(format, 0) && stillsOf(format, 0).empty(), "whatever the tier's cap");
    }
    require(limitedStillFormats(CameraPosition::front, std::vector<VideoFormatFacts> {fmt(1920, 1080, {}), fmt(640, 480, {{0, 0}})})[1].photoSizes.empty(),
            "a format that lists nothing usable lists nothing");
    const auto mixed = limitedStillFormats(CameraPosition::front, std::vector<VideoFormatFacts> {fmt(3840, 2160, {{4032, 3024}}), fmt(1920, 1080, {{1920, 1080}})});
    require(mixed[0].photoSizes.empty() && mixed[1].photoSizes == Sizes({{1920, 1080}}), "a size inside the limit on another format: this one lists none");
    // The one place the position's limit is read from, for a list that belongs to no format.
    require(stillWithinPositionLimit(CameraPosition::front, {2016, 1512}) && !stillWithinPositionLimit(CameraPosition::front, {2560, 1440}) &&
                stillWithinPositionLimit(CameraPosition::back, {8064, 6048}) && stillWithinPositionLimit(CameraPosition::external, {8064, 6048}),
            "a front camera's stills are within 3.2 MP, no other camera's are limited here");
}

// A size is offered, prepared and made together: the offer (stillsOf), the photo output's maximum (photoMaximum) and
// the capture's check (exactPhotoSize on the live list against that maximum) read the limited facts and agree, and the
// size that was dropped is neither offered nor made.
void theOfferTheOutputAndTheCaptureAgreeOnTheLimitedList()
{
    const auto listed = factsOfRows(uml::testing::miniFrontFormats());
    const auto limited = limitedStillFormats(CameraPosition::front, listed);
    for (std::size_t index = 0; index < listed.size(); ++index) {
        const auto offered = stillsOf(limited[index], 12'700'000);
        const auto maximum = photoMaximum(limited[index], 12'700'000);
        require(offered.empty() == !maximum, "an offer has an output maximum");
        for (const auto& size : offered) {
            require(areaOfSize(size) <= frontStillMaximumPixels, "an offered size is within the limit");
            require(formatMakesPhoto(limited[index], size, 12'700'000), "an offered size is made");
            require(exactPhotoSize(listed[index].photoSizes, size, *maximum) == std::optional<CameraStillSize>(size),
                    "the capture check on the live (listed) sizes passes for an offered size");
        }
        if (maximum) {
            require(!exactPhotoSize(listed[index].photoSizes, {3088, 2316}, *maximum), "3088x2316 is refused by the output's maximum");
            require(!formatMakesPhoto(limited[index], {3088, 2316}, 12'700'000), "and is not made");
        }
    }
    // The 2016x1512 format of the iPad mini: the output is prepared at 2016x1512.
    require(photoMaximum(limited[31], 12'700'000) == std::optional<CameraStillSize>(CameraStillSize {2016, 1512}) &&
                photoMaximum(listed[31], 12'700'000) == std::optional<CameraStillSize>(CameraStillSize {3088, 2316}),
            "the output's maximum is 2016x1512 where it was 3088x2316");
}

// Decision 3's pure half: the limit changes what an anchor offers for stills and nothing it offers for video, and the
// ranking on the listed formats is what a camera's concurrency is decided on (keepsConcurrency): the iPad mini's front
// does not keep it listed, and would if its formats were limited first.
void theFrontStillLimitLeavesTheVideoOfferAndDecidesNoConcurrency()
{
    const auto listed = factsOfRows(uml::testing::miniFrontFormats());
    const auto limited = limitedStillFormats(CameraPosition::front, listed);
    const auto rules = smallRules(listed);
    const auto listedAnchor = chooseAnchor(listed, rules, false);
    const auto limitedAnchor = chooseAnchor(limited, rules, false);
    require(listedAnchor && limitedAnchor, "both rank");
    const auto before = offerOf(listed[*listedAnchor], rules);
    const auto after = offerOf(limited[*limitedAnchor], rules);
    require(before.videoSizes == after.videoSizes && before.rates == after.rates && before.box == after.box,
            "the video sizes, the rates and the box are the same");
    require(before.stills.front() == CameraStillSize({3088, 2316}) && areaOfSize(after.stills.front()) <= frontStillMaximumPixels,
            "the stills are not");
    require(!keepsConcurrency(listed, rules), "the iPad mini's front does not keep its concurrency, as listed");
    require(keepsConcurrency(limited, rules), "it would on the limited formats: the limit must not be what decides it");
}

// The platform's own mounting is the rotation coordinator's angle for portrait: an iPhone 15 Pro Max states 90 for its
// front camera as for its back ones (the front camera is upright with the port on the left, the back ones with the port on
// the right). A back camera states it as it is. A front camera states 270, the angle Android's front cameras have, and the
// host turns every picture of it by the half turn that brings its own mounting there (`presentationTurn`).
void aCameraStatesThePlatformsMountingAndAFrontCameraStates270()
{
    require(sensorOrientationOf(CameraPosition::back, 90) == 90, "a back camera upright with the port on the right states 90");
    require(sensorOrientationOf(CameraPosition::front, 90) == 90, "the platform's angle of a front camera at the port on the left is 90");
    for (const double angle : {0.0, 90.0, 180.0, 270.0})
        for (const auto position : {CameraPosition::back, CameraPosition::front})
            require(sensorOrientationOf(position, angle) == std::uint32_t(angle), "a quarter turn is the angle the platform gives");
    require(sensorOrientationOf(CameraPosition::front, 360) == 0 && sensorOrientationOf(CameraPosition::front, -90) == 270,
            "an angle is taken within one turn");
    require(sensorOrientationOf(CameraPosition::back, 89.6) == 90 && sensorOrientationOf(CameraPosition::back, 314) == 270,
            "an angle is rounded to a quarter turn");
    require(sensorOrientationOf(CameraPosition::external, 0) == 90 && sensorOrientationOf(CameraPosition::external, 180) == 90,
            "an external camera states 90 whatever the angle");
    require(sensorOrientationOf(CameraPosition::front, std::numeric_limits<double>::quiet_NaN()) == 0, "no angle states none");

    // What the guest is told: a front camera upright with the port on the left (90) states 270 and its pictures are turned
    // by 180 degrees; the back cameras state the platform's own angle and are not turned.
    require(presentedMountingOf(CameraPosition::front, 90).stated == 270, "a front camera at 90 states 270, not 90");
    require(presentedMountingOf(CameraPosition::front, 90).presentationTurn == 180, "and its pictures are turned by 180 degrees");
    for (const double angle : {0.0, 90.0, 180.0, 270.0}) {
        const auto back = presentedMountingOf(CameraPosition::back, angle);
        require(back.stated == std::uint32_t(angle) && back.connectionTurn == 0 && !back.emulatesEarlierFront && back.presentationTurn == 0,
                "a back camera states the platform's angle and is not turned at any angle");
    }
}

// A front camera whose coordinator states portrait 0 or 180 is mounted with its sensor's top at the device's top, where an
// earlier front camera's is at the side (90). It is presented as one of those first: the platform turns the video connection
// by the difference, so that the buffers are upright where the earlier cameras' are, and the host then turns every picture by
// 180 degrees like that of any front camera at 90. A front camera states 270 whatever its mounting; the half turn is for the
// mountings that put it at 90 and not for 270, which is the angle itself. Nothing else changes: not a back camera at any
// angle, not an external camera, not an angle that is not a number.
void aFrontCameraStates270AndIsTurnedHalfAroundUnlessItAlreadyIs()
{
    const auto of = [](CameraPosition position, double angle) { return presentedMountingOf(position, angle); };
    require(of(CameraPosition::front, 90) == PresentedMounting({270, 0, false, 180}), "a front camera at 90 states 270 and is turned 180");
    require(of(CameraPosition::front, 0) == PresentedMounting({270, 270, true, 180}),
            "a front camera at 0 is imitated (video connection 270) and then turned 180");
    require(of(CameraPosition::front, 180) == PresentedMounting({270, 90, true, 180}),
            "a front camera at 180 is imitated (video connection 90) and then turned 180");
    require(of(CameraPosition::front, 270) == PresentedMounting({270, 0, false, 0}), "a front camera at 270 states it already and is not turned");
    require(of(CameraPosition::back, 90) == PresentedMounting({90, 0, false, 0}), "a back camera at 90 is as it states");
    require(of(CameraPosition::back, 0) == PresentedMounting({0, 0, false, 0}), "a back camera at 0 is not imitated");
    require(of(CameraPosition::back, 180) == PresentedMounting({180, 0, false, 0}) && of(CameraPosition::back, 270) == PresentedMounting({270, 0, false, 0}),
            "nor at 180 or 270");
    require(of(CameraPosition::external, 0) == PresentedMounting({90, 0, false, 0}) && of(CameraPosition::external, 180) == PresentedMounting({90, 0, false, 0}),
            "an external camera states 90 and is not turned");
    require(of(CameraPosition::front, std::numeric_limits<double>::quiet_NaN()) == PresentedMounting({0, 0, false, 0}), "no angle is not imitated or turned");
    require(of(CameraPosition::front, std::numeric_limits<double>::infinity()) == PresentedMounting({0, 0, false, 0}), "nor an infinite one");
    require(of(CameraPosition::front, 0.4) == of(CameraPosition::front, 0) && of(CameraPosition::front, 359.7) == of(CameraPosition::front, 0),
            "an angle is rounded to a quarter turn first");
    require(of(CameraPosition::front, 89.6) == PresentedMounting({270, 0, false, 180}), "so a front camera at 89.6 is at 90");
    require(of(CameraPosition::front, -180) == of(CameraPosition::front, 180), "and taken within one turn");
    // Whatever the angle: a built-in front camera with an angle states 270, and the turns the platform and the host apply
    // take its own angle there (the connection's turn first, which brings an imitated camera to 90, then the half turn); a
    // camera that is not a front one states what sensorOrientationOf does and is not turned at all.
    for (const auto position : {CameraPosition::back, CameraPosition::front, CameraPosition::external})
        for (double angle = -720; angle <= 720; angle += 22.5) {
            const auto mounting = presentedMountingOf(position, angle);
            require(mounting.presentationTurn == 0 || mounting.presentationTurn == 180, "the host's turn is a half turn or none");
            if (position == CameraPosition::front && std::isfinite(angle)) {
                require(mounting.stated == 270, "a front camera states 270");
                require((sensorOrientationOf(position, angle) + 720 - mounting.connectionTurn - mounting.presentationTurn) % 360 ==
                            mounting.stated,
                        "the turns take the camera's own angle to the stated one");
                require(mounting.emulatesEarlierFront == (sensorOrientationOf(position, angle) % 180 == 0), "only a camera at the top is imitated");
                require(mounting.connectionTurn == (mounting.emulatesEarlierFront ? (sensorOrientationOf(position, angle) + 270) % 360 : 0),
                        "the connection's turn is the imitation's, as before");
            } else {
                require(!mounting.emulatesEarlierFront && mounting.connectionTurn == 0 && mounting.presentationTurn == 0 &&
                            mounting.stated == sensorOrientationOf(position, angle),
                        "a camera that is not a front one with an angle states the platform's angle and is not turned");
            }
        }
}

// A metering or focus region of the picture the guest sees is the same area of the picture before the host turned it: each
// layer that turned the picture maps the guest's region back through its own turn, the device through the camera's half
// turn and the backend through its video connection's quarter turn.
void aRegionIsMappedBackThroughATurnOfThePicture()
{
    const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-6f; };
    const auto same = [&](const CameraRegion& a, const CameraRegion& b) {
        return near(a.x, b.x) && near(a.y, b.y) && near(a.width, b.width) && near(a.height, b.height) && a.weight == b.weight;
    };
    const CameraRegion region {0.25f, 0.5f, 0.125f, 0.25f, 7};
    require(same(regionBeforeTurn(region, 0), region), "no turn leaves a region as it is");
    // 180: the area opposite, of the same size.
    require(same(regionBeforeTurn(region, 180), {0.625f, 0.25f, 0.125f, 0.25f, 7}), "a half turn mirrors both axes");
    // 90 clockwise: the picture's left edge is the turned picture's top, read from the bottom. The region's top-left corner
    // (0.25, 0.5) of the turned picture is, before the turn, (y, 1 - x - width) = (0.5, 0.625) with the sides swapped.
    require(same(regionBeforeTurn(region, 90), {0.5f, 0.625f, 0.25f, 0.125f, 7}), "a quarter turn clockwise swaps the sides");
    require(same(regionBeforeTurn(region, 270), {0.25f, 0.25f, 0.25f, 0.125f, 7}), "and a quarter turn the other way");
    for (const std::uint32_t turn : {45u, 360u, 1u, 450u}) require(same(regionBeforeTurn(region, turn), region), "a turn that is no quarter turn is none");

    // The turns compose: mapping back through 90 and then 90 again is mapping back through 180, and so on; four make none.
    CameraRegion walked = region;
    for (int step = 0; step < 4; ++step) walked = regionBeforeTurn(walked, 90);
    require(same(walked, region), "four quarter turns are none");
    require(same(regionBeforeTurn(regionBeforeTurn(region, 90), 90), regionBeforeTurn(region, 180)), "two quarter turns are a half turn");
    require(same(regionBeforeTurn(regionBeforeTurn(region, 90), 270), region), "a quarter turn and its inverse are none");
    require(same(regionBeforeTurn(regionBeforeTurn(region, 180), 180), region), "a half turn twice is none");

    // The centre of the region, which is what the platform's points of interest take, goes where the point does: the turned
    // picture's point (u, v) is the picture's (v, 1 - u) after a quarter turn clockwise.
    const auto centre = [](const CameraRegion& r) { return std::pair<float, float>(r.x + r.width / 2, r.y + r.height / 2); };
    const auto [cx, cy] = centre(region);
    require(near(centre(regionBeforeTurn(region, 90)).first, cy) && near(centre(regionBeforeTurn(region, 90)).second, 1 - cx), "its centre");
    require(near(centre(regionBeforeTurn(region, 270)).first, 1 - cy) && near(centre(regionBeforeTurn(region, 270)).second, cx), "likewise");
    require(near(centre(regionBeforeTurn(region, 180)).first, 1 - cx) && near(centre(regionBeforeTurn(region, 180)).second, 1 - cy), "and for 180");

    // A whole-frame region is the whole frame, and a region inside the picture stays inside and one outside stays outside:
    // the mapping validates nothing and invalidates nothing.
    for (const std::uint32_t turn : {0u, 90u, 180u, 270u}) {
        require(same(regionBeforeTurn({0, 0, 1, 1, 1}, turn), {0, 0, 1, 1, 1}), "the whole picture");
        const auto inside = regionBeforeTurn({0.1f, 0.2f, 0.3f, 0.4f, 1}, turn);
        require(inside.x >= 0 && inside.y >= 0 && inside.x + inside.width <= 1.0001f && inside.y + inside.height <= 1.0001f, "inside stays inside");
        const auto outside = regionBeforeTurn({1.5f, 0.2f, 0.3f, 0.4f, 1}, turn);
        require(outside.x < 0 || outside.y < 0 || outside.x + outside.width > 1.0f || outside.y + outside.height > 1.0f, "outside stays outside");
        const auto huge = regionBeforeTurn({60000.0f, 60000.0f, 60000.0f, 60000.0f, 9}, turn);
        require(std::isfinite(huge.x) && std::isfinite(huge.y) && std::isfinite(huge.width) && std::isfinite(huge.height) && huge.weight == 9,
                "the largest value the wire carries stays a number");
    }
}

// An imitated front camera (the square sensor of the newest phones) is turned twice: by its video connection's quarter turn and
// then by the host's half turn. The guest's region is in the twice turned picture, the device maps it back through the half
// turn and the backend through the connection's quarter turn, and what reaches the platform is the area of the camera's own
// picture the guest meant, whichever turn the connection has.
// The session's own choice of its devices' colour spaces is left alone unless a camera whose stills the host turns is in it.
void theSessionKeepsItsColourChoiceUnlessATurnedCameraIsIn()
{
    const std::uint32_t notTurned[] = {0};
    const std::uint32_t backAndExternal[] = {0, 0};
    const std::uint32_t front[] = {180};
    const std::uint32_t frontAndBack[] = {180, 0};
    const std::uint32_t backAndFront[] = {0, 180};
    require(!sessionHoldsColourSpaces({}), "an empty session keeps the system's choice");
    require(!sessionHoldsColourSpaces(notTurned) && !sessionHoldsColourSpaces(backAndExternal),
            "a session of cameras that are not turned keeps it, a back-only one included");
    require(sessionHoldsColourSpaces(front) && sessionHoldsColourSpaces(frontAndBack) && sessionHoldsColourSpaces(backAndFront),
            "a session with a camera presented half around does not, in either order of the cameras");
}

void aRegionIsMappedBackThroughTheConnectionAndTheHalfTurnTogether()
{
    const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };
    const auto same = [&](const CameraRegion& a, const CameraRegion& b) {
        return near(a.x, b.x) && near(a.y, b.y) && near(a.width, b.width) && near(a.height, b.height) && a.weight == b.weight;
    };
    // The picture turned clockwise by `turn`: the area of it the region of the picture before the turn is. (A corner at the top
    // left goes to the top right, so a quarter turn sends (x, y, w, h) to (1 - y - h, x, h, w).)
    const auto turned = [](CameraRegion region, std::uint32_t turn) {
        for (std::uint32_t quarter = 0; quarter < turn / 90; ++quarter)
            region = {1.0f - region.y - region.height, region.x, region.height, region.width, region.weight};
        return region;
    };
    const CameraRegion regions[] = {{0.25f, 0.5f, 0.125f, 0.25f, 7}, {0.0f, 0.0f, 1.0f, 1.0f, 1}, {0.7f, 0.1f, 0.2f, 0.3f, 3}, {0.4f, 0.4f, 0.05f, 0.05f, 255}};
    std::size_t imitated = 0;
    for (const double angle : {0.0, 180.0, 90.0, 270.0}) {
        const auto mounting = presentedMountingOf(CameraPosition::front, angle);
        for (const auto& own : regions) {
            // What the platform holds is its own picture's area; the guest's picture is that turned by the connection and then
            // by the host.
            const CameraRegion guests = turned(turned(own, mounting.connectionTurn), mounting.presentationTurn);
            const CameraRegion afterDevice = regionBeforeTurn(guests, mounting.presentationTurn);
            const CameraRegion afterBackend = regionBeforeTurn(afterDevice, mounting.connectionTurn);
            require(same(afterBackend, own), "the area the guest meant is the area of the camera's own picture");
            require(same(afterBackend, regionBeforeTurn(guests, (mounting.connectionTurn + mounting.presentationTurn) % 360)),
                    "the two layers together are the one turn of both");
            // The device's layer alone is not enough where the connection turns, and the backend's alone is never enough.
            if (mounting.connectionTurn != 0 && !same(own, turned(own, 180)))
                require(!same(afterDevice, own), "a connection that turns leaves the region turned after the device's layer");
        }
        imitated += mounting.connectionTurn != 0 ? 1 : 0;
    }
    require(imitated == 2, "the two imitated mountings are the ones with a quarter turn in the connection (portrait 0 and 180)");
    const auto bottom = presentedMountingOf(CameraPosition::front, 0.0);
    const auto top = presentedMountingOf(CameraPosition::front, 180.0);
    require(bottom.connectionTurn == 270 && top.connectionTurn == 90 && bottom.presentationTurn == 180 && top.presentationTurn == 180,
            "their connections turn by 270 and by 90, and the host by half a turn");
}

// A square format that lists dynamic aspect ratios is described by the crop the device is pinned at: 3x4 where the video
// connection is turned a quarter turn, else 4x3, a landscape frame of N x 3N/4 either way, with the landscape 4:3 photo
// sizes it lists. A format that lists none is as listed. One that cannot be described is dropped, never offered in
// another shape.
void aSquareFormatThatListsRatiosIsDescribedByItsPinnedCrop()
{
    constexpr std::uint8_t all = 0x1f;
    const std::uint8_t tallAndWide = aspectRatioBit(CameraAspectRatio::r3x4) | aspectRatioBit(CameraAspectRatio::r4x3);
    const auto square = [](std::uint32_t side, std::uint8_t ratios, Sizes photos) {
        auto facts = fmt(side, side, std::move(photos), {{1, 30}, {1, 60}}, false, true);
        facts.aspectRatios = ratios;
        return facts;
    };
    const Sizes photos {{4032, 4032}, {3024, 4032}, {4032, 3024}, {2016, 1512}, {1512, 2016}, {4032, 2268}};
    for (const std::uint8_t ratios : {all, tallAndWide}) {
        const auto presented = presentedFormatOf(square(4032, ratios, photos), 270);
        require(presented && presented->width == 4032 && presented->height == 3024, "a 4032 square is 4032x3024");
        require(presented->pinnedRatio == CameraAspectRatio::r3x4, "pinned at 3x4");
        require(presented->photoSizes == Sizes({{4032, 3024}, {2016, 1512}}), "only the landscape 4:3 photos stay, in order");
        require(presented->aspectRatios == ratios && presented->fullRange == false && presented->multiCamera &&
                    presented->frameRates == Spans({{1, 30}, {1, 60}}),
                "the rest is as listed");
        const auto quarter = presentedFormatOf(square(4032, ratios, photos), 90);
        require(quarter && *quarter == *presented, "a quarter turn the other way pins the same");
    }
    for (const std::uint32_t turn : {0u, 180u}) {
        const auto presented = presentedFormatOf(square(1280, all, photos), turn);
        require(presented && presented->width == 1280 && presented->height == 960 && presented->pinnedRatio == CameraAspectRatio::r4x3,
                "a turn that is no quarter pins 4x3, still N x 3N/4");
    }
    // A format that lists none is as listed, whatever its turn.
    const auto plain = fmt(1920, 1440, photos, {{1, 30}}, true, true);
    for (const std::uint32_t turn : {0u, 90u, 180u, 270u}) {
        const auto presented = presentedFormatOf(plain, turn);
        require(presented && *presented == plain && presented->pinnedRatio == CameraAspectRatio::none, "a format without ratios is unchanged");
    }
    // The ratio the turn needs is not listed: dropped, but the other turn's format is not.
    const std::uint8_t noTall = aspectRatioBit(CameraAspectRatio::r1x1) | aspectRatioBit(CameraAspectRatio::r16x9) | aspectRatioBit(CameraAspectRatio::r4x3);
    require(!presentedFormatOf(square(2016, noTall, photos), 270).has_value(), "a format without 3x4 is dropped for a quarter turn");
    require(presentedFormatOf(square(2016, noTall, photos), 0).has_value(), "and kept for none");
    require(!presentedFormatOf(square(2016, aspectRatioBit(CameraAspectRatio::r1x1), photos), 0).has_value() &&
                !presentedFormatOf(square(2016, aspectRatioBit(CameraAspectRatio::r1x1), photos), 270).has_value(),
            "a format that lists only 1x1 is dropped for any turn");
    // One that is not square, or whose side is not divisible by four, cannot be described.
    auto wide = fmt(1920, 1440, photos, {{1, 30}});
    wide.aspectRatios = all;
    require(!presentedFormatOf(wide, 270).has_value() && !presentedFormatOf(wide, 0).has_value(), "a format that lists ratios and is not square is dropped");
    require(!presentedFormatOf(square(1282, all, photos), 270).has_value() && !presentedFormatOf(square(0, all, photos), 270).has_value(),
            "so is one whose side does not divide by four, or has none");
    // No landscape 4:3 photo listed: a format with no photo.
    const auto none = presentedFormatOf(square(1280, all, {{1280, 1280}, {960, 1280}}), 270);
    require(none && none->photoSizes.empty() && none->width == 1280 && none->height == 960, "a format with no such photo lists none");
    // A pinned crop's width is the side and its frame is exactly 4:3 for every side divisible by four.
    for (std::uint32_t side = 4; side <= 4032; side += 4) {
        const auto presented = presentedFormatOf(square(side, all, {}), 270);
        require(presented && presented->width == side && std::uint64_t(presented->height) * 4 == std::uint64_t(side) * 3, "exactly 4:3");
    }
}

// The field of view of a pinned crop is the larger of the two crops', so a view cropped from the square is never stated
// narrower than it is; the order of the arguments does not matter.
void thePinnedCropsFieldOfViewIsTheLargerOfTheTwo()
{
    require(pinnedFieldOfView(80.0f, 90.0f) == 90.0f && pinnedFieldOfView(90.0f, 80.0f) == 90.0f, "the larger");
    require(pinnedFieldOfView(85.5f, 85.5f) == 85.5f, "equal");
    require(pinnedFieldOfView(0.0f, 72.0f) == 72.0f, "a crop that states none does not narrow it");
}

// The ratios a format lists are a mask of one bit each, and none has no bit.
void theRatioBitsAreDistinctAndNoneHasNone()
{
    std::uint32_t seen = 0;
    for (const auto ratio : {CameraAspectRatio::r1x1, CameraAspectRatio::r16x9, CameraAspectRatio::r9x16, CameraAspectRatio::r4x3,
                             CameraAspectRatio::r3x4}) {
        require(std::popcount(unsigned(aspectRatioBit(ratio))) == 1 && (seen & aspectRatioBit(ratio)) == 0, "one bit each");
        seen |= aspectRatioBit(ratio);
        require(aspectRatioName(ratio) != "-", "and a name");
    }
    require(seen == 0x1f && aspectRatioBit(CameraAspectRatio::none) == 0 && aspectRatioName(CameraAspectRatio::none) == "-", "five bits, none has none");
}

// The same rule over generated tables: nothing above the limit survives on a front camera, ever; every kept size was
// listed, and every listed size within the limit is kept, in its order; the formats are otherwise as listed; limiting
// twice is limiting once; and a back camera is not limited.
void everyGeneratedFrontTableKeepsNothingAboveTheLimit()
{
    constexpr std::uint64_t seed = 20261114;
    constexpr int tables = 20000;
    Rng rng {seed};
    std::uint64_t outside = 0, emptied = 0;
    for (int table = 0; table < tables; ++table) {
        const std::string where = " (seed " + std::to_string(seed) + ", table " + std::to_string(table) + ")";
        const auto formats = generateTable(rng);
        const auto limited = limitedStillFormats(CameraPosition::front, formats);
        require(limited.size() == formats.size(), "one format per format" + where);
        for (std::size_t index = 0; index < formats.size(); ++index) {
            require(sameButForThePhotoSizes(formats[index], limited[index]), "only the photo sizes change" + where);
            Sizes within;
            for (const auto& size : formats[index].photoSizes) {
                outside += areaOfSize(size) > frontStillMaximumPixels;
                if (size.width != 0 && size.height != 0 && areaOfSize(size) <= frontStillMaximumPixels) within.push_back(size);
            }
            require(limited[index].photoSizes == within, "exactly the listed sizes within the limit survive, in order" + where);
            for (const auto& size : limited[index].photoSizes)
                require(areaOfSize(size) <= frontStillMaximumPixels, "nothing above the limit survives" + where);
            // What a camera is offered and prepared can never be above it either, whatever the tier's cap.
            for (const std::uint64_t cap : {std::uint64_t(0), std::uint64_t(12'700'000)}) {
                for (const auto& size : stillsOf(limited[index], cap)) require(areaOfSize(size) <= frontStillMaximumPixels, "no offered size above it" + where);
                const auto maximum = photoMaximum(limited[index], cap);
                require(!maximum || areaOfSize(*maximum) <= frontStillMaximumPixels, "no output maximum above it" + where);
            }
            emptied += limited[index].photoSizes.empty() && !formats[index].photoSizes.empty();
        }
        require(limitedStillFormats(CameraPosition::front, limited) == limited, "limiting twice is limiting once" + where);
        require(limitedStillFormats(CameraPosition::back, formats) == formats, "a back camera is not limited" + where);
    }
    require(outside > 0 && emptied > 0, "the generated tables reach sizes above the limit and formats left with none");
}

// The second real front table, an iPhone 15 Pro Max's (44 formats): its multi-camera formats make 4032x3024 and
// 2016x1512, and the camera keeps its concurrency. The limit leaves each format the sizes it lists within it (the formats
// that list only larger sizes list none), the largest size kept anywhere is 2016x1512, the video offer and the
// concurrency are what they were, and the anchor's stills become 2016x1512 and 1920x1440 where they were 4032x3024 and
// 1920x1440.
void theFrontStillLimitOnTheSecondRealFrontTable()
{
    const auto listed = factsOfRows(uml::testing::phoneFrontFormats());
    require(listed.size() == 44, "the iPhone's front table");
    const auto limited = limitedStillFormats(CameraPosition::front, listed);
    require(limited.size() == listed.size());
    std::uint64_t largest = 0;
    for (std::size_t index = 0; index < listed.size(); ++index) {
        require(sameButForThePhotoSizes(listed[index], limited[index]), "only the photo sizes are limited");
        Sizes within;
        for (const auto& size : listed[index].photoSizes)
            if (areaOfSize(size) <= frontStillMaximumPixels) within.push_back(size);
        require(limited[index].photoSizes == within, "each format keeps the sizes it lists within the limit, in order");
        for (const auto& size : within) largest = std::max(largest, areaOfSize(size));
    }
    require(largest == areaOfSize({2016, 1512}), "the largest photo kept anywhere is 2016x1512");
    const auto rules = smallRules(listed);
    const bool concurrent = keepsConcurrency(listed, rules);
    require(concurrent && keepsConcurrency(limited, rules), "it keeps its concurrency listed and limited");
    const auto before = chooseAnchor(listed, rules, concurrent);
    const auto after = chooseAnchor(limited, rules, concurrent);
    require(before && after && listed[*before].multiCamera && listed[*after].multiCamera, "both anchors are multi-camera formats");
    require(listed[*after].width == 1920 && listed[*after].height == 1440, "the anchor is a 1920x1440 format");
    const auto offerBefore = offerOf(listed[*before], rules);
    const auto offerAfter = offerOf(limited[*after], rules);
    require(offerBefore.stills == Sizes({{4032, 3024}, {1920, 1440}}) && offerAfter.stills == Sizes({{2016, 1512}, {1920, 1440}}),
            "the stills were 4032x3024 and 1920x1440 and are 2016x1512 and 1920x1440");
    require(offerBefore.videoSizes == offerAfter.videoSizes && offerBefore.rates == offerAfter.rates && offerBefore.box == offerAfter.box,
            "the video sizes, rates and box are the same");
    for (const auto& size : offerAfter.stills) {
        require(formatMakesPhoto(limited[*after], size, 12'700'000), "an offered size is made");
        require(exactPhotoSize(listed[*after].photoSizes, size, *photoMaximum(limited[*after], 12'700'000)) == std::optional<CameraStillSize>(size),
                "and passes the capture check on the live list");
    }
    require(photoMaximum(limited[*after], 12'700'000) == std::optional<CameraStillSize>(CameraStillSize {2016, 1512}), "the output's maximum");
}

void aCamerasLastUseSurvivesTheNextSessionsDiscovery()
{
    // A platform session that closes when the cameras are idle states the mounting again when it opens: the record keeps
    // what the last admissions of the camera set (the video connection, the photo output, the ratio, the refusals and
    // the Exif orientation of the last photo) beside the new discovery.
    CameraPresentation kept;
    kept.position = CameraPosition::front;
    kept.stated = 90;
    kept.turn = 0;
    kept.anchorWidth = 1920;
    kept.anchorHeight = 1440;
    kept.ratioKnown = true;
    kept.ratioPinned = CameraAspectRatio::r3x4;
    kept.ratioNow = CameraAspectRatio::r3x4;
    kept.ratioWidth = 1512;
    kept.ratioHeight = 2016;
    kept.ratioError = 0;
    kept.videoKnown = true;
    kept.videoDefaultAngle = 0;
    kept.videoAngle = 270;
    kept.videoAngleSupported = CameraFact::yes;
    kept.videoAskedWidth = 1920;
    kept.videoAskedHeight = 1440;
    kept.videoFirstWidth = 1920;
    kept.videoFirstHeight = 1440;
    kept.photoKnown = true;
    kept.compensationSupported = CameraFact::yes;
    kept.compensationEnabled = CameraFact::yes;
    kept.photoAngle = 0;
    kept.photoMaximumWidth = 2016;
    kept.photoMaximumHeight = 1512;
    kept.exifOrientation = 6;
    kept.refusals = 2;
    kept.lastRefusal = 4;
    CameraPresentation discovered;
    discovered.position = CameraPosition::front;
    discovered.deviceType = CameraDeviceType::wide;
    discovered.coordinatorAngles = {0, 180, 90, 270};
    discovered.stated = 90;
    discovered.turn = 270;
    discovered.presentationTurn = 180;
    discovered.earlier = true;
    discovered.anchorWidth = 1920;
    discovered.anchorHeight = 1440;
    discovered.anchorRatios = std::uint8_t(aspectRatioBit(CameraAspectRatio::r4x3) | aspectRatioBit(CameraAspectRatio::r3x4));
    discovered.anchorFieldOfView = 85;
    discovered.frontDevices = 2;
    discovered.squareFormats = true;
    discovered.dynamicRatios = true;
    const CameraPresentation merged = withDiscovery(kept, discovered);
    require(merged.deviceType == CameraDeviceType::wide && merged.coordinatorAngles == discovered.coordinatorAngles && merged.turn == 270 &&
                merged.presentationTurn == 180 && merged.earlier && merged.anchorRatios == discovered.anchorRatios && merged.anchorFieldOfView == 85 &&
                merged.frontDevices == 2 && merged.squareFormats && merged.dynamicRatios,
            "the discovery is the new session's");
    require(merged.ratioKnown && merged.ratioPinned == CameraAspectRatio::r3x4 && merged.ratioWidth == 1512 && merged.videoKnown &&
                merged.videoAngle == 270 && merged.videoAskedWidth == 1920 && merged.videoFirstWidth == 1920 && merged.photoKnown &&
                merged.compensationSupported == CameraFact::yes && merged.compensationEnabled == CameraFact::yes &&
                merged.photoMaximumWidth == 2016 && merged.exifOrientation == 6 && merged.refusals == 2 && merged.lastRefusal == 4,
            "the last use of the camera stays");
    require(withDiscovery(CameraPresentation {}, discovered) == discovered, "a camera that was not used is its discovery");
}

void aCameraThatImitatesAnEarlierOneKeepsItsPhotoConnectionAngle()
{
    // The photo connection's angle changes the Exif alone, and the host drops the platform's Exif: a camera that imitates
    // an earlier front camera leaves its connection at its default, as the setup proven on that camera did. Every other
    // camera is set to 0, as before.
    require(photoConnectionTurnOf(presentedMountingOf(CameraPosition::front, 0)) == std::nullopt, "a front camera at 0 leaves it");
    require(photoConnectionTurnOf(presentedMountingOf(CameraPosition::front, 180)) == std::nullopt, "a front camera at 180 leaves it");
    require(photoConnectionTurnOf(presentedMountingOf(CameraPosition::front, 90)) == std::optional<std::uint32_t>(0), "a front camera at 90 is set to 0");
    require(photoConnectionTurnOf(presentedMountingOf(CameraPosition::front, 270)) == std::optional<std::uint32_t>(0), "a front camera at 270 is set to 0");
    require(photoConnectionTurnOf(presentedMountingOf(CameraPosition::back, 90)) == std::optional<std::uint32_t>(0), "a back camera at 90 is set to 0");
    require(photoConnectionTurnOf(presentedMountingOf(CameraPosition::back, 0)) == std::optional<std::uint32_t>(0), "a back camera at 0 is set to 0");
    require(photoConnectionTurnOf(presentedMountingOf(CameraPosition::external, 0)) == std::optional<std::uint32_t>(0), "an external camera is set to 0");
    require(photoConnectionTurnOf(presentedMountingOf(CameraPosition::front, std::nan(""))) == std::optional<std::uint32_t>(0),
            "a front camera with no angle is not imitated and is set to 0");
}

void aStillReadsTheCompensationAfterTheCommit()
{
    const auto step = [](bool imitates, CameraFact supported, CameraFact enabled, CameraReading reading) {
        return stillCompensationStep(imitates, supported, enabled, reading);
    };
    using Step = CompensationStep;
    const CameraFact yes = CameraFact::yes, no = CameraFact::no, unknown = CameraFact::unknown;
    const CameraReading before = CameraReading::beforeCommit, after = CameraReading::afterCommit;
    // A reading taken before the commit decides nothing, whatever it says.
    for (const auto supported : {yes, no, unknown})
        for (const auto enabled : {yes, no, unknown})
            require(step(true, supported, enabled, before) == Step::proceed && step(true, supported, enabled, CameraReading::unknown) == Step::proceed,
                    "a reading before the commit never refuses a still");
    // After it: on is accepted, supported and off is turned on, everything else refuses.
    require(step(true, yes, yes, after) == Step::proceed, "supported and on is accepted");
    require(step(true, yes, no, after) == Step::enable, "supported and off is turned on");
    require(step(true, no, no, after) == Step::refuse && step(true, no, yes, after) == Step::refuse, "unsupported refuses");
    require(step(true, unknown, unknown, after) == Step::refuse && step(true, yes, unknown, after) == Step::refuse && step(true, unknown, yes, after) == Step::refuse,
            "a reading that failed refuses");
    // A camera that imitates nothing is never asked.
    for (const auto supported : {yes, no, unknown})
        for (const auto enabled : {yes, no, unknown})
            for (const auto reading : {before, after, CameraReading::unknown})
                require(step(false, supported, enabled, reading) == Step::proceed, "a camera that imitates nothing is not asked");
    require(readingName(CameraReading::beforeCommit) == "before-commit" && readingName(CameraReading::afterCommit) == "after-commit" &&
                readingName(CameraReading::unknown) == "-",
            "the readings have their words");
}

void aStillWaitsForAPendingRatioPinInsideTheStillBudget()
{
    using R = CameraAspectRatio;
    using Step = RatioStillStep;
    require(ratioStillStep(R::none, R::r4x3, false, 0) == Step::proceed && ratioStillStep(R::none, R::none, true, 7) == Step::proceed,
            "no pin asks nothing");
    require(ratioStillStep(R::r3x4, R::r3x4, false, 0) == Step::proceed, "a ratio held after the completion proceeds");
    require(ratioStillStep(R::r3x4, R::r4x3, true, 0) == Step::wait && ratioStillStep(R::r3x4, R::r3x4, true, 0) == Step::wait,
            "a completion that has not arrived is waited for, whatever the device holds meanwhile");
    require(ratioStillStep(R::r3x4, R::r4x3, false, 0) == Step::refuseMoved, "a ratio that differs after the completion refuses");
    require(ratioStillStep(R::r3x4, R::r3x4, false, -11800) == Step::refuseFailed && ratioStillStep(R::r3x4, R::r4x3, false, 5) == Step::refuseFailed,
            "a completion error refuses");
    // The wait is the settle wait's, inside the same budget.
    const std::uint64_t settled = 1'000'000, now = 10'000'000'000;
    const auto idle = scheduledStillStep(false, true, settled, now, 0, false);
    require(idle.step == ScheduledStillStep::fire, "a settled camera with no pending pin fires");
    const auto pending = scheduledStillStep(false, true, settled, now, 0, true);
    require(pending.step == ScheduledStillStep::wait && pending.waitNanoseconds == ratioPinPollNanoseconds &&
                pending.waitedNanoseconds == ratioPinPollNanoseconds,
            "a pending pin waits one poll, which counts against the budget");
    require(scheduledStillStep(false, true, now - 100'000'000, now, 0, true).waitNanoseconds == photoOutputSettleNanoseconds - 100'000'000,
            "the settle time left outlasts a poll");
    require(scheduledStillStep(false, true, settled, now, stillWaitBudgetNanoseconds - ratioPinPollNanoseconds, true).step == ScheduledStillStep::wait,
            "the last poll the budget allows is waited");
    require(scheduledStillStep(false, true, settled, now, stillWaitBudgetNanoseconds - ratioPinPollNanoseconds + 1, true).step == ScheduledStillStep::fail,
            "a poll past the budget fails the still");
    require(scheduledStillStep(false, true, settled, now, stillWaitBudgetNanoseconds, true).step == ScheduledStillStep::fail,
            "a spent budget fails it");
    require(scheduledStillStep(true, true, settled, now, 0, true).step == ScheduledStillStep::drop &&
                scheduledStillStep(false, false, settled, now, 0, true).step == ScheduledStillStep::drop,
            "an ended session or a gone request drops it");
    // The five-argument form is the one without a pin.
    require(scheduledStillStep(false, true, settled, now, 0).step == ScheduledStillStep::fire, "no pin is the old rule");
    // A wait of polls cannot outlast the budget.
    std::uint64_t waited = 0;
    int polls = 0;
    for (;; ++polls) {
        const auto next = scheduledStillStep(false, true, settled, now, waited, true);
        if (next.step != ScheduledStillStep::wait) {
            require(next.step == ScheduledStillStep::fail, "the polls end in a failure");
            break;
        }
        waited = next.waitedNanoseconds;
        require(polls < 1000, "the polls end");
    }
    require(waited <= stillWaitBudgetNanoseconds && waited + ratioPinPollNanoseconds > stillWaitBudgetNanoseconds, "and what they waited is within the budget");
}

// A ratio change is one operation at a time: asked before the device is told, so that a completion that comes at once, or
// never, or an earlier operation's, is never taken for the one under way; frames are held until the completion arrives
// without an error; a completion that does not come within the budget of a running session ends the camera's frames.
void aRatioChangeIsOneOperationAtATime()
{
    constexpr std::uint64_t budget = ratioCompletionBudgetNanoseconds;
    constexpr std::uint64_t start = 5'000'000'000;
    using Step = RatioFrameStep;
    require(budget == stillWaitBudgetNanoseconds, "a ratio change waits no longer than a still");
    require(ratioFrameStep(RatioPin {}) == Step::deliver, "a camera that pinned nothing delivers");
    // A deferred success: held while it is under way, delivered once it came.
    {
        RatioPin pin;
        ratioPinAsked(pin, 1);
        require(pin.pending && ratioFrameStep(pin) == Step::hold, "frames are held while the change is under way");
        require(ratioPinArmed(pin, start) && !ratioPinArmed(pin, start + 1), "the budget begins once");
        require(!ratioPinTimedOut(pin, start + budget - 1), "inside the budget it waits");
        require(ratioPinCompleted(pin, 1, 0) && !pin.pending && pin.error == 0, "the completion arrives");
        require(ratioFrameStep(pin) == Step::deliver && ratioPinErrorForStill(pin) == 0, "frames are delivered once it did, without error");
        require(!ratioPinTimedOut(pin, start + 10 * budget), "a completed change never times out");
    }
    // A completion error: held, and the camera fails, with the error for the still.
    {
        RatioPin pin;
        ratioPinAsked(pin, 1);
        (void)ratioPinArmed(pin, start);
        require(ratioPinCompleted(pin, 1, 42) && ratioFrameStep(pin) == Step::failed && ratioPinErrorForStill(pin) == 42,
                "an error ends it, and the still is told the code");
        require(ratioPinCompleted(pin, 1, -11800) && ratioPinErrorForStill(pin) == -11800, "the newest completion of the operation stands");
    }
    // A completion at once (the setter calls it before it returns, or the device already holds the ratio): never pending.
    {
        RatioPin pin;
        ratioPinAsked(pin, 1);
        require(ratioPinCompleted(pin, 1, 0), "the completion comes before the session runs");
        require(!pin.pending && ratioFrameStep(pin) == Step::deliver && !ratioPinArmed(pin, start), "it is not pending, and never armed");
    }
    // A later change while the earlier one's result is kept: pending again, and the earlier operation's completion is ignored.
    {
        RatioPin pin;
        ratioPinAsked(pin, 1);
        require(ratioPinCompleted(pin, 1, 0) && ratioFrameStep(pin) == Step::deliver, "the first change completed");
        ratioPinAsked(pin, 2);
        require(pin.pending && pin.error == 0 && ratioFrameStep(pin) == Step::hold,
                "a later change is under way, whatever the earlier one's result was");
        require(!ratioPinCompleted(pin, 1, 7) && pin.pending && pin.error == 0, "the earlier operation's late completion is ignored");
        ratioPinAsked(pin, 3);
        require(!ratioPinCompleted(pin, 2, 0) && pin.pending, "and so is a later one's of an operation that was replaced");
        require(ratioPinCompleted(pin, 3, 0) && ratioFrameStep(pin) == Step::deliver, "the current operation's completes it");
    }
    // A completion that never comes: held while the budget runs, then timed out once, and the guest's camera ends.
    {
        RatioPin pin;
        ratioPinAsked(pin, 1);
        require(!ratioPinTimedOut(pin, start + 100 * budget), "a change of a session that does not run yet cannot time out");
        (void)ratioPinArmed(pin, start);
        require(!ratioPinTimedOut(pin, start - 1), "a clock that went back does not time it out");
        require(ratioFrameStep(pin) == Step::hold && !ratioPinTimedOut(pin, start + budget - 1), "it holds until the budget is spent");
        require(ratioPinTimedOut(pin, start + budget) && !ratioPinTimedOut(pin, start + budget + 1), "it times out once");
        require(ratioFrameStep(pin) == Step::timedOut && ratioPinErrorForStill(pin) == ratioNoCompletion, "frames stay held, the still is told");
        require(ratioPinCompleted(pin, 1, 0) && ratioFrameStep(pin) == Step::timedOut,
                "a completion after the time ran out is kept but does not undo it");
        // A zero clock still arms.
        RatioPin other;
        ratioPinAsked(other, 9);
        require(ratioPinArmed(other, 0) && other.armedAt != 0, "a clock at zero arms too");
    }
    // Without a change asked for, a completion belongs to nothing.
    RatioPin none;
    require(!ratioPinCompleted(none, 0, 5) && !ratioPinArmed(none, start) && ratioFrameStep(none) == Step::deliver, "no change, nothing to complete");
}

// The notes the platform keeps across its sessions are bounded: the current cameras' and the most recently used others'.
void theHeldNotesAreBoundedToTheCurrentCamerasAndRecentOnes()
{
    std::vector<HeldNoteUse> held;
    for (std::uint64_t index = 0; index < 20; ++index) held.push_back({"device-" + std::to_string(index), index + 1});
    const std::vector<std::string> current {"device-0", "device-1"};
    const auto dropped = heldNotesToDrop(held, current, 8);
    require(dropped.size() == 20 - 2 - 8, "everything but the current cameras and eight others is let go");
    for (const auto& name : dropped) require(name != "device-0" && name != "device-1", "a current camera is never let go");
    std::set<std::string> gone(dropped.begin(), dropped.end());
    for (std::uint64_t index = 12; index < 20; ++index) require(gone.count("device-" + std::to_string(index)) == 0, "the newest are kept");
    require(gone.count("device-2") == 1 && gone.count("device-11") == 1, "the oldest are dropped");
    require(heldNotesToDrop(held, {}, 20).empty() && heldNotesToDrop(held, current, 18).empty(), "nothing is dropped within the bound");
    require(heldNotesToDrop({}, current, 8).empty(), "an empty cache drops nothing");
    // Equal uses fall in a fixed order.
    const std::vector<HeldNoteUse> equal {{"b", 1}, {"a", 1}, {"c", 1}};
    require(heldNotesToDrop(equal, {}, 2) == std::vector<std::string> {"c"}, "ties are broken by the identifier");
    // A growing cache that is trimmed at each insertion never holds more than the current cameras and eight others.
    std::vector<HeldNoteUse> growing;
    std::uint64_t used = 0;
    for (std::uint64_t round = 0; round < 200; ++round) {
        growing.push_back({"external-" + std::to_string(round), ++used});
        for (const auto& name : heldNotesToDrop(growing, current, heldNotesRecent))
            std::erase_if(growing, [&](const HeldNoteUse& note) { return note.identifier == name; });
        require(growing.size() <= heldNotesRecent + 1, "the cache stays small");
    }
}

// The completion of a ratio change does not make the frames already made right: the device delivers buffers of the ratio it
// had until the first buffer the change applies to, and a callback that was entered before the completion came must not
// pass because the completion has. The samples before that buffer's timestamp (on the capture's clock) are held; where the
// timestamp could not be converted, the samples until the first of the anchor's shape.
void aSampleMadeBeforeTheChangeTookEffectNeverPasses()
{
    using B = RatioBoundary;
    const std::optional<std::int64_t> none;
    // The reviewer's interleaving: a sample of timestamp 100 is held while the change is pending, and is still held once the
    // completion has arrived and named 200 as the first buffer the change applies to.
    {
        RatioPin pin;
        ratioPinAsked(pin, 1);
        require(!ratioSampleDelivered(pin, 100, true), "a sample is held while the change is under way");
        require(ratioPinCompleted(pin, 1, 0, B::atTime, 200), "the completion arrives");
        require(!ratioSampleDelivered(pin, 100, true), "a sample made before the first buffer is not delivered after it");
        require(!ratioSampleDelivered(pin, 199, true) && ratioSampleDelivered(pin, 200, true) && ratioSampleDelivered(pin, 201, false),
                "the first buffer and every later one are, whatever the shape");
        require(!ratioSampleDelivered(pin, none, false), "a sample with no timestamp falls to the shape");
        require(ratioSampleDelivered(pin, none, true) && ratioSampleDelivered(pin, 100, true) == false, "and the timestamp still decides where it has one");
    }
    // Where the timestamp could not be converted: held until a sample of the anchor's shape, then every sample.
    {
        RatioPin pin;
        ratioPinAsked(pin, 1);
        require(ratioPinCompleted(pin, 1, 0, B::shape, 0), "the completion arrives");
        require(!ratioSampleDelivered(pin, 50, false) && !ratioSampleDelivered(pin, 60, false), "samples of another shape are held");
        require(ratioSampleDelivered(pin, 70, true), "the first of the anchor's shape passes");
        require(ratioSampleDelivered(pin, 80, false), "and the rule is over");
    }
    // A device that already held the ratio had nothing before the change.
    {
        RatioPin pin;
        ratioPinAsked(pin, 1);
        require(ratioPinCompleted(pin, 1, 0, B::none, 0) && ratioSampleDelivered(pin, 1, false) && ratioSampleDelivered(pin, none, false),
                "nothing precedes a change that was not made");
    }
    // An error, a time-out and a camera that asked for nothing.
    {
        RatioPin failed;
        ratioPinAsked(failed, 1);
        require(ratioPinCompleted(failed, 1, 42, B::atTime, 0) && !ratioSampleDelivered(failed, 1'000, true), "an error delivers nothing");
        RatioPin late;
        ratioPinAsked(late, 1);
        (void)ratioPinArmed(late, 5);
        require(ratioPinTimedOut(late, 5 + ratioCompletionBudgetNanoseconds), "the time runs out");
        require(ratioPinCompleted(late, 1, 0, B::atTime, 0) && !ratioSampleDelivered(late, 1'000, true), "a late completion delivers nothing");
        RatioPin idle;
        require(ratioSampleDelivered(idle, 1, false), "a camera with no change delivers");
    }
    // The same through the session's map; a camera with no change delivers.
    RatioPins pins;
    require(pins.sampleDelivered(7, 5, false), "a camera that asked nothing delivers");
    pins.asked(0, 3);
    require(!pins.sampleDelivered(0, 100, true) && pins.holdsFrames(0), "held while under way");
    require(pins.completed(0, 3, 0, B::atTime, 200) == RatioPins::Completion::succeeded, "the change took effect");
    require(!pins.sampleDelivered(0, 100, true) && pins.sampleDelivered(0, 200, true) && !pins.holdsFrames(0),
            "the old sample is held and the first buffer passes");
}

// A camera that left the session has no change: a watch or a completion of its operation does nothing, so it cannot fail the
// session a healthy other camera is in, and the pin of a camera that stays (an output or a still in flight needs it) is kept.
void aCameraThatLeftCannotFailTheSessionTheOthersAreIn()
{
    using B = RatioBoundary;
    using C = RatioPins::Completion;
    RatioPins pins;
    constexpr std::size_t front = 0, back = 1;
    pins.asked(front, 5);
    require(pins.armed(front, 1'000) == std::optional<std::uint64_t>(5), "the front camera's change is armed");
    pins.asked(back, 6);
    require(pins.completed(back, 6, 0, B::none, 0) == C::succeeded, "the back camera's change took effect");
    // The camera's last output goes while its change is pending.
    require(!cameraLeavesSession(true, true, false) && !cameraLeavesSession(true, false, true) && !cameraLeavesSession(false, false, false),
            "a camera stays while an output, or a still in flight, needs it, and has to have an input to leave");
    require(cameraLeavesSession(true, false, false), "and leaves with its last output");
    pins.invalidate(front);
    require(!pins.find(front) && !pins.holdsFrames(front), "its change is gone");
    require(!pins.timedOut(front, 5, 1'000 + ratioCompletionBudgetNanoseconds * 100), "its watch does nothing");
    require(pins.completed(front, 5, 42, B::none, 0) == C::ignored, "its late error does not fail the session");
    require(pins.completed(front, 5, 0, B::atTime, 1) == C::ignored, "nor does its late success revive it");
    require(!pins.armed(front, 9'000), "and nothing is armed for it");
    // The back camera is as it was.
    require(pins.find(back) && pins.find(back)->operation == 6 && !pins.holdsFrames(back) && pins.sampleDelivered(back, 1, false),
            "the other camera is untouched");
    // The camera comes back with another change: only that one counts.
    pins.asked(front, 7);
    require(pins.completed(front, 5, 42, B::none, 0) == C::ignored && pins.holdsFrames(front), "the old operation's error is still ignored");
    require(pins.armed(front, 2'000) == std::optional<std::uint64_t>(7), "the new change is armed");
    require(pins.timedOut(front, 7, 2'000 + ratioCompletionBudgetNanoseconds) && !pins.timedOut(front, 7, 2'000 + ratioCompletionBudgetNanoseconds + 1),
            "it times out once");
    // Invalidating a camera that has no change, or twice, is harmless.
    pins.invalidate(9);
    pins.invalidate(front);
    pins.invalidate(front);
    require(!pins.find(front), "a change is gone once");
    // A completion that arrives for a camera that never asked does nothing.
    require(pins.completed(9, 1, 42, B::none, 0) == C::ignored, "no change, nothing to fail");
    // An error of the current operation fails once; a late completion after the time ran out does not fail again.
    pins.asked(back, 8);
    require(pins.completed(back, 8, -11800, B::none, 0) == C::failed, "an error fails the camera");
    pins.asked(back, 9);
    (void)pins.armed(back, 3'000);
    require(pins.timedOut(back, 9, 3'000 + ratioCompletionBudgetNanoseconds), "the other time-out");
    require(pins.completed(back, 9, 42, B::none, 0) == C::afterTimeout, "a completion after it is kept and fails nothing more");
}

// What a note says of a camera's change is read where the note is published: pending while under way or never completed, else
// the code; a camera with no change keeps what its note has; a later change is pending again, whatever the earlier one said.
void theNoteTakesTheChangesStateAsItIsPublished()
{
    require(!ratioNoteCode(std::nullopt), "a camera with no change says nothing");
    require(!ratioNoteCode(RatioPin {}), "and so does a pin that asked nothing");
    RatioPin pin;
    ratioPinAsked(pin, 1);
    require(ratioNoteCode(pin) == std::optional<std::int32_t>(ratioPending), "under way is pending");
    require(ratioPinCompleted(pin, 1, 0, RatioBoundary::atTime, 5) && ratioNoteCode(pin) == std::optional<std::int32_t>(0),
            "a completion that came before the note is published says success, never pending");
    ratioPinAsked(pin, 2);
    require(ratioNoteCode(pin) == std::optional<std::int32_t>(ratioPending), "a later change is pending, whatever the earlier one said");
    require(ratioPinCompleted(pin, 2, 42, RatioBoundary::none, 0) && ratioNoteCode(pin) == std::optional<std::int32_t>(42), "an error is the code");
    ratioPinAsked(pin, 3);
    (void)ratioPinArmed(pin, 1);
    require(ratioPinTimedOut(pin, 1 + ratioCompletionBudgetNanoseconds) && ratioNoteCode(pin) == std::optional<std::int32_t>(ratioPending),
            "no completion in time stays pending");
}

int main()
{
    theDurationThatMovesOutOfTheOthersWayIsWrittenFirst();
    aRestoreThatIsRefusedLeavesNothingInTheCacheTrue();
    aRequestAfterARefusedRestoreFailsWhileTheDeviceCannotBeWritten();
    settingsAreWrittenAgainAfterARefusedRestoreEvenWhenUnchanged();
    theRecordStatesTheReadingTakenAfterTheAdmissionCommitted();
    aNewConnectionIsStabilizedByAnApplyThatLeavesTheOtherSettingsAlone();
    anApplyThatLeavesTheSettingsAloneStillStatesTheWish();
    aFormatThatSupportsNoModeLeavesTheConnectionAloneAndSaysSoOnce();
    aRefusedFirstJoinWithARefusedRestoreIsWrittenAgainByTheNextJoin();
    theStreamCacheSaysWhatIsKnown();
    aSpansWholeRatesSnapToTheNearWholeNumber();
    aRateAHairOutsideASpanIsProgrammedFromTheSpansOwnDuration();
    theOfferedRatesAreWholeRatesUpToTheCap();
    aPressureCapChoosesTheLargestOfferedRateBelowIt();
    aFormatsPhotoMaximumIsTheLargestSizeWithinTheCap();
    theStillsAFormatMakesAreWithinTheMaximumInBothDimensions();
    theSizesAnyFormatMakesAreTheUnionLargestFirst();
    theOutputSizesFollowTheAnchor();
    theTabletRunsInTheFormatThatMakesTheLargestStillAtTheSmallestCost();
    aFormatThatHasNoRateWithinTheCapLosesToOneThatDoes();
    everyAdmittedStreamSetFitsInsideTheAnchor();
    aFormatAtExactlyTheCapStillOffersItsPhotos();
    aCameraNeverRunsInAFormatLargerThanItsAnchor();
    aCameraWithOnlyFastFormatsKeepsAModeAndItsPhotos();
    aFormatThatTakesTheTierSizeKeepsItsModesWhenAnotherWins();
    everyOfferedSizeStartsInTheAnchor();
    aFrameThatFitsTightlyButNotInTheReservedBufferIsNotOffered();
    aCameraThatListsNearIntegerRatesIsOfferedWholeRates();
    theLargeTierOffersWhatItsLimitsHold();
    aFormatThatCarriesNoStreamIsNotAnAnchor();
    theFrameBoundKeepsAnOversizedFormatOutOfTheBox();
    aTiesBreakByCostThenLowRatesThenFullRangeThenIndex();
    theKeyOrderIsRateThenStillThenVideo();
    aCameraKeepsConcurrencyOnlyWhenItsMultiCameraFormatsOfferAsMuch();
    aCameraJoinsAMultiCameraSessionOnlyWhenEveryAnchorIsAFormatItTakes();
    aRefusedStreamRemovesOnlyTheOutputItAdded();
    aRefusedStreamBeforeItsInputLeavesTheSessionAsItWas();
    aNewVideoConnectionIsProgrammedBeforeTheConfigurationCommits();
    aRefusedInputOrFormatAddsNothingElse();
    aRefusedPhotoOutputRemovesTheVideoAndInputItAdded();
    aSessionOverItsBudgetRemovesWhatTheJoinAdded();
    aRefusedStreamOnACameraInTheSessionPutsItsDurationsBack();
    aRefusedStreamPutsTheManualExposureBackToo();
    aRefusedFirstJoinPutsTheDeviceBackAsItWasBeforeTheJoin();
    putBackTimingOrdersItsWritesAndStopsAtARefusal();
    aResizeSetsTheDurationsThenTheSizeAndRestoresTheOldTimingOnARefusal();
    everyCameraIsRepairedBeforeTheSessionIsToldItIsBack();
    aRefusedRepairIsAFailureAndNothingIsAnnouncedAsBack();
    aFailedRecoveryIsFinalForItsSession();
    aSampleThatWasInsideTheCallbackWhenTheRepairCompletedIsRejected();
    aSampleWhoseCallbackBeganAfterTheRepairIsJudgedByItsCaptureTime();
    aFreshFrameAfterTheRepairPassesAndAStreamIsNotMuted();
    aCaptureClockThatDiffersFromTheHostsChangesNothing();
    aSampleWithoutATimestampOrARepairWithoutAClockIsJudgedByTheGenerationAlone();
    everyRepairDrawsANewBoundary();
    aSampleIsDeliveredWhileTheLockIsHeld();
    aPressureNotificationIsAppliedWhileTheStreamRunsAndNeverAfterIt();
    aRefusalIsLoggedOncePerCameraAndReasonUntilTheCameraChanges();
    theRefusalLogIsUsedFromTwoThreads();
    everyGeneratedTableHoldsTheInvariants();
    theFrontStillLimitIsAboutThreeMegapixelsOnAnyTable();
    theFrontStillLimitChangesOnlyThePhotoSizesOfTheTablesTheTestsModel();
    aFrontCameraNeverKeepsASizeAboveTheLimit();
    theOfferTheOutputAndTheCaptureAgreeOnTheLimitedList();
    theFrontStillLimitLeavesTheVideoOfferAndDecidesNoConcurrency();
    theFrontStillLimitOnTheSecondRealFrontTable();
    everyGeneratedFrontTableKeepsNothingAboveTheLimit();
    aCameraStatesThePlatformsMountingAndAFrontCameraStates270();
    aFrontCameraStates270AndIsTurnedHalfAroundUnlessItAlreadyIs();
    aRegionIsMappedBackThroughATurnOfThePicture();
    aRegionIsMappedBackThroughTheConnectionAndTheHalfTurnTogether();
    theSessionKeepsItsColourChoiceUnlessATurnedCameraIsIn();
    aSquareFormatThatListsRatiosIsDescribedByItsPinnedCrop();
    aCamerasLastUseSurvivesTheNextSessionsDiscovery();
    aCameraThatImitatesAnEarlierOneKeepsItsPhotoConnectionAngle();
    aStillReadsTheCompensationAfterTheCommit();
    aStillWaitsForAPendingRatioPinInsideTheStillBudget();
    aRatioChangeIsOneOperationAtATime();
    theHeldNotesAreBoundedToTheCurrentCamerasAndRecentOnes();
    aSampleMadeBeforeTheChangeTookEffectNeverPasses();
    aCameraThatLeftCannotFailTheSessionTheOthersAreIn();
    theNoteTakesTheChangesStateAsItIsPublished();
    thePinnedCropsFieldOfViewIsTheLargerOfTheTwo();
    theRatioBitsAreDistinctAndNoneHasNone();
    return 0;
}
