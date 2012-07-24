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
 * The Original Code is the Cisco Systems SIP Stack.
 *
 * The Initial Developer of the Original Code is
 * Cisco Systems (CSCO).
 * Portions created by the Initial Developer are Copyright (C) 2002
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *  Enda Mannion <emannion@cisco.com>
 *  Suhas Nandakumar <snandaku@cisco.com>
 *  Ethan Hugg <ehugg@cisco.com>
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


#include <iostream>
#include <string>
using namespace std;

#include "base/basictypes.h"
#include "nsStaticComponents.h"

#define GTEST_HAS_RTTI 0
#include "gtest/gtest.h"
#include "gtest_utils.h"

#include "nspr.h"
#include "nss.h"
#include "ssl.h"
#include "prthread.h"

#include "FakeMediaStreams.h"
#include "FakeMediaStreamsImpl.h"
#include "PeerConnectionImpl.h"
#include "runnable_utils.h"

#include "mtransport_test_utils.h"
MtransportTestUtils test_utils;

static int kDefaultTimeout = 1000;
namespace {

static const std::string strSampleSdpAudioVideoNoIce =  
  "v=0\r\n" 
  "o=Cisco-SIPUA 4949 0 IN IP4 10.86.255.143\r\n"
  "s=SIP Call\r\n"
  "t=0 0\r\n"
  "a=ice-ufrag:qkEP\r\n"
  "a=ice-pwd:ed6f9GuHjLcoCN6sC/Eh7fVl\r\n"
  "m=audio 16384 RTP/AVP 0 8 9 101\r\n"
  "c=IN IP4 10.86.255.143\r\n"
  "a=rtpmap:0 PCMU/8000\r\n"
  "a=rtpmap:8 PCMA/8000\r\n"
  "a=rtpmap:9 G722/8000\r\n"
  "a=rtpmap:101 telephone-event/8000\r\n"
  "a=fmtp:101 0-15\r\n"
  "a=sendrecv\r\n"
  "a=candidate:1 1 UDP 2130706431 192.168.2.1 50005 typ host\r\n"
  "a=candidate:2 2 UDP 2130706431 192.168.2.2 50006 typ host\r\n"
  "m=video 1024 RTP/AVP 97\r\n"
  "c=IN IP4 10.86.255.143\r\n"
  "a=rtpmap:97 H264/90000\r\n"
  "a=fmtp:97 profile-level-id=42E00C\r\n"
  "a=sendrecv\r\n"
  "a=candidate:1 1 UDP 2130706431 192.168.2.3 50007 typ host\r\n"
  "a=candidate:2 2 UDP 2130706431 192.168.2.4 50008 typ host\r\n";


class TestObserver : public sipcc::PeerConnectionObserver
{
public:
   
  TestObserver(sipcc::PeerConnectionInterface *peerConnection) :
    state(stateNoResponse),
    onAddStreamCalled(false),
    pc(peerConnection) {
  }

  virtual ~TestObserver() {}

  // PeerConnectionObserver
  void OnCreateOfferSuccess(const std::string& offer) 
  {
    state = stateSuccess;
    cout << "onCreateOfferSuccess = " << offer << endl;
    lastString = offer;
  }

  void OnCreateOfferError(StatusCode code) 
  {
    state = stateError;
    cout << "onCreateOfferError" << endl;
    lastStatusCode = code;
  }

  void OnCreateAnswerSuccess(const std::string& answer) 
  {
    state = stateSuccess;
    cout << "onCreateAnswerSuccess = " << answer << endl;
    lastString = answer;
  }

  void OnCreateAnswerError(StatusCode code) 
  {
    state = stateError;
    lastStatusCode = code;
  }

  void OnSetLocalDescriptionSuccess(StatusCode code)
  {
    state = stateSuccess;
    lastStatusCode = code;
  }

  void OnSetRemoteDescriptionSuccess(StatusCode code)
  { 
    state = stateSuccess;
    lastStatusCode = code;
  }

  void OnSetLocalDescriptionError(StatusCode code) 
  {
    state = stateError;
    lastStatusCode = code;
  }

  void OnSetRemoteDescriptionError(StatusCode code) 
  {
    state = stateError;
    lastStatusCode = code;
  }

