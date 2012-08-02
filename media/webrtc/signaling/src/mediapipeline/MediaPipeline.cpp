/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

// Original author: ekr@rtfm.com

#include "MediaPipeline.h"

#include <math.h>

#include "nspr.h"
#include <prlog.h>
#include "srtp.h"

#include "ImageLayers.h"
#include "logging.h"
#include "nsError.h"
#include "AudioSegment.h"
#include "ImageLayers.h"
#include "MediaSegment.h"
#include "transportflow.h"
#include "transportlayer.h"
#include "transportlayerdtls.h"
#include "transportlayerice.h"

#include "runnable_utils.h"

// Logging context
MLOG_INIT("mediapipeline");


namespace mozilla {

static char kDTLSExporterLabel[] = "EXTRACTOR-dtls_srtp";

nsresult MediaPipeline::Init() {
  conduit_->AttachTransport(transport_);
  
  PR_ASSERT(rtp_transport_);

  nsresult res;

  // TODO(ekr@rtfm.com): Danger....
  // Look to see if the transport is ready
  if (rtp_transport_->state() == TransportLayer::OPEN) {
    res = TransportReady(rtp_transport_);
    if (!NS_SUCCEEDED(res))
      return res;
  } else {
    rtp_transport_->SignalStateChange.connect(this,
                                              &MediaPipeline::StateChange);

    if (!muxed_) {
      if (rtcp_transport_->state() == TransportLayer::OPEN) {
        res = TransportReady(rtcp_transport_);
        if (!NS_SUCCEEDED(res))
          return res;
      } else {
        rtcp_transport_->SignalStateChange.connect(this,
                                                   &MediaPipeline::StateChange);
      }
    }
  }

  return NS_OK;
}

void MediaPipeline::StateChange(TransportFlow *flow, TransportLayer::State state) {
  // TODO(ekr@rtfm.com): check for double changes. This shouldn't happen,
  // but...
  MLOG(PR_LOG_DEBUG, "Flow is ready");
  if (state == TransportLayer::OPEN)
    TransportReady(flow);
}

nsresult MediaPipeline::TransportReady(TransportFlow *flow) {
  bool rtcp =  flow == rtp_transport_.get() ? false : true;
  nsresult res;

  MLOG(PR_LOG_DEBUG, "Transport ready for flow " << (rtcp ? "rtcp" : "rtp"));

  // Now instantiate the SRTP objects
  TransportLayerDtls *dtls = static_cast<TransportLayerDtls *>(
      flow->GetLayer(TransportLayerDtls::ID));
  PR_ASSERT(dtls);  // DTLS is mandatory

  PRUint16 cipher_suite;
  res = dtls->GetSrtpCipher(&cipher_suite);
  if (NS_FAILED(res)) {
    MLOG(PR_LOG_ERROR, "Failed to negotiate DTLS-SRTP. This is an error");
    return res;
  }

  // SRTP Key Exporter as per RFC 5764 S 4.2
  unsigned char srtp_block[SRTP_TOTAL_KEY_LENGTH * 2];
  res = dtls->ExportKeyingMaterial(kDTLSExporterLabel, false, "",
                                   srtp_block, sizeof(srtp_block));

  // Slice and dice as per RFC 5764 S 4.2
  unsigned char client_write_key[SRTP_TOTAL_KEY_LENGTH];
  unsigned char server_write_key[SRTP_TOTAL_KEY_LENGTH];
  int offset = 0;
  memcpy(client_write_key, srtp_block + offset, SRTP_MASTER_KEY_LENGTH);
  offset += SRTP_MASTER_KEY_LENGTH;
  memcpy(server_write_key, srtp_block + offset, SRTP_MASTER_KEY_LENGTH);
  offset += SRTP_MASTER_KEY_LENGTH;
  memcpy(client_write_key + SRTP_MASTER_KEY_LENGTH,
         srtp_block + offset, SRTP_MASTER_SALT_LENGTH);
  offset += SRTP_MASTER_SALT_LENGTH;
  memcpy(server_write_key + SRTP_MASTER_KEY_LENGTH,
         srtp_block + offset, SRTP_MASTER_KEY_LENGTH);
  offset += SRTP_MASTER_SALT_LENGTH;
  PR_ASSERT(offset == sizeof(srtp_block));

  unsigned char *write_key;
  unsigned char *read_key;

  if (dtls->role() == TransportLayerDtls::CLIENT) {
    write_key = client_write_key;
    read_key = server_write_key;
  } else {
    write_key = server_write_key;
    read_key = client_write_key;
  }

  if (!rtcp) {
    // RTP side
    PR_ASSERT(!rtp_send_srtp_ && !rtp_recv_srtp_);
    rtp_send_srtp_ = SrtpFlow::Create(cipher_suite, false,
                                      write_key, SRTP_TOTAL_KEY_LENGTH);
    rtp_recv_srtp_ = SrtpFlow::Create(cipher_suite, true,
                                      read_key, SRTP_TOTAL_KEY_LENGTH);
    if (!rtp_send_srtp_ || !rtp_recv_srtp_) {
      MLOG(PR_LOG_ERROR, "Couldn't create SRTP flow for RTCP");
      return NS_ERROR_FAILURE;
    }

    // Start listening
    if (muxed_) {
      PR_ASSERT(!rtcp_send_srtp_ && !rtcp_recv_srtp_);
      rtcp_send_srtp_ = rtp_send_srtp_;
      rtcp_recv_srtp_ = rtp_recv_srtp_;

      dtls->downward()->SignalPacketReceived.connect(this,
                                                     &MediaPipelineReceive::
                                                     PacketReceived);
    } else {
      dtls->downward()->SignalPacketReceived.connect(this,
                                                     &MediaPipelineReceive::
                                                     RtpPacketReceived);
    }
  }
  else {
    PR_ASSERT(!rtcp_send_srtp_ && !rtcp_recv_srtp_);
    rtcp_send_srtp_ = SrtpFlow::Create(cipher_suite, false,
                                       write_key, SRTP_TOTAL_KEY_LENGTH);
    rtcp_recv_srtp_ = SrtpFlow::Create(cipher_suite, true,
                                       read_key, SRTP_TOTAL_KEY_LENGTH);
    if (!rtcp_send_srtp_ || !rtcp_recv_srtp_) {
      MLOG(PR_LOG_ERROR, "Couldn't create SRTCP flow for RTCP");
      return NS_ERROR_FAILURE;
    }

    // Start listening
    dtls->downward()->SignalPacketReceived.connect(this,
                                                  &MediaPipelineReceive::
                                                  RtcpPacketReceived);
  }

  return NS_OK;
}

nsresult MediaPipeline::SendPacket(TransportFlow *flow, const void *data,
                                   int len) {
  // Note that we bypass the DTLS layer here
  TransportLayerDtls *dtls = static_cast<TransportLayerDtls *>(
      flow->GetLayer(TransportLayerDtls::ID));
  PR_ASSERT(dtls);

  TransportResult res = dtls->downward()->
      SendPacket(static_cast<const unsigned char *>(data), len);

  if (res != len) {
    // Ignore blocking indications
    if (res == TE_WOULDBLOCK)
      return NS_OK;

    MLOG(PR_LOG_ERROR, "Failed write on stream");
    return NS_BASE_STREAM_CLOSED;
  }

  return NS_OK;
}

void MediaPipeline::increment_rtp_packets_sent() {
  ++rtp_packets_sent_;
  if (!(rtp_packets_sent_ % 1000)) {
    MLOG(PR_LOG_DEBUG, "RTP packet count " << static_cast<void *>(this)
         << ": " << rtp_packets_sent_);
  }
}

void MediaPipeline::increment_rtcp_packets_sent() {
  if (!(rtcp_packets_sent_ % 1000)) {
    MLOG(PR_LOG_DEBUG, "RTCP packet count " << static_cast<void *>(this)
         << ": " << rtcp_packets_sent_);
  }
}

void MediaPipeline::increment_rtp_packets_received() {
  ++rtp_packets_received_;
  if (!(rtp_packets_received_ % 1000)) {
    MLOG(PR_LOG_DEBUG, "RTP packet count " << static_cast<void *>(this)
         << ": " << rtp_packets_received_);
  }
}

void MediaPipeline::increment_rtcp_packets_received() {
  if (!(rtcp_packets_received_ % 1000)) {
    MLOG(PR_LOG_DEBUG, "RTCP packet count " << static_cast<void *>(this)
         << ": " << rtcp_packets_received_);
  }
}


void MediaPipeline::RtpPacketReceived(TransportLayer *layer,
                                      const unsigned char *data,
                                      size_t len) {
  increment_rtp_packets_received();

  PR_ASSERT(rtp_recv_srtp_);  // This should never happen

  // Make a copy rather than cast away constness
  mozilla::ScopedDeletePtr<unsigned char> inner_data(
      new unsigned char[len]);
  memcpy(inner_data, data, len);
  int out_len;
  nsresult res = rtp_recv_srtp_->UnprotectRtp(inner_data,
                                         len, len, &out_len);
  if (!NS_SUCCEEDED(res))
    return;

  (void)conduit_->ReceivedRTPPacket(inner_data, out_len);  // Ignore error codes
}

void MediaPipeline::RtcpPacketReceived(TransportLayer *layer,
                                              const unsigned char *data,
                                              size_t len) {
  increment_rtcp_packets_received();

  PR_ASSERT(rtcp_recv_srtp_);  // This should never happen

  // Make a copy rather than cast away constness
  mozilla::ScopedDeletePtr<unsigned char> inner_data(
      new unsigned char[len]);
  memcpy(inner_data, data, len);
  int out_len;

  nsresult res = rtcp_recv_srtp_->UnprotectRtcp(inner_data, len, len, &out_len);

  if (!NS_SUCCEEDED(res))
    return;

  (void)conduit_->ReceivedRTCPPacket(inner_data, out_len);  // Ignore error codes
}

bool MediaPipeline::IsRtp(const unsigned char *data, size_t len) {
  if (len < 2)
    return false;

  // TODO(ekr@rtfm.com): this needs updating in light of RFC5761
  if ((data[1] >= 200) && (data[1] <= 204))
    return false;

  return true;

}

void MediaPipeline::PacketReceived(TransportLayer *layer,
                                   const unsigned char *data,
                                   size_t len) {
  if (IsRtp(data, len)) {
    RtpPacketReceived(layer, data, len);
  } else {
    RtcpPacketReceived(layer, data, len);
  }
}

nsresult MediaPipelineTransmit::Init() {
  // TODO(ekr@rtfm.com): Check for errors
  MLOG(PR_LOG_DEBUG, "Attaching pipeline to stream " << static_cast<void *>(stream_) << 
                    " conduit type=" << (conduit_->type() == MediaSessionConduit::AUDIO ?
                                         "audio" : "video") <<
                    " hints=" << stream_->GetHintContents());

  if (main_thread_) {
    main_thread_->Dispatch(WrapRunnable(
      stream_->GetStream(), &mozilla::MediaStream::AddListener, listener_),
      NS_DISPATCH_SYNC);
  }
  else {
    stream_->GetStream()->AddListener(listener_);
  }

  return NS_OK;
}

nsresult MediaPipeline::PipelineTransport::SendRtpPacket(
    const void *data, int len) {
  if (!pipeline_)
    return NS_OK;  // Detached

  if (!pipeline_->rtp_send_srtp_) {
    MLOG(PR_LOG_DEBUG, "Couldn't write RTP packet; SRTP not set up yet");
    return NS_OK;
  }

  PR_ASSERT(pipeline_->rtp_transport_);
  if (!pipeline_->rtp_transport_) {
    MLOG(PR_LOG_DEBUG, "Couldn't write RTP packet (null flow)");
    return NS_ERROR_NULL_POINTER;
  }

  // libsrtp enciphers in place, so we need a new, big enough
  // buffer.
  int max_len = len + SRTP_MAX_EXPANSION;
  mozilla::ScopedDeletePtr<unsigned char> inner_data(
      new unsigned char[max_len]);
  memcpy(inner_data, data, len);

  int out_len;
  nsresult res = pipeline_->rtp_send_srtp_->ProtectRtp(inner_data,
                                                       len,
                                                       max_len,
                                                       &out_len);
  if (!NS_SUCCEEDED(res))
    return res;

  pipeline_->increment_rtp_packets_sent();
  return pipeline_->SendPacket(pipeline_->rtp_transport_, inner_data,
                               out_len);
}

nsresult MediaPipeline::PipelineTransport::SendRtcpPacket(
    const void *data, int len) {
  if (!pipeline_)
    return NS_OK;  // Detached

  if (!pipeline_->rtcp_send_srtp_) {
    MLOG(PR_LOG_DEBUG, "Couldn't write RTCP packet; SRTCP not set up yet");
    return NS_OK;
  }

  if (!pipeline_->rtcp_transport_) {
    MLOG(PR_LOG_DEBUG, "Couldn't write RTCP packet (null flow)");
    return NS_ERROR_NULL_POINTER;
  }

  // libsrtp enciphers in place, so we need a new, big enough
  // buffer.
  int max_len = len + SRTP_MAX_EXPANSION;
  mozilla::ScopedDeletePtr<unsigned char> inner_data(
      new unsigned char[max_len]);
  memcpy(inner_data, data, len);

  int out_len;
  nsresult res = pipeline_->rtcp_send_srtp_->ProtectRtcp(inner_data,
                                                         len,
                                                         max_len,
                                                         &out_len);
  if (!NS_SUCCEEDED(res))
    return res;

  pipeline_->increment_rtcp_packets_sent();
  return pipeline_->SendPacket(pipeline_->rtcp_transport_, inner_data,
                               out_len);
}

void MediaPipelineTransmit::PipelineListener::
NotifyQueuedTrackChanges(MediaStreamGraph* graph, TrackID tid,
                         TrackRate rate,
                         TrackTicks offset,
                         PRUint32 events,
                         const MediaSegment& queued_media) {
  if (!pipeline_)
    return;  // Detached

  MLOG(PR_LOG_DEBUG, "MediaPipeline::NotifyQueuedTrackChanges()");
  // TODO(ekr@rtfm.com): For now assume that we have only one
  // track type and it's destined for us
  if (queued_media.GetType() == MediaSegment::AUDIO) {
    if (pipeline_->conduit_->type() != MediaSessionConduit::AUDIO) {
      // Ignore data in case we have a muxed stream
      return;
    }
    AudioSegment* audio = const_cast<AudioSegment *>(
        static_cast<const AudioSegment *>(&queued_media));

    AudioSegment::ChunkIterator iter(*audio);
    while(!iter.IsEnded()) {
      pipeline_->ProcessAudioChunk(static_cast<AudioSessionConduit *>
                                   (pipeline_->conduit_.get()),
                                   rate, *iter);
      iter.Next();
    }
  } else if (queued_media.GetType() == MediaSegment::VIDEO) {
    if (pipeline_->conduit_->type() != MediaSessionConduit::VIDEO) {
      // Ignore data in case we have a muxed stream
      return;
    }
    VideoSegment* video = const_cast<VideoSegment *>(
        static_cast<const VideoSegment *>(&queued_media));

    VideoSegment::ChunkIterator iter(*video);
    while(!iter.IsEnded()) {
      pipeline_->ProcessVideoChunk(static_cast<VideoSessionConduit *>
                                   (pipeline_->conduit_.get()),
                                   rate, *iter);
      iter.Next();
    }
  } else {
    // Ignore
  }
}

void MediaPipelineTransmit::ProcessAudioChunk(AudioSessionConduit *conduit,
                                              TrackRate rate,
                                              AudioChunk& chunk) {
  // TODO(ekr@rtfm.com): Do more than one channel
  nsAutoArrayPtr<int16_t> samples(new int16_t[chunk.mDuration]);

  if (chunk.mBuffer) {
    switch(chunk.mBufferFormat) {
      case nsAudioStream::FORMAT_U8:
      case nsAudioStream::FORMAT_FLOAT32:
        MLOG(PR_LOG_ERROR, "Can't process audio exceptin 16-bit PCM yet");
        PR_ASSERT(PR_FALSE);
        return;
        break;
      case nsAudioStream::FORMAT_S16_LE:
        {
          // Code based on nsAudioStream
          const short* buf = static_cast<const short *>(chunk.mBuffer->Data());

          PRInt32 volume = PRInt32((1 << 16) * chunk.mVolume);
          for (PRUint32 i = 0; i < chunk.mDuration; ++i) {
            int16_t s = buf[i];
#if defined(IS_BIG_ENDIAN)
            s = ((s & 0x00ff) << 8) | ((s & 0xff00) >> 8);
#endif
            samples[i] = short((PRInt32(s) * volume) >> 16);
          }
        }
        break;
      default:
        PR_ASSERT(PR_FALSE);
        return;
        break;
    }
  } else {
    for (PRUint32 i = 0; i < chunk.mDuration; ++i) {
      samples[i] = 0;
    }
  }

  MLOG(PR_LOG_DEBUG, "Sending an audio frame");
  conduit->SendAudioFrame(samples.get(), chunk.mDuration, rate, 0);
}


void MediaPipelineTransmit::ProcessVideoChunk(VideoSessionConduit *conduit,
                                              TrackRate rate,
                                              VideoChunk& chunk) {
#ifdef MOZILLA_INTERNAL_API
  // We now need to send the video frame to the other side
  mozilla::layers::Image *img = chunk.mFrame.GetImage();

  mozilla::layers::Image::Format format = img->GetFormat();

  if (format != mozilla::layers::Image::PLANAR_YCBCR) {
    MLOG(PR_LOG_ERROR, "Can't process non-YCBCR video");
    PR_ASSERT(PR_FALSE);
    return;
  }

  // Cast away constness b/c some of the accessors are non-const
  layers::PlanarYCbCrImage* yuv =
    const_cast<layers::PlanarYCbCrImage *>(
      static_cast<const layers::PlanarYCbCrImage *>(img));

  // TODO(ekr@rtfm.com): Is this really how we get the length?
  // It's the inverse of the code in MediaEngineDefault
  unsigned int length = ((yuv->GetSize().width * yuv->GetSize().height) * 3 / 2);

  // Big-time assumption here that this is all contiguous data coming
  // from Anant's version of gUM. This code here is an attempt to double-check
  // that
  PR_ASSERT(length == yuv->GetDataSize());
  if (length != yuv->GetDataSize())
    return;

  // OK, pass it on to the conduit
  // TODO(ekr@rtfm.com): Check return value
  conduit->SendVideoFrame(yuv->mBuffer.get(), yuv->GetDataSize(),
    yuv->GetSize().width, yuv->GetSize().height, mozilla::kVideoI420, 0);
#endif
}

nsresult MediaPipelineReceiveAudio::Init() {
  MLOG(PR_LOG_DEBUG, __FUNCTION__);
  if (main_thread_) {
    main_thread_->Dispatch(WrapRunnable(
      stream_->GetStream(), &mozilla::MediaStream::AddListener, listener_),
      NS_DISPATCH_SYNC);
  }
  else {
    stream_->GetStream()->AddListener(listener_);
  }

  return NS_OK;
}

void MediaPipelineReceiveAudio::PipelineListener::
NotifyPull(MediaStreamGraph* graph, StreamTime total) {
  mozilla::SourceMediaStream *source =
    pipeline_->stream_->GetStream()->AsSourceStream();

  PR_ASSERT(source);
  if (!source) {
    MLOG(PR_LOG_ERROR, "NotifyPull() called from a non-SourceMediaStream");
    return;
  }

  // "total" is absolute stream time.
  StreamTime desired = total - played_;
  played_ = total;
  double time_s = MediaTimeToSeconds(desired);

  // Number of 10 ms samples we need
  //int num_samples = ceil(time_s / .01f);

  // Doesn't matter what was asked for, always give 160 samples per 10 ms.
  int num_samples = 1;

  MLOG(PR_LOG_DEBUG, "Asking for " << num_samples << "sample from Audio Conduit");

  if (num_samples <= 0) {
    return;
  }

  while (num_samples--) {
    // TODO(ekr@rtfm.com): Is there a way to avoid mallocating here?
    nsRefPtr<SharedBuffer> samples = SharedBuffer::Create(1000);
    int samples_length;

    mozilla::MediaConduitErrorCode err =
      static_cast<mozilla::AudioSessionConduit*>(pipeline_->conduit_.get())->GetAudioFrame(
        static_cast<int16_t *>(samples->Data()),
        16000,  // Sampling rate fixed at 16 kHz for now
        0,  // TODO(ekr@rtfm.com): better estimate of capture delay
        samples_length);

    if (err != mozilla::kMediaConduitNoError)
      return;

    MLOG(PR_LOG_DEBUG, "Audio conduit returned buffer of length " << samples_length);

    mozilla::AudioSegment segment;
    segment.Init(1);
    segment.AppendFrames(samples.forget(), samples_length,
      0, samples_length, nsAudioStream::FORMAT_S16_LE);

    char buf[32];
    snprintf(buf, 32, "%p", source);
    MLOG(PR_LOG_DEBUG, "Appended segments to stream " << buf);
    source->AppendToTrack(1,  // TODO(ekr@rtfm.com): Track ID
      &segment);
  }
}

nsresult MediaPipelineReceiveVideo::Init() {
  MLOG(PR_LOG_DEBUG, __FUNCTION__);

  static_cast<mozilla::VideoSessionConduit *>(conduit_.get())->
      AttachRenderer(renderer_);

  return NS_OK;
}

MediaPipelineReceiveVideo::PipelineRenderer::PipelineRenderer(
    MediaPipelineReceiveVideo *pipeline) :
    pipeline_(pipeline),
#ifdef MOZILLA_INTERNAL_API
    image_container_(mozilla::layers::LayerManager::CreateImageContainer()),
#endif
    width_(640), height_(480) {

#ifdef MOZILLA_INTERNAL_API
  mozilla::SourceMediaStream *source =
    pipeline_->stream_->GetStream()->AsSourceStream();
  source->AddTrack(1, 10, 0, new mozilla::VideoSegment());
  source->AdvanceKnownTracksTime(mozilla::STREAM_TIME_MAX);
#endif
}

void MediaPipelineReceiveVideo::PipelineRenderer::RenderVideoFrame(
    const unsigned char* buffer,
    unsigned int buffer_size,
    uint32_t time_stamp,
    int64_t render_time) {
#ifdef MOZILLA_INTERNAL_API
  mozilla::SourceMediaStream *source =
    pipeline_->stream_->GetStream()->AsSourceStream();

  // Create a video frame and append it to the track.
  mozilla::layers::Image::Format format = mozilla::layers::Image::PLANAR_YCBCR;
  nsRefPtr<mozilla::layers::Image> image = image_container_->CreateImage(&format, 1);

  mozilla::layers::PlanarYCbCrImage* videoImage = static_cast<mozilla::layers::PlanarYCbCrImage*>(image.get());
  PRUint8* frame = const_cast<PRUint8*>(static_cast<const PRUint8*> (buffer));
  const PRUint8 lumaBpp = 8;
  const PRUint8 chromaBpp = 4;

  mozilla::layers::PlanarYCbCrImage::Data data;
  data.mYChannel = frame;
  data.mYSize = gfxIntSize(width_, height_);
  data.mYStride = width_ * lumaBpp/ 8;
  data.mCbCrStride = width_ * chromaBpp / 8;
  data.mCbChannel = frame + height_ * data.mYStride;
  data.mCrChannel = data.mCbChannel + height_ * data.mCbCrStride / 2;
  data.mCbCrSize = gfxIntSize(width_/ 2, height_/ 2);
  data.mPicX = 0;
  data.mPicY = 0;
  data.mPicSize = gfxIntSize(width_, height_);
  data.mStereoMode = mozilla::layers::STEREO_MODE_MONO;

  videoImage->SetData(data);

  VideoSegment segment;
  segment.AppendFrame(image.forget(), 1, gfxIntSize(width_, height_));
  source->AppendToTrack(1, &(segment));
#endif
}


}  // end namespace

