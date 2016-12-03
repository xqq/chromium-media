// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gpu/android/media_codec_video_decoder.h"

#include <stddef.h>

#include <memory>

#include "base/android/build_info.h"
#include "base/auto_reset.h"
#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/callback_helpers.h"
#include "base/command_line.h"
#include "base/lazy_instance.h"
#include "base/logging.h"
#include "base/message_loop/message_loop.h"
#include "base/metrics/histogram.h"
#include "base/sys_info.h"
#include "base/task_runner_util.h"
#include "base/threading/thread.h"
#include "base/threading/thread_checker.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/trace_event/trace_event.h"
#include "gpu/command_buffer/service/gles2_cmd_decoder.h"
#include "gpu/command_buffer/service/mailbox_manager.h"
#include "gpu/ipc/service/gpu_channel.h"
#include "media/base/android/media_codec_bridge.h"
#include "media/base/android/media_codec_util.h"
#include "media/base/bind_to_current_loop.h"
#include "media/base/bitstream_buffer.h"
#include "media/base/limits.h"
#include "media/base/media.h"
#include "media/base/timestamp_constants.h"
#include "media/base/video_decoder_config.h"
#include "media/gpu/avda_picture_buffer_manager.h"
#include "media/gpu/shared_memory_region.h"
#include "media/video/picture.h"
#include "ui/gl/android/scoped_java_surface.h"
#include "ui/gl/android/surface_texture.h"
#include "ui/gl/gl_bindings.h"

#if defined(ENABLE_MOJO_MEDIA_IN_GPU_PROCESS)
#include "media/mojo/services/mojo_cdm_service.h"
#endif

#define NOTIFY_ERROR(error_code, error_message)      \
  do {                                               \
    DLOG(ERROR) << error_message;                    \
    NotifyError(VideoDecodeAccelerator::error_code); \
  } while (0)

namespace media {

namespace {

// Max number of bitstreams notified to the client with
// NotifyEndOfBitstreamBuffer() before getting output from the bitstream.
enum { kMaxBitstreamsNotifiedInAdvance = 32 };

// Because MediaCodec is thread-hostile (must be poked on a single thread) and
// has no callback mechanism (b/11990118), we must drive it by polling for
// complete frames (and available input buffers, when the codec is fully
// saturated).  This function defines the polling delay.  The value used is an
// arbitrary choice that trades off CPU utilization (spinning) against latency.
// Mirrors android_video_encode_accelerator.cc:EncodePollDelay().
//
// An alternative to this polling scheme could be to dedicate a new thread
// (instead of using the ChildThread) to run the MediaCodec, and make that
// thread use the timeout-based flavor of MediaCodec's dequeue methods when it
// believes the codec should complete "soon" (e.g. waiting for an input
// buffer, or waiting for a picture when it knows enough complete input
// pictures have been fed to saturate any internal buffering).  This is
// speculative and it's unclear that this would be a win (nor that there's a
// reasonably device-agnostic way to fill in the "believes" above).
constexpr base::TimeDelta DecodePollDelay =
    base::TimeDelta::FromMilliseconds(10);

constexpr base::TimeDelta NoWaitTimeOut = base::TimeDelta::FromMicroseconds(0);

constexpr base::TimeDelta IdleTimerTimeOut = base::TimeDelta::FromSeconds(1);

// On low end devices (< KitKat is always low-end due to buggy MediaCodec),
// defer the surface creation until the codec is actually used if we know no
// software fallback exists.
bool ShouldDeferSurfaceCreation(int surface_id, VideoCodec codec) {
  return surface_id == SurfaceManager::kNoSurfaceID && codec == kCodecH264 &&
         AVDACodecAllocator::Instance()->IsAnyRegisteredAVDA() &&
         (base::android::BuildInfo::GetInstance()->sdk_int() <= 18 ||
          base::SysInfo::IsLowEndDevice());
}

// Don't use MediaCodecs internal software decoders when we have more secure and
// up to date versions in the renderer process.
bool IsMediaCodecSoftwareDecodingForbidden(const VideoDecoderConfig& config) {
  return !config.is_encrypted() &&
         (config.codec() == kCodecVP8 || _config.codec() == kCodecVP9);
}

bool ConfigSupported(const VideoDecoderConfig& config) {
  const auto codec = config.codec();

  // Only use MediaCodec for VP8 or VP9 if it's likely backed by hardware or if
  // the stream is encrypted.
  if (IsMediaCodecSoftwareDecodingForbidden(config) &&
      VideoCodecBridge::IsKnownUnaccelerated(codec, MEDIA_CODEC_DECODER)) {
    DVLOG(1) << "Config not supported: " << GetCodecName(codec)
             << " is not hardware accelerated";
    return false;
  }

  // Don't support larger than 4k because it won't perform well on many devices.
  const auto size = config.coded_size();
  if (size.width() > 3840 || size.height() > 2160)
    return false;

  switch (codec) {
    case kCodecVP8:
    case kCodecVP9: {
      if ((codec == kCodecVP8 && !MediaCodecUtil::IsVp8DecoderAvailable()) ||
          (codec == kCodecVP9 && !MediaCodecUtil::IsVp9DecoderAvailable())) {
        return false;
      }

      // There's no fallback for encrypted content so we support all sizes.
      if (config.is_encrypted())
        return true;

      // Below 360p there's little to no power benefit to using MediaCodec over
      // libvpx so we prefer to use our newer version of libvpx, sandboxed in
      // the renderer.
      if (size.width() < 480 || size.height() < 360)
        return false;

      return true;
    }
    case kCodecH264:
      return true;
#if BUILDFLAG(ENABLE_HEVC_DEMUXING)
    case kCodecHEVC:
      return true;
#endif
    default:
      return false;
  }
}

}  // namespace

// MCVDManager manages shared resources for a number of MCVD instances.
// Its responsibilities include:
//  - Starting and stopping a shared "construction" thread for instantiating and
//    releasing MediaCodecs.
//  - Detecting when a task has hung on the construction thread so MCVDs can
//    stop using it.
//  - Running a RepeatingTimer so that MCVDs can get a regular callback to
//    DoIOTask().
//  - Tracking the allocation of surfaces to MCVDs and delivering callbacks when
//    surfaces are released.
class MCVDManager {
 public:
  // Request periodic callback of |mcvd|->DoIOTask(). Does nothing if the
  // instance is already registered and the timer started. The first request
  // will start the repeating timer on an interval of DecodePollDelay.
  void StartTimer(MediaCodecVideoDecoder* mcvd) {
    DCHECK(thread_checker_.CalledOnValidThread());

    timer_mcvd_instances_.insert(mcvd);

    // If the timer is running, StopTimer() might have been called earlier, if
    // so remove the instance from the pending erasures.
    if (timer_running_)
      pending_erase_.erase(mcvd);

    if (io_timer_.IsRunning())
      return;
    io_timer_.Start(FROM_HERE, DecodePollDelay, this, &MCVDManager::RunTimer);
  }