  void OnStateChange(StateType state_type) 
  {
    switch (state_type)
    {
    case kReadyState:
      cout << "Ready State: " << pc->ready_state() << endl;
      break;
    case kIceState:
      cout << "ICE State: " << pc->ice_state() << endl;
      break;
    case kSdpState:
      cout << "SDP State: " << endl;
      break;
    case kSipccState:
      cout << "SIPCC State: " << pc->sipcc_state() << endl;
      break;
    default:
       // Unknown State
       ASSERT_TRUE(false);
    }
    state = stateSuccess;
    lastStateType = state_type;
  }
  
  void OnAddStream(MediaTrackTable* stream)
  {
    state = stateSuccess;
    onAddStreamCalled = true;
  }
  
  void OnRemoveStream()
  {
    state = stateSuccess;
  }
  
  void OnAddTrack()
  {
    state = stateSuccess;
  }
  
  void OnRemoveTrack()
  {
    state = stateSuccess;
  }
  
  void FoundIceCandidate(const std::string& strCandidate)
  {
  }

public:
  enum ResponseState 
  {
    stateNoResponse,
    stateSuccess,
    stateError
  };
  
  ResponseState state;
  std::string lastString;
  StatusCode lastStatusCode;
  StateType lastStateType;
  bool onAddStreamCalled;
  
private:
  sipcc::PeerConnectionInterface *pc;
};


class SignalingAgent {
 public:
  SignalingAgent() {
    Init();
  }
  
  ~SignalingAgent() {
    Close();
  }

  void Init()
  {
    size_t found = 2;
    ASSERT_TRUE(found > 0);

    pc = sipcc::PeerConnectionInterface::CreatePeerConnection();
    ASSERT_TRUE(pc);

    pObserver = new TestObserver(pc);
    ASSERT_TRUE(pObserver);

    ASSERT_EQ(pc->Initialize(pObserver), PC_OK);
    ASSERT_TRUE_WAIT(pc->sipcc_state() == sipcc::PeerConnectionInterface::kStarted,
                     kDefaultTimeout);
    ASSERT_TRUE_WAIT(pc->ice_state() == sipcc::PeerConnectionInterface::kIceWaiting, 5000);
    cout << "Init Complete" << endl;

  }

  void Close()
  {
    cout << "Close" << endl;
    pc->Close();
    // Shutdown is synchronous evidently.
    // ASSERT_TRUE(pObserver->WaitForObserverCall());
    // ASSERT_EQ(pc->sipcc_state(), sipcc::PeerConnectionInterface::kIdle);

    delete pObserver;
  }

  const std::string offer() const { return offer_; }
  const std::string answer() const { return answer_; }

  void CreateOffer(const std::string hints, bool audio, bool video) {

    // Create a media stream as if it came from GUM
    mozilla::RefPtr<Fake_AudioStreamSource> audio_stream = 
      new Fake_AudioStreamSource();

    nsresult ret;
    test_utils.sts_target()->Dispatch(
      WrapRunnableRet(audio_stream, &Fake_MediaStream::Start, &ret),
        NS_DISPATCH_SYNC);

    ASSERT_TRUE(NS_SUCCEEDED(ret));

    
    // store in object to be used by RemoveStream
    nsRefPtr<nsDOMMediaStream> domMediaStream = new nsDOMMediaStream(audio_stream);
    domMediaStream_ = domMediaStream;


    PRUint32 aHintContents = 0;
    
    if (audio)
      aHintContents |= nsDOMMediaStream::HINT_CONTENTS_AUDIO;
    if (video)
      aHintContents |= nsDOMMediaStream::HINT_CONTENTS_VIDEO;
    
    PR_ASSERT(aHintContents);

    domMediaStream->SetHintContents(aHintContents);
      
    pc->AddStream(domMediaStream);

    // Now call CreateOffer as JS would
    pObserver->state = TestObserver::stateNoResponse;
    ASSERT_EQ(pc->CreateOffer(hints), PC_OK);
    ASSERT_TRUE_WAIT(pObserver->state == TestObserver::stateSuccess, kDefaultTimeout);
    SDPSanityCheck(pObserver->lastString, audio, video);
    offer_ = pObserver->lastString;
  }

  void CreateOfferExpectError(const std::string hints) {
    std::string strHints(hints);
    ASSERT_EQ(pc->CreateOffer(strHints), PC_OK);
    ASSERT_TRUE_WAIT(pObserver->state == TestObserver::stateError, kDefaultTimeout);
  }

