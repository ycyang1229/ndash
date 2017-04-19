// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mp4/mp4_stream_parser.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <memory>
#include <string>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/time/time.h"
#include "mp4/audio_decoder_config.h"
#include "mp4/decoder_buffer.h"
#include "mp4/media_track.h"
#include "mp4/media_tracks.h"
#include "mp4/mock_media_log.h"
#include "mp4/stream_parser.h"
#include "mp4/stream_parser_buffer.h"
#include "mp4/test_data_util.h"
#include "mp4/text_track_config.h"
#include "mp4/video_decoder_config.h"
#include "mp4/es_descriptor.h"
#include "mp4/fourccs.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using ::testing::InSequence;
using ::testing::StrictMock;
using base::TimeDelta;

namespace media {
namespace mp4 {

// Matchers for verifying common media log entry strings.
MATCHER_P(VideoCodecLog, codec_string, "") {
  return CONTAINS_STRING(arg, "Video codec: " + std::string(codec_string));
}

MATCHER_P(AudioCodecLog, codec_string, "") {
  return CONTAINS_STRING(arg, "Audio codec: " + std::string(codec_string));
}

MATCHER(SampleEncryptionInfoUnavailableLog, "") {
  return CONTAINS_STRING(arg, "Sample encryption info is not available.");
}

MATCHER_P(ErrorLog, error_string, "") {
  return CONTAINS_STRING(arg, error_string);
}

class MP4StreamParserTest : public testing::Test {
 public:
  MP4StreamParserTest()
      : media_log_(new StrictMock<MockMediaLog>()),
        configs_received_(false),
        lower_bound_(
            DecodeTimestamp::FromPresentationTime(base::TimeDelta::Max())) {
    std::set<int> audio_object_types;
    audio_object_types.insert(kISO_14496_3);
    parser_.reset(new MP4StreamParser(audio_object_types, false));
  }

 protected:
  scoped_refptr<StrictMock<MockMediaLog>> media_log_;
  std::unique_ptr<MP4StreamParser> parser_;
  bool configs_received_;
  std::unique_ptr<MediaTracks> media_tracks_;
  AudioDecoderConfig audio_decoder_config_;
  VideoDecoderConfig video_decoder_config_;
  DecodeTimestamp lower_bound_;

  bool AppendData(const uint8_t* data, size_t length) {
    return parser_->Parse(data, length);
  }

  bool AppendDataInPieces(const uint8_t* data,
                          size_t length,
                          size_t piece_size) {
    const uint8_t* start = data;
    const uint8_t* end = data + length;
    while (start < end) {
      size_t append_size = std::min(piece_size,
                                    static_cast<size_t>(end - start));
      if (!AppendData(start, append_size))
        return false;
      start += append_size;
    }
    return true;
  }

  void InitF(const StreamParser::InitParameters& expected_params,
             const StreamParser::InitParameters& params) {
    DVLOG(1) << "InitF: dur=" << params.duration.InMicroseconds()
             << ", autoTimestampOffset=" << params.auto_update_timestamp_offset;
    EXPECT_EQ(expected_params.duration, params.duration);
    EXPECT_EQ(expected_params.timeline_offset, params.timeline_offset);
    EXPECT_EQ(expected_params.auto_update_timestamp_offset,
              params.auto_update_timestamp_offset);
    EXPECT_EQ(expected_params.liveness, params.liveness);
    EXPECT_EQ(expected_params.detected_audio_track_count,
              params.detected_audio_track_count);
    EXPECT_EQ(expected_params.detected_video_track_count,
              params.detected_video_track_count);
    EXPECT_EQ(expected_params.detected_text_track_count,
              params.detected_text_track_count);
  }

  bool NewConfigF(std::unique_ptr<MediaTracks> tracks,
                  const StreamParser::TextTrackConfigMap& tc) {
    configs_received_ = true;
    CHECK(tracks.get());
    media_tracks_ = std::move(tracks);
    audio_decoder_config_ = media_tracks_->getFirstAudioConfig();
    video_decoder_config_ = media_tracks_->getFirstVideoConfig();
    DVLOG(1) << "NewConfigF: track count=" << media_tracks_->tracks().size()
             << " audio=" << audio_decoder_config_.IsValidConfig()
             << " video=" << video_decoder_config_.IsValidConfig();
    return true;
  }

