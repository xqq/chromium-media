// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stddef.h>
#include <stdint.h>
#include <vector>

#include "base/at_exit.h"
#include "base/bind.h"
#include "base/command_line.h"
#include "base/logging.h"
#include "media/base/bind_to_current_loop.h"
#include "media/base/eme_constants.h"
#include "media/base/media.h"
#include "media/base/media_switches.h"
#include "media/base/pipeline_status.h"
#include "media/test/mock_media_source.h"
#include "media/test/pipeline_integration_test_base.h"

namespace {

// Keep these aligned with BUILD.gn's pipeline_integration_fuzzer_variants
enum FuzzerVariant {
  SRC,
  WEBM_OPUS,
  WEBM_VORBIS,
  WEBM_VP8,
  WEBM_VP9,
  WEBM_OPUS_VP9,
#if BUILDFLAG(USE_PROPRIETARY_CODECS)
  ADTS,
  MP3,
  MP4_AACLC,
  MP4_AACSBR,
  MP4_AVC1,
  MP4_FLAC,
  MP4_AACLC_AVC,
#if BUILDFLAG(ENABLE_MSE_MPEG2TS_STREAM_PARSER)
  MP2T_AACLC,
  MP2T_AACSBR,
  MP2T_AVC,
  MP2T_MP3,
  MP2T_AACLC_AVC,
#endif  // BUILDFLAG(ENABLE_MSE_MPEG2TS_STREAM_PARSER)
#endif  // BUILDFLAG(USE_PROPRIETARY_CODECS)
};

std::string MseFuzzerVariantEnumToMimeTypeString(FuzzerVariant variant) {
  switch (variant) {
    case WEBM_OPUS:
      return "audio/webm; codecs=\"opus\"";
    case WEBM_VORBIS:
      return "audio/webm; codecs=\"vorbis\"";
    case WEBM_VP8:
      return "video/webm; codecs=\"vp8\"";
    case WEBM_VP9:
      return "video/webm; codecs=\"vp9\"";
    case WEBM_OPUS_VP9:
      return "video/webm; codecs=\"opus,vp9\"";
#if BUILDFLAG(USE_PROPRIETARY_CODECS)
    case ADTS:
      return "audio/aac";
    case MP3:
      return "audio/mpeg";
    case MP4_AACLC:
      return "audio/mp4; codecs=\"mp4a.40.2\"";
    case MP4_AACSBR:
      return "audio/mp4; codecs=\"mp4a.40.5\"";
    case MP4_AVC1:
      return "video/mp4; codecs=\"avc1.42E01E\"";
    case MP4_FLAC:
      return "audio/mp4; codecs=\"flac\"";
    case MP4_AACLC_AVC:
      return "video/mp4; codecs=\"mp4a.40.2,avc1.42E01E\"";
#if BUILDFLAG(ENABLE_MSE_MPEG2TS_STREAM_PARSER)
    case MP2T_AACLC:
      return "video/mp2t; codecs=\"mp4a.67\"";
    case MP2T_AACSBR:
      return "video/mp2t; codecs=\"mp4a.40.5\"";
    case MP2T_AVC:
      return "video/mp2t; codecs=\"avc1.42E01E\"";
    case MP2T_MP3:
      // Note, "mp4a.6B" appears to be an equivalent codec substring.
      return "video/mp2t; codecs=\"mp4a.69\"";
    case MP2T_AACLC_AVC:
      return "video/mp2t; codecs=\"mp4a.40.2,avc1.42E01E\"";
#endif  // BUILDFLAG(ENABLE_MSE_MPEG2TS_STREAM_PARSER)
#endif  // BUILDFLAG(USE_PROPRIETARY_CODECS)

    case SRC:
      NOTREACHED() << "SRC is an invalid MSE fuzzer variant";
      break;
  }

  return "";
}

}  // namespace

