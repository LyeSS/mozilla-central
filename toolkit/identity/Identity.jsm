/* -*- Mode: js2; js2-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* vim: set ft=javascript ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

let Cu = Components.utils;
let Ci = Components.interfaces;
let Cc = Components.classes;
let Cr = Components.results;

Cu.import("resource://gre/modules/XPCOMUtils.jsm");
Cu.import("resource://gre/modules/Services.jsm");

var EXPORTED_SYMBOLS = ["IdentityService",];

const ALGORITHMS = { RS256: 1, DS160: 2, };

XPCOMUtils.defineLazyGetter(this, "IDKeyPair", function () {
  return Cc["@mozilla.org/identityservice-keypair;1"].
    createInstance(Ci.nsIIdentityServiceKeyPair);
});

XPCOMUtils.defineLazyServiceGetter(this,
                                   "uuidGenerator",
                                   "@mozilla.org/uuid-generator;1",
                                   "nsIUUIDGenerator");

function uuid()
{
  return uuidGenerator.generateUUID();
}

function log(aMsg)
{
  dump("IDService: " + aMsg + "\n");
}

function supString(aString)
{
  let str = Cc["@mozilla.org/supports-string;1"].
    createInstance(Ci.nsISupportsString);
  str.data = aString;
  return str;
}

function IDService()
{
  Services.obs.addObserver(this, "quit-application-granted", false);
  Services.obs.addObserver(this, "identity-login", false);
}
IDService.prototype = {
  // DOM Methods.

  /**
   * Register a listener for a given windowID as a result of a call to
   * navigator.id.watch().
   *
   * @param aOptions
   *        (Object)  An object containing the same properties as handed
   *                  to the watch call made in DOM. See nsIDOMIdentity.idl
   *                  for more information on each property.
   *
   * @param aWindowID
   *        (int)     A unique number representing the window from which this
   *                  call was made.
   */
  watch: function watch(aOptions, aWindowID)
  {
    // TODO: do type checking on aOptions properties.
    // TODO: only persist a whitelist of properties
  },

  /**
   * Initiate a login with user interaction as a result of a call to
   * navigator.id.request().
   *
   * @param aWindowID
   *        int       A unique number representing a window which is requesting
   *                  the assertion.
   *
   * If an assertion is obtained successfully, aOptions.onlogin will be called,
   * as registered with a preceding call to watch for the same window ID. It is
   * an error to invoke request() without first calling watch().
   */
  request: function request(aWindowID)
  {
    Services.obs.notifyObservers(aWindowID, "identity-request", null);
  },

  /**
   * Notify the Identity module that content has finished loading its
   * provisioning context and is ready to being the provisioning process.
   * 
   * @param aCallback
   *        (Function)  A callback that will be called with (email, time), where
   *                    email is the address for which a certificate is
   *                    requested, and the time is the *maximum* time allowed
   *                    for the validity of the ceritificate.
   *
   * @param aWindowID
   *        int         A unique number representing the window in which the
   *                    provisioning page for the IdP has been loaded.
   */
  beginProvisioning: function beginProvisioning(aCallback, aWindowID)
  {

  },

  /**
   * Generates a keypair for the current user being provisioned and returns
   * the public key via the callback.
   *
   * @param aCallback
   *        (Function)  A callback that will be called with the public key
   *                    of the generated keypair.
   *
   * @param aWindowID
   *        int         A unique number representing the window in which this
   *                    call was made.
   *
   * It is an error to call genKeypair without receiving the callback for
   * the beginProvisioning() call first.
   */
  genKeypair: function genKeypair(aCallback, aWindowID)
  {

  },

  /**
   * Sets the certificate for the user for which a certificate was requested
   * via a preceding call to beginProvisioning (and genKeypair).
   *
   * @param aCert
   *        (String)  A JWT representing the signed certificate for the user
   *                  being provisioned, provided by the IdP.
   */
  registerCertificate: function registerCertificate(aCert)
  {

  },

  // Public utility methods.

  /**
   * Obtain a BrowserID assertion with the specified characteristics.
   *
   * @param aCallback
   *        (Function) Callback to be called with (err, assertion) where 'err'
   *        can be an Error or NULL, and 'assertion' can be NULL or a valid
   *        BrowserID assertion. If no callback is provided, an exception is
   *        thrown.
   *
   * @param aOptions
   *        (Object) An object that may contain the following properties:
   *
   *          "requiredEmail" : An email for which the assertion is to be
   *                            issued. If one could not be obtained, the call
   *                            will fail. If this property is not specified,
   *                            the default email as set by the user will be
   *                            chosen. If both this property and "sameEmailAs"
   *                            are set, an exception will be thrown.
   *
   *          "sameEmailAs"   : If set, instructs the function to issue an
   *                            assertion for the same email that was provided
   *                            to the domain specified by this value. If this
   *                            information could not be obtained, the call
   *                            will fail. If both this property and
   *                            "requiredEmail" are set, an exception will be
   *                            thrown.
   *
   *          "audience"      : The audience for which the assertion is to be
   *                            issued. If this property is not set an exception
   *                            will be thrown.
   *
   *        Any properties not listed above will be ignored.
   */
  getAssertion: function getAssertion(aCallback, aOptions)
  {

  },

  /**
   * Obtain a BrowserID assertion by asking the user to login and select an
   * email address.
   *
   * @param aCallback
   *        (Function) Callback to be called with (err, assertion) where 'err'
   *        can be an Error or NULL, and 'assertion' can be NULL or a valid
   *        BrowserID assertion. If no callback is provided, an exception is
   *        thrown.
   *
   * @param aOptions
   *        (Object) An object that may contain the following properties:
   *
   *          "audience"      : The audience for which the assertion is to be
   *                            issued. If this property is not set an exception
   *                            will be thrown.
   *
   * @param aFrame
   *        (iframe) A XUL iframe element where the login dialog will be
   *        rendered.
   */
  getAssertionWithLogin: function getAssertionWithLogin(
    aCallback, aOptions, aFrame)
  {

  },

  /**
   * Generates an nsIIdentityServiceKeyPair object that can sign data. It also
   * provides all of the public key properties needed by a JW* formatted object
   *
   * @param string aAlgorithm
   *        Either RS256: "1" or DS160: "2", see constant ALGORITHMS above
   * @param string aOrigin
   *        a 'prepath' url, ex: https://www.mozilla.org:1234/
   * @param string aUserID
   *        Most likely, this is an email address
   * @returns void
   *          An internal callback object will notifyObservers of topic
   *          "id-service-key-gen-finished" when the keypair is ready.
   *          Access to the keypair is via the getIdentityServiceKeyPair() method
   **/
  generateKeyPair: function generateKeyPair(aAlgorithm, aOrigin, aUserID)
  {
    if (!ALGORITHMS[aAlgorithm]) {
      throw new Error("IdentityService: Unsupported algorithm");
    }

    var self = this;

    function keyGenCallback() { }

    keyGenCallback.prototype = {

      QueryInterface: function (aIID)
      {
        if (aIID.equals(Ci.nsIIdentityServiceKeyGenCallback)) {
          return this;
        }
        throw Cr.NS_ERROR_NO_INTERFACE;
      },

      keyPairGenFinished: function (aKeyPair)
      {
        let alg = ALGORITHMS[aAlgorithm];
        let url = Services.io.newURI(aOrigin, null, null).prePath;
        let id = uuid();
        var keyWrapper;
        let pubK = aKeyPair.encodedPublicKey; // DER encoded, then base64 urlencoded
        let key = { userID: aUserID, url: url };

        switch (alg) {
        case ALGORITHMS.RS256:
          keyWrapper = {
            algorithm: alg,
            userID: aUserID,
            sign:        aKeyPair.sign,
            url:         url,
            publicKey:   aKeyPair.encodedPublicKey,
            exponent:    aKeyPair.encodedRSAPublicKeyExponent,
            modulus:     aKeyPair.encodedRSAPublicKeyModulus,
          };

          break;

        case ALGORITHMS.DS160:
          keyWrapper = {
            algorithm: alg,
            userID: aUserID,
            sign:       aKeyPair.sign,
            url:        url,
            publicKey:  pubK,
            generator:  aKeyPair.encodedDSAGenerator,
            prime:      aKeyPair.encodedDSAPrime,
            subPrime:   aKeyPair.encodedDSASubPrime,
          };

          break;
        default:
          throw new Error("Unsupported algorithm");
        }

        self._registry[key.userID + "__" + key.url] = keyWrapper;
        Services.obs.notifyObservers(null,
                                     "id-service-key-gen-finished",
                                     JSON.stringify({ url: url, userID: aUserID }));
      },
    };

    IDKeyPair.generateKeyPair(ALGORITHMS[aAlgorithm], new keyGenCallback());
  },

  shutdown: function shutdown()
  {
    this._registry = null;
    this._endpoints = null;
  },

  /**
   * Returns a keypair object from the Identity in-memory storage
   *
   * @param string aUserID
   *        Most likely an email address
   * @param string aUrl
   *        a "prepath" url: https://www.mozilla.org:1234/
   * @returns object
   *
   * The returned obejct will have different properties based on which algorithm
   * was used to generate the keypair. Check the 'algorithm' property before
   * accessing additional properties.
   *
   * RSA keypair properties:
   *   algorithm
   *   userID
   *   sign()
   *   url
   *   publicKey
   *   exponent
   *   modulus
   *
   * DSA keypair properties:
   *   algorithm
   *   userID
   *   sign()
   *   url
   *   publicKey
   *   generator
   *   prime
   *   subPrime
   **/
  getIdentityServiceKeyPair: function getIdentityServiceKeypair(aUserID, aUrl)
  {
    let uri = Services.io.newURI(aUrl, null, null);
    let key = aUserID + "__" + uri.prePath;
    let keyObj =  this._registry[key];

    if (!keyObj) {
      throw new Error("getIdentityServiceKeyPair: Invalid Key");
    }
    return keyObj;
  },

  /**
   * Return the list of identities a user may want to use to login to aOrigin.
   */
  getIdentitiesForSite: function getIdentitiesForSite(aOrigin) {
    return {
      "result": [
        "foo@bar.com",
        "joe@bob.com"
      ],
      lastUsed: "joe@bob.com", // or null if a new origin
    };
  },

  // Private.
  _registry: { },
  _endpoints: { },

  /**
   * Determine the IdP endpoints for provisioning an authorization for a
   * given email address. The order of resolution is as follows:
   *
   * 1) Attempt to fetch /.well-known/browserid for the domain of the provided
   * email address. If a delegation was found, follow to the delegated domain
   * and repeat. If a valid IdP descriptin is found, parse and return values. 
   *
   * 2) Attempt to verify that the domain is supported by the ProxyIdP service
   * by Persona/BrowserID. If the domain is supported, treat persona.org as the
   * primary IdP and return the endpoint values accordingly.
   *
   * 3) Fallback to using persona.org as a secondary verifier. Return endpoints
   * for secondary authorization and provisioning provided by BrowserID/Persona.
   */
  _getEndpoints: function _getEndpoints(email, aCallback)
  {
    log("_getEndpoints\n");
    // TODO: validate email
    let domain = email.substring(email.indexOf("@") + 1);
    log("_getEndpoints: " + domain + "\n");
    // TODO: lookup in cache
    let onSuccess = function(domain, wellKnown) {
      this._endpoints[domain] = {};
      this._endpoints[domain].authentication = wellKnown.authentication;
      this._endpoints[domain].provisioning = wellKnown.provisioning;
      aCallback(this._endpoints[domain]);
    }.bind(this);
    this._fetchWellKnownFile(domain, onSuccess, function onFailure() {
      // TODO: use proxy IDP service
      //this._fetchWellKnownFile(..., onSuccess);
      // TODO: fallback to persona.org
      aCallback(null);
    }.bind(this));
  },

  _fetchWellKnownFile: function _fetchWellKnownFile(domain, aSuccess, aFailure) {
    var XMLHttpRequest = Cc["@mozilla.org/appshell/appShellService;1"]
                           .getService(Ci.nsIAppShellService)
                           .hiddenDOMWindow.XMLHttpRequest;
    var req  = new XMLHttpRequest();
    req.open("GET", "https://" + domain + "/.well-known/browserid", true);
    req.responseType = "json";
    req.mozBackgroundRequest = true;
    req.onreadystatechange = function(oEvent) {
      if (req.readyState === 4) {
        if (req.status === 200) {
          // TODO validate format
          aSuccess(domain, req.response);
          log(req.response + "\n");
          log(JSON.stringify(this._endpoints[domain], null, 4) + "\n");
        } else {
          log("Error: " + req.statusText + "\n");
          aFailure(req.statusText);
        }
      }
    }.bind(this);
    req.send(null);
  },

  /**
   * Load the provisioning URL in a hidden frame to start the provisioning
   * process.
   */
  _beginProvisioning: function _beginProvisioning(aURL)
  {
    // TODO: do something with toolkit/identity/Sandbox.jsm
  },

  QueryInterface: XPCOMUtils.generateQI([Ci.nsISupports, Ci.nsIObserver]),

  observe: function observe(aSubject, aTopic, aData)
  {
    switch (aTopic) {
      case "quit-application-granted":
        Services.obs.removeObserver(this, "quit-application-granted", false);
        Services.obs.removeObserver(this, "identity-login", false);
        this.shutdown();
        break;
      case "identity-login": // User chose a new or exiting identity after a request() call
        let identity = aData; // String
        // aData is the email address chosen (TODO: or null if cancelled?)
        // TODO: either do the authentication or provisioning flow depending on the email
        break;
    }
  },
};

var IdentityService = new IDService();
