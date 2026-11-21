#include "tls.h"
#include <Preferences.h>
#include <esp_https_server.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/oid.h>
#include <WiFi.h>

// Self-signed certificate and key (DER) handed to the HTTPS server.
static uint8_t* certDer = nullptr; static size_t certLen = 0;  // kept for the server's lifetime
static uint8_t* keyDer = nullptr;  static size_t keyLen = 0;
static httpd_handle_t server = nullptr;

// Bump when the certificate contents change, so devices regenerate it.
static const int CERT_FORMAT = 2;


// Generate an ECDSA P-256 key and a self-signed certificate for CN=<hostname>. DER output.
// The certificate must carry extensions: with none, mbedTLS writes an empty extensions block, which
// Chromium-based browsers reject outright as ERR_CERT_INVALID (no "proceed anyway" option).
static bool generate(const String& hostname, IPAddress ip) {
  mbedtls_pk_context pk; mbedtls_entropy_context ent; mbedtls_ctr_drbg_context drbg; mbedtls_x509write_cert crt;
  mbedtls_pk_init(&pk); mbedtls_entropy_init(&ent); mbedtls_ctr_drbg_init(&drbg); mbedtls_x509write_crt_init(&crt);
  bool ok = false;
  const size_t BUF = 1024;
  uint8_t* buf = (uint8_t*)malloc(BUF);
  do {
    const char* pers = "skydisplay-tls";
    if (!buf) break;
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, (const uint8_t*)pers, strlen(pers))) break;
    if (mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) break;
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), mbedtls_ctr_drbg_random, &drbg)) break;

    String subject = "CN=" + hostname + ",O=SkyDisplay";
    // Serial: positive, and with a non-zero first byte so the DER integer is minimally encoded.
    uint8_t serial[8]; mbedtls_ctr_drbg_random(&drbg, serial, sizeof serial); serial[0] = (serial[0] & 0x3F) | 0x40;
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &pk);
    mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
    if (mbedtls_x509write_crt_set_subject_name(&crt, subject.c_str())) break;
    if (mbedtls_x509write_crt_set_issuer_name(&crt, subject.c_str())) break;
    if (mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof serial)) break;
    if (mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20491231235959")) break;

    // Extensions: names the browser can match, not a CA, used for TLS server auth.
    String local = hostname + ".local";
    uint8_t ipb[4] = {ip[0], ip[1], ip[2], ip[3]};
    mbedtls_x509_san_list san[3] = {};
    san[0].node.type = MBEDTLS_X509_SAN_DNS_NAME;
    san[0].node.san.unstructured_name.p = (unsigned char*)hostname.c_str(); san[0].node.san.unstructured_name.len = hostname.length();
    san[1].node.type = MBEDTLS_X509_SAN_DNS_NAME;
    san[1].node.san.unstructured_name.p = (unsigned char*)local.c_str();    san[1].node.san.unstructured_name.len = local.length();
    san[2].node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
    san[2].node.san.unstructured_name.p = ipb;                               san[2].node.san.unstructured_name.len = 4;
    san[0].next = &san[1];
    san[1].next = (uint32_t)ip ? &san[2] : nullptr;
    if (mbedtls_x509write_crt_set_subject_alternative_name(&crt, san)) break;
    if (mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1)) break;
    if (mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE)) break;
    mbedtls_asn1_sequence eku = {};
    eku.buf.tag = MBEDTLS_ASN1_OID;
    eku.buf.p = (unsigned char*)MBEDTLS_OID_SERVER_AUTH; eku.buf.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);
    if (mbedtls_x509write_crt_set_ext_key_usage(&crt, &eku)) break;

    // DER writers fill the buffer from the end and return the length.
    int n = mbedtls_x509write_crt_der(&crt, buf, BUF, mbedtls_ctr_drbg_random, &drbg);
    if (n <= 0) break;
    free(certDer); certDer = (uint8_t*)malloc(n); if (!certDer) break;
    memcpy(certDer, buf + BUF - n, n); certLen = n;
    n = mbedtls_pk_write_key_der(&pk, buf, BUF);
    if (n <= 0) break;
    free(keyDer); keyDer = (uint8_t*)malloc(n); if (!keyDer) break;
    memcpy(keyDer, buf + BUF - n, n); keyLen = n;
    ok = true;
  } while (false);
  if (buf) { memset(buf, 0, BUF); free(buf); }
  mbedtls_x509write_crt_free(&crt); mbedtls_ctr_drbg_free(&drbg); mbedtls_entropy_free(&ent); mbedtls_pk_free(&pk);
  return ok;
}

