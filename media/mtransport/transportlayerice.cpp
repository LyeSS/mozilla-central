/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

// Original author: ekr@rtfm.com

// Some of this code is cut-and-pasted from nICEr. Copyright is:

/*
Copyright (c) 2007, Adobe Systems, Incorporated
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

* Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.

* Neither the name of Adobe Systems, Network Resonance nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include <string>
#include <queue>
#include <vector>

#include "nspr.h"
#include "nss.h"
#include "pk11pub.h"
#include "prlog.h"

#include "nsCOMPtr.h"
#include "nsComponentManagerUtils.h"
#include "nsError.h"
#include "nsIEventTarget.h"
#include "nsNetCID.h"
#include "nsComponentManagerUtils.h"
#include "nsServiceManagerUtils.h"

// nICEr includes
extern "C" {
#include "nr_api.h"
#include "registry.h"
#include "async_timer.h"
#include "ice_util.h"
#include "transport_addr.h"
#include "nr_crypto.h"
#include "nr_socket.h"
#include "nr_socket_local.h"
#include "stun_client_ctx.h"
#include "stun_server_ctx.h"
#include "ice_ctx.h"
#include "ice_candidate.h"
#include "ice_handler.h"
}

// Local includes
#include "logging.h"
#include "transportflow.h"
#include "transportlayerice.h"


MLOG_INIT("mtransport");

std::string TransportLayerIce::ID("mt_ice");
static bool initialized = false;


// Implement NSPR-based crypto algorithms
static int nr_crypto_nss_random_bytes(UCHAR *buf, int len) {
  SECStatus rv;

  rv = PK11_GenerateRandom(buf, len);
  if (rv != SECSuccess)
    return R_INTERNAL;

  return 0;
}

static int nr_crypto_nss_hmac(UCHAR *key, int keyl, UCHAR *buf, int bufl,
                              UCHAR *result) {
  CK_MECHANISM_TYPE mech = CKM_SHA_1_HMAC;
  PK11SlotInfo *slot = 0;
  SECItem keyi = { siBuffer, key, keyl};
  PK11SymKey *skey = 0;
  PK11Context *hmac_ctx = 0;
  SECStatus status;
  unsigned int hmac_len;
  SECItem param = { siBuffer, NULL, 0 };
  int err = R_INTERNAL;
  
  slot = PK11_GetInternalKeySlot();
  if (!slot)
    goto abort;

  skey = PK11_ImportSymKey(slot, mech, PK11_OriginUnwrap,
                          CKA_SIGN, &keyi, NULL);
  if (!skey)
    goto abort;

  
  hmac_ctx = PK11_CreateContextBySymKey(mech, CKA_SIGN,
                                        skey, &param);

  status = PK11_DigestBegin(hmac_ctx);
  if (status != SECSuccess)
    goto abort;

  status = PK11_DigestOp(hmac_ctx, buf, bufl);
  if (status != SECSuccess)
    goto abort;

  status = PK11_DigestFinal(hmac_ctx, result, &hmac_len, 20);
  if (status != SECSuccess)
    goto abort;
  
  PR_ASSERT(hmac_len == 20);
  
  err = 0;

 abort:
  if(hmac_ctx) PK11_DestroyContext(hmac_ctx, PR_TRUE);
  if (skey) PK11_FreeSymKey(skey);
  if (slot) PK11_FreeSlot(slot);

  return err;
}

static nr_ice_crypto_vtbl nr_ice_crypto_nss_vtbl = {
  nr_crypto_nss_random_bytes,
  nr_crypto_nss_hmac
};



// NrIceCtx
// Handler callbacks
static int select_pair(void *obj,nr_ice_media_stream *stream, 
                   int component_id, nr_ice_cand_pair **potentials,
                   int potential_ct) {
  MLOG(PR_LOG_DEBUG, "select pair called: potential_ct = " << potential_ct);

  return 0;
}

static int stream_ready(void *obj, nr_ice_media_stream *stream) {
  MLOG(PR_LOG_DEBUG, "stream_ready called");

  return 0;
}

static int stream_failed(void *obj, nr_ice_media_stream *stream) {
  MLOG(PR_LOG_DEBUG, "stream_failed called");
  return 0;
}

static int ice_completed(void *obj, nr_ice_peer_ctx *pctx) {
  MLOG(PR_LOG_DEBUG, "ice_completed called");

  return 0;
}
 
static int msg_recvd(void *obj, nr_ice_peer_ctx *pctx, nr_ice_media_stream *stream, int component_id, UCHAR *msg, int len) {
  return 0;
}

static nr_ice_handler_vtbl handler_vtbl = {
  select_pair,
  stream_ready,
  stream_failed,
  ice_completed,
  msg_recvd
};


mozilla::RefPtr<NrIceCtx> NrIceCtx::Create(const std::string& name,
                                           bool offerer) {
  mozilla::RefPtr<NrIceCtx> ctx = new NrIceCtx(name, offerer);

  // Initialize the crypto callbacks
  if (!initialized) {
    NR_reg_init(NR_REG_MODE_LOCAL);
    nr_crypto_vtbl = &nr_ice_crypto_nss_vtbl;
    initialized = true;
    
    // Set the priorites for candidate type preferences
    NR_reg_set_uchar((char *)"ice.pref.type.srv_rflx",100);
    NR_reg_set_uchar((char *)"ice.pref.type.peer_rflx",105);
    NR_reg_set_uchar((char *)"ice.pref.type.prflx",99);
    NR_reg_set_uchar((char *)"ice.pref.type.host",125);
    NR_reg_set_uchar((char *)"ice.pref.type.relayed",126);

    // Interface preferences: TODO(ekr@rtfm.com); this doesn't
    // scale to multiple addresses
    NR_reg_set_uchar((char *)"ice.pref.interface.rl0", 255);
    NR_reg_set_uchar((char *)"ice.pref.interface.wi0", 254);
    NR_reg_set_uchar((char *)"ice.pref.interface.lo0", 253);
    NR_reg_set_uchar((char *)"ice.pref.interface.en1", 252);
    NR_reg_set_uchar((char *)"ice.pref.interface.en0", 251);
    NR_reg_set_uchar((char *)"ice.pref.interface.ppp", 250);
    NR_reg_set_uchar((char *)"ice.pref.interface.ppp0", 249);
    NR_reg_set_uchar((char *)"ice.pref.interface.en2", 248);
    NR_reg_set_uchar((char *)"ice.pref.interface.en3", 247);
    NR_reg_set_uchar((char *)"ice.pref.interface.em0", 251);
    NR_reg_set_uchar((char *)"ice.pref.interface.em1", 252);
    NR_reg_set_uchar((char *)"ice.pref.interface.vmnet0", 240);
    NR_reg_set_uchar((char *)"ice.pref.interface.vmnet1", 241);
    NR_reg_set_uchar((char *)"ice.pref.interface.vmnet3", 239);
    NR_reg_set_uchar((char *)"ice.pref.interface.vmnet4", 238);
    NR_reg_set_uchar((char *)"ice.pref.interface.vmnet5", 237);
    NR_reg_set_uchar((char *)"ice.pref.interface.vmnet6", 236);
    NR_reg_set_uchar((char *)"ice.pref.interface.vmnet7", 235);
    NR_reg_set_uchar((char *)"ice.pref.interface.vmnet8", 234);


  }

  // Create the ICE context
  int r;
  
  UINT4 flags = offerer ? NR_ICE_CTX_FLAGS_OFFERER:
      NR_ICE_CTX_FLAGS_ANSWERER;
  flags |= NR_ICE_CTX_FLAGS_AGGRESSIVE_NOMINATION;

  r = nr_ice_ctx_create(const_cast<char *>(name.c_str()), flags,
                        &ctx->ctx_);
  if (r) {
    MLOG(PR_LOG_ERROR, "Couldn't create ICE ctx for '" << name << "'");
    return NULL;
  }

  // Set up the handler
  ctx->ice_handler_ = new nr_ice_handler();
  ctx->ice_handler_->vtbl = &handler_vtbl;
  ctx->ice_handler_->obj = ctx;

  // Create the peer ctx. Because we do not support parallel forking, we
  // only have one peer ctx.
  std::string peer_name = name + ":default";
  r = nr_ice_peer_ctx_create(ctx->ctx_, ctx->ice_handler_,
                             const_cast<char *>(peer_name.c_str()),
                             &ctx->peer_);
  if (r) {
    MLOG(PR_LOG_ERROR, "Couldn't create ICE peer ctx for '" << name << "'");
    return NULL;
  }

  return ctx;
}


NrIceCtx::~NrIceCtx() {
  // TODO(ekr@rtfm.com): Implement this
}

mozilla::RefPtr<NrIceMediaStream>
NrIceCtx::CreateStream(const std::string& name, int components) {
  mozilla::RefPtr<NrIceMediaStream> stream =
    NrIceMediaStream::Create(this, name, components);
  
  streams_.push_back(stream);

  return stream;
}


nsresult NrIceCtx::StartGathering() {
  int r = nr_ice_initialize(ctx_, &NrIceCtx::initialized_cb,
                            this);

  this->AddRef();
  
  if (r && r != R_WOULDBLOCK) {
      MLOG(PR_LOG_ERROR, "Couldn't gather ICE candidates for '"
           << name_ << "'");
      return NS_ERROR_FAILURE;
  }
  
  return NS_OK;
}

void NrIceCtx::EmitAllCandidates() {
  MLOG(PR_LOG_NOTICE, "Gathered all ICE candidates for '"
       << name_ << "'");
  
  for(size_t i=0; i<streams_.size(); ++i) {
    streams_[i]->EmitAllCandidates();
  }
  
  // Report that we are done gathering
  SignalGatheringComplete(this);
}

std::vector<std::string> NrIceCtx::GetGlobalAttributes() {
  char **attrs = 0;
  int attrct;
  int r;
  std::vector<std::string> ret;

  r = nr_ice_get_global_attributes(ctx_, &attrs, &attrct);
  if (r) {
    MLOG(PR_LOG_ERROR, "Couldn't get ufrag and password for '"
         << name_ << "'");
    return ret;
  }

  for (int i=0; i<attrct; i++) {
    ret.push_back(std::string(attrs[i]));
    RFREE(attrs[i]);
  }
  RFREE(attrs);

  return ret;
}

nsresult NrIceCtx::ParseGlobalAttributes(std::vector<std::string> attrs) {
  std::vector<char *> attrs_in;

  for (size_t i=0; i<attrs.size(); ++i) {
    attrs_in.push_back(const_cast<char *>(attrs[i].c_str()));
  }
  
  int r = nr_ice_peer_ctx_parse_global_attributes(peer_, &attrs_in[0],
                                                  attrs.size());
  if (r) {
    MLOG(PR_LOG_ERROR, "Couldn't parse global attributes for "
         << name_ << "'");
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

nsresult NrIceCtx::StartChecks() {
  int r;

  r=nr_ice_peer_ctx_pair_candidates(peer_); 
  if (r) {
    MLOG(PR_LOG_ERROR, "Couldn't pair candidates on "
         << name_ << "'");
    return NS_ERROR_FAILURE;
  }

  r = nr_ice_peer_ctx_start_checks(peer_);
  if (r) {
    MLOG(PR_LOG_ERROR, "Couldn't start peer checks on "
         << name_ << "'");
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}


void NrIceCtx::initialized_cb(NR_SOCKET s, int h, void *arg) {
  NrIceCtx *ctx = static_cast<NrIceCtx *>(arg);
  
  ctx->EmitAllCandidates();
  
  ctx->Release();
}



// NrIceMediaStream
mozilla::RefPtr<NrIceMediaStream>
NrIceMediaStream::Create(mozilla::RefPtr<NrIceCtx> ctx,
                         const std::string& name,
                         int components) {
  mozilla::RefPtr<NrIceMediaStream> stream =
    new NrIceMediaStream(ctx, name, components);
  
  int r = nr_ice_add_media_stream(ctx->ctx(),
                                  const_cast<char *>(name.c_str()),
                                  components, &stream->stream_);
  if (r) {
    MLOG(PR_LOG_ERROR, "Couldn't create ICE media stream for '"
         << name << "'");
    return NULL;
  }

  return stream;
}

NrIceMediaStream::~NrIceMediaStream() {
  // TODO(ekr@rtfm.com): Implement this
}
                                           
nsresult NrIceMediaStream::ParseCandidates(std::vector<std::string>&
                                           candidates) {
  std::vector<char *> candidates_in;

  for (size_t i=0; i<candidates.size(); ++i) {
    candidates_in.push_back(const_cast<char *>(candidates[i].c_str()));
  }
  
  int r = nr_ice_peer_ctx_parse_stream_attributes(ctx_->peer(),
                                                  stream_,
                                                  &candidates_in[0],
                                                  candidates_in.size());
  if (r) {
    MLOG(PR_LOG_ERROR, "Couldn't parse attributes for stream "
         << name_ << "'");
    return NS_ERROR_FAILURE;
  }
  
  return NS_OK;
}

void NrIceMediaStream::EmitAllCandidates() {
  char **attrs = 0;
  int attrct;
  int r;
  r = nr_ice_media_stream_get_attributes(stream_,
                                         &attrs, &attrct);
  if (r) {
    MLOG(PR_LOG_ERROR, "Couldn't get ICE candidates for '"
         << name_ << "'");
    return;
  }
  
  for (size_t i=0; i<attrct; i++) {
    SignalCandidate(this, attrs[i]);
    RFREE(attrs[i]);
  }

  RFREE(attrs);
}


