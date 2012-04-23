/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

// Original author: ekr@rtfm.com
#include "logging.h"
#include "prlog.h"
#include "transportflow.h"
#include "transportlayer.h"

// Logging context
MLOG_INIT("mtransport");

void TransportLayer::Inserted(TransportFlow *flow, TransportLayer *downward) {
  flow_ = flow;
  downward_ = downward;

  MLOG(PR_LOG_DEBUG, LAYER_INFO << "Inserted: downward='" << 
    (downward ? downward->id(): "none") << "'");

  WasInserted();
}

void TransportLayer::SetState(State state) {
  if (state != state_) {
    MLOG(PR_LOG_DEBUG, LAYER_INFO << "state " << state_ << "->" << state);
    state_ = state;
    SignalStateChange(this, state);
  }
}
