#include "auth_core.h"
#include "test.h"

#define S 1000000LL

static void rnd(uint8_t out[AUTH_TOKEN_BYTES], uint8_t seed)
{
    for (int i = 0; i < AUTH_TOKEN_BYTES; i++) {
        out[i] = (uint8_t)(seed * 31 + i * 7);
    }
}

static void test_hex(void)
{
    uint8_t in[16], out[16];
    char hex[AUTH_TOKEN_HEX + 1];
    rnd(in, 3);
    auth_hex_encode(in, hex);
    CHECK_EQ_INT(strlen(hex), 32);
    CHECK(auth_hex_decode(hex, out));
    CHECK(memcmp(in, out, 16) == 0);
    CHECK(!auth_hex_decode("abc", out));
    CHECK(!auth_hex_decode("ZZ000000000000000000000000000000", out));
    CHECK(!auth_hex_decode("AB000000000000000000000000000000", out)); /* lower case only */
    CHECK(!auth_hex_decode(NULL, out));
}

static void test_sessions(void)
{
    auth_state_t a;
    auth_core_init(&a);
    uint8_t r[16];
    char ids[6][AUTH_TOKEN_HEX + 1];
    for (int i = 0; i < 4; i++) {
        rnd(r, (uint8_t)(i + 1));
        auth_session_create(&a, r, i * S, ids[i]);
    }
    for (int i = 0; i < 4; i++) {
        CHECK(auth_session_check(&a, ids[i], 10 * S + i));
    }
    /* Touch 0 so 1 becomes least recently used, then a fifth evicts 1. */
    CHECK(auth_session_check(&a, ids[0], 20 * S));
    rnd(r, 9);
    auth_session_create(&a, r, 21 * S, ids[4]);
    CHECK(auth_session_check(&a, ids[4], 22 * S));
    CHECK(auth_session_check(&a, ids[0], 22 * S));
    CHECK(!auth_session_check(&a, ids[1], 22 * S));
    CHECK(auth_session_check(&a, ids[2], 22 * S));

    CHECK(!auth_session_check(&a, "", 0));
    CHECK(!auth_session_check(&a, "00000000000000000000000000000000", 0));

    auth_session_drop(&a, ids[2]);
    CHECK(!auth_session_check(&a, ids[2], 23 * S));

}

static void test_qr_single_use(void)
{
    auth_state_t a;
    auth_core_init(&a);
    uint8_t r[16];
    char tok[AUTH_TOKEN_HEX + 1];
    CHECK(!auth_qr_redeem(&a, "00000000000000000000000000000000", 0)); /* none issued */
    rnd(r, 5);
    auth_qr_issue(&a, r, 100 * S, 35 * S, tok);
    CHECK(auth_qr_redeem(&a, tok, 101 * S));
    CHECK(!auth_qr_redeem(&a, tok, 102 * S)); /* spent */
}

static void test_qr_expiry_and_replacement(void)
{
    auth_state_t a;
    auth_core_init(&a);
    uint8_t r[16];
    char t1[AUTH_TOKEN_HEX + 1], t2[AUTH_TOKEN_HEX + 1];
    rnd(r, 1);
    auth_qr_issue(&a, r, 0, 35 * S, t1);
    CHECK(!auth_qr_redeem(&a, t1, 35 * S)); /* exactly at expiry: gone */
    CHECK(!auth_qr_redeem(&a, t1, 0));               /* and stays gone */
    /* The lifetime is whatever the caller asked for: screen time plus grace,
     * not a fixed two minutes. */
    rnd(r, 4);
    auth_qr_issue(&a, r, 0, 20 * S, t1);
    CHECK(!auth_qr_redeem(&a, t1, 20 * S));

    rnd(r, 1);
    auth_qr_issue(&a, r, 0, 35 * S, t1);
    rnd(r, 2);
    auth_qr_issue(&a, r, S, 35 * S, t2); /* showing the screen again voids the old code */
    CHECK(!auth_qr_redeem(&a, t1, 2 * S));
    CHECK(auth_qr_redeem(&a, t2, 2 * S));

    /* A wrong guess does not burn the real token. */
    rnd(r, 3);
    auth_qr_issue(&a, r, 0, 35 * S, t1);
    CHECK(!auth_qr_redeem(&a, "ffffffffffffffffffffffffffffffff", S));
    CHECK(auth_qr_redeem(&a, t1, S));
}