  // Stop callbacks to |mcvd|->DoIOTask(). Does nothing if the instance is not
  // registered. If there are no instances left, the repeating timer will be
  // stopped.
  void StopTimer(MediaCodecVideoDecoder* mcvd) {
    DCHECK(thread_checker_.CalledOnValidThread());

    // If the timer is running, defer erasures to avoid iterator invalidation.
    if (timer_running_) {
      pending_erase_.insert(mcvd);
      return;
    }

    timer_mcvd_instances_.erase(mcvd);
    if (timer_mcvd_instances_.empty())
      io_timer_.Stop();
  }

 private:
  friend struct base::DefaultLazyInstanceTraits<MCVDManager>;

  MCVDManager() {}
  ~MCVDManager() { NOTREACHED(); }

  void RunTimer() {
    {
      // Call out to all MCVD instances, some of which may attempt to remove
      // themselves from the list during this operation; those removals will be
      // deferred until after all iterations are complete.
      base::AutoReset<bool> scoper(&timer_running_, true);
      for (auto* mcvd : timer_mcvd_instances_)
        mcvd->DoIOTask(false);
    }

    // Take care of any deferred erasures.
    for (auto* mcvd : pending_erase_)
      StopTimer(mcvd);
    pending_erase_.clear();

    // TODO(dalecurtis): We may want to consider chunking this if task execution
    // takes too long for the combined timer.
  }

  // All MCVD instances that would like us to poll DoIOTask.
  std::set<MediaCodecVideoDecoder*> timer_mcvd_instances_;

  // Since we can't delete while iterating when using a set, defer erasure until
  // after iteration complete.
  bool timer_running_ = false;
  std::set<MediaCodecVideoDecoder*> pending_erase_;

  // Repeating timer responsible for draining pending IO to the codecs.
  base::RepeatingTimer io_timer_;

  base::ThreadChecker thread_checker_;

