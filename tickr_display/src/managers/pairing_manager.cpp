#include "pairing_manager.h"
#include "../log.h"
#include "web_auth.h"
#include "peer_manager.h"
#include "../logic/pairing.h"
#include "../logic/peer_table.h"
#include "../hal/hal_display.h"
#include "../hal/hal_indication.h"
#include "../hal/hal_power.h"

#include <mbedtls/md.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/base64.h>
#include <esp_system.h>

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static const char     CT_JSON[]        = "application/json";
static const uint32_t MIN_FREE_BLOCK   = 32 * 1024;     // never starve the pull path
static const uint32_t SECRET_RATE_MS   = 2000;          // one GET /api/group/secret per 2 s
static const uint32_t SECRET_TTL_MS    = 15000;         // a computed box waits this long to be fetched
static const char     COMPUTING[]      = "{\"status\":\"computing\",\"retry_after_ms\":300}";

enum PairState : uint8_t { PS_IDLE, PS_STARTING, PS_PAIRING, PS_COOLDOWN };

struct Session {
    uint8_t  sid[4];
    uint8_t  pk_i[PAIR_PK_LEN];
    uint8_t  pk_d[PAIR_PK_LEN];
    uint8_t  n_d[PAIR_NONCE_LEN];
    PairKeys keys;
    char     code[PAIR_CODE_DIGITS + 1];
    uint8_t  attempts;
    bool     rekey;
    uint32_t deadline_ms;
};

static AppConfig*   s_cfg = nullptr;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile PairState s_state = PS_IDLE;
static Session      s_sess;
static uint8_t      s_streak = 0;                  // exhausted sessions in a row (cooldown exponent)
static uint32_t     s_last_fail_ms = 0;
static uint32_t     s_cooldown_until_ms = 0;
// Outcome frame (drawn partial, docs/DEVICE_UI.md "E-ink refresh rules"): the handler records what to show, the
// main task draws it; the display HAL holds it RESULT_HOLD_MS and restores.
enum PairResult : uint8_t { PR_NONE, PR_OK, PR_FAILED, PR_CANCELLED };
static const uint32_t RESULT_HOLD_MS = 5000;
static volatile PairResult s_result_pending = PR_NONE;   // main task: draw the outcome frame
static volatile bool s_bye_pending = false;        // main task: one beacon right after leave
static char         s_last_error[32] = "";

// GET /api/group/secret work item: the handler records pk_i, the main task
// computes the box, the next GET with the same pk_i collects it.
enum GsState : uint8_t { GS_NONE, GS_REQUESTED, GS_READY, GS_FAILED };
static volatile GsState s_gs_state = GS_NONE;
static uint8_t  s_gs_pk_i[PAIR_PK_LEN], s_gs_pk_a[PAIR_PK_LEN], s_gs_n_a[PAIR_NONCE_LEN], s_gs_box[GROUP_BOX_LEN];
static uint32_t s_gs_ready_ms = 0, s_gs_last_req_ms = 0;

// ---------------------------------------------------------------------------
// Crypto ports (mbedTLS)
// ---------------------------------------------------------------------------
static void hmac_port(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len, uint8_t out[32]) {
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, key_len, msg, msg_len, out);
}

static int rng_cb(void*, unsigned char* buf, size_t len) {
    esp_fill_random(buf, len);
    return 0;
}

// out = sk * (peer ? peer : base point) on Curve25519 (RFC 7748 clamping and
// high-bit masking). False on a bad point or an all-zero result.
static bool x25519(const uint8_t sk[32], const uint8_t* peer, uint8_t out[32]) {
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_mpi d, z;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);
    uint8_t k[32];
    memcpy(k, sk, 32);
    k[0] &= 248; k[31] &= 127; k[31] |= 64;
    int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    if (rc == 0) rc = mbedtls_mpi_read_binary_le(&d, k, 32);
    if (rc == 0) {
        if (peer) {
            uint8_t u[32];
            memcpy(u, peer, 32);
            u[31] &= 127;
            rc = mbedtls_mpi_read_binary_le(&Q.X, u, 32);
            if (rc == 0) rc = mbedtls_mpi_lset(&Q.Z, 1);
            if (rc == 0) rc = mbedtls_ecdh_compute_shared(&grp, &z, &Q, &d, rng_cb, nullptr);
            if (rc == 0) rc = mbedtls_mpi_write_binary_le(&z, out, 32);
        } else {
            rc = mbedtls_ecp_mul(&grp, &Q, &d, &grp.G, rng_cb, nullptr);
            if (rc == 0) rc = mbedtls_mpi_write_binary_le(&Q.X, out, 32);
        }
    }
    mbedtls_ecp_group_free(&grp);
    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&d);
    mbedtls_mpi_free(&z);
    memset(k, 0, sizeof(k));
    if (rc != 0) return false;
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= out[i];
    return acc != 0;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void b64_str(const uint8_t* in, size_t len, char* out, size_t cap) {
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char*)out, cap, &olen, in, len) != 0) olen = 0;
    out[olen] = '\0';
}

