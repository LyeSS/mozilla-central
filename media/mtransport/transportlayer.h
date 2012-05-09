/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

// Original author: ekr@rtfm.com

#ifndef transportlayer_h__
#define transportlayer_h__

// Pull in sigslot from libjingle. If we remove libjingle, we will
// need to either bring that in separately or refactor this code
// to not use sigslot
#include <talk/base/sigslot.h>

class TransportFlow;

// Abstract base class for network transport layers.
class TransportLayer : public sigslot::has_slots<> {
 public:
  // The state of the transport flow
  enum State { INIT, CONNECTING, OPEN, CLOSED, ERROR };

  // Is this a stream or datagram flow
  enum Mode { STREAM, DGRAM };

  // Write errors
  enum WriteResult { ERR_WOULDBLOCK = -1, ERR_ERROR = -2, ERR_INTERNAL = -3 };

  // NULL constructor
  TransportLayer() : state_(INIT), flow_(NULL), downward_(NULL) {}
  virtual ~TransportLayer() {}

  // Inserted
  virtual void Inserted(TransportFlow *flow, TransportLayer *downward);

  // Get the state
  State state() const { return state_; }
  // Must be implemented by derived classes
  virtual int SendPacket(const unsigned char *data, size_t len) = 0;
  
  // Event definitions that one can register for
  // State has changed
  sigslot::signal2<TransportLayer*, State> SignalStateChange;
  // Data received on the flow
  sigslot::signal3<TransportLayer*, const unsigned char *, size_t>
                         SignalPacketReceived;
  
  // Return the layer id for this layer
  virtual const std::string& id() = 0;

  virtual const std::string& flow_id() { 
    static std::string empty;

    return flow_ ? flow_->id() : empty;
  }

 protected:
  // Called by Inserted
  virtual void WasInserted() {}
  virtual void SetState(State state);

  State state_;
  TransportFlow *flow_;  // The flow this is part of
  TransportLayer *downward_; // The next layer in the stack
};

#define LAYER_INFO "Flow[" << flow_id() << "(none)" << "]; Layer[" << id() << "]: "

#endif