  DISALLOW_COPY_AND_ASSIGN(MCVDManager);
};

static base::LazyInstance<MCVDManager>::Leaky g_mcvd_manager =
    LAZY_INSTANCE_INITIALIZER;

MediaCodecVideoDecoder::BitstreamRecord::BitstreamRecord(
    const BitstreamBuffer& bitstream_buffer)
    : buffer(bitstream_buffer) {
  if (buffer.id() != -1)
    memory.reset(new SharedMemoryRegion(buffer, true));
}

MediaCodecVideoDecoder::BitstreamRecord::BitstreamRecord(
    BitstreamRecord&& other)
    : buffer(std::move(other.buffer)), memory(std::move(other.memory)) {}

MediaCodecVideoDecoder::BitstreamRecord::~BitstreamRecord() {}

MediaCodecVideoDecoder::MediaCodecVideoDecoder(
    const MakeGLContextCurrentCallback& make_context_current_cb,
    const GetGLES2DecoderCallback& get_gles2_decoder_cb)
    : client_(NULL),
      make_context_current_cb_(make_context_current_cb),
      get_gles2_decoder_cb_(get_gles2_decoder_cb),
      state_(NO_ERROR),
      picture_buffer_manager_(get_gles2_decoder_cb),
      drain_type_(DRAIN_TYPE_NONE),
      media_drm_bridge_cdm_context_(nullptr),
      cdm_registration_id_(0),
      pending_input_buf_index_(-1),
      deferred_initialization_pending_(false),
      codec_needs_reset_(false),
      defer_surface_creation_(false),
      weak_this_factory_(this) {}

MediaCodecVideoDecoder::~MediaCodecVideoDecoder() {
  DCHECK(thread_checker_.CalledOnValidThread());
  g_mcvd_manager.Get().StopTimer(this);
  AVDACodecAllocator::Instance()->StopThread(this);

#if defined(ENABLE_MOJO_MEDIA_IN_GPU_PROCESS)
  if (!media_drm_bridge_cdm_context_)
    return;

  DCHECK(cdm_registration_id_);

  // Cancel previously registered callback (if any).
  media_drm_bridge_cdm_context_->SetMediaCryptoReadyCB(
      MediaDrmBridgeCdmContext::MediaCryptoReadyCB());

  media_drm_bridge_cdm_context_->UnregisterPlayer(cdm_registration_id_);
#endif  // defined(ENABLE_MOJO_MEDIA_IN_GPU_PROCESS)
}

bool MediaCodecVideoDecoder::Initialize(const Config& config, Client* client) {
  DVLOG(1) << __func__ << ": " << config.AsHumanReadableString();
  TRACE_EVENT0("media", "MCVD::Initialize");
  DCHECK(!media_codec_);
  DCHECK(thread_checker_.CalledOnValidThread());

  if (make_context_current_cb_.is_null() || get_gles2_decoder_cb_.is_null()) {
    DLOG(ERROR) << "GL callbacks are required for this VDA";
    return false;
  }

  if (config.output_mode != Config::OutputMode::ALLOCATE) {
    DLOG(ERROR) << "Only ALLOCATE OutputMode is supported by this VDA";
    return false;
  }

  DCHECK(client);
  client_ = client;
  config_ = config;
  codec_config_ = new CodecConfig();
  codec_config_->codec_ = VideoCodecProfileToVideoCodec(config.profile);
  codec_config_->initial_expected_coded_size_ =
      config.initial_expected_coded_size;

  if (codec_config_->codec_ != kCodecVP8 &&
      codec_config_->codec_ != kCodecVP9 &&
#if BUILDFLAG(ENABLE_HEVC_DEMUXING)
      codec_config_->codec_ != kCodecHEVC &&
#endif
      codec_config_->codec_ != kCodecH264) {
    DLOG(ERROR) << "Unsupported profile: " << config.profile;
    return false;
  }

  if (codec_config_->codec_ == kCodecH264) {
    codec_config_->csd0_ = config.sps;
    codec_config_->csd1_ = config.pps;
  }

  // Only use MediaCodec for VP8/9 if it's likely backed by hardware
  // or if the stream is encrypted.
  if (IsMediaCodecSoftwareDecodingForbidden() &&
      VideoCodecBridge::IsKnownUnaccelerated(codec_config_->codec_,
                                             MEDIA_CODEC_DECODER)) {
    DVLOG(1) << "Initialization failed: "
             << (codec_config_->codec_ == kCodecVP8 ? "vp8" : "vp9")
             << " is not hardware accelerated";
    return false;
  }

  auto gles_decoder = get_gles2_decoder_cb_.Run();
  if (!gles_decoder) {
    DLOG(ERROR) << "Failed to get gles2 decoder instance.";
    return false;
  }

  // SetSurface() can't be called before Initialize(), so we pick up our first
  // surface ID from the codec configuration.
  DCHECK(!pending_surface_id_);

  // If we're low on resources, we may decide to defer creation of the surface
  // until the codec is actually used.
  if (ShouldDeferSurfaceCreation(config_.surface_id, codec_config_->codec_)) {
    DCHECK(!deferred_initialization_pending_);
    // We should never be here if a SurfaceView is required.
    DCHECK_EQ(config_.surface_id, SurfaceManager::kNoSurfaceID);
    defer_surface_creation_ = true;
    NotifyInitializationComplete(true);
    return true;
  }

  // We signaled that we support deferred initialization, so see if the client
  // does also.
  deferred_initialization_pending_ = config.is_deferred_initialization_allowed;
  if (config_.is_encrypted && !deferred_initialization_pending_) {
    DLOG(ERROR) << "Deferred initialization must be used for encrypted streams";
    return false;
  }

  if (AVDACodecAllocator::Instance()->AllocateSurface(this,
                                                      config_.surface_id)) {
    // We now own the surface, so finish initialization.
    return InitializePictureBufferManager();
  }

  // We have to wait for some other MCVD instance to free up the surface.
  // OnSurfaceAvailable will be called when it's available.
  return true;
}

void MediaCodecVideoDecoder::OnSurfaceAvailable(bool success) {
  DCHECK(deferred_initialization_pending_);
  DCHECK(!defer_surface_creation_);

  if (!success || !InitializePictureBufferManager()) {
    NotifyInitializationComplete(false);
    deferred_initialization_pending_ = false;
  }
}

bool MediaCodecVideoDecoder::InitializePictureBufferManager() {
  if (!make_context_current_cb_.Run()) {
    LOG(ERROR) << "Failed to make this decoder's GL context current.";
    return false;
  }

  codec_config_->surface_ =
      picture_buffer_manager_.Initialize(config_.surface_id);
  if (codec_config_->surface_.IsEmpty())
    return false;

  if (!AVDACodecAllocator::Instance()->StartThread(this))
    return false;

  // If we are encrypted, then we aren't able to create the codec yet.
  if (config_.is_encrypted) {
    InitializeCdm();
    return true;
  }

  if (deferred_initialization_pending_ || defer_surface_creation_) {
    defer_surface_creation_ = false;
    ConfigureMediaCodecAsynchronously();
    return true;
  }
}

void MediaCodecVideoDecoder::DoIOTask(bool start_timer) {
  DCHECK(thread_checker_.CalledOnValidThread());
  TRACE_EVENT0("media", "MCVD::DoIOTask");
  if (state_ == ERROR || state_ == WAITING_FOR_CODEC ||
      state_ == SURFACE_DESTROYED) {
    return;
  }

  picture_buffer_manager_.MaybeRenderEarly();
  bool did_work = false, did_input = false, did_output = false;
  do {
    did_input = QueueInput();
    did_output = DequeueOutput();
    if (did_input || did_output)
      did_work = true;
  } while (did_input || did_output);

  ManageTimer(did_work || start_timer);
}

bool MediaCodecVideoDecoder::QueueInput() {
  DCHECK(thread_checker_.CalledOnValidThread());
  TRACE_EVENT0("media", "MCVD::QueueInput");
  if (state_ == ERROR || state_ == WAITING_FOR_CODEC ||
      state_ == WAITING_FOR_KEY) {
    return false;
  }
  if (bitstreams_notified_in_advance_.size() > kMaxBitstreamsNotifiedInAdvance)
    return false;
  if (pending_bitstream_records_.empty())
    return false;

  int input_buf_index = pending_input_buf_index_;

  // Do not dequeue a new input buffer if we failed with MEDIA_CODEC_NO_KEY.
  // That status does not return this buffer back to the pool of
  // available input buffers. We have to reuse it in QueueSecureInputBuffer().
  if (input_buf_index == -1) {
    MediaCodecStatus status =
        media_codec_->DequeueInputBuffer(NoWaitTimeOut, &input_buf_index);
    switch (status) {
      case MEDIA_CODEC_DEQUEUE_INPUT_AGAIN_LATER:
        return false;
      case MEDIA_CODEC_ERROR:
        NOTIFY_ERROR(PLATFORM_FAILURE, "DequeueInputBuffer failed");
        return false;
      case MEDIA_CODEC_OK:
        break;
      default:
        NOTREACHED();
        return false;
    }
  }

  DCHECK_NE(input_buf_index, -1);

  BitstreamBuffer bitstream_buffer = pending_bitstream_records_.front().buffer;

  if (bitstream_buffer.id() == -1) {
    pending_bitstream_records_.pop();
    TRACE_COUNTER1("media", "MCVD::PendingBitstreamBufferCount",
                   pending_bitstream_records_.size());

    media_codec_->QueueEOS(input_buf_index);
    return true;
  }

  std::unique_ptr<SharedMemoryRegion> shm;

  if (pending_input_buf_index_ == -1) {
    // When |pending_input_buf_index_| is not -1, the buffer is already dequeued
    // from MediaCodec, filled with data and bitstream_buffer.handle() is
    // closed.
    shm = std::move(pending_bitstream_records_.front().memory);

    if (!shm->Map()) {
      NOTIFY_ERROR(UNREADABLE_INPUT, "SharedMemoryRegion::Map() failed");
      return false;
    }
  }

  const base::TimeDelta presentation_timestamp =
      bitstream_buffer.presentation_timestamp();
  DCHECK(presentation_timestamp != kNoTimestamp)
      << "Bitstream buffers must have valid presentation timestamps";

  // There may already be a bitstream buffer with this timestamp, e.g., VP9 alt
  // ref frames, but it's OK to overwrite it because we only expect a single
  // output frame to have that timestamp. MCVD clients only use the bitstream
  // buffer id in the returned Pictures to map a bitstream buffer back to a
  // timestamp on their side, so either one of the bitstream buffer ids will
  // result in them finding the right timestamp.
  bitstream_buffers_in_decoder_[presentation_timestamp] = bitstream_buffer.id();

  // Notice that |memory| will be null if we repeatedly enqueue the same buffer,
  // this happens after MEDIA_CODEC_NO_KEY.
  const uint8_t* memory =
      shm ? static_cast<const uint8_t*>(shm->memory()) : nullptr;
  const std::string& key_id = bitstream_buffer.key_id();
  const std::string& iv = bitstream_buffer.iv();
  const std::vector<SubsampleEntry>& subsamples = bitstream_buffer.subsamples();

  MediaCodecStatus status;
  if (key_id.empty() || iv.empty()) {
    status = media_codec_->QueueInputBuffer(input_buf_index, memory,
                                            bitstream_buffer.size(),
                                            presentation_timestamp);
  } else {
    status = media_codec_->QueueSecureInputBuffer(
        input_buf_index, memory, bitstream_buffer.size(), key_id, iv,
        subsamples, presentation_timestamp);
  }

  DVLOG(2) << __func__
           << ": Queue(Secure)InputBuffer: pts:" << presentation_timestamp
           << " status:" << status;

  if (status == MEDIA_CODEC_NO_KEY) {
    // Keep trying to enqueue the same input buffer.
    // The buffer is owned by us (not the MediaCodec) and is filled with data.
    DVLOG(1) << "QueueSecureInputBuffer failed: NO_KEY";
    pending_input_buf_index_ = input_buf_index;
    state_ = WAITING_FOR_KEY;
    return false;
  }

  pending_input_buf_index_ = -1;
  pending_bitstream_records_.pop();
  TRACE_COUNTER1("media", "MCVD::PendingBitstreamBufferCount",
                 pending_bitstream_records_.size());
  // We should call NotifyEndOfBitstreamBuffer(), when no more decoded output
  // will be returned from the bitstream buffer. However, MediaCodec API is
  // not enough to guarantee it.
  // So, here, we calls NotifyEndOfBitstreamBuffer() in advance in order to
  // keep getting more bitstreams from the client, and throttle them by using
  // |bitstreams_notified_in_advance_|.
  // TODO(dwkang): check if there is a way to remove this workaround.
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::Bind(&MediaCodecVideoDecoder::NotifyEndOfBitstreamBuffer,
                 weak_this_factory_.GetWeakPtr(), bitstream_buffer.id()));
  bitstreams_notified_in_advance_.push_back(bitstream_buffer.id());