// Base64 parameter (POST form field or query string) of exactly `want` bytes.
static bool b64_param(AsyncWebServerRequest* r, const char* name, uint8_t* out, size_t want) {
    const AsyncWebParameter* p = r->hasParam(name, true) ? r->getParam(name, true) : (r->hasParam(name) ? r->getParam(name) : nullptr);
    if (!p) return false;
    String v = p->value();
    v.replace(' ', '+');                          // an unencoded '+' in a form arrives as a space
    uint8_t buf[GROUP_BOX_LEN + 8];
    size_t olen = 0;
    if (mbedtls_base64_decode(buf, sizeof(buf), &olen, (const unsigned char*)v.c_str(), v.length()) != 0 || olen != want) return false;
    memcpy(out, buf, want);
    return true;
}

static void send_error(AsyncWebServerRequest* r, int code, const char* msg, uint32_t retry_after_s = 0) {
    char body[96];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    AsyncWebServerResponse* resp = r->beginResponse(code, CT_JSON, body);
    if (retry_after_s) resp->addHeader("Retry-After", String(retry_after_s));
    r->send(resp);
}

static bool have_group() {
    return s_cfg && s_cfg->group_id[0] && s_cfg->group_secret[0];
}

static bool creds_from_cfg(GroupCreds* c) {
    memset(c, 0, sizeof(*c));
    if (!have_group()) return false;
    if (!pairing_unhex(s_cfg->group_id, c->id, GROUP_ID_LEN) || !pairing_unhex(s_cfg->group_secret, c->secret, GROUP_SECRET_LEN)) return false;
    c->epoch = s_cfg->group_epoch;
    strlcpy(c->name, s_cfg->group_name, sizeof(c->name));
    return true;
}

static void publish_group() {
    GroupCreds c;
    if (creds_from_cfg(&c)) peers_set_group(s_cfg->group_id, c.epoch, c.secret);
    else peers_set_group(nullptr, 0, nullptr);
    memset(&c, 0, sizeof(c));
}

// Writes credentials (or wipes them with c == NULL) into config.json atomically.
static bool adopt(const GroupCreds* c) {
    char old_id[sizeof(s_cfg->group_id)], old_secret[sizeof(s_cfg->group_secret)], old_name[sizeof(s_cfg->group_name)];
    uint16_t old_epoch = s_cfg->group_epoch;
    memcpy(old_id, s_cfg->group_id, sizeof(old_id));
    memcpy(old_secret, s_cfg->group_secret, sizeof(old_secret));
    memcpy(old_name, s_cfg->group_name, sizeof(old_name));
    if (c) {
        pairing_hex(c->id, GROUP_ID_LEN, s_cfg->group_id);
        pairing_hex(c->secret, GROUP_SECRET_LEN, s_cfg->group_secret);
        strlcpy(s_cfg->group_name, c->name, sizeof(s_cfg->group_name));
        s_cfg->group_epoch = c->epoch;
    } else {
        s_cfg->group_id[0] = '\0';
        s_cfg->group_secret[0] = '\0';
        s_cfg->group_name[0] = '\0';
        s_cfg->group_epoch = 0;
    }
    if (!config_save(*s_cfg)) {
        memcpy(s_cfg->group_id, old_id, sizeof(old_id));
        memcpy(s_cfg->group_secret, old_secret, sizeof(old_secret));
        memcpy(s_cfg->group_name, old_name, sizeof(old_name));
        s_cfg->group_epoch = old_epoch;
        return false;
    }
    memset(old_secret, 0, sizeof(old_secret));
    publish_group();
    return true;
}