static bool loadOrCreate(const String& hostname) {
  IPAddress ip = WiFi.localIP();
  Preferences p;
  p.begin("tls", true);
  size_t cl = p.getBytesLength("crt"), kl = p.getBytesLength("key");
  String host = p.getString("host", "");
  int fmt = p.getInt("fmt", 0);
  uint32_t savedIp = p.getUInt("ip", 0);
  // Reuse the stored certificate only if it was made for this hostname, IP and format.
  if (cl && kl && host == hostname && fmt == CERT_FORMAT && savedIp == (uint32_t)ip) {
    certDer = (uint8_t*)malloc(cl); keyDer = (uint8_t*)malloc(kl);
    if (certDer && keyDer) { certLen = p.getBytes("crt", certDer, cl); keyLen = p.getBytes("key", keyDer, kl); }
  }
  p.end();
  if (certLen && keyLen) return true;
  Serial.println("[tls] generating certificate (first boot)...");
  uint32_t t0 = millis();
  if (!generate(hostname, ip)) { Serial.println("[tls] certificate generation FAILED"); return false; }
  p.begin("tls", false);
  p.putBytes("crt", certDer, certLen); p.putBytes("key", keyDer, keyLen); p.putString("host", hostname);
  p.putInt("fmt", CERT_FORMAT); p.putUInt("ip", (uint32_t)ip);
  p.end();
  Serial.printf("[tls] certificate ready (%u ms)\n", (unsigned)(millis() - t0));
  return true;
}

// The location page. Runs entirely in the browser: asks for the position, then goes back to the
// HTTP settings page with ?lat=&lon=&acc= so the fields can be filled in (nothing is saved yet).
static const char LOCATE_HTML[] = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>SkyDisplay location</title>
<style>:root{color-scheme:dark}body{margin:0;background:#070a12;color:#e2e8f2;font:16px/1.5 system-ui,sans-serif;padding:24px 18px}
main{max-width:520px;margin:0 auto;display:grid;gap:16px}h1{font-size:1.4rem;margin:0}p{margin:0;color:#94a0b8}
button,a.btn{font:inherit;background:#6fb6ff;color:#04101f;border:0;border-radius:10px;padding:14px 18px;font-weight:600;text-align:center;text-decoration:none}
a.btn{background:#1b2742;color:#e2e8f2}#msg{color:#ffb43d;min-height:1.5em}</style></head><body><main>
<h1>Use this phone's location</h1>
<p>Your phone will ask permission to share its location. The coordinates go straight back to the display's settings page; you can check them before saving.</p>
<button id="go" type="button">Share my location</button>
<div id="msg"></div>
<a class="btn" id="back" href="#">Back to settings without location</a>
</main><script>
const back = 'http://' + location.hostname + '/';
document.getElementById('back').href = back;
const msg = t => document.getElementById('msg').textContent = t;
document.getElementById('go').addEventListener('click', () => {
  if (!window.isSecureContext) { msg('This page is not treated as secure by the browser, so location is blocked.'); return; }
  if (!navigator.geolocation) { msg('This browser does not offer location.'); return; }
  msg('Waiting for location...');
  navigator.geolocation.getCurrentPosition(p => {
    const c = p.coords;
    location.href = back + '?lat=' + c.latitude.toFixed(5) + '&lon=' + c.longitude.toFixed(5) + '&acc=' + Math.round(c.accuracy);
  }, e => msg(e.code === 1 ? 'Location permission was denied. Allow it for this page in the browser settings and try again.'
                            : 'Could not get a location (' + e.message + '). Try again outdoors or with location turned on.'),
  { enableHighAccuracy: true, timeout: 20000, maximumAge: 0 });
});
</script></body></html>)HTML";

static esp_err_t handleLocate(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, LOCATE_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handleRedirect(httpd_req_t* req) {  // anything else on HTTPS: go to the location page
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/locate");
  return httpd_resp_send(req, nullptr, 0);
}

bool httpsBegin(const String& hostname) {
  if (server) return true;
  if (!loadOrCreate(hostname)) return false;
  httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
  conf.servercert = certDer; conf.servercert_len = certLen;
  conf.prvtkey_pem = keyDer;  conf.prvtkey_len = keyLen;   // DER is accepted here too
  conf.httpd.max_open_sockets = 2;
  conf.httpd.stack_size = 10240;
  conf.httpd.ctrl_port = 32770;  // keep clear of anything else using the default control port
  conf.httpd.uri_match_fn = httpd_uri_match_wildcard;
  esp_err_t err = httpd_ssl_start(&server, &conf);
  if (err != ESP_OK) { Serial.printf("[tls] HTTPS server failed to start: %s\n", esp_err_to_name(err)); server = nullptr; return false; }
  static const httpd_uri_t locate = {"/locate", HTTP_GET, handleLocate, nullptr};
  static const httpd_uri_t other  = {"/*", HTTP_GET, handleRedirect, nullptr};
  httpd_register_uri_handler(server, &locate);
  httpd_register_uri_handler(server, &other);
  Serial.println("[tls] HTTPS location page on port 443");
  return true;
}
