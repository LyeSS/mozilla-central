/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include <string>
#include <iostream>

#include "CSFLog.h"
#include "CSFLogStream.h"
#include "ccapi_call_info.h"
#include "CC_SIPCCCallInfo.h"
#include "ccapi_device_info.h"
#include "CC_SIPCCDeviceInfo.h"
#include "vcm.h"
#include "PeerConnection.h"
#include "PeerConnectionCtx.h"
#include "PeerConnectionImpl.h"
#include "nsThreadUtils.h"
#include "runnable_utils.h"

#ifndef USE_FAKE_MEDIA_STREAMS
#include "MediaSegment.h"
#endif

static const char* logTag = "PeerConnectionImpl";

namespace sipcc {

// LocalSourceStreamInfo
LocalSourceStreamInfo::LocalSourceStreamInfo(nsRefPtr<nsDOMMediaStream>& aMediaStream) :
  mMediaStream(aMediaStream)  
{  
}
  
LocalSourceStreamInfo:: ~LocalSourceStreamInfo()
{
}

// We get this callback in order to find out which tracks are audio and which are video
// We should get this callback right away for existing streams after we add this class 
// as a listener.
void LocalSourceStreamInfo::NotifyQueuedTrackChanges(
  mozilla::MediaStreamGraph* aGraph, 
  mozilla::TrackID aID,
  mozilla::TrackRate aTrackRate,
  mozilla::TrackTicks aTrackOffset,
  PRUint32 aTrackEvents,
  const mozilla::MediaSegment& aQueuedMedia) 
{
  // Add the track ID to the list for audio/video so they can be counted when
  // createOffer/createAnswer is called.  
  // This tells us whether we have the camera, mic, or both for example.
  mozilla::MediaSegment::Type trackType = aQueuedMedia.GetType();
  
  if (trackType == mozilla::MediaSegment::AUDIO)
  {
    // Should have very few tracks so not a hashtable
    bool found = false;
    for (unsigned u = 0; u < mAudioTracks.Length(); u++)
    {
      if (aID == mAudioTracks.ElementAt(u))
      {
        found = true;
        break;
      }
    }

    if (!found)
    {
      mAudioTracks.AppendElement(aID);
    }
  }
  else if (trackType == mozilla::MediaSegment::VIDEO)
  {
    // Should have very few tracks so not a hashtable
    bool found = false;
    for (unsigned u = 0; u < mVideoTracks.Length(); u++)
    {
      if (aID == mVideoTracks.ElementAt(u))
      {
        found = true;
        break;
      }
    }

    if (!found)
    {
      mVideoTracks.AppendElement(aID);
    }
  }
  else
  {
    CSFLogError(logTag, "NotifyQueuedTrackChanges - unknown media type");
  }
}

nsRefPtr<nsDOMMediaStream> LocalSourceStreamInfo::GetMediaStream()
{
  return mMediaStream;
}

// If the ExpectAudio hint is on we will add a track at the default first
// audio track ID (0)
// FIX - Do we need to iterate over the tracks instead of taking these expect hints?
void LocalSourceStreamInfo::ExpectAudio()
{
  mAudioTracks.AppendElement(0);
} 

// If the ExpectVideo hint is on we will add a track at the default first
// video track ID (1).
// FIX - Do we need to iterate over the tracks instead of taking these expect hints?
void LocalSourceStreamInfo::ExpectVideo()
{
  mVideoTracks.AppendElement(1);
}

unsigned LocalSourceStreamInfo::AudioTrackCount()
{
  return mAudioTracks.Length();  
}
  
unsigned LocalSourceStreamInfo::VideoTrackCount()
{
  return mVideoTracks.Length();
}
  


            
PeerConnectionInterface* PeerConnectionInterface::CreatePeerConnection() 
{
  PeerConnectionImpl *pc = new PeerConnectionImpl();
  return pc;
}
  
std::map<const std::string, PeerConnectionImpl *> 
   PeerConnectionImpl::peerconnections;

PeerConnectionImpl::PeerConnectionImpl() : 
  mCall(NULL), 
  mPCObserver(NULL), 
  mReadyState(kNew), 
  mLocalSourceStreamsLock(PR_NewLock()),
  mIceCtx(NULL),
  mIceStreams(NULL),
  mIceState(kIceGathering)
{
}

                         
PeerConnectionImpl::~PeerConnectionImpl() 
{
  peerconnections.erase(mHandle);
  Close();
  PR_DestroyLock(mLocalSourceStreamsLock);
}

StatusCode PeerConnectionImpl::Initialize(PeerConnectionObserver* observer) {
  if (!observer)
    return PC_NO_OBSERVER;	

  mPCObserver = observer;
    
  PeerConnectionCtx *pcctx = PeerConnectionCtx::GetInstance();
  if (!pcctx)
    return PC_INTERNAL_ERROR;
    
  mCall = pcctx->createCall();
  if (!mCall.get())
    return PC_INTERNAL_ERROR;
    
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
  mIceCtx->thread()->Dispatch(WrapRunnableRet(mIceCtx, 
      &NrIceCtx::StartGathering, &res), NS_DISPATCH_SYNC);
  PR_ASSERT(NS_SUCCEEDED(res));
    
  // Store under mHandle
  mCall->setPeerConnection(mHandle);
  peerconnections[mHandle] = this;
   
  return PC_OK;
}

/*
 * CC_SDP_DIRECTION_SENDRECV will not be used when Constraints are implemented
 */
StatusCode PeerConnectionImpl::CreateOffer(const std::string& hints) {
  mCall->createOffer(hints);
  return PC_OK;
}

StatusCode PeerConnectionImpl::CreateAnswer(const std::string& hints, const  std::string& offer) {
  mCall->createAnswer(hints, offer);
  return PC_OK;
}

StatusCode PeerConnectionImpl::SetLocalDescription(Action action, const  std::string& sdp) {
  mLocalRequestedSDP = sdp;
  mCall->setLocalDescription((cc_jsep_action_t)action, sdp);
  return PC_OK;
}

StatusCode PeerConnectionImpl::SetRemoteDescription(Action action, const std::string& sdp) {
  mRemoteRequestedSDP = sdp;
  mCall->setRemoteDescription((cc_jsep_action_t)action, sdp);
  return PC_OK;
} 

const std::string& PeerConnectionImpl::localDescription() const {
  return mLocalSDP;
}

const std::string& PeerConnectionImpl::remoteDescription() const {
  return mRemoteSDP;
}

void PeerConnectionImpl::AddStream(nsRefPtr<nsDOMMediaStream>& aMediaStream)
{
  CSFLogDebug(logTag, "AddStream");

  // TODO(ekr@rtfm.com): Remove these asserts?
  // Adding tracks here based on nsDOMMediaStream expectation settings
  PRUint32 hints = aMediaStream->GetHintContents();

  if (!(hints & (nsDOMMediaStream::HINT_CONTENTS_AUDIO |
        nsDOMMediaStream::HINT_CONTENTS_VIDEO))) {
    CSFLogError(logTag, "Stream must contain either audio or video");
    return;
  }

  // Now see if we already have a stream of this type, since we only 
  // allow one of each. 
  // TODO(ekr@rtfm.com): remove this when multiple of each stream 
  // is allowed
  PR_Lock(mLocalSourceStreamsLock);
  for (unsigned u = 0; u < mLocalSourceStreams.Length(); u++)
  {
    nsRefPtr<LocalSourceStreamInfo> localSourceStream = mLocalSourceStreams[u];
    
    if (localSourceStream->GetMediaStream()->GetHintContents() & hints) {
      CSFLogError(logTag, "Only one stream of any given type allowed");
      PR_Unlock(mLocalSourceStreamsLock);
      PR_ASSERT(PR_FALSE);
      return;
    }
  }

  // OK, we're good to add
  nsRefPtr<LocalSourceStreamInfo> localSourceStream = new LocalSourceStreamInfo(aMediaStream);
  cc_media_track_id_t track_id = mLocalSourceStreams.Length();

  if (hints & nsDOMMediaStream::HINT_CONTENTS_AUDIO)
  {
    localSourceStream->ExpectAudio();
    mCall->addStream(0, track_id, AUDIO);
  }

  if (hints & nsDOMMediaStream::HINT_CONTENTS_VIDEO)
  {
    localSourceStream->ExpectVideo();
    mCall->addStream(0, track_id, VIDEO);
  }

  // Make it the listener for info from the MediaStream and add it to the list
  mozilla::MediaStream *plainMediaStream = aMediaStream->GetStream();

  if (plainMediaStream)
  {
    plainMediaStream->AddListener(localSourceStream);
  }

  mLocalSourceStreams.AppendElement(localSourceStream);
  PR_Unlock(mLocalSourceStreamsLock);
}
  
void PeerConnectionImpl::RemoveStream(nsRefPtr<nsDOMMediaStream>& aMediaStream)
{
  CSFLogDebug(logTag, "RemoveStream");

  PR_Lock(mLocalSourceStreamsLock);
  for (unsigned u = 0; u < mLocalSourceStreams.Length(); u++)
  {
    nsRefPtr<LocalSourceStreamInfo> localSourceStream = mLocalSourceStreams[u];
    if (localSourceStream->GetMediaStream() == aMediaStream)
    {
      PRUint32 hints = aMediaStream->GetHintContents();
      if (hints & nsDOMMediaStream::HINT_CONTENTS_AUDIO)
      {
        // <emannion>  This API will change when we implement multiple streams
        //             It will only need the ID
        mCall->removeStream(u, 0, AUDIO);
      }
      if (hints & nsDOMMediaStream::HINT_CONTENTS_VIDEO)
      {
        mCall->removeStream(u, 1, VIDEO);
      }
      break;
    }    
  }
  PR_Unlock(mLocalSourceStreamsLock);
}
  
void PeerConnectionImpl::CloseStreams() {

  if (mReadyState != PeerConnectionInterface::kClosed)  {
    ChangeReadyState(PeerConnectionInterface::kClosing);
  }
  
  mCall->endCall();
}


void PeerConnectionImpl::AddIceCandidate(const std::string& strCandidate)
{
  mCall->addIceCandidate(strCandidate);
}

PeerConnectionInterface::ReadyState PeerConnectionImpl::ready_state() {
  return mReadyState;
}

PeerConnectionInterface::SipccState PeerConnectionImpl::sipcc_state() {
  PeerConnectionCtx* pcctx = PeerConnectionCtx::GetInstance();
  return pcctx ? pcctx->sipcc_state() : kIdle;
}

void PeerConnectionImpl::Close() {
  mCall->endCall();
}

void PeerConnectionImpl::Shutdown() {
  PeerConnectionCtx::Destroy();
}

void PeerConnectionImpl::onCallEvent(ccapi_call_event_e callEvent, CSF::CC_CallPtr call, CSF::CC_CallInfoPtr info)  {
  cc_call_state_t state = info->getCallState();
  std::string statestr = info->callStateToString(state);
  std::string eventstr = info->callEventToString(callEvent);

  if(CCAPI_CALL_EV_CREATED == callEvent || CCAPI_CALL_EV_STATE == callEvent) {
    std::string sdpstr;
    StatusCode code;
    MediaTrackTable* stream;

    switch (state) {
      case CREATEOFFER:
        sdpstr = info->getSDP();
        if (mPCObserver)
          mPCObserver->OnCreateOfferSuccess(sdpstr);
        break;

      case CREATEANSWER:
        sdpstr = info->getSDP();
        if (mPCObserver)
          mPCObserver->OnCreateAnswerSuccess(sdpstr);
        break;

      case CREATEOFFERERROR:
        code = (StatusCode)info->getStatusCode();
        if (mPCObserver)
          mPCObserver->OnCreateOfferError(code);
        break;

      case CREATEANSWERERROR:
        code = (StatusCode)info->getStatusCode();
        if (mPCObserver)
          mPCObserver->OnCreateAnswerError(code);
        break;
        
      case SETLOCALDESC:
        mLocalSDP = mLocalRequestedSDP;
        code = (StatusCode)info->getStatusCode();
        if (mPCObserver)
          mPCObserver->OnSetLocalDescriptionSuccess(code);
        break;

      case SETREMOTEDESC:
        mRemoteSDP = mRemoteRequestedSDP;
        code = (StatusCode)info->getStatusCode();
        if (mPCObserver)
          mPCObserver->OnSetRemoteDescriptionSuccess(code);
        break;
        
      case SETLOCALDESCERROR:
        code = (StatusCode)info->getStatusCode();
        if (mPCObserver)
          mPCObserver->OnSetLocalDescriptionError(code);
        break;    

      case SETREMOTEDESCERROR:
        code = (StatusCode)info->getStatusCode();
        if (mPCObserver)
          mPCObserver->OnSetRemoteDescriptionError(code);
        break;
        
      case REMOTESTREAMADD:
        stream = info->getMediaTracks();
        if (mPCObserver)
          mPCObserver->OnAddStream(stream);
        break;

      default:
    	CSFLogDebugS(logTag, ": **** CALL HANDLE IS: " << mHandle << ": **** CALL STATE IS: " << statestr);
        break;
    }
  } else {
	  CSFLogDebugS(logTag, ": **** CALL HANDLE IS: " << mHandle << ": **** CALL STATE IS: " << statestr);
  }
}

void PeerConnectionImpl::ChangeReadyState(PeerConnectionInterface::ReadyState ready_state) {
  mReadyState = ready_state;
  if (mPCObserver)
    mPCObserver->OnStateChange(PeerConnectionObserver::kReadyState);
}

PeerConnectionWrapper *PeerConnectionImpl::AcquireInstance(const std::string& handle) {
  if (peerconnections.find(handle) == peerconnections.end())
    return NULL;
  
  PeerConnectionImpl *impl = peerconnections[handle];
  impl->AddRef();

  return new PeerConnectionWrapper(impl);
}

void PeerConnectionImpl::ReleaseInstance() {
  Release();
}
 
const std::string& PeerConnectionImpl::GetHandle() {
  return mHandle;
}

void PeerConnectionImpl::IceGatheringCompleted(NrIceCtx *ctx) {
  CSFLogDebug(logTag, "ICE gathering complete");
  mIceState = kIceWaiting;
  if (mPCObserver) {
    mPCObserver->OnStateChange(PeerConnectionObserver::kIceState);
  }
}

void PeerConnectionImpl::IceCompleted(NrIceCtx *ctx) {
  CSFLogDebug(logTag, "ICE completed");
  mIceState = kIceConnected;
  if (mPCObserver) {
    mPCObserver->OnStateChange(PeerConnectionObserver::kIceState);
  }
}

void PeerConnectionImpl::IceStreamReady(NrIceMediaStream *stream) {
  CSFLogDebug(logTag, "ICE stream ready : %s", stream->name().c_str());
}

PeerConnectionInterface::IceState PeerConnectionImpl::ice_state() {
  return mIceState;
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