// `result` = what the screen shows next (PR_NONE: nothing was drawn, e.g. rekey).
static void close_session(PairResult result) {
    portENTER_CRITICAL(&s_mux);
    memset(&s_sess, 0, sizeof(s_sess));
    if (s_state != PS_COOLDOWN) s_state = PS_IDLE;
    portEXIT_CRITICAL(&s_mux);
    if (result != PR_NONE) s_result_pending = result;
}

static void fail(const char* why) {
    strlcpy(s_last_error, why, sizeof(s_last_error));
    close_session(PR_FAILED);
}

// Sanitised copy for JSON output (config.json may be hand-edited).
static void safe_copy(const char* src, char* dst, size_t dst_len) {
    size_t i = 0;
    for (; src[i] && i + 1 < dst_len; i++) {
        unsigned char ch = (unsigned char)src[i];
        dst[i] = (ch < 0x20 || ch > 0x7E || ch == '"' || ch == '\\') ? '_' : (char)ch;
    }
    dst[i] = '\0';
}

static void group_object(char* out, size_t out_len) {
    char name[CONFIG_NAME_LEN];
    safe_copy(s_cfg->group_name, name, sizeof(name));
    snprintf(out, out_len, "{\"id\":\"%s\",\"name\":\"%s\",\"epoch\":%u}", s_cfg->group_id, name, (unsigned)s_cfg->group_epoch);
}

// ---------------------------------------------------------------------------
// Main-task work
// ---------------------------------------------------------------------------
static void start_session() {
    uint8_t sk[32], K[32];
    esp_fill_random(sk, sizeof(sk));
    uint32_t t0 = millis();
    bool ok = x25519(sk, nullptr, s_sess.pk_d) && x25519(sk, s_sess.pk_i, K);
    memset(sk, 0, sizeof(sk));
    LOGV("[Pair] session %s: 2x X25519 in %lu ms\n", ok ? "start" : "rejected (bad public key)", (unsigned long)(millis() - t0));
    if (!ok) {
        strlcpy(s_last_error, "bad public key", sizeof(s_last_error));
        close_session(PR_NONE);                      // nothing was drawn yet
        return;
    }
    esp_fill_random(s_sess.n_d, sizeof(s_sess.n_d));
    pairing_derive(K, s_sess.n_d, s_sess.pk_i, s_sess.pk_d, &s_sess.keys);
    memset(K, 0, sizeof(K));
    s_sess.attempts = 0;
    s_last_error[0] = '\0';
    if (!s_sess.rekey) {
        pairing_code_from_rng(esp_random, s_sess.code);
        char name[CONFIG_NAME_LEN];
        if (s_cfg->device_name[0]) strlcpy(name, s_cfg->device_name, sizeof(name));
        else peers_default_name(name, sizeof(name));
        display_show_pairing(s_sess.code, s_sess.keys.chk, name, have_group() ? s_cfg->group_name : nullptr, PAIR_WINDOW_S);
        indication_overlay(LED_OVL_PAIRING, true);   // amber while the code is up (docs/DEVICE_UI.md "LED and sound")
    }
    s_sess.deadline_ms = millis() + PAIR_WINDOW_S * 1000UL;
    portENTER_CRITICAL(&s_mux);
    bool still = s_state == PS_STARTING;
    if (still) s_state = PS_PAIRING;
    portEXIT_CRITICAL(&s_mux);
    if (!still) close_session(s_sess.rekey ? PR_NONE : PR_CANCELLED);   // cancelled while we were computing
}

static void compute_group_secret() {
    GroupCreds c;
    uint8_t sk[32], K[32], ke[32], packed[GROUP_CREDS_LEN];
    esp_fill_random(sk, sizeof(sk));
    bool ok = creds_from_cfg(&c) && x25519(sk, nullptr, s_gs_pk_a) && x25519(sk, s_gs_pk_i, K);
    memset(sk, 0, sizeof(sk));
    if (ok) {
        esp_fill_random(s_gs_n_a, sizeof(s_gs_n_a));
        pairing_group_box_key(K, s_gs_n_a, s_gs_pk_i, s_gs_pk_a, ke);
        pairing_creds_pack(c, packed);
        pairing_box_seal(ke, packed, sizeof(packed), s_gs_box);
        s_gs_ready_ms = millis();
    }
    memset(K, 0, sizeof(K));
    memset(ke, 0, sizeof(ke));
    memset(packed, 0, sizeof(packed));
    memset(&c, 0, sizeof(c));
    s_gs_state = ok ? GS_READY : GS_FAILED;
}