static void test_session_idle_expiry(void)
{
    auth_state_t a;
    auth_core_init(&a);
    uint8_t r[16];
    char id[AUTH_TOKEN_HEX + 1];
    rnd(r, 1);
    auth_session_create(&a, r, 0, id);
    /* Use keeps it alive... */
    CHECK(auth_session_check(&a, id, AUTH_SESSION_IDLE_US - 1));
    CHECK(auth_session_check(&a, id, 2 * AUTH_SESSION_IDLE_US - 2));
    /* ...idleness ends it, and it stays ended. */
    CHECK(!auth_session_check(&a, id, 3 * AUTH_SESSION_IDLE_US));
    CHECK(!auth_session_check(&a, id, 3 * AUTH_SESSION_IDLE_US + 1));
}

static void test_lockout(void)
{
    auth_state_t a;
    auth_core_init(&a);
    for (int i = 0; i < AUTH_MAX_FAILS - 1; i++) {
        CHECK(auth_login_allowed(&a, i * S));
        auth_login_result(&a, false, i * S);
    }
    CHECK(auth_login_allowed(&a, 10 * S));
    auth_login_result(&a, false, 10 * S); /* fifth */
    CHECK(!auth_login_allowed(&a, 10 * S + 1));
    CHECK(!auth_login_allowed(&a, 10 * S + AUTH_LOCKOUT_US - 1));
    CHECK(auth_login_allowed(&a, 10 * S + AUTH_LOCKOUT_US));
    /* The next failure locks again straight away. */
    auth_login_result(&a, false, 50 * S);
    CHECK(!auth_login_allowed(&a, 51 * S));
    /* A success clears the count. */
    auth_login_result(&a, true, 100 * S);
    auth_login_result(&a, false, 101 * S);
    CHECK(auth_login_allowed(&a, 101 * S));
}

static void test_password(void)
{
    CHECK(auth_password_ok("AB3K9", "AB3K9"));
    CHECK(auth_password_ok("ab3k9", "AB3K9")); /* phone keyboards start lower case */
    CHECK(!auth_password_ok("AB3K", "AB3K9"));
    CHECK(!auth_password_ok("AB3K99", "AB3K9"));
    CHECK(!auth_password_ok("AB3K8", "AB3K9"));
    CHECK(!auth_password_ok("", ""));
}

static void test_cookie(void)
{
    char v[40];
    CHECK(auth_cookie_value("sid=abc", "sid", v, sizeof(v)));
    CHECK_EQ_STR(v, "abc");
    CHECK(auth_cookie_value("theme=dark; sid=123; x=y", "sid", v, sizeof(v)));
    CHECK_EQ_STR(v, "123");
    CHECK(!auth_cookie_value("xsid=nope", "sid", v, sizeof(v)));
    CHECK(!auth_cookie_value("sidx=nope", "sid", v, sizeof(v)));
    CHECK(!auth_cookie_value("", "sid", v, sizeof(v)));
    CHECK(!auth_cookie_value(NULL, "sid", v, sizeof(v)));
    char small[4];
    CHECK(!auth_cookie_value("sid=toolong", "sid", small, sizeof(small)));
}

int main(void)
{
    RUN(test_hex);
    RUN(test_sessions);
    RUN(test_qr_single_use);
    RUN(test_qr_expiry_and_replacement);
    RUN(test_session_idle_expiry);
    RUN(test_lockout);
    RUN(test_password);
    RUN(test_cookie);
    TEST_MAIN_END("auth_core");
}
