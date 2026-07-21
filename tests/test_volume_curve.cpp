#include <cstddef>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "exec/volume_curve.hpp"

using exec::kCurveScale;
using exec::kVolumeCurveBuckets;
using exec::VolumeCurve;

TEST_CASE("volume curve cumulative weights are non-decreasing and end exactly at kCurveScale") {
    VolumeCurve curve;
    std::uint64_t prev = 0;
    for (std::size_t i = 0; i < kVolumeCurveBuckets; ++i) {
        const std::uint64_t cur = curve.cumulative_weight(i);
        CHECK(cur >= prev);
        prev = cur;
    }
    CHECK(curve.cumulative_weight(kVolumeCurveBuckets - 1) == kCurveScale);
}

TEST_CASE("volume curve out-of-range bucket index clamps to the last bucket") {
    VolumeCurve curve;
    CHECK(curve.cumulative_weight(kVolumeCurveBuckets) == kCurveScale);
    CHECK(curve.cumulative_weight(kVolumeCurveBuckets + 100) == kCurveScale);
}

TEST_CASE("volume curve is U-shaped: open/close buckets are heavier than a mid-day bucket") {
    VolumeCurve curve;
    const std::uint64_t flat_average = kCurveScale / kVolumeCurveBuckets;

    const std::uint64_t first_bucket_weight = curve.cumulative_weight(0);
    CHECK(first_bucket_weight > flat_average);

    const std::size_t last = kVolumeCurveBuckets - 1;
    const std::uint64_t last_bucket_weight =
        curve.cumulative_weight(last) - curve.cumulative_weight(last - 1);
    CHECK(last_bucket_weight > flat_average);

    const std::size_t mid = kVolumeCurveBuckets / 2;
    const std::uint64_t mid_bucket_weight =
        curve.cumulative_weight(mid) - curve.cumulative_weight(mid - 1);
    CHECK(mid_bucket_weight < flat_average);

    // The open and the close are both heavier than the mid-day trough --
    // the classic U, not just "front-loaded" or just "back-loaded".
    CHECK(first_bucket_weight > mid_bucket_weight);
    CHECK(last_bucket_weight > mid_bucket_weight);
}