  if (status != MEDIA_CODEC_OK) {
    NOTIFY_ERROR(PLATFORM_FAILURE, "QueueInputBuffer failed:" << status);
    return false;
  }

  return true;
}

bool MediaCodecVideoDecoder::DequeueOutput() {
  DCHECK(thread_checker_.CalledOnValidThread());
  TRACE_EVENT0("media", "MCVD::DequeueOutput");
  if (state_ == ERROR || state_ == WAITING_FOR_CODEC)
    return false;
  if (!output_picture_buffers_.empty() && free_picture_ids_.empty() &&
      !IsDrainingForResetOrDestroy()) {
    // Don't have any picture buffer to send. Need to wait.
    return false;
  }

  // If we're waiting to switch surfaces pause output release until we have all
  // picture buffers returned. This is so we can ensure the right flags are set
  // on the picture buffers returned to the client.
  if (pending_surface_id_) {
    if (picture_buffer_manager_.HasUnrenderedPictures())
      return false;
    if (!UpdateSurface())
      return false;
  }

  bool eos = false;
  base::TimeDelta presentation_timestamp;
  int32_t buf_index = 0;
  do {
    size_t offset = 0;
    size_t size = 0;

    TRACE_EVENT_BEGIN0("media", "MCVD::DequeueOutput");
    MediaCodecStatus status = media_codec_->DequeueOutputBuffer(
        NoWaitTimeOut, &buf_index, &offset, &size, &presentation_timestamp,
        &eos, NULL);
    TRACE_EVENT_END2("media", "MCVD::DequeueOutput", "status", status,
                     "presentation_timestamp (ms)",
                     presentation_timestamp.InMilliseconds());

    switch (status) {
      case MEDIA_CODEC_ERROR:
        // Do not post an error if we are draining for reset and destroy.
        // Instead, run the drain completion task.
        if (IsDrainingForResetOrDestroy()) {
          DVLOG(1) << __func__ << ": error while codec draining";
          state_ = ERROR;
          OnDrainCompleted();
        } else {
          NOTIFY_ERROR(PLATFORM_FAILURE, "DequeueOutputBuffer failed.");
        }
        return false;

      case MEDIA_CODEC_DEQUEUE_OUTPUT_AGAIN_LATER:
        return false;

      case MEDIA_CODEC_OUTPUT_FORMAT_CHANGED: {
        // An OUTPUT_FORMAT_CHANGED is not reported after flush() if the frame
        // size does not change. Therefore we have to keep track on the format
        // even if draining, unless we are draining for destroy.
        if (drain_type_ == DRAIN_FOR_DESTROY)
          return true;  // ignore

        if (media_codec_->GetOutputSize(&size_) != MEDIA_CODEC_OK) {
          NOTIFY_ERROR(PLATFORM_FAILURE, "GetOutputSize failed.");
          return false;
        }

        DVLOG(3) << __func__
                 << " OUTPUT_FORMAT_CHANGED, new size: " << size_.ToString();
        return true;
      }

      case MEDIA_CODEC_OUTPUT_BUFFERS_CHANGED:
        break;

      case MEDIA_CODEC_OK:
        DCHECK_GE(buf_index, 0);
        DVLOG(3) << __func__ << ": pts:" << presentation_timestamp
                 << " buf_index:" << buf_index << " offset:" << offset
                 << " size:" << size << " eos:" << eos;
        break;

      default:
        NOTREACHED();
        break;
    }
  } while (buf_index < 0);

  if (eos) {
    OnDrainCompleted();
    return false;
  }

  if (IsDrainingForResetOrDestroy()) {
    media_codec_->ReleaseOutputBuffer(buf_index, false);
    return true;
  }

  // TODO(watk): Handle the case where we get a decoded buffer before
  // FORMAT_CHANGED.
  // In 0.01% of playbacks MediaCodec returns a frame before FORMAT_CHANGED.
  // Occurs on JB and M. (See the Media.MCVD.MissingFormatChanged histogram.)

  // Get the bitstream buffer id from the timestamp.
  auto it = bitstream_buffers_in_decoder_.find(presentation_timestamp);

  if (it != bitstream_buffers_in_decoder_.end()) {
    const int32_t bitstream_buffer_id = it->second;
    bitstream_buffers_in_decoder_.erase(bitstream_buffers_in_decoder_.begin(),
                                        ++it);
    SendDecodedFrameToClient(buf_index, bitstream_buffer_id);

    // Removes ids former or equal than the id from decoder. Note that
    // |bitstreams_notified_in_advance_| does not mean bitstream ids in decoder
    // because of frame reordering issue. We just maintain this roughly and use
    // it for throttling.
    for (auto bitstream_it = bitstreams_notified_in_advance_.begin();
         bitstream_it != bitstreams_notified_in_advance_.end();
         ++bitstream_it) {
      if (*bitstream_it == bitstream_buffer_id) {
        bitstreams_notified_in_advance_.erase(
            bitstreams_notified_in_advance_.begin(), ++bitstream_it);
        break;
      }
    }
  } else {
    // Normally we assume that the decoder makes at most one output frame for
    // each distinct input timestamp. However MediaCodecBridge uses timestamp
    // correction and provides a non-decreasing timestamp sequence, which might
    // result in timestamp duplicates. Discard the frame if we cannot get the
    // corresponding buffer id.
    DVLOG(3) << __func__ << ": Releasing buffer with unexpected PTS: "
             << presentation_timestamp;
    media_codec_->ReleaseOutputBuffer(buf_index, false);
  }

  // We got a decoded frame, so try for another.
  return true;
}

