// Ported to Mbed TLS 4 (ESP-IDF 6): SHA-1 and AES-CTR through PSA, HMAC and PBKDF2 on top
// of it (mbedtls_md no longer does HMAC), DH through the builtin (private) bignum module,
// random data from the hardware RNG

// bignum is declared for "private use" only in Mbed TLS 4
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "Crypto.h"

#include <mbedtls/base64.h>          // for mbedtls_base64_encode, mbedtls_base64_...
#include <mbedtls/private/bignum.h>  // for mbedtls_mpi_free, mbedtls_mpi_init
#include <cstdint>                   // for uint8_t
#include <cstring>                   // for memcpy
#include <stdexcept>                 // for runtime_error

#include "esp_random.h"

extern "C" {
#include "aes.h"  // for AES_ECB_decrypt, AES_init_ctx, AES_ctx
}

static unsigned char DHGenerator[1] = {2};

static const size_t SHA1_SIZE = 20, SHA1_BLOCK = 64;

CryptoMbedTLS::CryptoMbedTLS() {
  psa_crypto_init();
}

CryptoMbedTLS::~CryptoMbedTLS() {
  psa_hash_abort(&sha1Context);
}

std::vector<uint8_t> CryptoMbedTLS::base64Decode(const std::string& data) {
  // Calculate max decode length
  size_t requiredSize;

  mbedtls_base64_encode(nullptr, 0, &requiredSize, (unsigned char*)data.c_str(),
                        data.size());

  std::vector<uint8_t> output(requiredSize);
  size_t outputLen = 0;
  mbedtls_base64_decode(output.data(), requiredSize, &outputLen,
                        (unsigned char*)data.c_str(), data.size());

  return std::vector<uint8_t>(output.begin(), output.begin() + outputLen);
}

std::string CryptoMbedTLS::base64Encode(const std::vector<uint8_t>& data) {
  // Calculate max output length
  size_t requiredSize;
  mbedtls_base64_encode(nullptr, 0, &requiredSize, data.data(), data.size());

  std::vector<uint8_t> output(requiredSize);
  size_t outputLen = 0;

  mbedtls_base64_encode(output.data(), requiredSize, &outputLen, data.data(),
                        data.size());

  return std::string(output.begin(), output.begin() + outputLen);
}

// Sha1
void CryptoMbedTLS::sha1Init() {
  psa_hash_abort(&sha1Context);
  if (psa_hash_setup(&sha1Context, PSA_ALG_SHA_1) != PSA_SUCCESS) {
    throw std::runtime_error("SHA-1 unavailable");
  }
}

void CryptoMbedTLS::sha1Update(const std::string& s) {
  psa_hash_update(&sha1Context, (const uint8_t*)s.data(), s.size());
}
void CryptoMbedTLS::sha1Update(const std::vector<uint8_t>& vec) {
  psa_hash_update(&sha1Context, vec.data(), vec.size());
}

std::vector<uint8_t> CryptoMbedTLS::sha1FinalBytes() {
  std::vector<uint8_t> digest(SHA1_SIZE);
  size_t len = 0;
  if (psa_hash_finish(&sha1Context, digest.data(), digest.size(), &len) !=
          PSA_SUCCESS ||
      len != SHA1_SIZE) {
    throw std::runtime_error("SHA-1 failed");
  }
  return digest;
}

std::string CryptoMbedTLS::sha1Final() {
  auto digest = sha1FinalBytes();
  return std::string(digest.begin(), digest.end());
}

// HMAC SHA1 (RFC 2104)
std::vector<uint8_t> CryptoMbedTLS::sha1HMAC(
    const std::vector<uint8_t>& inputKey, const std::vector<uint8_t>& message) {
  std::vector<uint8_t> key = inputKey;
  if (key.size() > SHA1_BLOCK) {
    sha1Init();
    sha1Update(key);
    key = sha1FinalBytes();
  }
  key.resize(SHA1_BLOCK, 0);

  std::vector<uint8_t> pad(SHA1_BLOCK);
  for (size_t i = 0; i < SHA1_BLOCK; i++) {
    pad[i] = key[i] ^ 0x36;
  }
  sha1Init();
  sha1Update(pad);
  sha1Update(message);
  auto inner = sha1FinalBytes();

  for (size_t i = 0; i < SHA1_BLOCK; i++) {
    pad[i] = key[i] ^ 0x5c;
  }
  sha1Init();
  sha1Update(pad);
  sha1Update(inner);
  return sha1FinalBytes();
}

