/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

// Original author: ekr@rtfm.com

#ifndef transportlayerdtls_h__
#define transportlayerdtls_h__

#include <queue>

#include <talk/base/sigslot.h>

#include "nspr.h"
#include "prio.h"

#include "transportflow.h"
#include "transportlayer.h"

class Packet;

class NSPRHelper {
 public:
  NSPRHelper(TransportLayer *output) :
      output_(output),
      input_() {}
  
  void PacketReceived(const void *data, PRInt32 len);
  PRInt32 Read(void *data, PRInt32 len);
  PRInt32 Write(const void *buf, PRInt32 length);

 private:
  TransportLayer *output_;
  std::queue<Packet *> input_;
};


class TransportLayerDtls : public TransportLayer {
public:
 TransportLayerDtls() :
     TransportLayer(DGRAM),
     identity_(NULL),
     role_(CLIENT),
     pr_fd_(NULL),
     ssl_fd_(NULL),
     helper_(NULL) {}
  virtual ~TransportLayerDtls() {}
  
  enum Role { CLIENT, SERVER};

  // DTLS-specific operations
  void SetRole(Role role) { role_ = role;}
  void SetIdentity(DtlsIdentity *identity) { identity_ = identity; }

  // Transport layer overrides.
  virtual void WasInserted();
  virtual TransportResult SendPacket(const unsigned char *data, size_t len);

  // Signals
  void StateChange(TransportLayer *layer, State state);
  void PacketReceived(TransportLayer* layer, const unsigned char *data,
      size_t len);

  // Return the layer id for this layer
  virtual const std::string& id() { return ID; }

 
  // A static version of the layer ID
  static std::string ID;
  
private:
  bool Setup();

  DtlsIdentity *identity_;
  Role role_;

  PRFileDesc *pr_fd_;
  PRFileDesc *ssl_fd_;
  NSPRHelper *helper_;
  static PRDescIdentity nspr_layer_identity;  // The NSPR layer identity
};


#endif