void MediaCodecVideoDecoder::SendDecodedFrameToClient(
    int32_t codec_buffer_index,
    int32_t bitstream_id) {
  DCHECK(thread_checker_.CalledOnValidThread());
  DCHECK_NE(bitstream_id, -1);
  DCHECK(!free_picture_ids_.empty());
  TRACE_EVENT0("media", "MCVD::SendDecodedFrameToClient");

  if (!make_context_current_cb_.Run()) {
    NOTIFY_ERROR(PLATFORM_FAILURE, "Failed to make the GL context current.");
    return;
  }

  int32_t picture_buffer_id = free_picture_ids_.front();
  free_picture_ids_.pop();
  TRACE_COUNTER1("media", "MCVD::FreePictureIds", free_picture_ids_.size());

  const auto it = output_picture_buffers_.find(picture_buffer_id);
  if (it == output_picture_buffers_.end()) {
    NOTIFY_ERROR(PLATFORM_FAILURE,
                 "Can't find PictureBuffer id: " << picture_buffer_id);
    return;
  }

  PictureBuffer& picture_buffer = it->second;
  const bool size_changed = picture_buffer.size() != size_;
  if (size_changed)
    picture_buffer.set_size(size_);

  const bool allow_overlay = picture_buffer_manager_.ArePicturesOverlayable();
  UMA_HISTOGRAM_BOOLEAN("Media.AVDA.FrameSentAsOverlay", allow_overlay);
  // TODO(hubbe): Insert the correct color space. http://crbug.com/647725
  Picture picture(picture_buffer_id, bitstream_id, gfx::Rect(size_),
                  gfx::ColorSpace(), allow_overlay);
  picture.set_size_changed(size_changed);

  // Notify picture ready before calling UseCodecBufferForPictureBuffer() since
  // that process may be slow and shouldn't delay delivery of the frame to the
  // renderer. The picture is only used on the same thread as this method is
  // called, so it is safe to do this.
  NotifyPictureReady(picture);

  // Connect the PictureBuffer to the decoded frame.
  if (!picture_buffer_manager_.UseCodecBufferForPictureBuffer(
          codec_buffer_index, picture_buffer, size_)) {
    NOTIFY_ERROR(PLATFORM_FAILURE,
                 "Failed to attach the codec buffer to a picture buffer.");
  }
}

void MediaCodecVideoDecoder::Decode(const BitstreamBuffer& bitstream_buffer) {
  DCHECK(thread_checker_.CalledOnValidThread());

  if (defer_surface_creation_ && !InitializePictureBufferManager()) {
    NOTIFY_ERROR(PLATFORM_FAILURE,
                 "Failed deferred surface and MediaCodec initialization.");
    return;
  }

  // If we previously deferred a codec restart, take care of it now. This can
  // happen on older devices where configuration changes require a codec reset.
  if (codec_needs_reset_) {
    DCHECK_EQ(drain_type_, DRAIN_TYPE_NONE);
    ResetCodecState();
  }

  if (bitstream_buffer.id() >= 0 && bitstream_buffer.size() > 0) {
    DecodeBuffer(bitstream_buffer);
    return;
  }

  if (base::SharedMemory::IsHandleValid(bitstream_buffer.handle()))
    base::SharedMemory::CloseHandle(bitstream_buffer.handle());

  if (bitstream_buffer.id() < 0) {
    NOTIFY_ERROR(INVALID_ARGUMENT,
                 "Invalid bistream_buffer, id: " << bitstream_buffer.id());
  } else {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE,
        base::Bind(&MediaCodecVideoDecoder::NotifyEndOfBitstreamBuffer,
                   weak_this_factory_.GetWeakPtr(), bitstream_buffer.id()));
  }
}

void MediaCodecVideoDecoder::DecodeBuffer(
    const BitstreamBuffer& bitstream_buffer) {
  pending_bitstream_records_.push(BitstreamRecord(bitstream_buffer));
  TRACE_COUNTER1("media", "MCVD::PendingBitstreamBufferCount",
                 pending_bitstream_records_.size());

  DoIOTask(true);
}

void MediaCodecVideoDecoder::Flush() {
  DVLOG(1) << __func__;
  DCHECK(thread_checker_.CalledOnValidThread());

  if (state_ == SURFACE_DESTROYED || defer_surface_creation_)
    NotifyFlushDone();
  else
    StartCodecDrain(DRAIN_FOR_FLUSH);
}

