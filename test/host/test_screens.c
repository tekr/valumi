#include "screens.h"
#include "test.h"

static void test_login_screen(void)
{
    screen_t s;
    screens_login(&s, "LOG IN", 0xC0A8011Eu, "0123456789abcdef0123456789abcdef",
                  "HomeWiFi", "AB3K9");
    CHECK_EQ_STR(s.title, "LOG IN");
    /* The code is a one-time login link to this ticker's own address. */
    CHECK_EQ_STR(s.qr_text, "http://192.168.1.30/login?t=0123456789abcdef0123456789abcdef");
    /* ...and the manual way in: name.local and the address. */
    CHECK_EQ_INT(s.n_lines, 5);
    CHECK_EQ_STR(s.lines[1], " valumi.LOCAL");
    CHECK_EQ_STR(s.lines[2], "*192.168.1.30");
    CHECK_EQ_STR(s.lines[3], "PASSWORD"); /* always: the way in without a camera */
    CHECK_EQ_STR(s.lines[4], "*AB3K9");
}

static void test_login_screen_shows_generated_password(void)
{
    screen_t s;
    screens_login(&s, "CONNECTED", 0xC0A8011Eu, "0123456789abcdef0123456789abcdef",
                  "HomeWiFi", "AB3K9");
    CHECK_EQ_INT(s.n_lines, 5);
    CHECK_EQ_STR(s.lines[3], "PASSWORD");
    CHECK_EQ_STR(s.lines[4], "*AB3K9");
    /* The address and the way in are still there. */
    CHECK_EQ_STR(s.lines[2], "*192.168.1.30");
}

static void test_login_screen_offline(void)
{
    screen_t s;
    screens_login(&s, "LOG IN", 0, "ignored", "HomeWiFi", "AB3K9");
    CHECK_EQ_STR(s.qr_text, ""); /* no code to a place nobody can reach */
    CHECK_EQ_STR(s.lines[0], "NOT CONNECTED YET");
    CHECK_EQ_STR(s.lines[1], " HomeWiFi");
    /* The password still shows: the owner always needs it. */
    CHECK_EQ_STR(s.lines[2], "PASSWORD");
    CHECK_EQ_STR(s.lines[3], "*AB3K9");
}

static void test_setup_screens(void)
{
    screen_t s;
    screens_setup(&s, false, "VALUMI-8381", "ABCD2345");
    /* The join code carries the password, so a phone never types it. */
    CHECK_EQ_STR(s.qr_text, "WIFI:T:WPA;S:VALUMI-8381;P:ABCD2345;;");
    CHECK_EQ_STR(s.lines[1], " VALUMI-8381");
    CHECK_EQ_STR(s.lines[3], "*ABCD2345");
    /* The address shows before anyone joins, as well as after. */
    CHECK(strstr(s.lines[4], "4.3.2.1") != NULL);
    CHECK_EQ_INT(s.n_lines, SCREEN_MAX_LINES);

    screens_setup(&s, true, "VALUMI-8381", "ABCD2345");
    CHECK_EQ_STR(s.qr_text, "http://4.3.2.1/");
    CHECK_EQ_STR(s.lines[1], "*4.3.2.1");
}

static void test_screen_bounds(void)
{
    /* A 32-character SSID and the longest address still fit, terminated. */
    screen_t s;
    screens_login(&s, "CONNECTED", 0, "", "12345678901234567890123456789012", "AB3K9");
    for (int i = 0; i < s.n_lines; i++) {
        CHECK(strlen(s.lines[i]) < SCREEN_LINE_MAX);
    }
    screens_login(&s, "CONNECTED", 0xFFFFFFFEu, "0123456789abcdef0123456789abcdef", "", "AB3K9");
    CHECK_EQ_STR(s.lines[2], "*255.255.255.254");
    CHECK(strlen(s.qr_text) < sizeof(s.qr_text) - 1);
}

int main(void)
{
    RUN(test_login_screen);
    RUN(test_login_screen_shows_generated_password);
    RUN(test_login_screen_offline);
    RUN(test_setup_screens);
    RUN(test_screen_bounds);
    TEST_MAIN_END("screens");
}
