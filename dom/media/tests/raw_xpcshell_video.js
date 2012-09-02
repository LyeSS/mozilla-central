const Cc = Components.classes;
const Ci = Components.interfaces;

Components.utils.import("resource://gre/modules/Services.jsm");
Components.utils.import("resource://gre/modules/XPCOMUtils.jsm");

Cc["@mozilla.org/psm;1"].getService(Ci.nsISupports);

let gScriptDone = false;

let pc1 = Cc["@mozilla.org/peerconnection;1"]
         .createInstance(Ci.IPeerConnection);
let pc2 = Cc["@mozilla.org/peerconnection;1"]
         .createInstance(Ci.IPeerConnection);

let pc1_offer;
let pc2_answer;

let observer1 = {
  QueryInterface: XPCOMUtils.generateQI([Ci.IPeerConnectionObserver]),
  onCreateOfferError: function(code) {
    print("pc1 onCreateOfferError " + code);
  },
  onSetLocalDescriptionError: function(code) {
    print("pc1 onSetLocalDescriptionError " + code);
  },
  onSetRemoteDescriptionError: function(code) {
    print("pc1 onSetRemoteDescriptionError " + code);
  },
  onStateChange: function(state) {
    print("pc1 onStateChange " + state);
  },
  onAddStream: function(stream) {
    print("pc1 onAddStream " + stream);
  }
};

let observer2 = {
  QueryInterface: XPCOMUtils.generateQI([Ci.IPeerConnectionObserver]),
  onCreateAnswerError: function(code) {
    print("pc2 onCreateAnswerError " + code);
  },
  onSetLocalDescriptionError: function(code) {
    print("pc2 onSetLocalDescriptionError " + code);
  },
  onSetRemoteDescriptionError: function(code) {
    print("pc2 onSetRemoteDescriptionError " + code);
  },
  onStateChange: function(state) {
    print("pc2 onStateChange " + state);
  },
  onAddStream: function(stream) {
    print("pc2 onAddStream " + stream);
  }
};

// pc1.createOffer -> pc1.setLocal
observer1.onCreateOfferSuccess = function(offer) {
  print("pc1 got offer: \n" + offer);
  pc1.setLocalDescription(Ci.IPeerConnection.kActionOffer, offer);
  pc1_offer = offer;
};

// pc1.setLocal -> pc2.setRemote
observer1.onSetLocalDescriptionSuccess = function(code) {
  print("pc1 onSetLocalDescriptionSuccess: " + code);
  pc2.setRemoteDescription(Ci.IPeerConnection.kActionOffer, pc1_offer);
};

// pc2.setRemote -> pc2.createAnswer
observer2.onSetRemoteDescriptionSuccess = function(code) {
  print("pc2 onSetRemoteDescriptionSuccess: " + code);
  pc2.createAnswer("", pc1_offer);
};

// pc2.createAnswer -> pc2.setLocal
observer2.onCreateAnswerSuccess = function(answer) {
  print("pc2 got answer: \n" + answer);
  pc2.setLocalDescription(Ci.IPeerConnection.kActionAnswer, answer);
  pc2_answer = answer;
};

// pc2.setLocal -> pc1.setRemote
observer2.onSetLocalDescriptionSuccess = function(code) {
  print("pc2 onSetLocalDescriptionSuccess: " + code);
  pc1.setRemoteDescription(Ci.IPeerConnection.kActionAnswer, pc2_answer);
};

// pc1.setRemote -> finish!
observer1.onSetRemoteDescriptionSuccess = function(code) {
  print("pc1 onSetRemoteDescriptionSuccess: " + code);
  // run traffic for 5 seconds, then terminate
  let timer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
  timer.initWithCallback(function() {
    gScriptDone = true;
  }, 5000, Ci.nsITimer.TYPE_ONE_SHOT);
};

let mainThread = Services.tm.currentThread;
pc1.initialize(observer1, mainThread);
pc2.initialize(observer2, mainThread);

let stream1v = pc1.createFakeMediaStream(Ci.IPeerConnection.kHintVideo);
pc1.addStream(stream1v);

let stream1a = pc1.createFakeMediaStream(Ci.IPeerConnection.kHintAudio | 0x80);
pc1.addStream(stream1a);

let stream2v = pc2.createFakeMediaStream(Ci.IPeerConnection.kHintVideo);
pc2.addStream(stream2v);

let stream2a = pc2.createFakeMediaStream(Ci.IPeerConnection.kHintAudio | 0x80);
pc2.addStream(stream2a);

// start the chain.
pc1.createOffer("");

while (!gScriptDone)
  mainThread.processNextEvent(true);
while (mainThread.hasPendingEvents())
  mainThread.processNextEvent(true);
