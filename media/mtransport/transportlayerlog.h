/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

// Original author: ekr@rtfm.com

#ifndef transportlayerlog_h__
#define transportlayerlog_h__

#include "m_cpp_utils.h"
#include "transportflow.h"
#include "transportlayer.h"

class TransportLayerLogging : public TransportLayer {
public:
  TransportLayerLogging() {}

  // Overrides for TransportLayer
  virtual TransportResult SendPacket(const unsigned char *data, size_t len);
  
  // Signals (forwarded to upper layer)
  void StateChange(TransportLayer *layer, State state);
  void PacketReceived(TransportLayer* layer, const unsigned char *data,
                      size_t len);

  // Return the layer id for this layer
  virtual const std::string& id() { return ID; }

  // A static version of the layer ID
  static std::string ID;

protected:
  virtual void WasInserted();

private:
  DISALLOW_COPY_ASSIGN(TransportLayerLogging);
};


#endif
