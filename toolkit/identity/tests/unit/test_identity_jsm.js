/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

Cu.import("resource://gre/modules/XPCOMUtils.jsm");
Cu.import("resource://gre/modules/Services.jsm");

// delay the loading of the IDService for performance purposes
XPCOMUtils.defineLazyGetter(this, "IDService", function (){
  let scope = {};
  Cu.import("resource:///modules/identity/Identity.jsm", scope);
  return scope.IdentityService;
});

const TEST_URL = "https://myfavoritebacon.com";
const TEST_URL2 = "https://myfavoritebaconinacan.com";
const TEST_USER = "user@mozilla.com";

const ALGORITHMS = { RS256: 1, DS160: 2, };

let idObserver = {
  // nsISupports provides type management in C++
  // nsIObserver is to be an observer
  QueryInterface: XPCOMUtils.generateQI([Ci.nsISupports, Ci.nsIObserver]),

  observe: function (aSubject, aTopic, aData)
  {
    var kpo;
    if (aTopic == "id-service-key-gen-finished") {
      // now we can pluck the keyPair from the store
      let key = JSON.parse(aData);
      kpo = IDService.getIdentityServiceKeyPair(key.userID, key.url);
      do_check_true(kpo != undefined);

      if (kpo.algorithm == ALGORITHMS.RS256) {
        checkRsa(kpo);
      }
      else if (kpo.algorithm == ALGORITHMS.DS160) {
        checkDsa(kpo);
      }
    }
  },
};

// we use observers likely for e10s process separation,
// but maybe a cb interface would work well here, tbd.
Services.obs.addObserver(idObserver, "id-service-key-gen-finished", false);

function checkRsa(kpo)
{
  do_check_true(kpo.sign != null);
  do_check_true(typeof kpo.sign == "function");
  do_check_true(kpo.userID != null);
  do_check_true(kpo.url != null);
  do_check_true(kpo.url == TEST_URL);
  do_check_true(kpo.publicKey != null);
  do_check_true(kpo.exponent != null);
  do_check_true(kpo.modulus != null);

  // TODO: should sign be async?
  let sig = kpo.sign("This is a message to sign");

  do_check_true(sig != null && typeof sig == "string" && sig.length > 1);

  IDService.generateKeyPair("DS160", TEST_URL2, TEST_USER);
}

function checkDsa(kpo)
{
  do_check_true(kpo.sign != null);
  do_check_true(typeof kpo.sign == "function");
  do_check_true(kpo.userID != null);
  do_check_true(kpo.url != null);
  do_check_true(kpo.url == TEST_URL2);
  do_check_true(kpo.publicKey != null);
  do_check_true(kpo.generator != null);
  do_check_true(kpo.prime != null);
  do_check_true(kpo.subPrime != null);

  let sig = kpo.sign("This is a message to sign");

  do_check_true(sig != null && typeof sig == "string" && sig.length > 1);

  Services.obs.removeObserver(idObserver, "id-service-key-gen-finished");

  // pre-emptively shut down to clear resources
  IDService.shutdown();
  
  do_test_finished();
  run_next_test();
}

function test_keypairs()
{
  do_check_true(IDService != null);

  IDService.generateKeyPair("RS256", TEST_URL, TEST_USER);
}

[test_keypairs].forEach(add_test);

do_test_pending();

function run_test()
{
  run_next_test();
}