void pairing_loop() {
    uint32_t now = millis();
    if (s_state == PS_STARTING) {
        start_session();
    } else if (s_state == PS_PAIRING && (int32_t)(now - s_sess.deadline_ms) >= 0) {
        Serial.println("[Pair] window expired");
        bool rekey = s_sess.rekey;
        strlcpy(s_last_error, "timeout", sizeof(s_last_error));
        close_session(rekey ? PR_NONE : PR_FAILED);
    } else if (s_state == PS_COOLDOWN && (int32_t)(now - s_cooldown_until_ms) >= 0) {
        s_state = PS_IDLE;
    }
    if (s_streak && (int32_t)(now - s_last_fail_ms) > (int32_t)(PAIR_STREAK_RESET_S * 1000UL)) s_streak = 0;

    if (s_gs_state == GS_REQUESTED) compute_group_secret();
    if (s_gs_state == GS_READY && (int32_t)(now - s_gs_ready_ms) > (int32_t)SECRET_TTL_MS) {
        memset(s_gs_box, 0, sizeof(s_gs_box));
        s_gs_state = GS_NONE;
    }
    // Outcome frame: draw once, the HAL holds it and restores
    // the content - unless a new session puts its own screen up meanwhile.
    PairResult res = s_result_pending;
    if (res != PR_NONE) {
        s_result_pending = PR_NONE;
        indication_overlay(LED_OVL_PAIRING, false);
        if (display_overlay_active()) {
            if (res == PR_OK) indication_flash(0, 255, 0, 1000);    // outcome: green / red 1 s
            else indication_flash(255, 0, 0, 1000);
            char big[CONFIG_NAME_LEN + 8];
            if (res == PR_OK) snprintf(big, sizeof(big), "Paired: %s", s_cfg->group_name);
            else strcpy(big, res == PR_FAILED ? "Pairing failed" : "Pairing cancelled");
            display_show_pairing_result(big, res == PR_FAILED ? s_last_error : "");
            display_temp_hold(RESULT_HOLD_MS);
        }
    }
    if (s_bye_pending) {
        s_bye_pending = false;
        peers_send_beacon_now();
    }
}

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------
static void h_start(AsyncWebServerRequest* r) {
    REQUIRE_GROUP_AUTH(r);
    uint8_t pk[PAIR_PK_LEN];
    if (!b64_param(r, "pk", pk, sizeof(pk))) { send_error(r, 400, "pk (base64, 32 bytes) required"); return; }
    bool rekey = r->hasParam("mode", true) && r->getParam("mode", true)->value() == "rekey";
    if (rekey && !have_group()) { send_error(r, 403, "no group to rekey"); return; }
    uint32_t now = millis();
    PairState st = s_state;
    bool cooldown = st == PS_COOLDOWN && (int32_t)(now - s_cooldown_until_ms) < 0;
    bool open = st == PS_STARTING || st == PS_PAIRING;
    int code = pairing_start_check(power_get_source() == POWER_USB, open, cooldown, ESP.getMaxAllocHeap() >= MIN_FREE_BLOCK);
    if (code != 200) {
        const char* msg = code == 403 ? "pairable only on USB power" : code == 409 ? "pairing session already open"
                        : code == 429 ? "cooldown after failed attempts" : "low memory, retry later";
        send_error(r, code, msg, code == 429 ? (s_cooldown_until_ms - now) / 1000 + 1 : (code == 503 ? 5 : 0));
        return;
    }
    portENTER_CRITICAL(&s_mux);
    memset(&s_sess, 0, sizeof(s_sess));
    esp_fill_random(s_sess.sid, sizeof(s_sess.sid));
    memcpy(s_sess.pk_i, pk, sizeof(pk));
    s_sess.rekey = rekey;
    s_state = PS_STARTING;
    portEXIT_CRITICAL(&s_mux);
    char sid[9], body[80];
    pairing_hex(s_sess.sid, 4, sid);
    snprintf(body, sizeof(body), "{\"sid\":\"%s\",\"state\":\"starting\",\"mode\":\"%s\"}", sid, rekey ? "rekey" : "code");
    r->send(202, CT_JSON, body);
}

