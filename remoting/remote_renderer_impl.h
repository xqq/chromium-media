// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_REMOTING_REMOTE_RENDERER_IMPL_H_
#define MEDIA_REMOTING_REMOTE_RENDERER_IMPL_H_

#include <stdint.h>

#include <memory>

#include "base/callback.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/synchronization/lock.h"
#include "media/base/buffering_state.h"
#include "media/base/pipeline_status.h"
#include "media/base/renderer.h"
#include "media/base/renderer_client.h"
#include "media/mojo/interfaces/remoting.mojom.h"
#include "media/remoting/remoting_interstitial_ui.h"
#include "media/remoting/rpc/rpc_broker.h"
#include "mojo/public/cpp/system/data_pipe.h"

namespace media {

class RemotingRendererController;
class Renderer;

namespace remoting {
class RemoteDemuxerStreamAdapter;
};

// A media::Renderer implementation that use a media::Renderer to render
// media streams.
class RemoteRendererImpl : public Renderer {
 public:
  // The whole class except for constructor and GetMediaTime() runs on
  // |media_task_runner|. The constructor and GetMediaTime() run on render main
  // thread.
  RemoteRendererImpl(
      scoped_refptr<base::SingleThreadTaskRunner> media_task_runner,
      const base::WeakPtr<RemotingRendererController>&
          remoting_renderer_controller,
      VideoRendererSink* video_renderer_sink);
  ~RemoteRendererImpl() final;

 private:
  // Callback when attempting to establish data pipe. The function is set to
  // static in order to post task to media thread in order to avoid threading
  // race condition.
  static void OnDataPipeCreatedOnMainThread(
      scoped_refptr<base::SingleThreadTaskRunner> media_task_runner,
      base::WeakPtr<RemoteRendererImpl> self,
      base::WeakPtr<remoting::RpcBroker> rpc_broker,
      mojom::RemotingDataStreamSenderPtrInfo audio,
      mojom::RemotingDataStreamSenderPtrInfo video,
      mojo::ScopedDataPipeProducerHandle audio_handle,
      mojo::ScopedDataPipeProducerHandle video_handle);

  // Callback function when RPC message is received. The function is set to
  // static in order to post task to media thread in order to avoid threading
  // race condition.
  static void OnMessageReceivedOnMainThread(
      scoped_refptr<base::SingleThreadTaskRunner> media_task_runner,
      base::WeakPtr<RemoteRendererImpl> self,
      std::unique_ptr<remoting::pb::RpcMessage> message);

  // Callback when remoting interstitial needs to be updated. Will post task to
  // media thread to avoid threading race condition.
  static void RequestUpdateInterstitialOnMainThread(
      scoped_refptr<base::SingleThreadTaskRunner> media_task_runner,
      base::WeakPtr<RemoteRendererImpl> remote_renderer_impl,
      const SkBitmap& background_image,
      const gfx::Size& canvas_size,
      RemotingInterstitialType interstitial_type);

 public:
  // media::Renderer implementation.
  void Initialize(DemuxerStreamProvider* demuxer_stream_provider,
                  media::RendererClient* client,
                  const PipelineStatusCB& init_cb) final;
  void SetCdm(CdmContext* cdm_context,
              const CdmAttachedCB& cdm_attached_cb) final;
  void Flush(const base::Closure& flush_cb) final;
  void StartPlayingFrom(base::TimeDelta time) final;
  void SetPlaybackRate(double playback_rate) final;
  void SetVolume(float volume) final;
  base::TimeDelta GetMediaTime() final;

 private:
  friend class RemoteRendererImplTest;

  enum State {
    STATE_UNINITIALIZED,
    STATE_CREATE_PIPE,
    STATE_ACQUIRING,
    STATE_INITIALIZING,
    STATE_FLUSHING,
    STATE_PLAYING,
    STATE_ERROR
  };

