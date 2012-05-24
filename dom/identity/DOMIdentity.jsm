/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const {classes: Cc, interfaces: Ci, utils: Cu} = Components;

// This is the parent process corresponding to nsDOMIdentity.
let EXPORTED_SYMBOLS = ["DOMIdentity"];

Cu.import("resource://gre/modules/Services.jsm");
Cu.import("resource://gre/modules/XPCOMUtils.jsm");

XPCOMUtils.defineLazyGetter(this, "ppmm", function() {
  return Cc["@mozilla.org/parentprocessmessagemanager;1"].getService(
    Ci.nsIFrameMessageManager
  );
});

XPCOMUtils.defineLazyModuleGetter(this, "IdentityService",
                                  "resource://gre/modules/identity/Identity.jsm");

let DOMIdentity = {
  // nsIFrameMessageListener
  receiveMessage: function(aMessage) {
    let msg = aMessage.json;
    switch (aMessage.name) {
      case "Identity:Watch":
        this._watch(msg);
        break;
      case "Identity:Request":
        this._request(msg);
        break;
      case "Identity:Logout":
        this._logout(msg);
        break;
      case "Identity:IDP:ProvisioningFailure":
        this._provisioningFailure(msg);
        break;
    }
  },

  // nsIObserver
  observe: function(aSubject, aTopic, aData) {
    if (aTopic != "xpcom-shutdown") {
      return;
    }
    
    this.messages.forEach((function(msgName) {
      ppmm.removeMessageListener(msgName, this);
    }).bind(this));
    Services.obs.removeObserver(this, "xpcom-shutdown");
    ppmm = null;
  },

  // Private.
  _init: function() {
    this.messages = ["Identity:Watch", "Identity:Request", "Identity:Logout", "Identity:IDP:ProvisioningFailure"];

    this.messages.forEach((function(msgName) {
      ppmm.addMessageListener(msgName, this);
    }).bind(this));

    Services.obs.addObserver(this, "xpcom-shutdown", false);

    this._pending = [];
  },

  _watch: function(message) {
    // Forward to Identity.jsm and stash the oid somewhere so we can make
    // callback after sending a message to parent process.
    this._pending.push(message.oid);
  },

  _request: function(message) {
    // Forward to Identity.jsm and stash oid somewhere?
    
    // Oh look we got a fake assertion back from the JSM, send it onward.
    let message = { 
      // Should not be empty because _watch was *definitely* called before this.
      // Right? Right.
      oid: this._pending[0],
      assertion: "fake.jwt.token"
    };
    ppmm.sendAsyncMessage("Identity:Watch:OnLogin", message);

    IdentityService.request(this._pending[0]);
  },

  _logout: function(message) {
    // TODO: forward to Identity.jsm
  },

  _provisioningFailure: function(message) {
    IdentityService.provisioningFailure(message.reason, message.oid);
  }
};

// Object is initialized by nsIDService.js
