// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "build/build_config.h"
#include "media/base/media_switches.h"
#include "ppapi/features/features.h"

namespace switches {

// Allow users to specify a custom buffer size for debugging purpose.
const char kAudioBufferSize[] = "audio-buffer-size";

// Set number of threads to use for video decoding.
const char kVideoThreads[] = "video-threads";

// Suspend media pipeline on background tabs.
const char kEnableMediaSuspend[] = "enable-media-suspend";
const char kDisableMediaSuspend[] = "disable-media-suspend";

// Force to report VP9 as an unsupported MIME type.
const char kReportVp9AsAnUnsupportedMimeType[] =
    "report-vp9-as-an-unsupported-mime-type";

// Enable parsing of new multi-part VP9 string for webm.
const char kEnableNewVp9CodecString[] = "enable-new-vp9-codec-string";

#if defined(OS_LINUX) || defined(OS_FREEBSD) || defined(OS_SOLARIS)
// The Alsa device to use when opening an audio input stream.
const char kAlsaInputDevice[] = "alsa-input-device";
// The Alsa device to use when opening an audio stream.
const char kAlsaOutputDevice[] = "alsa-output-device";
#endif

#if defined(OS_WIN)
// Use exclusive mode audio streaming for Windows Vista and higher.
// Leads to lower latencies for audio streams which uses the
// AudioParameters::AUDIO_PCM_LOW_LATENCY audio path.
// See http://msdn.microsoft.com/en-us/library/windows/desktop/dd370844.aspx
// for details.
const char kEnableExclusiveAudio[] = "enable-exclusive-audio";

// Force the use of MediaFoundation for video capture. This is only supported in
// Windows 7 and above. Used, like |kForceDirectShowVideoCapture|, to
// troubleshoot problems in Windows platforms.
const char kForceMediaFoundationVideoCapture[] = "force-mediafoundation";

// Use Windows WaveOut/In audio API even if Core Audio is supported.
const char kForceWaveAudio[] = "force-wave-audio";

// Instead of always using the hardware channel layout, check if a driver
// supports the source channel layout.  Avoids outputting empty channels and
// permits drivers to enable stereo to multichannel expansion.  Kept behind a
// flag since some drivers lie about supported layouts and hang when used.  See
// http://crbug.com/259165 for more details.
const char kTrySupportedChannelLayouts[] = "try-supported-channel-layouts";

// Number of buffers to use for WaveOut.
const char kWaveOutBuffers[] = "waveout-buffers";
#endif

#if defined(USE_CRAS)
// Use CRAS, the ChromeOS audio server.
const char kUseCras[] = "use-cras";
#endif

#if !defined(OS_ANDROID) || BUILDFLAG(ENABLE_PLUGINS)
// Use a media session for each tabs in a way that two tabs can't play on top of
// each other. This is different from the Media Session API as it is enabling a
// default behaviour for the browser. The allowed values are: "" (empty),
// |kEnableDefaultMediaSessionDuckFlash|.
const char kEnableDefaultMediaSession[] = "enable-default-media-session";
#endif  // !defined(OS_ANDROID) || BUILDFLAG(ENABLE_PLUGINS)

#if BUILDFLAG(ENABLE_PLUGINS)
// This value is used as an option for |kEnableDefaultMediaSession|. Flash will
// be ducked when losing audio focus.
const char kEnableDefaultMediaSessionDuckFlash[] = "duck-flash";
#endif  // BUILDFLAG(ENABLE_PLUGINS)

#if BUILDFLAG(ENABLE_RUNTIME_MEDIA_RENDERER_SELECTION)
// Rather than use the renderer hosted remotely in the media service, fall back
// to the default renderer within content_renderer. Does not change the behavior
// of the media service.
const char kDisableMojoRenderer[] = "disable-mojo-renderer";
#endif  // BUILDFLAG(ENABLE_RUNTIME_MEDIA_RENDERER_SELECTION)

// Use fake device for Media Stream to replace actual camera and microphone.
const char kUseFakeDeviceForMediaStream[] = "use-fake-device-for-media-stream";

// Use an .y4m file to play as the webcam. See the comments in
// media/capture/video/file_video_capture_device.h for more details.
const char kUseFileForFakeVideoCapture[] = "use-file-for-fake-video-capture";

// Play a .wav file as the microphone. Note that for WebRTC calls we'll treat
// the bits as if they came from the microphone, which means you should disable
// audio processing (lest your audio file will play back distorted). The input
// file is converted to suit Chrome's audio buses if necessary, so most sane
// .wav files should work. You can pass either <path> to play the file looping
// or <path>%noloop to stop after playing the file to completion.
const char kUseFileForFakeAudioCapture[] = "use-file-for-fake-audio-capture";

// Use fake device for accelerated decoding of JPEG. This allows, for example,
// testing of the communication to the GPU service without requiring actual
// accelerator hardware to be present.
const char kUseFakeJpegDecodeAccelerator[] = "use-fake-jpeg-decode-accelerator";

// Enables support for inband text tracks in media content.
const char kEnableInbandTextTracks[] = "enable-inband-text-tracks";

// When running tests on a system without the required hardware or libraries,
// this flag will cause the tests to fail. Otherwise, they silently succeed.
const char kRequireAudioHardwareForTesting[] =
    "require-audio-hardware-for-testing";

// Allows clients to override the threshold for when the media renderer will
// declare the underflow state for the video stream when audio is present.
// TODO(dalecurtis): Remove once experiments for http://crbug.com/470940 finish.
const char kVideoUnderflowThresholdMs[] = "video-underflow-threshold-ms";

// Disables the new rendering algorithm for webrtc, which is designed to improve
// the rendering smoothness.
const char kDisableRTCSmoothnessAlgorithm[] =
    "disable-rtc-smoothness-algorithm";

// Enables demuxing of vp9 in mp4. Note that this flag will not have any effect
// if MP4 demuxing is not enabled in the build.
const char kEnableVp9InMp4[] = "enable-vp9-in-mp4";

// Force media player using SurfaceView instead of SurfaceTexture on Android.
const char kForceVideoOverlays[] = "force-video-overlays";

// Allows explicitly specifying MSE audio/video buffer sizes.
// Default values are 150M for video and 12M for audio.
const char kMSEAudioBufferSizeLimit[] = "mse-audio-buffer-size-limit";
const char kMSEVideoBufferSizeLimit[] = "mse-video-buffer-size-limit";

}  // namespace switches