  void CreateAnswer(const std::string hints, const std::string offer) {
    // Create a media stream as if it came from GUM
	nsRefPtr<nsDOMMediaStream> domMediaStream = new nsDOMMediaStream();
	// Pretend GUM got both audio and video.
	domMediaStream->SetHintContents(nsDOMMediaStream::HINT_CONTENTS_AUDIO | nsDOMMediaStream::HINT_CONTENTS_VIDEO);

	pc->AddStream(domMediaStream);

    pObserver->state = TestObserver::stateNoResponse;
    ASSERT_EQ(pc->CreateAnswer(hints, offer), PC_OK);
    ASSERT_TRUE_WAIT(pObserver->state == TestObserver::stateSuccess, kDefaultTimeout);
    SDPSanityCheck(pObserver->lastString, true, true);
    answer_ = pObserver->lastString;
  }

  void CreateOfferRemoveStream(const std::string hints, bool audio, bool video) {

    PRUint32 aHintContents = 0;

	if (!audio)
      aHintContents |= nsDOMMediaStream::HINT_CONTENTS_VIDEO;
    if (!video)
      aHintContents |= nsDOMMediaStream::HINT_CONTENTS_AUDIO;

	domMediaStream_->SetHintContents(aHintContents);

	// When complete RemoveStream will remove and entire stream and its tracks
	// not just disable a track as this is currently doing
    pc->RemoveStream(domMediaStream_);

    // Now call CreateOffer as JS would
    pObserver->state = TestObserver::stateNoResponse;
    ASSERT_EQ(pc->CreateOffer(hints), PC_OK);
    ASSERT_TRUE_WAIT(pObserver->state == TestObserver::stateSuccess, kDefaultTimeout);
    SDPSanityCheck(pObserver->lastString, video, audio);
    offer_ = pObserver->lastString;
  }

  void SetRemote(sipcc::Action action, std::string remote) {
    pObserver->state = TestObserver::stateNoResponse;    
    ASSERT_EQ(pc->SetRemoteDescription(action, remote), PC_OK);
    ASSERT_TRUE_WAIT(pObserver->state == TestObserver::stateSuccess, kDefaultTimeout);
  }

  void SetLocal(sipcc::Action action, std::string local) {
    pObserver->state = TestObserver::stateNoResponse;    
    ASSERT_EQ(pc->SetLocalDescription(action, local), PC_OK);
    ASSERT_TRUE_WAIT(pObserver->state == TestObserver::stateSuccess, kDefaultTimeout);
  }

  bool IceCompleted() {
    return pc->ice_state() == sipcc::PeerConnectionInterface::kIceConnected;
  }

#if 0
  void CreateOfferSetLocal(const char* hints) {
      CreateOffer(hints);

      pObserver->state = TestObserver::stateNoResponse;
      ASSERT_EQ(pc->SetLocalDescription(sipcc::OFFER, pObserver->lastString), PC_OK);
      ASSERT_TRUE(pObserver->WaitForObserverCall());
      ASSERT_EQ(pObserver->state, TestObserver::stateSuccess);
      ASSERT_EQ(pc->SetRemoteDescription(sipcc::OFFER, strSampleSdpAudioVideoNoIce), PC_OK);
      ASSERT_TRUE(pObserver->WaitForObserverCall());
      ASSERT_EQ(pObserver->state, TestObserver::stateSuccess);
    }

