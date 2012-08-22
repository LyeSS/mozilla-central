#include "nspr.h"
#include "cert.h"
#include "cryptohi.h"
#include "hasht.h"
#include "keyhi.h"
#include "nss.h"
#include "pk11pub.h"
#include "sechash.h"
#include "ssl.h"

#include "nsError.h"
#include "dtlsidentity.h"
#include "logging.h"

MLOG_INIT("mtransport");

// Helper class to avoid having a crapload of if (!NULL) statements at
// the end to clean up. The way you use this is you instantiate the
// object as scoped_c_ptr<PtrType> obj(value, destructor);
// TODO(ekr@rtfm.com): Move this to some generic location
template <class T> class scoped_c_ptr {
 public:
  scoped_c_ptr(T *t, void (*d)(T *)) : t_(t), d_(d) {}
  scoped_c_ptr(void (*d)(T *)) : t_(NULL), d_(d) {}

  void reset(T *t) { t_ = t; }
  T* forget() { T* t = t_; t_ = NULL; return t; }
  T *get() { return t_; }
  ~scoped_c_ptr() {
    if (t_) {
      d_(t_);
    }
  }
  void operator=(T *t) {
    t_ = t;
  }

 private:
  // TODO: implement copy and assignment operators
  // to remove danger
  T *t_;
  void (*d_)(T *);
};

// Auto-generate an identity based on name=name
mozilla::RefPtr<DtlsIdentity> DtlsIdentity::Generate(const std::string name) {
  SECStatus rv;

  std::string subject_name_string = "CN=" + name;
  CERTName *subject_name = CERT_AsciiToName(
      const_cast<char *>(subject_name_string.c_str()));
  if (!subject_name) {
    return NULL;
  }

  PK11RSAGenParams rsaparams;
  rsaparams.keySizeInBits = 1024;
  rsaparams.pe = 0x010001;

  scoped_c_ptr<SECKEYPrivateKey> private_key(SECKEY_DestroyPrivateKey);
  scoped_c_ptr<SECKEYPublicKey> public_key(SECKEY_DestroyPublicKey);
  SECKEYPublicKey *pubkey;

  private_key = PK11_GenerateKeyPair(PK11_GetInternalSlot(),
                                 CKM_RSA_PKCS_KEY_PAIR_GEN, &rsaparams, &pubkey,
                                 PR_FALSE, PR_TRUE, NULL);
  if (private_key.get() == NULL)
    return NULL;
  public_key= pubkey;
  
  scoped_c_ptr<CERTSubjectPublicKeyInfo> spki(SECKEY_DestroySubjectPublicKeyInfo);
  spki = SECKEY_CreateSubjectPublicKeyInfo(pubkey);
  if (!spki.get()) {
    return NULL;
  }

  scoped_c_ptr<CERTCertificateRequest> certreq(CERT_DestroyCertificateRequest);
  certreq = CERT_CreateCertificateRequest(subject_name, spki.get(), NULL);
  if (!certreq.get()) {
    return NULL;
  }

  PRTime notBefore = PR_Now() - (86400UL * PR_USEC_PER_SEC);
  PRTime notAfter = PR_Now() + (86400UL * 30 * PR_USEC_PER_SEC);

  scoped_c_ptr<CERTValidity> validity(CERT_DestroyValidity);
  validity = CERT_CreateValidity(notBefore, notAfter);
  if (!validity.get()) {
    return NULL;
  }

  unsigned long serial;
  // Note: This serial in principle could collide, but it's unlikely
  rv = PK11_GenerateRandom(reinterpret_cast<unsigned char *>(&serial),
                           sizeof(serial));
  if (rv != SECSuccess) {
    return NULL;
  }

  scoped_c_ptr<CERTCertificate> certificate(CERT_DestroyCertificate);
  certificate = CERT_CreateCertificate(serial, subject_name, validity.get(), certreq.get());
  if (!certificate.get()) {
    return NULL;
  }

  PRArenaPool *arena = certificate.get()->arena;

  rv = SECOID_SetAlgorithmID(arena, &certificate.get()->signature,
                             SEC_OID_PKCS1_SHA1_WITH_RSA_ENCRYPTION, 0);
  if (rv != SECSuccess)
    return NULL;

  // Set version to X509v3.
  *(certificate.get()->version.data) = 2;
  certificate.get()->version.len = 1;

  SECItem innerDER;
  innerDER.len = 0;
  innerDER.data = NULL;

  if (!SEC_ASN1EncodeItem(arena, &innerDER, certificate.get(),
                          SEC_ASN1_GET(CERT_CertificateTemplate)))
    return NULL;

  SECItem *signedCert 
      = reinterpret_cast<SECItem *>(PORT_ArenaZAlloc(arena, sizeof(SECItem)));
  if (!signedCert) {
    return NULL;
  }

  rv = SEC_DerSignData(arena, signedCert, innerDER.data, innerDER.len,
                       private_key.get(),
                       SEC_OID_PKCS1_SHA1_WITH_RSA_ENCRYPTION);
  if (rv != SECSuccess) {
    return NULL;
  }
  certificate.get()->derCert = *signedCert;

  return mozilla::RefPtr<DtlsIdentity>(new DtlsIdentity(
      private_key.forget(), certificate.forget()));
}



