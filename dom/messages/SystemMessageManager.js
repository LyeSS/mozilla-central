/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const Cc = Components.classes;
const Ci = Components.interfaces;
const Cu = Components.utils;

Cu.import("resource://gre/modules/XPCOMUtils.jsm");
Cu.import("resource://gre/modules/DOMRequestHelper.jsm");
Cu.import("resource://gre/modules/Services.jsm");
Cu.import("resource://gre/modules/ObjectWrapper.jsm");

const kSystemMessageInternalReady = "system-message-internal-ready";

XPCOMUtils.defineLazyServiceGetter(this, "cpmm",
                                   "@mozilla.org/childprocessmessagemanager;1",
                                   "nsISyncMessageSender");

// Limit the number of pending messages for a given type.
let kMaxPendingMessages;
try {
  kMaxPendingMessages = Services.prefs.getIntPref("dom.messages.maxPendingMessages");
} catch(e) {
  // getIntPref throws when the pref is not set.
  kMaxPendingMessages = 5;
}

function debug(aMsg) {
  //dump("-- SystemMessageManager " + Date.now() + " : " + aMsg + "\n");
}

// Implementation of the DOM API for system messages

function SystemMessageManager() {
  // Message handlers for this page.
  // We can have only one handler per message type.
  this._handlers = {};

  // Pending messages for this page, keyed by message type.
  this._pendings = {};

  // Flag to specify if this process has already registered manifest.
  this._registerManifestReady = false;

  // Flag to determine this process is a parent or child process.
  let appInfo = Cc["@mozilla.org/xre/app-info;1"];
  this._isParentProcess =
    !appInfo || appInfo.getService(Ci.nsIXULRuntime)
                  .processType == Ci.nsIXULRuntime.PROCESS_TYPE_DEFAULT;

  // An oberver to listen to whether the |SystemMessageInternal| is ready.
  if (this._isParentProcess) {
    Services.obs.addObserver(this, kSystemMessageInternalReady, false);
  }
}