    void CreateAnswer(const char* hints)
    {
      std::string offer = strSampleSdpAudioVideoNoIce;
      std::string strHints(hints);

      ASSERT_EQ(pc->CreateAnswer(strHints, offer), PC_OK);
      ASSERT_TRUE(pObserver->WaitForObserverCall());
      ASSERT_EQ(pObserver->state, TestObserver::stateSuccess);
      SDPSanityCheck(pObserver->lastString, true, true);
    }
#endif

public:
  mozilla::RefPtr<sipcc::PeerConnectionInterface> pc;
  TestObserver *pObserver;
  std::string offer_;
  std::string answer_;
  nsRefPtr<nsDOMMediaStream> domMediaStream_;

  
private:
  void SDPSanityCheck(const std::string& sdp, bool shouldHaveAudio, bool shouldHaveVideo)
  {
    ASSERT_NE(sdp.find("v=0"), std::string::npos);
    ASSERT_NE(sdp.find("c=IN IP4"), std::string::npos);
    
    if (shouldHaveAudio)
    {
      ASSERT_NE(sdp.find("a=rtpmap:0 PCMU/8000"), std::string::npos);
    }
    
    if (shouldHaveVideo)
    {
      ASSERT_NE(sdp.find("a=rtpmap:97 H264/90000"), std::string::npos);
    }
  }
};

class SignalingEnvironment : public ::testing::Environment {
 public:
  void TearDown() {
    sipcc::PeerConnectionImpl::Shutdown();
  }
};

class SignalingTest : public ::testing::Test {
 public:
  void CreateOffer(std::string hints) {
    a1_.CreateOffer(hints, true, true);
  }

  void CreateSetOffer(std::string hints) {
    a1_.CreateOffer(hints, true, true);
    a1_.SetLocal(sipcc::OFFER, a1_.offer());
  }

  void OfferAnswer(std::string ahints, std::string bhints) {
    a1_.CreateOffer(ahints, true, true);
    a1_.SetLocal(sipcc::OFFER, a1_.offer());
    a2_.SetRemote(sipcc::OFFER, a1_.offer());
    a2_.CreateAnswer(bhints, a1_.offer());
    a2_.SetLocal(sipcc::ANSWER, a2_.answer());
    a1_.SetRemote(sipcc::ANSWER, a2_.answer());
    ASSERT_TRUE_WAIT(a1_.IceCompleted() == true, 10000);
    ASSERT_TRUE_WAIT(a2_.IceCompleted() == true, 10000);
  }

  void CreateOfferVideoOnly(std::string hints) {
    a1_.CreateOffer(hints, false, true);
  }

  void CreateOfferAudioOnly(std::string hints) {
    a1_.CreateOffer(hints, true, false);
  }

  void CreateOfferRemoveStream(std::string hints) {
	a1_.CreateOffer(hints, true, true);
    a1_.CreateOfferRemoveStream(hints, false, true);
  }

 private:
  SignalingAgent a1_;  // Canonically "caller"
  SignalingAgent a2_;  // Canonically "callee"
};


TEST_F(SignalingTest, DISABLED_JustInit)
{
}

TEST_F(SignalingTest, DISABLED_CreateOfferNoHints)
{
  CreateOffer("");
}

TEST_F(SignalingTest, DISABLED_CreateSetOffer)
{
  CreateSetOffer("");
}

TEST_F(SignalingTest, DISABLED_CreateOfferVideoOnly)
{
  CreateOfferVideoOnly("");
}

TEST_F(SignalingTest, DISABLED_CreateOfferAudioOnly)
{
  CreateOfferAudioOnly("");
}

TEST_F(SignalingTest, DISABLED_CreateOfferRemoveStream)
{
	CreateOfferRemoveStream("");
}

TEST_F(SignalingTest, DISABLED_OfferAnswer)
{
  OfferAnswer("", "");
}

TEST_F(SignalingTest, FullCall)
{
  OfferAnswer("", "");
  ASSERT_TRUE_WAIT(false, 10000);
}

//TEST_F(SignalingTest, CreateOfferHints)
//{
//  CreateOffer("audio,video");
//}

//TEST_F(SignalingTest, CreateOfferBadHints)
//{
//  CreateOfferExpectError("9.uoeuhaoensthuaeugc.pdu8g");
//}

//TEST_F(SignalingTest, CreateOfferSetLocal)
//{
//  CreateOfferSetLocal("");
//}

//TEST_F(SignalingTest, CreateAnswerNoHints)
//{
//  CreateAnswer("");
//}


} // End Namespace

int main(int argc, char **argv)
{
  test_utils.InitServices();
  NSS_NoDB_Init(NULL);
  NSS_SetDomesticPolicy();

  ::testing::AddGlobalTestEnvironment(new SignalingEnvironment);
  ::testing::InitGoogleTest(&argc, argv);

  for(int i=0; i<argc; i++) {
    if (!strcmp(argv[i],"-t")) {
      kDefaultTimeout = 20000;
    }

  }

  int result = RUN_ALL_TESTS();

  return result;
}


