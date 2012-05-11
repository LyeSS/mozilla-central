/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

// Original author: ekr@rtfm.com

#include <iostream>

// From libjingle.
#include <talk/base/sigslot.h>

#include "nsThreadUtils.h"
#include "nsXPCOM.h"

#include "transportflow.h"
#include "transportlayer.h"
#include "transportlayerdtls.h"
#include "transportlayerlog.h"
#include "transportlayerprsock.h"

#include "mtransport_test_utils.h"
#include "runnable_utils.h"

#define GTEST_HAS_RTTI 0
#include "gtest/gtest.h"
#include "gtest_utils.h"

MtransportTestUtils test_utils;

namespace {
class TransportTestPeer : public sigslot::has_slots<> {
 public:
  TransportTestPeer(nsCOMPtr<nsIEventTarget> target) : target_(target),
                                                       received_(0), flow_(), 
                                                       prsock_(new TransportLayerPrsock()),
                                                       dtls_(new TransportLayerDtls()),
                                                       logging_(new TransportLayerLogging()) {
  }

  void Connect(PRFileDesc *fd) {
    nsresult res;
    target_->Dispatch(WrapRunnable(prsock_, &TransportLayerPrsock::Import,
                                   fd, &res), NS_DISPATCH_SYNC);
    ASSERT_TRUE(NS_SUCCEEDED(res));
    flow_.PushLayer(prsock_);
    flow_.PushLayer(logging_);
    flow_.top()->SignalPacketReceived.connect(this, &TransportTestPeer::PacketReceived);
  }

  void SendPacket(const unsigned char* data, size_t len) {
    flow_.top()->SendPacket(data, len);
  }

  void PacketReceived(TransportLayer* flow, const unsigned char* data,
                      size_t len) {
    ++received_;
  }

  size_t received() { return received_; }

 private:
  nsCOMPtr<nsIEventTarget> target_;  
  size_t received_;
  TransportFlow flow_;
  TransportLayerPrsock *prsock_;
  TransportLayerDtls *dtls_;
  TransportLayerLogging *logging_;
};


class TransportTest : public ::testing::Test {
 public:
  TransportTest() {
    fds_[0] = NULL;
    fds_[1] = NULL;
  }

  ~TransportTest() {
    delete p1_;
    delete p2_;

    //    Can't detach these
    //    PR_Close(fds_[0]);  
    //    PR_Close(fds_[1]);
  }

  void SetUp() {
    nsresult rv;
    target_ = do_GetService(NS_SOCKETTRANSPORTSERVICE_CONTRACTID, &rv);
    ASSERT_TRUE(NS_SUCCEEDED(rv));

    p1_ = new TransportTestPeer(target_);
    p2_ = new TransportTestPeer(target_);
  }

  void Connect() {

    PRStatus status = PR_NewTCPSocketPair(fds_);
    ASSERT_EQ(status, PR_SUCCESS);

    PRSocketOptionData opt;
    opt.option = PR_SockOpt_Nonblocking;
    opt.value.non_blocking = PR_FALSE;
    status = PR_SetSocketOption(fds_[0], &opt);
    ASSERT_EQ(status, PR_SUCCESS);
    status = PR_SetSocketOption(fds_[1], &opt);
    ASSERT_EQ(status, PR_SUCCESS);    
    
    p1_->Connect(fds_[0]);
    p2_->Connect(fds_[1]);
  }

  void TransferTest(size_t count) {
    unsigned char buf[1000];
    
    for (size_t i= 0; i<count; ++i) {
      memset(buf, count & 0xff, sizeof(buf));
      p1_->SendPacket(buf, sizeof(buf));
    }
    
    std::cerr << "Received == " << p2_->received() << std::endl;
    ASSERT_TRUE_WAIT(count == p2_->received(), 5000);
  }

 private:
  PRFileDesc *fds_[2];
  TransportTestPeer *p1_;
  TransportTestPeer *p2_;
  nsCOMPtr<nsIEventTarget> target_;  
};


TEST_F(TransportTest, TestTransfer) {
  Connect();
  TransferTest(1);
}

}  // end namespace

int main(int argc, char **argv)
{
  test_utils.InitServices();

  // Start the tests
  ::testing::InitGoogleTest(&argc, argv);
  
  return RUN_ALL_TESTS();
}
