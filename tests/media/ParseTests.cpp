// SPDX-License-Identifier: GPL-3.0-only
// Fluvi media, CPU only: Annex-B splitting, slice headers and frame_num continuity on the committed clips.
#include <doctest/doctest.h>

#include "Clip.h"
#include "H264Parse.h"

using namespace fluvi;
using namespace fluvi::media;

TEST_CASE("h264 parse: clips split into access units with IDRs where expected") {
    const auto clip = test::LoadClip("dup720p.h264");
    REQUIRE(clip.size() == 180);
    h264::Parser parser;
    for (size_t i = 0; i < clip.size(); ++i) {
        const auto info = parser.Parse(clip[i].data(), clip[i].size());
        CAPTURE(i);
        CHECK(info.hasSlice);
        CHECK(info.headerOk);
        CHECK(info.firstMb == 0);
        CHECK(info.hasIdr == (i % 60 == 0));
        CHECK(info.sliceType == (i % 60 == 0 ? 2 : 0));
        if (i % 60 == 0) CHECK(info.frameNum == 0);
    }
    REQUIRE(parser.Sps());
    CHECK(parser.Sps()->width == 1280);
    CHECK(parser.Sps()->height == 720);
    CHECK(parser.Sps()->frameMbsOnly);
    CHECK(parser.Sps()->log2MaxFrameNum >= 4);
    CHECK(test::LoadClip("dup1440p.h264").size() == 60);
}

TEST_CASE("h264 parse: frame_num continuity sees a dropped reference picture") {
    const auto clip = test::LoadClip("dup720p.h264");
    REQUIRE(clip.size() == 180);
    const auto run = [&](size_t dropFrom, size_t dropCount) {
        h264::Parser parser;
        h264::FrameNumTracker tracker;
        std::vector<size_t> gaps;
        for (size_t i = 0; i < clip.size(); ++i) {
            if (i >= dropFrom && i < dropFrom + dropCount) continue;
            const auto info = parser.Parse(clip[i].data(), clip[i].size());
            const auto* sps = parser.Sps();
            REQUIRE(sps);
            if (tracker.Observe(info, 1u << sps->log2MaxFrameNum, sps->gapsInFrameNumAllowed)) gaps.push_back(i);
        }
        return gaps;
    };
    CHECK(run(0, 0).empty());                                  // intact: frame_num wraps without a gap
    CHECK(run(20, 1) == std::vector<size_t>{21});              // one P lost
    CHECK(run(20, 5) == std::vector<size_t>{25});              // several lost
    CHECK(run(59, 1).empty());                                 // the next AU is an IDR: the chain restarts
}

TEST_CASE("h264 parse: a headless access unit and a bare parameter-set unit") {
    const auto clip = test::LoadClip("dup720p.h264");
    REQUIRE(clip.size() > 2);
    h264::Parser parser;
    const auto idr = parser.Parse(clip[0].data(), clip[0].size());
    CHECK(idr.hasSps);
    CHECK(idr.hasIdr);
    CHECK(idr.sliceBytes > 0);
    CHECK(idr.sliceBytes < clip[0].size());   // SPS/PPS are not counted
    // Garbage with no start code: nothing parsed, nothing claimed.
    const std::vector<uint8_t> junk(100, 0x5A);
    const auto none = parser.Parse(junk.data(), junk.size());
    CHECK_FALSE(none.hasSlice);
    CHECK_FALSE(none.hasIdr);
    CHECK_FALSE(none.headerOk);
}