void MediaCodecVideoDecoder::ConfigureMediaCodecAsynchronously() {
  DCHECK(thread_checker_.CalledOnValidThread());

  DCHECK_NE(state_, WAITING_FOR_CODEC);
  state_ = WAITING_FOR_CODEC;

  if (media_codec_) {
    AVDACodecAllocator::Instance()->ReleaseMediaCodec(
        std::move(media_codec_), codec_config_->task_type_, config_.surface_id);
    picture_buffer_manager_.CodecChanged(nullptr);
  }

  codec_config_->task_type_ =
      AVDACodecAllocator::Instance()->TaskTypeForAllocation();
  if (codec_config_->task_type_ == TaskType::FAILED_CODEC) {
    // If there is no free thread, then just fail.
    OnCodecConfigured(nullptr);
    return;
  }

  // If autodetection is disallowed, fall back to Chrome's software decoders
  // instead of using the software decoders provided by MediaCodec.
  if (codec_config_->task_type_ == TaskType::SW_CODEC &&
      IsMediaCodecSoftwareDecodingForbidden()) {
    OnCodecConfigured(nullptr);
    return;
  }

  AVDACodecAllocator::Instance()->CreateMediaCodecAsync(
      weak_this_factory_.GetWeakPtr(), codec_config_);
}

void MediaCodecVideoDecoder::OnCodecConfigured(
    std::unique_ptr<VideoCodecBridge> media_codec) {
  DCHECK(thread_checker_.CalledOnValidThread());
  DCHECK(state_ == WAITING_FOR_CODEC || state_ == SURFACE_DESTROYED);

  // If we are supposed to notify that initialization is complete, then do so
  // now.  Otherwise, this is a reconfiguration.
  if (deferred_initialization_pending_) {
    // Losing the output surface is not considered an error state, so notify
    // success. The client will destroy this soon.
    NotifyInitializationComplete(state_ == SURFACE_DESTROYED ? true
                                                             : !!media_codec);
    deferred_initialization_pending_ = false;
  }

  // If |state_| changed to SURFACE_DESTROYED while we were configuring a codec,
  // then the codec is already invalid so we return early and drop it.
  if (state_ == SURFACE_DESTROYED)
    return;

  DCHECK(!media_codec_);
  media_codec_ = std::move(media_codec);
  picture_buffer_manager_.CodecChanged(media_codec_.get());
  if (!media_codec_) {
    NOTIFY_ERROR(PLATFORM_FAILURE, "Failed to create MediaCodec");
    return;
  }

  state_ = NO_ERROR;

  ManageTimer(true);
}

void MediaCodecVideoDecoder::StartCodecDrain(DrainType drain_type) {
  DVLOG(2) << __func__ << " drain_type:" << drain_type;
  DCHECK(thread_checker_.CalledOnValidThread());

  // We assume that DRAIN_FOR_FLUSH and DRAIN_FOR_RESET cannot come while
  // another drain request is present, but DRAIN_FOR_DESTROY can.
  DCHECK_NE(drain_type, DRAIN_TYPE_NONE);
  DCHECK(drain_type_ == DRAIN_TYPE_NONE || drain_type == DRAIN_FOR_DESTROY)
      << "Unexpected StartCodecDrain() with drain type " << drain_type
      << " while already draining with drain type " << drain_type_;

  const bool enqueue_eos = drain_type_ == DRAIN_TYPE_NONE;
  drain_type_ = drain_type;

  if (enqueue_eos)
    DecodeBuffer(BitstreamBuffer(-1, base::SharedMemoryHandle(), 0));
}

bool MediaCodecVideoDecoder::IsDrainingForResetOrDestroy() const {
  return drain_type_ == DRAIN_FOR_RESET || drain_type_ == DRAIN_FOR_DESTROY;
}

void MediaCodecVideoDecoder::OnDrainCompleted() {
  DVLOG(2) << __func__;
  DCHECK(thread_checker_.CalledOnValidThread());

  // If we were waiting for an EOS, clear the state and reset the MediaCodec
  // as normal.
  //
  // Some Android platforms seem to send an EOS buffer even when we're not
  // expecting it. In this case, destroy and reset the codec but don't notify
  // flush done since it violates the state machine. http://crbug.com/585959.

  switch (drain_type_) {
    case DRAIN_TYPE_NONE:
      // Unexpected EOS.
      state_ = ERROR;
      ResetCodecState();
      break;
    case DRAIN_FOR_FLUSH:
      ResetCodecState();
      base::ThreadTaskRunnerHandle::Get()->PostTask(
          FROM_HERE, base::Bind(&MediaCodecVideoDecoder::NotifyFlushDone,
                                weak_this_factory_.GetWeakPtr()));
      break;
    case DRAIN_FOR_RESET:
      ResetCodecState();
      base::ThreadTaskRunnerHandle::Get()->PostTask(
          FROM_HERE, base::Bind(&MediaCodecVideoDecoder::NotifyResetDone,
                                weak_this_factory_.GetWeakPtr()));
      break;
    case DRAIN_FOR_DESTROY:
      ResetCodecState();
      base::ThreadTaskRunnerHandle::Get()->PostTask(
          FROM_HERE, base::Bind(&MediaCodecVideoDecoder::ActualDestroy,
                                weak_this_factory_.GetWeakPtr()));
      break;
  }
  drain_type_ = DRAIN_TYPE_NONE;
}

void MediaCodecVideoDecoder::ResetCodecState() {
  DCHECK(thread_checker_.CalledOnValidThread());

  // If there is already a reset in flight, then that counts.  This can really
  // only happen if somebody calls Reset.
  // If the surface is destroyed there's nothing to do.
  if (state_ == WAITING_FOR_CODEC || state_ == SURFACE_DESTROYED)
    return;

  bitstream_buffers_in_decoder_.clear();

  if (pending_input_buf_index_ != -1) {
    // The data for that index exists in the input buffer, but corresponding
    // shm block been deleted. Check that it is safe to flush the codec, i.e.
    // |pending_bitstream_records_| is empty.
    // TODO(timav): keep shm block for that buffer and remove this restriction.
    DCHECK(pending_bitstream_records_.empty());
    pending_input_buf_index_ = -1;
  }

  const bool did_codec_error_happen = state_ == ERROR;
  state_ = NO_ERROR;

  // Don't reset the codec here if there's no error and we're only flushing;
  // instead defer until the next decode call; this prevents us from unbacking
  // frames that might be out for display at end of stream.
  codec_needs_reset_ = false;
  if (drain_type_ == DRAIN_FOR_FLUSH && !did_codec_error_happen) {
    codec_needs_reset_ = true;
    return;
  }

  // Flush the codec if possible, or create a new one if not.
  if (!did_codec_error_happen &&
      !MediaCodecUtil::CodecNeedsFlushWorkaround(media_codec_.get())) {
    DVLOG(3) << __func__ << " Flushing MediaCodec.";
    media_codec_->Flush();
    // Since we just flushed all the output buffers, make sure that nothing is
    // using them.
    picture_buffer_manager_.CodecChanged(media_codec_.get());
  } else {
    DVLOG(3) << __func__ << " Deleting the MediaCodec and creating a new one.";
    g_mcvd_manager.Get().StopTimer(this);
    ConfigureMediaCodecAsynchronously();
  }
}