  void DumpBuffers(const std::string& label,
                   const StreamParser::BufferQueue& buffers) {
    DVLOG(2) << "DumpBuffers: " << label << " size " << buffers.size();
    for (StreamParser::BufferQueue::const_iterator buf = buffers.begin();
         buf != buffers.end(); buf++) {
      DVLOG(3) << "  n=" << buf - buffers.begin()
               << ", size=" << (*buf)->data_size()
               << ", dur=" << (*buf)->duration().InMilliseconds();
    }
  }

  bool NewBuffersF(const StreamParser::BufferQueue& audio_buffers,
                   const StreamParser::BufferQueue& video_buffers,
                   const StreamParser::TextBufferQueueMap& text_map) {
    DumpBuffers("audio_buffers", audio_buffers);
    DumpBuffers("video_buffers", video_buffers);

    // TODO(wolenetz/acolwell): Add text track support to more MSE parsers. See
    // http://crbug.com/336926.
    if (!text_map.empty())
      return false;

    // Find the second highest timestamp so that we know what the
    // timestamps on the next set of buffers must be >= than.
    DecodeTimestamp audio = !audio_buffers.empty() ?
        audio_buffers.back()->GetDecodeTimestamp() : kNoDecodeTimestamp();
    DecodeTimestamp video = !video_buffers.empty() ?
        video_buffers.back()->GetDecodeTimestamp() : kNoDecodeTimestamp();
    DecodeTimestamp second_highest_timestamp =
        (audio == kNoDecodeTimestamp() ||
         (video != kNoDecodeTimestamp() && audio > video)) ? video : audio;

    DCHECK(second_highest_timestamp != kNoDecodeTimestamp());

    if (lower_bound_ != kNoDecodeTimestamp() &&
        second_highest_timestamp < lower_bound_) {
      return false;
    }

    lower_bound_ = second_highest_timestamp;
    return true;
  }

  void KeyNeededF(EmeInitDataType type, const std::vector<uint8_t>& init_data) {
    DVLOG(1) << "KeyNeededF: " << init_data.size();
    EXPECT_EQ(EmeInitDataType::CENC, type);
    EXPECT_FALSE(init_data.empty());
  }

  void NewSegmentF() {
    DVLOG(1) << "NewSegmentF";
    lower_bound_ = kNoDecodeTimestamp();
  }

  void NewSIDXStub(std::unique_ptr<std::vector<uint32_t>> sizes,
                   std::unique_ptr<std::vector<uint64_t>> offsets,
                   std::unique_ptr<std::vector<uint64_t>> durations_us,
                   std::unique_ptr<std::vector<uint64_t>> times_us) {
    DVLOG(1) << "NewSIDX";
  }

  void NewSIDX(std::unique_ptr<std::vector<uint32_t>> sizes,
               std::unique_ptr<std::vector<uint64_t>> offsets,
               std::unique_ptr<std::vector<uint64_t>> durations_us,
               std::unique_ptr<std::vector<uint64_t>> times_us) {
    DVLOG(1) << "NewSIDX";

    // Compare to known values for bear-1280x720-v_frag-cenc.mp4
    ASSERT_TRUE(6 == sizes->size());
    ASSERT_TRUE(6 == offsets->size());
    ASSERT_TRUE(6 == durations_us->size());
    ASSERT_TRUE(6 == times_us->size());

    EXPECT_EQ(141470, sizes->at(0));
    EXPECT_EQ(2048, offsets->at(0));
    EXPECT_EQ(500500, durations_us->at(0));
    EXPECT_EQ(0, times_us->at(0));

    EXPECT_EQ(151048, sizes->at(1));
    EXPECT_EQ(143518, offsets->at(1));
    EXPECT_EQ(500500, durations_us->at(1));
    EXPECT_EQ(500500, times_us->at(1));

    EXPECT_EQ(170943, sizes->at(2));
    EXPECT_EQ(294566, offsets->at(2));
    EXPECT_EQ(500500, durations_us->at(2));
    EXPECT_EQ(1001000, times_us->at(2));

    EXPECT_EQ(176897, sizes->at(3));
    EXPECT_EQ(465509, offsets->at(3));
    EXPECT_EQ(500500, durations_us->at(3));
    EXPECT_EQ(1501500, times_us->at(3));

    EXPECT_EQ(158175, sizes->at(4));
    EXPECT_EQ(642406, offsets->at(4));
    EXPECT_EQ(500500, durations_us->at(4));
    EXPECT_EQ(2002000, times_us->at(4));

    EXPECT_EQ(77668, sizes->at(5));
    EXPECT_EQ(800581, offsets->at(5));
    EXPECT_EQ(233566, durations_us->at(5));
    EXPECT_EQ(2502500, times_us->at(5));
  }