namespace media {

// Limit the amount of initial (or post-seek) audio silence padding allowed in
// rendering of fuzzed input.
constexpr base::TimeDelta kMaxPlayDelay = base::TimeDelta::FromSeconds(10);

void OnEncryptedMediaInitData(media::PipelineIntegrationTestBase* test,
                              media::EmeInitDataType /* type */,
                              const std::vector<uint8_t>& /* init_data */) {
  // Encrypted media is not supported in this test. For an encrypted media file,
  // we will start demuxing the data but media pipeline will wait for a CDM to
  // be available to start initialization, which will not happen in this case.
  // To prevent the test timeout, we'll just fail the test immediately here.
  // TODO(xhwang): Support encrypted media in this fuzzer test.
  test->FailTest(media::PIPELINE_ERROR_INITIALIZATION_FAILED);
}

void OnAudioPlayDelay(media::PipelineIntegrationTestBase* test,
                      base::TimeDelta play_delay) {
  CHECK_GT(play_delay, base::TimeDelta());
  if (play_delay > kMaxPlayDelay)
    test->FailTest(media::PIPELINE_ERROR_INITIALIZATION_FAILED);
}

class ProgressivePipelineIntegrationFuzzerTest
    : public PipelineIntegrationTestBase {
 public:
  ProgressivePipelineIntegrationFuzzerTest() {
    set_encrypted_media_init_data_cb(
        base::Bind(&OnEncryptedMediaInitData, this));
    set_audio_play_delay_cb(
        BindToCurrentLoop(base::BindRepeating(&OnAudioPlayDelay, this)));
  }

  ~ProgressivePipelineIntegrationFuzzerTest() override{};

  void RunTest(const uint8_t* data, size_t size) {
    if (PIPELINE_OK != Start(data, size, kUnreliableDuration))
      return;

    Play();
    if (PIPELINE_OK != WaitUntilEndedOrError())
      return;

    Seek(base::TimeDelta());
  }
};

class MediaSourcePipelineIntegrationFuzzerTest
    : public PipelineIntegrationTestBase {
 public:
  MediaSourcePipelineIntegrationFuzzerTest() {
    set_encrypted_media_init_data_cb(
        base::Bind(&OnEncryptedMediaInitData, this));
    set_audio_play_delay_cb(
        BindToCurrentLoop(base::BindRepeating(&OnAudioPlayDelay, this)));
  }

  ~MediaSourcePipelineIntegrationFuzzerTest() override{};

  void RunTest(const uint8_t* data, size_t size, const std::string& mimetype) {
    if (size == 0)
      return;

    scoped_refptr<media::DecoderBuffer> buffer(
        DecoderBuffer::CopyFrom(data, size));

    MockMediaSource source(buffer, mimetype, kAppendWholeFile);

    // Prevent timeout in the case of not enough media appended to complete
    // demuxer initialization, yet no error in the media appended.  The
    // following will trigger DEMUXER_ERROR_COULD_NOT_OPEN state transition in
    // this case.
    source.set_do_eos_after_next_append(true);

    source.set_encrypted_media_init_data_cb(
        base::Bind(&OnEncryptedMediaInitData, this));

    // TODO(wolenetz): Vary the behavior (abort/remove/seek/endOfStream/Append
    // in pieces/append near play-head/vary append mode/etc), perhaps using
    // CustomMutator and Seed to insert/update the variation information into/in
    // the |data| we process here.  See https://crbug.com/750818.
    if (PIPELINE_OK != StartPipelineWithMediaSource(&source))
      return;

    Play();
  }
};

}  // namespace media

// Disable noisy logging.
struct Environment {
  Environment() {
    // Note, use logging::LOG_VERBOSE here to assist local debugging.
    logging::SetMinLogLevel(logging::LOG_FATAL);
  }
};

Environment* env = new Environment();

// Entry point for LibFuzzer.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Media pipeline starts new threads, which needs AtExitManager.
  base::AtExitManager at_exit;

  // Media pipeline checks command line arguments internally.
  base::CommandLine::Init(0, nullptr);

  media::InitializeMediaLibrary();

  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(media::kMseFlacInIsobmff);

  FuzzerVariant variant = PIPELINE_FUZZER_VARIANT;

  if (variant == SRC) {
    media::ProgressivePipelineIntegrationFuzzerTest test;
    test.RunTest(data, size);
  } else {
    // Sequentially fuzz with new and old MSE buffering APIs.  See
    // https://crbug.com/718641.
    {
      base::test::ScopedFeatureList features_with_buffering_api_forced;
      features_with_buffering_api_forced.InitAndEnableFeature(
          media::kMseBufferByPts);
      media::MediaSourcePipelineIntegrationFuzzerTest test;
      test.RunTest(data, size, MseFuzzerVariantEnumToMimeTypeString(variant));
    }
    {
      base::test::ScopedFeatureList features_with_buffering_api_forced;
      features_with_buffering_api_forced.InitAndDisableFeature(
          media::kMseBufferByPts);
      media::MediaSourcePipelineIntegrationFuzzerTest test;
      test.RunTest(data, size, MseFuzzerVariantEnumToMimeTypeString(variant));
    }
  }

  return 0;
}