void MediaCodecVideoDecoder::Reset() {
  DVLOG(1) << __func__;
  DCHECK(thread_checker_.CalledOnValidThread());
  TRACE_EVENT0("media", "MCVD::Reset");

  if (defer_surface_creation_) {
    DCHECK(!media_codec_);
    DCHECK(pending_bitstream_records_.empty());
    DCHECK_EQ(state_, NO_ERROR);
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::Bind(&MediaCodecVideoDecoder::NotifyResetDone,
                              weak_this_factory_.GetWeakPtr()));
    return;
  }

  while (!pending_bitstream_records_.empty()) {
    int32_t bitstream_buffer_id =
        pending_bitstream_records_.front().buffer.id();
    pending_bitstream_records_.pop();

    if (bitstream_buffer_id != -1) {
      base::ThreadTaskRunnerHandle::Get()->PostTask(
          FROM_HERE,
          base::Bind(&MediaCodecVideoDecoder::NotifyEndOfBitstreamBuffer,
                     weak_this_factory_.GetWeakPtr(), bitstream_buffer_id));
    }
  }
  TRACE_COUNTER1("media", "MCVD::PendingBitstreamBufferCount", 0);
  bitstreams_notified_in_advance_.clear();

  picture_buffer_manager_.ReleaseCodecBuffers(output_picture_buffers_);

  // Some VP8 files require complete MediaCodec drain before we can call
  // MediaCodec.flush() or MediaCodec.reset(). http://crbug.com/598963.
  if (media_codec_ && codec_config_->codec_ == kCodecVP8 &&
      !bitstream_buffers_in_decoder_.empty()) {
    // Postpone ResetCodecState() after the drain.
    StartCodecDrain(DRAIN_FOR_RESET);
  } else {
    ResetCodecState();
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::Bind(&MediaCodecVideoDecoder::NotifyResetDone,
                              weak_this_factory_.GetWeakPtr()));
  }
}

void MediaCodecVideoDecoder::SetSurface(int32_t surface_id) {
  DVLOG(1) << __func__;
  DCHECK(thread_checker_.CalledOnValidThread());

  if (surface_id == config_.surface_id) {
    pending_surface_id_.reset();
    return;
  }

  // Surface changes never take effect immediately, they will be handled during
  // DequeOutput() once we get to a good switch point or immediately during an
  // OnSurfaceDestroyed() call.
  pending_surface_id_ = surface_id;
}

void MediaCodecVideoDecoder::Destroy() {
  DVLOG(1) << __func__;
  DCHECK(thread_checker_.CalledOnValidThread());

  picture_buffer_manager_.Destroy(output_picture_buffers_);

  client_ = nullptr;

  // Some VP8 files require a complete MediaCodec drain before we can call
  // MediaCodec.flush() or MediaCodec.release(). http://crbug.com/598963. In
  // that case, postpone ActualDestroy() until after the drain.
  if (media_codec_ && codec_config_->codec_ == kCodecVP8) {
    // Clear |pending_bitstream_records_|.
    while (!pending_bitstream_records_.empty())
      pending_bitstream_records_.pop();

    StartCodecDrain(DRAIN_FOR_DESTROY);
  } else {
    ActualDestroy();
  }
}

void MediaCodecVideoDecoder::ActualDestroy() {
  DVLOG(1) << __func__;
  DCHECK(thread_checker_.CalledOnValidThread());

  // Note that async codec construction might still be in progress.  In that
  // case, the codec will be deleted when it completes once we invalidate all
  // our weak refs.
  weak_this_factory_.InvalidateWeakPtrs();
  g_mcvd_manager.Get().StopTimer(this);
  if (media_codec_) {
    AVDACodecAllocator::Instance()->ReleaseMediaCodec(
        std::move(media_codec_), codec_config_->task_type_, config_.surface_id);
  }

  // We no longer care about |surface_id|, in case we did before.  It's okay
  // if we have no surface and/or weren't the owner or a waiter.
  AVDACodecAllocator::Instance()->DeallocateSurface(this, config_.surface_id);

  delete this;
}

void MediaCodecVideoDecoder::OnSurfaceDestroyed() {
  DVLOG(1) << __func__;
  TRACE_EVENT0("media", "MCVD::OnSurfaceDestroyed");
  DCHECK(thread_checker_.CalledOnValidThread());

  // If the API is available avoid having to restart the decoder in order to
  // leave fullscreen. If we don't clear the surface immediately during this
  // callback, the MediaCodec will throw an error as the surface is destroyed.
  if (base::android::BuildInfo::GetInstance()->sdk_int() >= 23) {
    // Since we can't wait for a transition, we must invalidate all outstanding
    // picture buffers to avoid putting the GL system in a broken state.
    picture_buffer_manager_.ReleaseCodecBuffers(output_picture_buffers_);

    // Switch away from the surface being destroyed to a surface texture.
    DCHECK_NE(config_.surface_id, SurfaceManager::kNoSurfaceID);

    // The leaving fullscreen notification may come in before this point.
    if (pending_surface_id_)
      DCHECK_EQ(pending_surface_id_.value(), SurfaceManager::kNoSurfaceID);

    pending_surface_id_ = SurfaceManager::kNoSurfaceID;
    UpdateSurface();
    return;
  }

  // If we're currently asynchronously configuring a codec, it will be destroyed
  // when configuration completes and it notices that |state_| has changed to
  // SURFACE_DESTROYED.
  state_ = SURFACE_DESTROYED;
  if (media_codec_) {
    AVDACodecAllocator::Instance()->ReleaseMediaCodec(
        std::move(media_codec_), codec_config_->task_type_, config_.surface_id);
    picture_buffer_manager_.CodecChanged(nullptr);
  }

  // If we're draining, signal completion now because the drain can no longer
  // proceed.
  if (drain_type_ != DRAIN_TYPE_NONE)
    OnDrainCompleted();
}