static void h_status(AsyncWebServerRequest* r) {
    uint32_t now = millis();
    char body[320];
    PairState st = s_state;
    if (st == PS_PAIRING) {
        char sid[9], pk[48], n[28];
        pairing_hex(s_sess.sid, 4, sid);
        b64_str(s_sess.pk_d, sizeof(s_sess.pk_d), pk, sizeof(pk));
        b64_str(s_sess.n_d, sizeof(s_sess.n_d), n, sizeof(n));
        int32_t left = (int32_t)(s_sess.deadline_ms - now);
        snprintf(body, sizeof(body),
                 "{\"state\":\"pairing\",\"sid\":\"%s\",\"pk_d\":\"%s\",\"n_d\":\"%s\",\"chk\":\"%s\",\"expires_s\":%ld,"
                 "\"attempts_left\":%u,\"mode\":\"%s\"}",
                 sid, pk, n, s_sess.keys.chk, (long)(left > 0 ? left / 1000 : 0),
                 (unsigned)(PAIR_MAX_ATTEMPTS - s_sess.attempts), s_sess.rekey ? "rekey" : "code");
    } else if (st == PS_STARTING) {
        strcpy(body, "{\"state\":\"starting\"}");
    } else if (st == PS_COOLDOWN) {
        int32_t left = (int32_t)(s_cooldown_until_ms - now);
        snprintf(body, sizeof(body), "{\"state\":\"cooldown\",\"retry_after_s\":%ld}", (long)(left > 0 ? left / 1000 + 1 : 0));
    } else {
        snprintf(body, sizeof(body), "{\"state\":\"idle\",\"pairable\":%s,\"error\":\"%s\"}",
                 power_get_source() == POWER_USB ? "true" : "false", s_last_error);
    }
    AsyncWebServerResponse* resp = r->beginResponse(200, CT_JSON, body);
    resp->addHeader("Cache-Control", "no-store");
    r->send(resp);
}