  void EndOfSegmentF() {
    DVLOG(1) << "EndOfSegmentF()";
    lower_bound_ =
        DecodeTimestamp::FromPresentationTime(base::TimeDelta::Max());
  }

  void InitializeParserWithInitParametersExpectations(
      StreamParser::InitParameters params,
      bool test_sidx = false) {
    parser_->Init(
        base::Bind(&MP4StreamParserTest::InitF, base::Unretained(this), params),
        base::Bind(&MP4StreamParserTest::NewConfigF, base::Unretained(this)),
        base::Bind(&MP4StreamParserTest::NewBuffersF, base::Unretained(this)),
        true,
        base::Bind(&MP4StreamParserTest::KeyNeededF, base::Unretained(this)),
        base::Bind(&MP4StreamParserTest::NewSegmentF, base::Unretained(this)),
        base::Bind(&MP4StreamParserTest::EndOfSegmentF, base::Unretained(this)),
        test_sidx
            ? base::Bind(&MP4StreamParserTest::NewSIDX, base::Unretained(this))
            : base::Bind(&MP4StreamParserTest::NewSIDXStub,
                         base::Unretained(this)),
        media_log_);
  }

  StreamParser::InitParameters GetDefaultInitParametersExpectations() {
    // Most unencrypted test mp4 files have zero duration and are treated as
    // live streams.
    StreamParser::InitParameters params(kInfiniteDuration());
    params.liveness = DemuxerStream::LIVENESS_LIVE;
    params.detected_audio_track_count = 1;
    params.detected_video_track_count = 1;
    params.detected_text_track_count = 0;
    return params;
  }

  void InitializeParserAndExpectLiveness(DemuxerStream::Liveness liveness) {
    auto params = GetDefaultInitParametersExpectations();
    params.liveness = liveness;
    InitializeParserWithInitParametersExpectations(params);
  }

  void InitializeParser() {
    InitializeParserWithInitParametersExpectations(
        GetDefaultInitParametersExpectations());
  }