  // Callback when attempting to establish data pipe. Runs on media thread only.
  void OnDataPipeCreated(mojom::RemotingDataStreamSenderPtrInfo audio,
                         mojom::RemotingDataStreamSenderPtrInfo video,
                         mojo::ScopedDataPipeProducerHandle audio_handle,
                         mojo::ScopedDataPipeProducerHandle video_handle,
                         int audio_rpc_handle,
                         int video_rpc_handle);

  // Callback function when RPC message is received. Runs on media thread only.
  void OnReceivedRpc(std::unique_ptr<remoting::pb::RpcMessage> message);

  // Function to post task to main thread in order to send RPC message.
  void SendRpcToRemote(std::unique_ptr<remoting::pb::RpcMessage> message);

  // Functions when RPC message is received.
  void AcquireRendererDone(std::unique_ptr<remoting::pb::RpcMessage> message);
  void InitializeCallback(std::unique_ptr<remoting::pb::RpcMessage> message);
  void FlushUntilCallback();
  void SetCdmCallback(std::unique_ptr<remoting::pb::RpcMessage> message);
  void OnTimeUpdate(std::unique_ptr<remoting::pb::RpcMessage> message);
  void OnBufferingStateChange(
      std::unique_ptr<remoting::pb::RpcMessage> message);
  void OnVideoNaturalSizeChange(
      std::unique_ptr<remoting::pb::RpcMessage> message);
  void OnVideoOpacityChange(std::unique_ptr<remoting::pb::RpcMessage> message);
  void OnStatisticsUpdate(std::unique_ptr<remoting::pb::RpcMessage> message);
  void OnDurationChange(std::unique_ptr<remoting::pb::RpcMessage> message);

  // Called to update the remoting interstitial. Draw remoting interstitial on
  // |interstitial_background_| if |background_image| is empty. Update
  // |interstitial_background_| if |background_image| is not empty.
  void UpdateInterstitial(const SkBitmap& background_image,
                          const gfx::Size& canvas_size,
                          RemotingInterstitialType interstitial_type);

  // Shut down remoting session.
  void OnFatalError(PipelineStatus status);

  State state_;
  const scoped_refptr<base::SingleThreadTaskRunner> main_task_runner_;
  const scoped_refptr<base::SingleThreadTaskRunner> media_task_runner_;

  // Current renderer playback time information.
  base::TimeDelta current_media_time_;
  base::TimeDelta current_max_time_;
  // Both |current_media_time_| and |current_max_time_| should be protected by
  // lock because it can be accessed from both media and render main thread.
  base::Lock time_lock_;

  DemuxerStreamProvider* demuxer_stream_provider_;
  media::RendererClient* client_;
  std::unique_ptr<remoting::RemoteDemuxerStreamAdapter>
      audio_demuxer_stream_adapter_;
  std::unique_ptr<remoting::RemoteDemuxerStreamAdapter>
      video_demuxer_stream_adapter_;

  // Component to establish mojo remoting service on browser process.
  const base::WeakPtr<RemotingRendererController> remoting_renderer_controller_;
  // Broker class to process incoming and outgoing RPC message.
  const base::WeakPtr<remoting::RpcBroker> rpc_broker_;
  // RPC handle value for RemoteRendererImpl component.
  const int rpc_handle_;

  // RPC handle value for render on receiver endpoint.
  int remote_renderer_handle_;

  // Callbacks.
  PipelineStatusCB init_workflow_done_callback_;
  CdmAttachedCB cdm_attached_cb_;
  base::Closure flush_cb_;

  VideoRendererSink* const video_renderer_sink_;  // Outlives this class.
  // The background image for remoting interstitial. When |this| is destructed,
  // |interstitial_background_| will be paint to clear the cast messages on
  // the interstitial.
  SkBitmap interstitial_background_;
  gfx::Size canvas_size_;

  base::WeakPtrFactory<RemoteRendererImpl> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(RemoteRendererImpl);
};

}  // namespace media

#endif  // MEDIA_REMOTING_REMOTE_RENDERER_IMPL_H_
