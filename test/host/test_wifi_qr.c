#include "test.h"
#include "wifi_qr.h"

static void test_join_text(void)
{
    char b[96];
    CHECK(wifi_qr_join_text("VALUMI-3F2A", "ABCD2345", b, sizeof(b)));
    CHECK_EQ_STR(b, "WIFI:T:WPA;S:VALUMI-3F2A;P:ABCD2345;;");
    CHECK(wifi_qr_join_text("a;b", "p:w\\\"", b, sizeof(b)));
    CHECK_EQ_STR(b, "WIFI:T:WPA;S:a\\;b;P:p\\:w\\\\\\\";;");
    CHECK(wifi_qr_join_text("Open,Net", "", b, sizeof(b)));
    CHECK_EQ_STR(b, "WIFI:T:nopass;S:Open\\,Net;;");
}

static void test_overflow_is_reported(void)
{
    char b[16];
    CHECK(!wifi_qr_join_text("VALUMI-3F2A", "ABCD2345", b, sizeof(b)));
    CHECK(strlen(b) < sizeof(b)); /* still terminated */
}

int main(void)
{
    RUN(test_join_text);
    RUN(test_overflow_is_reported);
    TEST_MAIN_END("wifi_qr");
}