namespace media {

#if defined(OS_WIN)
// Enables video decode acceleration using the D3D11 video decoder api.
// This is completely insecure - DO NOT USE except for testing.
const base::Feature kD3D11VideoDecoding{"D3D11VideoDecoding",
                                        base::FEATURE_DISABLED_BY_DEFAULT};

// Enables H264 HW encode acceleration using Media Foundation for Windows.
const base::Feature kMediaFoundationH264Encoding{
    "MediaFoundationH264Encoding", base::FEATURE_ENABLED_BY_DEFAULT};
#endif  // defined(OS_WIN)

// Use new audio rendering mixer.
const base::Feature kNewAudioRenderingMixingStrategy{
    "NewAudioRenderingMixingStrategy", base::FEATURE_DISABLED_BY_DEFAULT};

// Only used for disabling overlay fullscreen (aka SurfaceView) in Clank.
const base::Feature kOverlayFullscreenVideo{"overlay-fullscreen-video",
                                            base::FEATURE_ENABLED_BY_DEFAULT};

// Let videos be resumed via remote controls (for example, the notification)
// when in background.
const base::Feature kResumeBackgroundVideo {
  "resume-background-video",
#if defined(OS_ANDROID)
      base::FEATURE_ENABLED_BY_DEFAULT
#else
      base::FEATURE_DISABLED_BY_DEFAULT
#endif
};

// Let video track be unselected when video is playing in the background.
const base::Feature kBackgroundVideoTrackOptimization{
    "BackgroundVideoTrackOptimization", base::FEATURE_DISABLED_BY_DEFAULT};

// Make MSE garbage collection algorithm more aggressive when we are under
// moderate or critical memory pressure. This will relieve memory pressure by
// releasing stale data from MSE buffers.
const base::Feature kMemoryPressureBasedSourceBufferGC{
    "MemoryPressureBasedSourceBufferGC", base::FEATURE_DISABLED_BY_DEFAULT};

// Use shared block-based buffering for media.
const base::Feature kUseNewMediaCache{"use-new-media-cache",
                                      base::FEATURE_ENABLED_BY_DEFAULT};

// Correct video colors based on output display?
const base::Feature kVideoColorManagement{"video-color-management",
                                          base::FEATURE_DISABLED_BY_DEFAULT};

// Inform video blitter of video color space.
const base::Feature kVideoBlitColorAccuracy{"video-blit-color-accuracy",
                                            base::FEATURE_DISABLED_BY_DEFAULT};

// Enables support for External Clear Key (ECK) key system for testing on
// supported platforms. On platforms that do not support ECK, this feature has
// no effect.
const base::Feature kExternalClearKeyForTesting{
    "external-clear-key-for-testing", base::FEATURE_DISABLED_BY_DEFAULT};

#if defined(OS_ANDROID)
// Lock the screen orientation when a video goes fullscreen.
const base::Feature kVideoFullscreenOrientationLock{
    "VideoFullscreenOrientationLock", base::FEATURE_ENABLED_BY_DEFAULT};

// An experimental feature to enable persistent-license type support in MediaDrm
// when using Encrypted Media Extensions (EME) API.
// TODO(xhwang): Remove this after feature launch. See http://crbug.com/493521
const base::Feature kMediaDrmPersistentLicense{
    "MediaDrmPersistentLicense", base::FEATURE_DISABLED_BY_DEFAULT};

#endif

}  // namespace media
