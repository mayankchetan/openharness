"""B2: source rules for main/cable_xport_tls.c, checked without a toolchain.

  * no ESP_LOG* call takes the key, the identity or anything named like them (error paths log mbedTLS codes only);
  * the ciphersuite list is exactly the two ECDHE-PSK CBC suites and is installed with mbedtls_ssl_conf_ciphersuites;
  * TLS 1.2 is pinned at both ends, session tickets are disabled, a USB inner transport is refused;
  * the PSK goes in with mbedtls_ssl_conf_psk (mbedTLS copies it), no certificate API is used, and esp-tls is not.
The behaviour is tested in test_cable_xport_tls.c (built by test_cable_xport_tls.py against the IDF mbedTLS sources).
"""
from pathlib import Path
import re

main = Path(__file__).resolve().parent / '../main'
src = (main / 'cable_xport_tls.c').read_text().replace('\r\n', '\n')
code = re.sub(r'//[^\n]*', '', src)   # the comments say what they like

# 1. logging
statements = re.findall(r'ESP_LOG[EWIDV]\s*\((?:[^;]|\n)*?\)\s*;', code)
assert statements, 'the wrapper logs its error codes'
for statement in statements:
    stripped = re.sub(r'"(?:[^"\\]|\\.)*"', '""', statement)   # the format text is free; the arguments must not be key material
    assert not re.search(r'\bpsk\w*\b|\bkey\w*\b|\bident\w*\b', stripped), f'a log call takes key or identity material: {statement}'
    # what may follow the format text: an mbedTLS error code, the inner transport's name, or nothing
    args = re.sub(r'\s+', '', stripped)
    rest = re.sub(r'^ESP_LOG[EWIDV]\(TAG,""', '', args)
    for allowed in ('(unsigned)-r', 't->inner->name', 'retryable(r)?"":""', ',', ');'):
        rest = rest.replace(allowed, '')
    assert rest == '', f'a log call carries something other than an error code: {statement}'

# 2. suites
assert 'mbedtls_ssl_conf_ciphersuites(' in code
suites = re.search(r's_suites\[\]\s*=\s*\{(.*?)\};', code, re.S)
assert suites, 's_suites list'
names = re.findall(r'MBEDTLS_TLS_\w+', suites.group(1))
assert names == ['MBEDTLS_TLS_ECDHE_PSK_WITH_AES_128_CBC_SHA256', 'MBEDTLS_TLS_ECDHE_PSK_WITH_AES_256_CBC_SHA384'], names
assert re.search(r'\b0,\s*$', suites.group(1).strip()), 'the list ends with 0'

# 3. version pins
assert re.search(r'mbedtls_ssl_conf_min_tls_version\([^)]*MBEDTLS_SSL_VERSION_TLS1_2\)', code)
assert re.search(r'mbedtls_ssl_conf_max_tls_version\([^)]*MBEDTLS_SSL_VERSION_TLS1_2\)', code)
assert 'TLS1_3' not in code
assert re.search(r'mbedtls_ssl_conf_session_tickets\([^)]*MBEDTLS_SSL_SESSION_TICKETS_DISABLED\)', code), 'session tickets are switched off'
assert re.search(r'inner->kind == CABLE_XPORT_USB\)\s*\{[^}]*return false', code), 'open() refuses a USB inner transport'

# 4. PSK only, mbedTLS direct
assert 'mbedtls_ssl_conf_psk(' in code
for forbidden in ('esp_tls', 'mbedtls_ssl_conf_own_cert', 'mbedtls_ssl_conf_ca_chain', 'mbedtls_x509', 'mbedtls_pk_', 'esp_crt_bundle',
                  'mbedtls_ssl_conf_authmode'):
    assert forbidden not in code, forbidden
assert re.search(r'mbedtls_ssl_set_bio\(&t->ssl, t, bio_send, NULL, bio_recv_timeout\)', code), 'BIO callbacks on the inner transport'

# 5. built into the image
assert '"cable_xport_tls.c"' in (main / 'CMakeLists.txt').read_text()
print('cable_xport_tls.c source rules: no key in logs, two ECDHE-PSK CBC suites, TLS 1.2 pinned, no cert API, no esp-tls PASS')
