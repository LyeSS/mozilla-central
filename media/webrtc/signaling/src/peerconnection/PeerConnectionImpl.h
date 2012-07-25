/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _PEER_CONNECTION_IMPL_H_
#define _PEER_CONNECTION_IMPL_H_

#include <string>
#include <vector>
#include <map>

#include "prlock.h"
#include "mozilla/RefPtr.h"
#include "IPeerConnection.h"

#ifdef USE_FAKE_MEDIA_STREAMS
#include "FakeMediaStreams.h"
#else
#include "nsDOMMediaStream.h"
#endif

#include "nricectx.h"
#include "nricemediastream.h"

#include "peer_connection_types.h"
#include "CallControlManager.h"
#include "CC_Device.h"
#include "CC_Call.h"
#include "CC_Observer.h"
#include "MediaPipeline.h"

namespace sipcc {

class LocalSourceStreamInfo : public mozilla::MediaStreamListener {
public:
  LocalSourceStreamInfo(nsDOMMediaStream* aMediaStream)
    : mMediaStream(aMediaStream) {}
  ~LocalSourceStreamInfo() {}

  /**
   * Notify that changes to one of the stream tracks have been queued.
   * aTrackEvents can be any combination of TRACK_EVENT_CREATED and
   * TRACK_EVENT_ENDED. aQueuedMedia is the data being added to the track
   * at aTrackOffset (relative to the start of the stream).
   */
  virtual void NotifyQueuedTrackChanges(
    mozilla::MediaStreamGraph* aGraph,
    mozilla::TrackID aID,
    mozilla::TrackRate aTrackRate,
    mozilla::TrackTicks aTrackOffset,
    PRUint32 aTrackEvents,
    const mozilla::MediaSegment& aQueuedMedia
  );

  virtual void NotifyPull(mozilla::MediaStreamGraph* aGraph,
    mozilla::StreamTime aDesiredTime) {}

  nsRefPtr<nsDOMMediaStream> GetMediaStream();
  void StorePipeline(int track, mozilla::RefPtr<mozilla::MediaPipeline> pipeline);

  void ExpectAudio();
  void ExpectVideo();
  unsigned AudioTrackCount();
  unsigned VideoTrackCount();

private:
  std::map<int, mozilla::RefPtr<mozilla::MediaPipeline> > mPipelines;
  nsRefPtr<nsDOMMediaStream> mMediaStream;
  nsTArray<mozilla::TrackID> mAudioTracks;
  nsTArray<mozilla::TrackID> mVideoTracks;
};

class RemoteSourceStreamInfo {
 public:
  RemoteSourceStreamInfo(nsDOMMediaStream* aMediaStream) :
      mMediaStream(aMediaStream),
      mPipelines() {}

  nsRefPtr<nsDOMMediaStream> GetMediaStream();
  void StorePipeline(int track, mozilla::RefPtr<mozilla::MediaPipeline> pipeline);  

  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(RemoteSourceStreamInfo);
 private:
  nsRefPtr<nsDOMMediaStream> mMediaStream;  
  std::map<int, mozilla::RefPtr<mozilla::MediaPipeline> > mPipelines;
};

class PeerConnectionWrapper;

class PeerConnectionImpl MOZ_FINAL : public IPeerConnection,
                                     public sigslot::has_slots<> {
public:
  PeerConnectionImpl();
  ~PeerConnectionImpl();

  enum ReadyState {
    kNew,
    kNegotiating,
    kActive,
    kClosing,
    kClosed
  };

  enum SipccState {
    kIdle,
    kStarting,
    kStarted
  };

  // TODO(ekr@rtfm.com): make this conform to the specifications
  enum IceState {
    kIceGathering,
    kIceWaiting,
    kIceChecking,
    kIceConnected,
    kIceFailed
  };

  NS_DECL_ISUPPORTS
  NS_DECL_IPEERCONNECTION

  static PeerConnectionImpl* CreatePeerConnection();
  static void Shutdown();

  // Implementation of the only observer we need
  virtual void onCallEvent(
    ccapi_call_event_e callEvent,
    CSF::CC_CallPtr call,
    CSF::CC_CallInfoPtr info
  );

  // Handle system to allow weak references to be passed through C code
  static PeerConnectionWrapper *AcquireInstance(const std::string& handle);
  virtual void ReleaseInstance();
  virtual const std::string& GetHandle();

  // ICE events
  void IceGatheringCompleted(NrIceCtx *ctx);
  void IceCompleted(NrIceCtx *ctx);
  void IceStreamReady(NrIceMediaStream *stream);

  mozilla::RefPtr<NrIceCtx> ice_ctx() const { return mIceCtx; }
  mozilla::RefPtr<NrIceMediaStream> ice_media_stream(size_t i) const {
    // TODO(ekr@rtfm.com): If someone asks for a value that doesn't exist,
    // make one.
    if (i >= mIceStreams.size()) {
      return NULL;
    }
    return mIceStreams[i];
  }

  // Get a specific local stream
  nsRefPtr<LocalSourceStreamInfo> GetLocalStream(int index);
  
  // Get a specific remote stream
  nsRefPtr<RemoteSourceStreamInfo> GetRemoteStream(int index);

  // Add a remote stream. Returns the index in index
  nsresult AddRemoteStream(nsRefPtr<RemoteSourceStreamInfo> info, int *index);

private:
  void ChangeReadyState(ReadyState ready_state);
  PeerConnectionImpl(const PeerConnectionImpl&rhs);
  PeerConnectionImpl& operator=(PeerConnectionImpl);
  CSF::CC_CallPtr mCall;
  IPeerConnectionObserver* mPCObserver;
  ReadyState mReadyState;

  // The SDP sent in from JS - here for debugging.
  std::string mLocalRequestedSDP;
  std::string mRemoteRequestedSDP;
  // The SDP we are using.
  std::string mLocalSDP;
  std::string mRemoteSDP;

  // A list of streams returned from GetUserMedia
  PRLock *mLocalSourceStreamsLock;
  nsTArray<nsRefPtr<LocalSourceStreamInfo> > mLocalSourceStreams;

  // A list of streams provided by the other side
  PRLock *mRemoteSourceStreamsLock;
  nsTArray<nsRefPtr<RemoteSourceStreamInfo> > mRemoteSourceStreams;

  // A handle to refer to this PC with
  std::string mHandle;

  // ICE objects
  mozilla::RefPtr<NrIceCtx> mIceCtx;
  std::vector<mozilla::RefPtr<NrIceMediaStream> > mIceStreams;
  IceState mIceState;

  // Singleton list of all the PeerConnections
  static std::map<const std::string, PeerConnectionImpl *> peerconnections;
};

// This is what is returned when you acquire on a handle
class PeerConnectionWrapper {
 public:
  PeerConnectionWrapper(PeerConnectionImpl *impl) : impl_(impl) {}

  ~PeerConnectionWrapper() {
    if (impl_)
      impl_->ReleaseInstance();
  }

  PeerConnectionImpl *impl() { return impl_; }

 private:
  PeerConnectionImpl *impl_;
};

}  // end sipcc namespace

#endif  // _PEER_CONNECTION_IMPL_H_