void MediaCodecVideoDecoder::InitializeCdm() {
  DVLOG(2) << __func__ << ": " << config_.cdm_id;

#if !defined(ENABLE_MOJO_MEDIA_IN_GPU_PROCESS)
  NOTIMPLEMENTED();
  NotifyInitializationComplete(false);
#else
  // Store the CDM to hold a reference to it.
  cdm_for_reference_holding_only_ =
      MojoCdmService::LegacyGetCdm(config_.cdm_id);
  DCHECK(cdm_for_reference_holding_only_);

  // On Android platform the CdmContext must be a MediaDrmBridgeCdmContext.
  media_drm_bridge_cdm_context_ = static_cast<MediaDrmBridgeCdmContext*>(
      cdm_for_reference_holding_only_->GetCdmContext());
  DCHECK(media_drm_bridge_cdm_context_);

  // Register CDM callbacks. The callbacks registered will be posted back to
  // this thread via BindToCurrentLoop.

  // Since |this| holds a reference to the |cdm_|, by the time the CDM is
  // destructed, UnregisterPlayer() must have been called and |this| has been
  // destructed as well. So the |cdm_unset_cb| will never have a chance to be
  // called.
  // TODO(xhwang): Remove |cdm_unset_cb| after it's not used on all platforms.
  cdm_registration_id_ = media_drm_bridge_cdm_context_->RegisterPlayer(
      BindToCurrentLoop(base::Bind(&MediaCodecVideoDecoder::OnKeyAdded,
                                   weak_this_factory_.GetWeakPtr())),
      base::Bind(&base::DoNothing));

  // Deferred initialization will continue in OnMediaCryptoReady().
  media_drm_bridge_cdm_context_->SetMediaCryptoReadyCB(
      BindToCurrentLoop(base::Bind(&MediaCodecVideoDecoder::OnMediaCryptoReady,
                                   weak_this_factory_.GetWeakPtr())));
#endif  // !defined(ENABLE_MOJO_MEDIA_IN_GPU_PROCESS)
}

void MediaCodecVideoDecoder::OnMediaCryptoReady(
    MediaDrmBridgeCdmContext::JavaObjectPtr media_crypto,
    bool needs_protected_surface) {
  DVLOG(1) << __func__;

  if (!media_crypto) {
    LOG(ERROR) << "MediaCrypto is not available, can't play encrypted stream.";
    cdm_for_reference_holding_only_ = nullptr;
    media_drm_bridge_cdm_context_ = nullptr;
    NotifyInitializationComplete(false);
    return;
  }

  DCHECK(!media_crypto->is_null());

  // We assume this is a part of the initialization process, thus MediaCodec
  // is not created yet.
  DCHECK(!media_codec_);

  codec_config_->media_crypto_ = std::move(media_crypto);
  codec_config_->needs_protected_surface_ = needs_protected_surface;

  // After receiving |media_crypto_| we can configure MediaCodec.
  ConfigureMediaCodecAsynchronously();
}

void MediaCodecVideoDecoder::OnKeyAdded() {
  DVLOG(1) << __func__;

  if (state_ == WAITING_FOR_KEY)
    state_ = NO_ERROR;

  DoIOTask(true);
}

void MediaCodecVideoDecoder::NotifyError(Error error) {
  state_ = ERROR;
  if (client_)
    client_->NotifyError(error);
}

void MediaCodecVideoDecoder::ManageTimer(bool did_work) {
  bool should_be_running = true;

  base::TimeTicks now = base::TimeTicks::Now();
  if (!did_work && !most_recent_work_.is_null()) {
    // Make sure that we have done work recently enough, else stop the timer.
    if (now - most_recent_work_ > IdleTimerTimeOut) {
      most_recent_work_ = base::TimeTicks();
      should_be_running = false;
    }
  } else {
    most_recent_work_ = now;
  }

  if (should_be_running)
    g_mcvd_manager.Get().StartTimer(this);
  else
    g_mcvd_manager.Get().StopTimer(this);
}


bool MediaCodecVideoDecoder::UpdateSurface() {
  DCHECK(pending_surface_id_);
  DCHECK_NE(config_.surface_id, pending_surface_id_.value());
  DCHECK(config_.surface_id == SurfaceManager::kNoSurfaceID ||
         pending_surface_id_.value() == SurfaceManager::kNoSurfaceID);

  const int previous_surface_id = config_.surface_id;
  const int new_surface_id = pending_surface_id_.value();
  pending_surface_id_.reset();
  bool success = true;

  // TODO(watk): Fix this so we can wait for the new surface to be allocated.
  if (!AVDACodecAllocator::Instance()->AllocateSurface(this, new_surface_id)) {
    NOTIFY_ERROR(PLATFORM_FAILURE, "Failed to allocate the new surface");
    success = false;
  }

  // Ensure the current context is active when switching surfaces; we may need
  // to create a new texture.
  if (success && !make_context_current_cb_.Run()) {
    NOTIFY_ERROR(PLATFORM_FAILURE,
                 "Failed to make this decoder's GL context current when "
                 "switching surfaces.");
    success = false;
  }

  if (success) {
    codec_config_->surface_ =
        picture_buffer_manager_.Initialize(new_surface_id);
    if (codec_config_->surface_.IsEmpty()) {
      NOTIFY_ERROR(PLATFORM_FAILURE, "Failed to switch surfaces.");
      success = false;
    }
  }

  if (success && media_codec_ &&
      !media_codec_->SetSurface(codec_config_->surface_.j_surface().obj())) {
    NOTIFY_ERROR(PLATFORM_FAILURE, "MediaCodec failed to switch surfaces.");
    success = false;
  }

  if (success) {
    config_.surface_id = new_surface_id;
  } else {
    // This might be called from OnSurfaceDestroyed(), so we have to release the
    // MediaCodec if we failed to switch the surface.
    if (media_codec_) {
      AVDACodecAllocator::Instance()->ReleaseMediaCodec(
          std::move(media_codec_), codec_config_->task_type_,
          previous_surface_id);
      picture_buffer_manager_.CodecChanged(nullptr);
    }
    AVDACodecAllocator::Instance()->DeallocateSurface(this, new_surface_id);
  }

  // Regardless of whether we succeeded, we no longer own the previous surface.
  AVDACodecAllocator::Instance()->DeallocateSurface(this, previous_surface_id);

  return success;
}

}  // namespace media