static void h_confirm(AsyncWebServerRequest* r) {
    REQUIRE_GROUP_AUTH(r);
    uint8_t sid[4];
    bool sid_ok = r->hasParam("sid", true) && pairing_unhex(r->getParam("sid", true)->value().c_str(), sid, 4);
    if (s_state != PS_PAIRING || !sid_ok || memcmp(sid, s_sess.sid, 4) != 0 || (int32_t)(millis() - s_sess.deadline_ms) >= 0) {
        send_error(r, 410, "no open pairing session for this sid");
        return;
    }
    uint8_t ci[32], want[32];
    if (!b64_param(r, "ci", ci, sizeof(ci))) { send_error(r, 400, "ci (base64, 32 bytes) required"); return; }
    GroupCreds cur;
    if (s_sess.rekey) {
        creds_from_cfg(&cur);
        pairing_confirm_rekey(s_sess.keys.kc, cur.secret, want);
    } else {
        pairing_confirm_i(s_sess.keys.kc, s_sess.code, want);
    }
    if (!pairing_ct_equal(ci, want, sizeof(want))) {
        s_sess.attempts++;
        s_last_fail_ms = millis();
        if (s_sess.attempts >= PAIR_MAX_ATTEMPTS) {
            if (s_streak < 6) s_streak++;
            uint32_t cd = pairing_cooldown_s(s_streak);
            s_cooldown_until_ms = millis() + cd * 1000UL;
            strlcpy(s_last_error, "too many wrong codes", sizeof(s_last_error));
            Serial.printf("[Pair] 3 wrong codes, cooldown %lu s\n", (unsigned long)cd);
            portENTER_CRITICAL(&s_mux);
            s_state = PS_COOLDOWN;
            portEXIT_CRITICAL(&s_mux);
            close_session(s_sess.rekey ? PR_NONE : PR_FAILED);
            send_error(r, 429, "too many wrong codes", cd);
        } else {
            char body[64];
            snprintf(body, sizeof(body), "{\"error\":\"wrong code\",\"attempts_left\":%u}", (unsigned)(PAIR_MAX_ATTEMPTS - s_sess.attempts));
            r->send(401, CT_JSON, body);
        }
        return;
    }

    // Confirmed. JOIN / rekey carry the credentials in the box; CREATE makes them here.
    GroupCreds c;
    memset(&c, 0, sizeof(c));
    if (r->hasParam("box", true)) {
        uint8_t box[GROUP_BOX_LEN], plain[GROUP_CREDS_LEN];
        bool ok = b64_param(r, "box", box, sizeof(box)) && pairing_box_open(s_sess.keys.ke, box, sizeof(box), plain) &&
                  pairing_creds_unpack(plain, &c);
        memset(plain, 0, sizeof(plain));
        if (!ok) { fail("box does not verify"); send_error(r, 400, "box does not verify"); return; }
    } else if (s_sess.rekey) {
        close_session(PR_NONE);
        send_error(r, 400, "box required for rekey");
        return;
    } else {
        if (have_group()) { fail("already in a group"); send_error(r, 403, "already in a group - leave it first"); return; }
        esp_fill_random(c.id, sizeof(c.id));
        esp_fill_random(c.secret, sizeof(c.secret));
        c.epoch = 1;
        String name = r->hasParam("name", true) ? r->getParam("name", true)->value() : String("Group");
        name.trim();
        if (name.length() == 0 || name.length() > GROUP_NAME_MAX) name = "Group";
        safe_copy(name.c_str(), c.name, sizeof(c.name));
    }
    uint8_t cd[32];
    if (s_sess.rekey) pairing_confirm_rekey_d(s_sess.keys.kc, c.secret, cd);
    else pairing_confirm_d(s_sess.keys.kc, s_sess.code, cd);
    if (!adopt(&c)) { fail("config write failed"); send_error(r, 500, "failed to write configuration"); return; }
    memset(&c, 0, sizeof(c));
    s_streak = 0;
    Serial.printf("[Pair] %s group %s (epoch %u)\n", s_sess.rekey ? "rekeyed" : "joined", s_cfg->group_id, (unsigned)s_cfg->group_epoch);
    char cdb[48], grp[128], body[200];
    b64_str(cd, sizeof(cd), cdb, sizeof(cdb));
    group_object(grp, sizeof(grp));
    snprintf(body, sizeof(body), "{\"c_d\":\"%s\",\"group\":%s}", cdb, grp);
    close_session(s_sess.rekey ? PR_NONE : PR_OK);   // "Paired: <name>" frame, then the content
    r->send(200, CT_JSON, body);
}

static void h_cancel(AsyncWebServerRequest* r) {
    REQUIRE_GROUP_AUTH(r);
    if (s_state == PS_STARTING || s_state == PS_PAIRING) {
        strlcpy(s_last_error, "cancelled", sizeof(s_last_error));
        close_session(s_sess.rekey ? PR_NONE : PR_CANCELLED);
    }
    r->send(200, CT_JSON, "{\"ok\":true}");
}

static void h_group(AsyncWebServerRequest* r) {
    REQUIRE_GROUP_AUTH(r);
    if (!have_group()) { send_error(r, 404, "no group"); return; }
    char grp[128], body[160];
    group_object(grp, sizeof(grp));
    grp[strlen(grp) - 1] = '\0';                      // reopen the object for one more field
    snprintf(body, sizeof(body), "%s,\"members\":%u}", grp, (unsigned)peers_member_count());
    r->send(200, CT_JSON, body);
}