  bool ParseMP4File(const std::string& filename, int append_bytes) {
    scoped_refptr<DecoderBuffer> buffer = ReadTestDataFile(filename);
    EXPECT_TRUE(AppendDataInPieces(buffer->data(),
                                   buffer->data_size(),
                                   append_bytes));
    return true;
  }
};

TEST_F(MP4StreamParserTest, UnalignedAppend) {
  // Test small, non-segment-aligned appends (small enough to exercise
  // incremental append system)
  EXPECT_MEDIA_LOG(VideoCodecLog("avc1.64001F"));
  EXPECT_MEDIA_LOG(AudioCodecLog("mp4a.40.2"));
  InitializeParser();
  ParseMP4File("bear-1280x720-av_frag.mp4", 512);
}

TEST_F(MP4StreamParserTest, BytewiseAppend) {
  // Ensure no incremental errors occur when parsing
  EXPECT_MEDIA_LOG(VideoCodecLog("avc1.64001F"));
  EXPECT_MEDIA_LOG(AudioCodecLog("mp4a.40.2"));
  InitializeParser();
  ParseMP4File("bear-1280x720-av_frag.mp4", 1);
}

TEST_F(MP4StreamParserTest, MultiFragmentAppend) {
  // Large size ensures multiple fragments are appended in one call (size is
  // larger than this particular test file)
  EXPECT_MEDIA_LOG(VideoCodecLog("avc1.64001F"));
  EXPECT_MEDIA_LOG(AudioCodecLog("mp4a.40.2"));
  InitializeParser();
  ParseMP4File("bear-1280x720-av_frag.mp4", 768432);
}

TEST_F(MP4StreamParserTest, Flush) {
  // Flush while reading sample data, then start a new stream.
  EXPECT_MEDIA_LOG(VideoCodecLog("avc1.64001F")).Times(2);
  EXPECT_MEDIA_LOG(AudioCodecLog("mp4a.40.2")).Times(2);
  InitializeParser();

  scoped_refptr<DecoderBuffer> buffer =
      ReadTestDataFile("bear-1280x720-av_frag.mp4");
  EXPECT_TRUE(AppendDataInPieces(buffer->data(), 65536, 512));
  parser_->Flush();
  EXPECT_TRUE(AppendDataInPieces(buffer->data(),
                                 buffer->data_size(),
                                 512));
}

TEST_F(MP4StreamParserTest, Reinitialization) {
  EXPECT_MEDIA_LOG(VideoCodecLog("avc1.64001F")).Times(2);
  EXPECT_MEDIA_LOG(AudioCodecLog("mp4a.40.2")).Times(2);
  InitializeParser();

  scoped_refptr<DecoderBuffer> buffer =
      ReadTestDataFile("bear-1280x720-av_frag.mp4");
  EXPECT_TRUE(AppendDataInPieces(buffer->data(),
                                 buffer->data_size(),
                                 512));
  EXPECT_TRUE(AppendDataInPieces(buffer->data(),
                                 buffer->data_size(),
                                 512));
}

TEST_F(MP4StreamParserTest, MPEG2_AAC_LC) {
  InSequence s;
  std::set<int> audio_object_types;
  audio_object_types.insert(kISO_13818_7_AAC_LC);
  parser_.reset(new MP4StreamParser(audio_object_types, false));
  EXPECT_MEDIA_LOG(AudioCodecLog("mp4a.67"));
  EXPECT_MEDIA_LOG(AudioCodecLog("mp4a.40.2"));
  auto params = GetDefaultInitParametersExpectations();
  params.detected_video_track_count = 0;
  InitializeParserWithInitParametersExpectations(params);
  ParseMP4File("bear-mpeg2-aac-only_frag.mp4", 512);
}

// Test that a moov box is not always required after Flush() is called.
TEST_F(MP4StreamParserTest, NoMoovAfterFlush) {
  EXPECT_MEDIA_LOG(VideoCodecLog("avc1.64001F"));
  EXPECT_MEDIA_LOG(AudioCodecLog("mp4a.40.2"));
  InitializeParser();

  scoped_refptr<DecoderBuffer> buffer =
      ReadTestDataFile("bear-1280x720-av_frag.mp4");
  EXPECT_TRUE(AppendDataInPieces(buffer->data(),
                                 buffer->data_size(),
                                 512));
  parser_->Flush();

  const int kFirstMoofOffset = 1307;
  EXPECT_TRUE(AppendDataInPieces(buffer->data() + kFirstMoofOffset,
                                 buffer->data_size() - kFirstMoofOffset,
                                 512));
}

// Test an invalid file where there are encrypted samples, but
// SampleEncryptionBox (senc) and SampleAuxiliaryInformation{Sizes|Offsets}Box
// (saiz|saio) are missing.
// The parser should fail instead of crash. See http://crbug.com/361347
TEST_F(MP4StreamParserTest, MissingSampleEncryptionInfo) {
  InSequence s;

  // Encrypted test mp4 files have non-zero duration and are treated as
  // recorded streams.
  auto params = GetDefaultInitParametersExpectations();
  params.duration = base::TimeDelta::FromMicroseconds(23219);
  params.liveness = DemuxerStream::LIVENESS_RECORDED;
  params.detected_video_track_count = 0;
  InitializeParserWithInitParametersExpectations(params);

  scoped_refptr<DecoderBuffer> buffer =
      ReadTestDataFile("bear-1280x720-a_frag-cenc_missing-saiz-saio.mp4");
  EXPECT_MEDIA_LOG(AudioCodecLog("mp4a.40.2")).Times(2);
  EXPECT_MEDIA_LOG(SampleEncryptionInfoUnavailableLog());
  EXPECT_FALSE(AppendDataInPieces(buffer->data(), buffer->data_size(), 512));
}

// Test a file where all video samples start with an Access Unit
// Delimiter (AUD) NALU.
TEST_F(MP4StreamParserTest, VideoSamplesStartWithAUDs) {
  EXPECT_MEDIA_LOG(VideoCodecLog("avc1.4D4028"));
  auto params = GetDefaultInitParametersExpectations();
  params.detected_audio_track_count = 0;
  InitializeParserWithInitParametersExpectations(params);
  ParseMP4File("bear-1280x720-av_with-aud-nalus_frag.mp4", 512);
}

TEST_F(MP4StreamParserTest, HEVC_in_MP4_container) {
  bool expect_success = false;
  EXPECT_MEDIA_LOG(ErrorLog("Parse unsupported video format hev1"));
  auto params = GetDefaultInitParametersExpectations();
  params.duration = base::TimeDelta::FromMicroseconds(1002000);
  params.liveness = DemuxerStream::LIVENESS_RECORDED;
  params.detected_audio_track_count = 0;
  InitializeParserWithInitParametersExpectations(params);

  scoped_refptr<DecoderBuffer> buffer = ReadTestDataFile("bear-hevc-frag.mp4");
  EXPECT_EQ(expect_success,
            AppendDataInPieces(buffer->data(), buffer->data_size(), 512));
}

// Sample encryption information is stored as CencSampleAuxiliaryDataFormat
// (ISO/IEC 23001-7:2015 8) inside 'mdat' box. No SampleEncryption ('senc') box.
TEST_F(MP4StreamParserTest, CencWithEncryptionInfoStoredAsAuxDataInMdat) {
  // Encrypted test mp4 files have non-zero duration and are treated as
  // recorded streams.
  auto params = GetDefaultInitParametersExpectations();
  params.duration = base::TimeDelta::FromMicroseconds(2736066);
  params.liveness = DemuxerStream::LIVENESS_RECORDED;
  params.detected_audio_track_count = 0;
  InitializeParserWithInitParametersExpectations(params, true /* test sidx */);

  scoped_refptr<DecoderBuffer> buffer =
      ReadTestDataFile("bear-1280x720-v_frag-cenc.mp4");
  EXPECT_MEDIA_LOG(VideoCodecLog("avc1.64001F"));
  EXPECT_TRUE(AppendDataInPieces(buffer->data(), buffer->data_size(), 512));
}

TEST_F(MP4StreamParserTest, CencWithSampleEncryptionBox) {
  // Encrypted test mp4 files have non-zero duration and are treated as
  // recorded streams.
  auto params = GetDefaultInitParametersExpectations();
  params.duration = base::TimeDelta::FromMicroseconds(2736066);
  params.liveness = DemuxerStream::LIVENESS_RECORDED;
  params.detected_audio_track_count = 0;
  InitializeParserWithInitParametersExpectations(params);

  scoped_refptr<DecoderBuffer> buffer =
      ReadTestDataFile("bear-640x360-v_frag-cenc-senc.mp4");
  EXPECT_MEDIA_LOG(VideoCodecLog("avc1.64001E"));
  EXPECT_TRUE(AppendDataInPieces(buffer->data(), buffer->data_size(), 512));
}

TEST_F(MP4StreamParserTest, NaturalSizeWithoutPASP) {
  auto params = GetDefaultInitParametersExpectations();
  params.duration = base::TimeDelta::FromMicroseconds(1000966);
  params.liveness = DemuxerStream::LIVENESS_RECORDED;
  params.detected_audio_track_count = 0;
  InitializeParserWithInitParametersExpectations(params);

  scoped_refptr<DecoderBuffer> buffer =
      ReadTestDataFile("bear-640x360-non_square_pixel-without_pasp.mp4");

  EXPECT_MEDIA_LOG(VideoCodecLog("avc1.64001E"));
  EXPECT_TRUE(AppendDataInPieces(buffer->data(), buffer->data_size(), 512));
  EXPECT_EQ(Size(639, 360), video_decoder_config_.natural_size());
}

TEST_F(MP4StreamParserTest, NaturalSizeWithPASP) {
  auto params = GetDefaultInitParametersExpectations();
  params.duration = base::TimeDelta::FromMicroseconds(1000966);
  params.liveness = DemuxerStream::LIVENESS_RECORDED;
  params.detected_audio_track_count = 0;
  InitializeParserWithInitParametersExpectations(params);

  scoped_refptr<DecoderBuffer> buffer =
      ReadTestDataFile("bear-640x360-non_square_pixel-with_pasp.mp4");

  EXPECT_MEDIA_LOG(VideoCodecLog("avc1.64001E"));
  EXPECT_TRUE(AppendDataInPieces(buffer->data(), buffer->data_size(), 512));
  EXPECT_EQ(Size(639, 360), video_decoder_config_.natural_size());
}

TEST_F(MP4StreamParserTest, DemuxingAC3) {
  std::set<int> audio_object_types;
  audio_object_types.insert(kAC3);
  parser_.reset(new MP4StreamParser(audio_object_types, false));

  bool expect_success = true;

  auto params = GetDefaultInitParametersExpectations();
  params.duration = base::TimeDelta::FromMicroseconds(1045000);
  params.liveness = DemuxerStream::LIVENESS_RECORDED;
  params.detected_video_track_count = 0;
  InitializeParserWithInitParametersExpectations(params);

  scoped_refptr<DecoderBuffer> buffer =
      ReadTestDataFile("bear-ac3-only-frag.mp4");
  EXPECT_EQ(expect_success,
            AppendDataInPieces(buffer->data(), buffer->data_size(), 512));
}

TEST_F(MP4StreamParserTest, DemuxingEAC3) {
  std::set<int> audio_object_types;
  audio_object_types.insert(kEAC3);
  parser_.reset(new MP4StreamParser(audio_object_types, false));

  bool expect_success = true;

  auto params = GetDefaultInitParametersExpectations();
  params.duration = base::TimeDelta::FromMicroseconds(1045000);
  params.liveness = DemuxerStream::LIVENESS_RECORDED;
  params.detected_video_track_count = 0;
  InitializeParserWithInitParametersExpectations(params);

  scoped_refptr<DecoderBuffer> buffer =
      ReadTestDataFile("bear-eac3-only-frag.mp4");
  EXPECT_EQ(expect_success,
            AppendDataInPieces(buffer->data(), buffer->data_size(), 512));
}

TEST_F(MP4StreamParserTest, FourCCToString) {
  // A real FOURCC should print.
  EXPECT_EQ("mvex", FourCCToString(FOURCC_MVEX));

  // Invalid FOURCC should also print whenever ASCII values are printable.
  EXPECT_EQ("fake", FourCCToString(static_cast<FourCC>(0x66616b65)));

  // Invalid FORCC with non-printable values should not give error message.
  EXPECT_EQ("0x66616b00", FourCCToString(static_cast<FourCC>(0x66616b00)));
}

TEST_F(MP4StreamParserTest, MediaTrackInfoSourcing) {
  EXPECT_MEDIA_LOG(VideoCodecLog("avc1.64001F"));
  EXPECT_MEDIA_LOG(AudioCodecLog("mp4a.40.2"));
  InitializeParser();
  ParseMP4File("bear-1280x720-av_frag.mp4", 4096);

  EXPECT_EQ(media_tracks_->tracks().size(), 2u);
  const MediaTrack& video_track = *(media_tracks_->tracks()[0]);
  EXPECT_EQ(video_track.type(), MediaTrack::Video);
  EXPECT_EQ(video_track.id(), "1");
  EXPECT_EQ(video_track.kind(), "main");
  EXPECT_EQ(video_track.label(), "VideoHandler");
  EXPECT_EQ(video_track.language(), "und");

  const MediaTrack& audio_track = *(media_tracks_->tracks()[1]);
  EXPECT_EQ(audio_track.type(), MediaTrack::Audio);
  EXPECT_EQ(audio_track.id(), "2");
  EXPECT_EQ(audio_track.kind(), "main");
  EXPECT_EQ(audio_track.label(), "SoundHandler");
  EXPECT_EQ(audio_track.language(), "und");
}

TEST_F(MP4StreamParserTest, TextTrackDetection) {
  auto params = GetDefaultInitParametersExpectations();
  params.detected_text_track_count = 1;
  InitializeParserWithInitParametersExpectations(params);

  scoped_refptr<DecoderBuffer> buffer =
      ReadTestDataFile("bear-1280x720-avt_subt_frag.mp4");

  EXPECT_MEDIA_LOG(AudioCodecLog("mp4a.40.2"));
  EXPECT_MEDIA_LOG(VideoCodecLog("avc1.64001F"));
  EXPECT_TRUE(AppendDataInPieces(buffer->data(), buffer->data_size(), 512));
}

}  // namespace mp4
}  // namespace media