// AES CTR, in place. Unlike mbedtls_aes_crypt_ctr() the counter in iv is not
// advanced: every caller passes a fresh IV for the position it decrypts.
void CryptoMbedTLS::aesCTRXcrypt(const std::vector<uint8_t>& key,
                                 std::vector<uint8_t>& iv, uint8_t* buffer,
                                 size_t nbytes) {
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&attr, key.size() * 8);
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
  psa_set_key_algorithm(&attr, PSA_ALG_CTR);

  psa_key_id_t keyId;
  if (psa_import_key(&attr, key.data(), key.size(), &keyId) != PSA_SUCCESS) {
    throw std::runtime_error("Failed to set AES key");
  }

  psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
  size_t outLen = 0, finLen = 0;
  bool ok = psa_cipher_decrypt_setup(&op, keyId, PSA_ALG_CTR) == PSA_SUCCESS &&
            psa_cipher_set_iv(&op, iv.data(), iv.size()) == PSA_SUCCESS &&
            psa_cipher_update(&op, buffer, nbytes, buffer, nbytes, &outLen) ==
                PSA_SUCCESS &&
            psa_cipher_finish(&op, buffer + outLen, nbytes - outLen, &finLen) ==
                PSA_SUCCESS;
  psa_cipher_abort(&op);
  psa_destroy_key(keyId);

  if (!ok || outLen + finLen != nbytes) {
    throw std::runtime_error("Failed to decrypt");
  }
}

void CryptoMbedTLS::aesECBdecrypt(const std::vector<uint8_t>& key,
                                  std::vector<uint8_t>& data) {

  struct AES_ctx aesCtr;
  AES_init_ctx(&aesCtr, key.data());

  for (unsigned int x = 0; x < data.size() / 16; x++) {

    AES_ECB_decrypt(&aesCtr, data.data() + (x * 16));
  }
}

// PBKDF2 (RFC 8018) with HMAC-SHA1
std::vector<uint8_t> CryptoMbedTLS::pbkdf2HmacSha1(
    const std::vector<uint8_t>& password, const std::vector<uint8_t>& salt,
    int iterations, int digestSize) {
  std::vector<uint8_t> digest;
  for (uint32_t block = 1; digest.size() < (size_t)digestSize; block++) {
    std::vector<uint8_t> u = salt;
    u.push_back(block >> 24);
    u.push_back(block >> 16);
    u.push_back(block >> 8);
    u.push_back(block);
    u = sha1HMAC(password, u);
    std::vector<uint8_t> t = u;
    for (int i = 1; i < iterations; i++) {
      u = sha1HMAC(password, u);
      for (size_t j = 0; j < t.size(); j++) {
        t[j] ^= u[j];
      }
    }
    digest.insert(digest.end(), t.begin(), t.end());
  }
  digest.resize(digestSize);
  return digest;
}

void CryptoMbedTLS::dhInit() {
  privateKey = generateVectorWithRandomData(DH_KEY_SIZE);

  // initialize big num
  mbedtls_mpi prime, generator, res, privKey;
  mbedtls_mpi_init(&prime);
  mbedtls_mpi_init(&generator);
  mbedtls_mpi_init(&privKey);
  mbedtls_mpi_init(&res);

  // Read bin into big num mpi
  mbedtls_mpi_read_binary(&prime, DHPrime, sizeof(DHPrime));
  mbedtls_mpi_read_binary(&generator, DHGenerator, sizeof(DHGenerator));
  mbedtls_mpi_read_binary(&privKey, privateKey.data(), DH_KEY_SIZE);

  // perform diffie hellman G^X mod P
  mbedtls_mpi_exp_mod(&res, &generator, &privKey, &prime, NULL);

  // Write generated public key to vector
  this->publicKey = std::vector<uint8_t>(DH_KEY_SIZE);
  mbedtls_mpi_write_binary(&res, publicKey.data(), DH_KEY_SIZE);

  // Release memory
  mbedtls_mpi_free(&prime);
  mbedtls_mpi_free(&generator);
  mbedtls_mpi_free(&privKey);
  mbedtls_mpi_free(&res);
}

std::vector<uint8_t> CryptoMbedTLS::dhCalculateShared(
    const std::vector<uint8_t>& remoteKey) {
  // initialize big num
  mbedtls_mpi prime, remKey, res, privKey;
  mbedtls_mpi_init(&prime);
  mbedtls_mpi_init(&remKey);
  mbedtls_mpi_init(&privKey);
  mbedtls_mpi_init(&res);

  // Read bin into big num mpi
  mbedtls_mpi_read_binary(&prime, DHPrime, sizeof(DHPrime));
  mbedtls_mpi_read_binary(&remKey, remoteKey.data(), remoteKey.size());
  mbedtls_mpi_read_binary(&privKey, privateKey.data(), DH_KEY_SIZE);

  // perform diffie hellman (G^Y)^X mod P (for shared secret)
  mbedtls_mpi_exp_mod(&res, &remKey, &privKey, &prime, NULL);

  auto sharedKey = std::vector<uint8_t>(DH_KEY_SIZE);
  mbedtls_mpi_write_binary(&res, sharedKey.data(), DH_KEY_SIZE);

  // Release memory
  mbedtls_mpi_free(&prime);
  mbedtls_mpi_free(&remKey);
  mbedtls_mpi_free(&privKey);
  mbedtls_mpi_free(&res);

  return sharedKey;
}

// Random stuff
std::vector<uint8_t> CryptoMbedTLS::generateVectorWithRandomData(
    size_t length) {
  std::vector<uint8_t> randomVector(length);
  esp_fill_random(randomVector.data(), length);
  return randomVector;
}