static void h_group_secret(AsyncWebServerRequest* r) {
    REQUIRE_GROUP_AUTH(r);
    if (!have_group()) { send_error(r, 404, "no group"); return; }
    uint8_t pk[PAIR_PK_LEN];
    if (!b64_param(r, "pk", pk, sizeof(pk))) { send_error(r, 400, "pk (base64, 32 bytes) required"); return; }
    uint32_t now = millis();
    GsState st = s_gs_state;
    bool same = memcmp(pk, s_gs_pk_i, sizeof(pk)) == 0;
    if (st == GS_READY && same) {
        char pka[48], na[28], box[128], body[256];
        b64_str(s_gs_pk_a, sizeof(s_gs_pk_a), pka, sizeof(pka));
        b64_str(s_gs_n_a, sizeof(s_gs_n_a), na, sizeof(na));
        b64_str(s_gs_box, sizeof(s_gs_box), box, sizeof(box));
        snprintf(body, sizeof(body), "{\"pk_a\":\"%s\",\"n_a\":\"%s\",\"box\":\"%s\"}", pka, na, box);
        memset(s_gs_box, 0, sizeof(s_gs_box));
        s_gs_state = GS_NONE;
        AsyncWebServerResponse* resp = r->beginResponse(200, CT_JSON, body);
        resp->addHeader("Cache-Control", "no-store");
        r->send(resp);
        return;
    }
    if (st == GS_FAILED && same) { s_gs_state = GS_NONE; send_error(r, 400, "bad public key"); return; }
    if (st == GS_REQUESTED) {
        if (same) r->send(202, CT_JSON, COMPUTING);
        else send_error(r, 409, "another request is being served", 1);
        return;
    }
    if (st != GS_NONE && !same) { s_gs_state = GS_NONE; }   // stale result for someone else: drop it
    if ((int32_t)(now - s_gs_last_req_ms) < (int32_t)SECRET_RATE_MS) { send_error(r, 429, "one request per 2 s", 2); return; }
    s_gs_last_req_ms = now;
    memcpy(s_gs_pk_i, pk, sizeof(pk));
    s_gs_state = GS_REQUESTED;                         // main task computes, repeat the GET to collect
    r->send(202, CT_JSON, COMPUTING);
}

bool pairing_leave_group() {
    if (!have_group() || !adopt(nullptr)) return false;
    s_bye_pending = true;                              // peers see an unsigned beacon -> member:false
    Serial.println("[Pair] left the group");
    return true;
}

static void h_leave(AsyncWebServerRequest* r) {
    REQUIRE_GROUP_AUTH(r);
    if (!have_group()) { send_error(r, 404, "no group"); return; }
    if (!pairing_leave_group()) { send_error(r, 500, "failed to write configuration"); return; }
    r->send(200, CT_JSON, "{\"ok\":true}");
}

static void h_rekey(AsyncWebServerRequest* r) {
    REQUIRE_GROUP_AUTH(r);
    GroupCreds c;
    if (!creds_from_cfg(&c)) { send_error(r, 404, "no group"); return; }
    esp_fill_random(c.secret, sizeof(c.secret));
    c.epoch++;
    bool ok = adopt(&c);
    memset(&c, 0, sizeof(c));
    if (!ok) { send_error(r, 500, "failed to write configuration"); return; }
    char grp[128];
    group_object(grp, sizeof(grp));
    r->send(200, CT_JSON, grp);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void pairing_begin(AppConfig* cfg) {
    s_cfg = cfg;
    g_hmac_sha256 = hmac_port;
    publish_group();
    if (have_group()) LOGV("[Pair] member of group %s (epoch %u)\n", cfg->group_id, (unsigned)cfg->group_epoch);
}

void pairing_register_routes(AsyncWebServer& server) {
    server.on("/api/pair/start", HTTP_POST, h_start);
    server.on("/api/pair/status", HTTP_GET, h_status);
    server.on("/api/pair/confirm", HTTP_POST, h_confirm);
    server.on("/api/pair/cancel", HTTP_POST, h_cancel);
    server.on("/api/pair", HTTP_DELETE, h_cancel);
    // ESPAsyncWebServer matches a plain URI as "equal OR startsWith(uri + '/')"
    // and the first registered handler wins, so the specific GET route must be
    // registered before the shorter "/api/group" or the latter would shadow it.
    server.on("/api/group/secret", HTTP_GET, h_group_secret);
    server.on("/api/group/leave", HTTP_POST, h_leave);
    server.on("/api/group/rekey", HTTP_POST, h_rekey);
    server.on("/api/group", HTTP_GET, h_group);
}

bool pairing_has_group() {
    return have_group();
}

void pairing_group_json(char* out, size_t out_len) {
    if (have_group()) group_object(out, out_len);
    else strlcpy(out, "null", out_len);
}