SystemMessageManager.prototype = {
  __proto__: DOMRequestIpcHelper.prototype,

  _dispatchMessage: function sysMessMgr_dispatchMessage(aType, aHandler, aMessage) {
    // We get a json blob, but in some cases we want another kind of object
    // to be dispatched.
    // To do so, we check if we have a with a contract ID of
    // "@mozilla.org/dom/system-messages/wrapper/TYPE;1" component implementing
    // nsISystemMessageWrapper.
    debug("Dispatching " + JSON.stringify(aMessage) + "\n");
    let contractID = "@mozilla.org/dom/system-messages/wrapper/" + aType + ";1";
    let wrapped = false;

    if (contractID in Cc) {
      debug(contractID + " is registered, creating an instance");
      let wrapper = Cc[contractID].createInstance(Ci.nsISystemMessagesWrapper);
      if (wrapper) {
        aMessage = wrapper.wrapMessage(aMessage, this._window);
        wrapped = true;
        debug("wrapped = " + aMessage);
      }
    }

    aHandler.handleMessage(wrapped ? aMessage
                                   : ObjectWrapper.wrap(aMessage, this._window));
  },

  mozSetMessageHandler: function sysMessMgr_setMessageHandler(aType, aHandler) {
    debug("setMessage handler for [" + aType + "] " + aHandler);
    if (!aType) {
      // Just bail out if we have no type.
      return;
    }

    let handlers = this._handlers;
    if (!aHandler) {
      // Setting the handler to null means we don't want to receive messages
      // of this type anymore.
      delete handlers[aType];
      return;
    }

    // Last registered handler wins.
    handlers[aType] = aHandler;

    // If we have pending messages, send them asynchronously.
    if (this._getPendingMessages(aType, true)) {
      let thread = Services.tm.mainThread;
      let pending = this._pendings[aType];
      this._pendings[aType] = [];
      let self = this;
      pending.forEach(function dispatch_pending(aPending) {
        thread.dispatch({
          run: function run() {
            self._dispatchMessage(aType, aHandler, aPending);
          }
        }, Ci.nsIEventTarget.DISPATCH_NORMAL);
      });
    }
  },

  _getPendingMessages: function sysMessMgr_getPendingMessages(aType, aForceUpdate) {
    debug("hasPendingMessage " + aType);
    let pendings = this._pendings;

    // If we have a handler for this type, we can't have any pending message.
    // If called from setMessageHandler, we still want to update the pending
    // queue to deliver existing messages.
    if (aType in this._handlers && !aForceUpdate) {
      return false;
    }

    // Send a sync message to the parent to check if we have a pending message
    // for this type.
    let messages = cpmm.sendSyncMessage("SystemMessageManager:GetPendingMessages",
                                        { type: aType,
                                          uri: this._uri,
                                          manifest: this._manifest })[0];
    if (!messages) {
      // No new pending messages, but the queue may not be empty yet.
      return pendings[aType] && pendings[aType].length != 0;
    }

    if (!pendings[aType]) {
      pendings[aType] = [];
    }

    // Doing that instead of pending.concat() to avoid array copy.
    messages.forEach(function hpm_addPendings(aMessage) {
      pendings[aType].push(aMessage);
      if (pendings[aType].length > kMaxPendingMessages) {
        pendings[aType].splice(0, 1);
      }
    });

    return pendings[aType].length != 0;
  },

  mozHasPendingMessage: function sysMessMgr_hasPendingMessage(aType) {
    return this._getPendingMessages(aType, false);
  },

  uninit: function sysMessMgr_uninit()  {
    this._handlers = null;
    this._pendings = null;

    if (this._isParentProcess) {
      Services.obs.removeObserver(this, kSystemMessageInternalReady);
    }
  },

  receiveMessage: function sysMessMgr_receiveMessage(aMessage) {
    debug("receiveMessage " + aMessage.name + " - " +
          aMessage.json.type + " for " + aMessage.json.manifest +
          " (" + this._manifest + ")");

    let msg = aMessage.json;
    if (msg.manifest != this._manifest)
      return;

    // Send an acknowledgement to parent to clean up the pending message,
    // so a re-launched app won't handle it again, which is redundant.
    cpmm.sendAsyncMessage(
      "SystemMessageManager:Message:Return:OK",
      { type: msg.type,
        manifest: msg.manifest,
        uri: msg.uri,
        msgID: msg.msgID });

    // Bail out if we have no handlers registered for this type.
    if (!(msg.type in this._handlers)) {
      debug("No handler for this type");
      return;
    }

    this._dispatchMessage(msg.type, this._handlers[msg.type], msg.msg);
  },

  // nsIDOMGlobalPropertyInitializer implementation.
  init: function sysMessMgr_init(aWindow) {
    debug("init");
    this.initHelper(aWindow, ["SystemMessageManager:Message"]);

    let principal = aWindow.document.nodePrincipal;
    this._uri = principal.URI.spec;

    let appsService = Cc["@mozilla.org/AppsService;1"]
                        .getService(Ci.nsIAppsService);
    this._manifest = appsService.getManifestURLByLocalId(principal.appId);
    this._window = aWindow;

    // Two cases are valid to register the manifest for the current process:
    // 1. This is asked by a child process (parent process must be ready).
    // 2. Parent process has already constructed the |SystemMessageInternal|.
    // Otherwise, delay to do it when the |SystemMessageInternal| is ready.
    let readyToRegister = true;
    if (this._isParentProcess) {
      let ready = cpmm.sendSyncMessage(
        "SystemMessageManager:AskReadyToRegister", null);
      if (ready.length == 0 || !ready[0]) {
        readyToRegister = false;
      }
    }
    if (readyToRegister) {
      this._registerManifest();
    }

    debug("done");
  },

  observe: function sysMessMgr_observe(aSubject, aTopic, aData) {
    if (aTopic === kSystemMessageInternalReady) {
      this._registerManifest();
    }
  },

  _registerManifest: function sysMessMgr_registerManifest() {
    if (!this._registerManifestReady) {
      cpmm.sendAsyncMessage("SystemMessageManager:Register",
                            { manifest: this._manifest });
      this._registerManifestReady = true;
    }
  },

  classID: Components.ID("{bc076ea0-609b-4d8f-83d7-5af7cbdc3bb2}"),

  QueryInterface: XPCOMUtils.generateQI([Ci.nsIDOMNavigatorSystemMessages,
                                         Ci.nsIDOMGlobalPropertyInitializer,
                                         Ci.nsIObserver]),

  classInfo: XPCOMUtils.generateCI({classID: Components.ID("{bc076ea0-609b-4d8f-83d7-5af7cbdc3bb2}"),
                                    contractID: "@mozilla.org/system-message-manager;1",
                                    interfaces: [Ci.nsIDOMNavigatorSystemMessages],
                                    flags: Ci.nsIClassInfo.DOM_OBJECT,
                                    classDescription: "System Messages"})
}

const NSGetFactory = XPCOMUtils.generateNSGetFactory([SystemMessageManager]);
