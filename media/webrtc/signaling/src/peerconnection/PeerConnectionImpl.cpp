/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <string>
#include <iostream>

#include "CSFLog.h"
#include "CSFLogStream.h"
#include "ccapi_call_info.h"
#include "CC_SIPCCCallInfo.h"
#include "ccapi_device_info.h"
#include "CC_SIPCCDeviceInfo.h"
#include "vcm.h"
#include "PeerConnectionCtx.h"
#include "PeerConnectionImpl.h"
#include "nsThreadUtils.h"
#include "runnable_utils.h"

#ifndef USE_FAKE_MEDIA_STREAMS
#include "MediaSegment.h"
#endif

static const char* logTag = "PeerConnectionImpl";

namespace sipcc {

/* We get this callback in order to find out which tracks are audio and which
 * are video. We should get this callback right away for existing streams after
 * we add this class as a listener.
 */
void
LocalSourceStreamInfo::NotifyQueuedTrackChanges(
  mozilla::MediaStreamGraph* aGraph,
  mozilla::TrackID aID,
  mozilla::TrackRate aTrackRate,
  mozilla::TrackTicks aTrackOffset,
  PRUint32 aTrackEvents,
  const mozilla::MediaSegment& aQueuedMedia)
{
  /* Add the track ID to the list for audio/video so they can be counted when
   * createOffer/createAnswer is called. This tells us whether we have the
   * camera, mic, or both for example.
   */
  mozilla::MediaSegment::Type trackType = aQueuedMedia.GetType();

  if (trackType == mozilla::MediaSegment::AUDIO) {
    // Should have very few tracks so not a hashtable
    bool found = false;
    for (unsigned u = 0; u < mAudioTracks.Length(); u++) {
      if (aID == mAudioTracks.ElementAt(u)) {
        found = true;
        break;
      }
    }

    if (!found) {
      mAudioTracks.AppendElement(aID);
    }
  } else if (trackType == mozilla::MediaSegment::VIDEO) {
    // Should have very few tracks so not a hashtable
    bool found = false;
    for (unsigned u = 0; u < mVideoTracks.Length(); u++) {
      if (aID == mVideoTracks.ElementAt(u)) {
        found = true;
        break;
      }
    }

    if (!found) {
      mVideoTracks.AppendElement(aID);
    }
  } else {
    CSFLogError(logTag, "NotifyQueuedTrackChanges - unknown media type");
  }
}

nsRefPtr<nsDOMMediaStream>
LocalSourceStreamInfo::GetMediaStream()
{
  return mMediaStream;
}

/* If the ExpectAudio hint is on we will add a track at the default first
 * audio track ID (0)
 * FIX - Do we need to iterate over the tracks instead of taking these hints?
 */
void
LocalSourceStreamInfo::ExpectAudio()
{
  mAudioTracks.AppendElement(0);
}

// If the ExpectVideo hint is on we will add a track at the default first
// video track ID (1).
void
LocalSourceStreamInfo::ExpectVideo()
{
  mVideoTracks.AppendElement(1);
}

unsigned
LocalSourceStreamInfo::AudioTrackCount()
{
  return mAudioTracks.Length();
}

unsigned
LocalSourceStreamInfo::VideoTrackCount()
{
  return mVideoTracks.Length();
}

PeerConnectionImpl* PeerConnectionImpl::CreatePeerConnection() 
{
  PeerConnectionImpl *pc = new PeerConnectionImpl();
  return pc;
}

std::map<const std::string, PeerConnectionImpl *>
  PeerConnectionImpl::peerconnections;

NS_IMPL_THREADSAFE_ISUPPORTS1(PeerConnectionImpl, IPeerConnection)

PeerConnectionImpl::PeerConnectionImpl()
  : mCall(NULL)
  , mPCObserver(NULL)
  , mReadyState(kNew)
  , mLocalSourceStreamsLock(PR_NewLock())
  , mIceCtx(NULL)
  , mIceStreams(NULL)
  , mIceState(kIceGathering) {}

PeerConnectionImpl::~PeerConnectionImpl()
{
  peerconnections.erase(mHandle);
  Close();
  PR_DestroyLock(mLocalSourceStreamsLock);
}

NS_IMETHODIMP
PeerConnectionImpl::Initialize(IPeerConnectionObserver* observer) {
  if (!observer) {
    return NS_ERROR_FAILURE;
  }

  mPCObserver = observer;
  PeerConnectionCtx *pcctx = PeerConnectionCtx::GetInstance();

  if (!pcctx) {
    return NS_ERROR_FAILURE;
  }

  mCall = pcctx->createCall();
  if (!mCall.get()) {
    return NS_ERROR_FAILURE;
  }

  // Generate a handle from our pointer.
  unsigned char handle_bin[sizeof(void*)];
  PeerConnectionImpl *handle = this;
  PR_ASSERT(sizeof(handle_bin) >= sizeof(handle));

  memcpy(handle_bin, &handle, sizeof(handle));
  for (size_t i = 0; i<sizeof(handle_bin); i++) {
    char hex[3];
    snprintf(hex, 3, "%.2x", handle_bin[i]);
    mHandle += hex;
  }

  // TODO(ekr@rtfm.com): need some way to set not offerer later
  // Looks like a bug in the NrIceCtx API.
  mIceCtx = NrIceCtx::Create("PC", true);
  mIceCtx->SignalGatheringCompleted.connect(this, &PeerConnectionImpl::IceGatheringCompleted);
  mIceCtx->SignalCompleted.connect(this, &PeerConnectionImpl::IceCompleted);

  // Create two streams to start with, assume one for audio and
  // one for video
  mIceStreams.push_back(mIceCtx->CreateStream("stream1", 2));
  mIceStreams.push_back(mIceCtx->CreateStream("stream2", 2));

  for (std::size_t i=0; i<mIceStreams.size(); i++) {
    mIceStreams[i]->SignalReady.connect(this, &PeerConnectionImpl::IceStreamReady);
  }

  // Start gathering
  nsresult res;
  mIceCtx->thread()->Dispatch(WrapRunnableRet(
    mIceCtx, &NrIceCtx::StartGathering, &res), NS_DISPATCH_SYNC
  );
  PR_ASSERT(NS_SUCCEEDED(res));

  // Store under mHandle
  mCall->setPeerConnection(mHandle);
  peerconnections[mHandle] = this;

  return NS_OK;
}

/*
 * CC_SDP_DIRECTION_SENDRECV will not be used when Constraints are implemented
 */
NS_IMETHODIMP
PeerConnectionImpl::CreateOffer(const char* hints) {
  mCall->createOffer(hints);
  return NS_OK;
}

NS_IMETHODIMP
PeerConnectionImpl::CreateAnswer(const char* hints, const char* offer) {
  mCall->createAnswer(hints, offer);
  return NS_OK;
}

NS_IMETHODIMP
PeerConnectionImpl::SetLocalDescription(PRUint32 action, const char* sdp) {
  mLocalRequestedSDP = sdp;
  mCall->setLocalDescription((cc_jsep_action_t)action, sdp);
  return NS_OK;
}

NS_IMETHODIMP
PeerConnectionImpl::SetRemoteDescription(PRUint32 action, const char* sdp) {
  mRemoteRequestedSDP = sdp;
  mCall->setRemoteDescription((cc_jsep_action_t)action, sdp);
  return NS_OK;
}

NS_IMETHODIMP
PeerConnectionImpl::AddStream(nsIDOMMediaStream* aMediaStream)
{
  nsDOMMediaStream* stream = static_cast<nsDOMMediaStream*>(aMediaStream);

  CSFLogDebug(logTag, "AddStream");

  // TODO(ekr@rtfm.com): Remove these asserts?
  // Adding tracks here based on nsDOMMediaStream expectation settings
  PRUint32 hints = stream->GetHintContents();

  if (!(hints & (nsDOMMediaStream::HINT_CONTENTS_AUDIO |
        nsDOMMediaStream::HINT_CONTENTS_VIDEO))) {
    CSFLogError(logTag, "Stream must contain either audio or video");
    return NS_ERROR_FAILURE;
  }

  // Now see if we already have a stream of this type, since we only
  // allow one of each.
  // TODO(ekr@rtfm.com): remove this when multiple of each stream
  // is allowed
  PR_Lock(mLocalSourceStreamsLock);
  for (unsigned u = 0; u < mLocalSourceStreams.Length(); u++) {
    nsRefPtr<LocalSourceStreamInfo> localSourceStream = mLocalSourceStreams[u];

    if (localSourceStream->GetMediaStream()->GetHintContents() & hints) {
      CSFLogError(logTag, "Only one stream of any given type allowed");
      PR_Unlock(mLocalSourceStreamsLock);
      PR_ASSERT(PR_FALSE);
      return NS_ERROR_FAILURE;
    }
  }

  // OK, we're good to add
  nsRefPtr<LocalSourceStreamInfo> localSourceStream =
    new LocalSourceStreamInfo(stream);
  cc_media_track_id_t media_stream_id = mLocalSourceStreams.Length();

  if (hints & nsDOMMediaStream::HINT_CONTENTS_AUDIO) {
    localSourceStream->ExpectAudio();
    mCall->addStream(media_stream_id, 0, AUDIO);
  }

  if (hints & nsDOMMediaStream::HINT_CONTENTS_VIDEO) {
    localSourceStream->ExpectVideo();
    mCall->addStream(media_stream_id, 0, VIDEO);
  }

  // Make it the listener for info from the MediaStream and add it to the list
  mozilla::MediaStream *plainMediaStream = stream->GetStream();

  if (plainMediaStream) {
    plainMediaStream->AddListener(localSourceStream);
  }

  mLocalSourceStreams.AppendElement(localSourceStream);

  PR_Unlock(mLocalSourceStreamsLock);
  return NS_OK;
}

NS_IMETHODIMP
PeerConnectionImpl::RemoveStream(nsIDOMMediaStream* aMediaStream)
{
  nsDOMMediaStream* stream = static_cast<nsDOMMediaStream*>(aMediaStream);
  CSFLogDebug(logTag, "RemoveStream");

  PR_Lock(mLocalSourceStreamsLock);
  for (unsigned u = 0; u < mLocalSourceStreams.Length(); u++) {
    nsRefPtr<LocalSourceStreamInfo> localSourceStream = mLocalSourceStreams[u];
    if (localSourceStream->GetMediaStream() == stream) {
      PRUint32 hints = stream->GetHintContents();
      if (hints & nsDOMMediaStream::HINT_CONTENTS_AUDIO) {
        // <emannion>  This API will change when we implement multiple streams
        //             It will only need the ID
        mCall->removeStream(u, 0, AUDIO);
      }
      if (hints & nsDOMMediaStream::HINT_CONTENTS_VIDEO) {
        mCall->removeStream(u, 1, VIDEO);
      }
      break;
    }
  }

  PR_Unlock(mLocalSourceStreamsLock);
  return NS_OK;
}

NS_IMETHODIMP
PeerConnectionImpl::CloseStreams() {
  if (mReadyState != PeerConnectionImpl::kClosed)  {
    ChangeReadyState(PeerConnectionImpl::kClosing);
  }

  mCall->endCall();
  return NS_OK;
}

NS_IMETHODIMP
PeerConnectionImpl::AddIceCandidate(const char* strCandidate)
{
  mCall->addIceCandidate(strCandidate);
  return NS_OK;
}

NS_IMETHODIMP
PeerConnectionImpl::GetLocalDescription(char** sdp)
{
  char* tmp = new char[mLocalSDP.size() + 1];
  std::copy(mLocalSDP.begin(), mLocalSDP.end(), tmp);
  tmp[mLocalSDP.size()] = '\0';

  *sdp = tmp;
  return NS_OK;
}

NS_IMETHODIMP
PeerConnectionImpl::GetRemoteDescription(char** sdp)
{
  char* tmp = new char[mRemoteSDP.size() + 1];
  std::copy(mRemoteSDP.begin(), mRemoteSDP.end(), tmp);
  tmp[mRemoteSDP.size()] = '\0';

  *sdp = tmp;
  return NS_OK;
}

NS_IMETHODIMP
PeerConnectionImpl::GetReadyState(PRUint32* state)
{
  *state = mReadyState;
  return NS_OK;
}

NS_IMETHODIMP
PeerConnectionImpl::GetSipccState(PRUint32* state)
{
  PeerConnectionCtx* pcctx = PeerConnectionCtx::GetInstance();
  *state = pcctx ? pcctx->sipcc_state() : kIdle;
  return NS_OK;
}

NS_IMETHODIMP
PeerConnectionImpl::GetIceState(PRUint32* state)
{
  *state = mIceState;
  return NS_OK;
}

NS_IMETHODIMP
PeerConnectionImpl::Close()
{
  mCall->endCall();
  return NS_OK;
}

void
PeerConnectionImpl::Shutdown()
{
  PeerConnectionCtx::Destroy();
}

void
PeerConnectionImpl::onCallEvent(ccapi_call_event_e callEvent,
  CSF::CC_CallPtr call, CSF::CC_CallInfoPtr info)
{
  cc_call_state_t state = info->getCallState();
  std::string statestr = info->callStateToString(state);
  std::string eventstr = info->callEventToString(callEvent);

  if (CCAPI_CALL_EV_CREATED != callEvent && CCAPI_CALL_EV_STATE != callEvent) {
    CSFLogDebugS(logTag, ": **** CALL HANDLE IS: " << mHandle <<
      ": **** CALL STATE IS: " << statestr);
    return;
  }

  std::string s_sdpstr;
  StatusCode code;
  MediaStreamTable* stream;

  switch (state) {
    case CREATEOFFER:
      s_sdpstr = info->getSDP();
      if (mPCObserver) {
        mPCObserver->OnCreateOfferSuccess(s_sdpstr.c_str());
      }
      break;

    case CREATEANSWER:
      s_sdpstr = info->getSDP();
      if (mPCObserver) {
        mPCObserver->OnCreateAnswerSuccess(s_sdpstr.c_str());
      }
      break;

    case CREATEOFFERERROR:
      code = (StatusCode)info->getStatusCode();
      if (mPCObserver) {
        mPCObserver->OnCreateOfferError(code);
      }
      break;

    case CREATEANSWERERROR:
      code = (StatusCode)info->getStatusCode();
      if (mPCObserver) {
        mPCObserver->OnCreateAnswerError(code);
      }
      break;

    case SETLOCALDESC:
      mLocalSDP = mLocalRequestedSDP;
      code = (StatusCode)info->getStatusCode();
      if (mPCObserver) {
        mPCObserver->OnSetLocalDescriptionSuccess(code);
      }
      break;

    case SETREMOTEDESC:
      mRemoteSDP = mRemoteRequestedSDP;
      code = (StatusCode)info->getStatusCode();
      if (mPCObserver) {
        mPCObserver->OnSetRemoteDescriptionSuccess(code);
      }
      break;

    case SETLOCALDESCERROR:
      code = (StatusCode)info->getStatusCode();
      if (mPCObserver) {
        mPCObserver->OnSetLocalDescriptionError(code);
      }
      break;

    case SETREMOTEDESCERROR:
      code = (StatusCode)info->getStatusCode();
      if (mPCObserver) {
        mPCObserver->OnSetRemoteDescriptionError(code);
      }
      break;

    case REMOTESTREAMADD:
      stream = info->getMediaStreams();
      if (mPCObserver) {
    	  nsRefPtr<nsDOMMediaStream> mMediaStream;
    	  // <emannion> can someone update the IDL for OnAddStream
    	  //            and create the MediaStream to pass up
    	  // next two lines show how to get data.
    	  // this will be vastly improved soon
    	  unsigned int sid = stream->media_stream_id;
    	  unsigned int tid = stream->track[0].media_stream_track_id;

          // mPCObserver->OnAddStream(mMediaStream);
      }
      break;

    default:
      CSFLogDebugS(logTag, ": **** CALL HANDLE IS: " << mHandle <<
        ": **** CALL STATE IS: " << statestr);
      break;
  }
}

void
PeerConnectionImpl::ChangeReadyState(PeerConnectionImpl::ReadyState ready_state) {
  mReadyState = ready_state;
  if (mPCObserver) {
    mPCObserver->OnStateChange(IPeerConnectionObserver::kReadyState);
  }
}

PeerConnectionWrapper *PeerConnectionImpl::AcquireInstance(const std::string& handle) {
  if (peerconnections.find(handle) == peerconnections.end()) {
    return NULL;
  }

  PeerConnectionImpl *impl = peerconnections[handle];
  impl->AddRef();

  return new PeerConnectionWrapper(impl);
}

void
PeerConnectionImpl::ReleaseInstance()
{
  Release();
}

const std::string&
PeerConnectionImpl::GetHandle() {
  return mHandle;
}

void
PeerConnectionImpl::IceGatheringCompleted(NrIceCtx *ctx) {
  CSFLogDebug(logTag, "ICE gathering complete");
  mIceState = kIceWaiting;
  if (mPCObserver) {
    mPCObserver->OnStateChange(IPeerConnectionObserver::kIceState);
  }
}

void
PeerConnectionImpl::IceCompleted(NrIceCtx *ctx) {
  CSFLogDebug(logTag, "ICE completed");
  mIceState = kIceConnected;
  if (mPCObserver) {
    mPCObserver->OnStateChange(IPeerConnectionObserver::kIceState);
  }
}

void
PeerConnectionImpl::IceStreamReady(NrIceMediaStream *stream) {
  CSFLogDebug(logTag, "ICE stream ready : %s", stream->name().c_str());
}

nsRefPtr<LocalSourceStreamInfo> PeerConnectionImpl::GetLocalStream(int index) {
  if (index >= mLocalSourceStreams.Length())
    return NULL;
  
  PR_ASSERT(mLocalSourceStreams[index]);
  return mLocalSourceStreams[index];
}

void LocalSourceStreamInfo::StorePipeline(int track,
  mozilla::RefPtr<mozilla::MediaPipeline> pipeline) {
  mPipelines[track] = pipeline;
}

}  // end sipcc namespace
