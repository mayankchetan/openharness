// Minimal mbedTLS 3.6 configuration for the host build of the TLS wrapper test (MBEDTLS_CONFIG_FILE).
//
// It mirrors what the product enables for a PSK client (IDF components/mbedtls/Kconfig with the symbols of
// sdkconfig.defaults.esp32s3): TLS 1.2, ECDHE-PSK key exchange, AES-CBC, SHA-256/384, secp256r1 and friends, record
// buffers IN 16,384 / OUT 4,096. The server half (SSL_SRV_C) is for the test's peer only. No certificates, no TLS 1.3,
// no PSA, no threading: the wrapper's own mutex is what the test checks.
#pragma once

#define MBEDTLS_PLATFORM_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_CIPHER_PADDING_PKCS7
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_OID_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_ERROR_C
#define MBEDTLS_ERROR_STRERROR_DUMMY

#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_PSK_ENABLED

// Superset on purpose, so the test server can OFFER suites the dial must refuse: plain PSK, DHE-PSK, GCM, SHA-1 MACs.
// The product does not build these (sdkconfig.defaults.esp32s3); the wrapper's own suite list is what keeps them out.
#ifndef HOST_PRODUCT_SUITES_ONLY
#define MBEDTLS_KEY_EXCHANGE_PSK_ENABLED
#define MBEDTLS_KEY_EXCHANGE_DHE_PSK_ENABLED
#define MBEDTLS_DHM_C
#define MBEDTLS_GCM_C
#define MBEDTLS_SHA1_C
#endif
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_IN_CONTENT_LEN 16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096