DtlsIdentity::~DtlsIdentity() {
  if (privkey_)
    SECKEY_DestroyPrivateKey(privkey_);

  if(cert_)
    CERT_DestroyCertificate(cert_);
}

nsresult DtlsIdentity::ComputeFingerprint(const std::string algorithm,
                                          unsigned char *digest,
                                          std::size_t size,
                                          std::size_t *digest_length) {
  PR_ASSERT(cert_);

  return ComputeFingerprint(cert_, algorithm, digest, size, digest_length);
}

nsresult DtlsIdentity::ComputeFingerprint(const CERTCertificate *cert,
                                          const std::string algorithm,
                                          unsigned char *digest,
                                          std::size_t size,
                                          std::size_t *digest_length) {
  PR_ASSERT(cert);

  HASH_HashType ht;

  if (algorithm == "sha-1") {
    ht = HASH_AlgSHA1;
  } else if (algorithm == "sha-224") {
    ht = HASH_AlgSHA224;
  } else if (algorithm == "sha-256") {
    ht = HASH_AlgSHA256;
  } else if (algorithm == "sha-384") {
    ht = HASH_AlgSHA384;
  }  else if (algorithm == "sha-512") {
    ht = HASH_AlgSHA512;
  } else {
    return NS_ERROR_INVALID_ARG;
  }

  const SECHashObject *ho = HASH_GetHashObject(ht);
  PR_ASSERT(ho);
  if (!ho)
    return NS_ERROR_INVALID_ARG;

  PR_ASSERT(ho->length >= 20);  // Double check

  if (size < ho->length)
    return NS_ERROR_INVALID_ARG;

  SECStatus rv = HASH_HashBuf(ho->type, digest,
                              cert->derCert.data,
                              cert->derCert.len);

  // This should not happen
  if (rv != SECSuccess)
    return NS_ERROR_FAILURE;

  *digest_length = ho->length;

  return NS_OK;
}

// Format the fingerprint in RFC 4572 Section 5 format, colons and
// all.
std::string DtlsIdentity::FormatFingerprint(const unsigned char *digest,
                                            std::size_t size) {
  std::string str("");
  char group[3];
  
  for (std::size_t i=0; i < size; i++) {
    snprintf(group, 3, "%.2x", digest[i]);
    if (i != 0){
      str += ":";
    }
    str += group;
  }

  PR_ASSERT(str.size() == (size * 3 - 1));  // Check result length
  return str;
}

// Parse a fingerprint in RFC 4572 format.
// Note that this tolerates some badly formatted data, in particular:
// (a) arbitrary runs of colons 
// (b) colons at the beginning or end.
nsresult DtlsIdentity::ParseFingerprint(const std::string fp,
                                        unsigned char *digest,
                                        size_t size,
                                        size_t *length) {
  size_t offset = 0;
  bool top_half = true;
  unsigned char val = 0;

  for (size_t i=0; i<fp.length(); i++) {
    if (offset >= size) {
      // Note: no known way for offset to get > size
      MLOG(PR_LOG_ERROR, "Fingerprint too long for buffer");
      return NS_ERROR_INVALID_ARG;
    }
      
    if (top_half && (fp[i] == ':')) {
      continue;
    } else if ((fp[i] >= '0') && (fp[i] <= '9')) {
      val |= fp[i] - '0';
    } else if ((fp[i] >= 'a') && (fp[i] <= 'f')) {
      val |= fp[i] - 'a' + 10;
    } else {
      MLOG(PR_LOG_ERROR, "Invalid fingerprint value " << fp[i]);
      return NS_ERROR_ILLEGAL_VALUE;;
    }

    if (top_half) {
      val <<= 4;
      top_half = false;
    } else {
      digest[offset++] = val;
      top_half = true;
      val = 0;
    }
  }
  
  *length = offset;

  return NS_OK;
}
    
