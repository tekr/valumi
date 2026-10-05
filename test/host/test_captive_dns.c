#include "captive_dns.h"
#include "test.h"

/* A query for "a.io" of type @p qtype, as a phone would send it. */
static size_t make_query(uint8_t *q, uint16_t qtype)
{
    static const uint8_t base[] = {
        0x12, 0x34,             /* id */
        0x01, 0x00,             /* standard query, RD */
        0x00, 0x01,             /* QDCOUNT 1 */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        1, 'a', 2, 'i', 'o', 0, /* a.io */
        0x00, 0x01,             /* type, patched below */
        0x00, 0x01,             /* class IN */
    };
    memcpy(q, base, sizeof(base));
    q[18] = (uint8_t)(qtype >> 8);
    q[19] = (uint8_t)qtype;
    return sizeof(base);
}

static void test_a_record(void)
{
    uint8_t q[64], r[128];
    size_t n = make_query(q, 1);
    int len = captive_dns_reply(q, n, r, sizeof(r), 0xC0A80401u);
    CHECK_EQ_INT(len, (int)n + 16);
    CHECK(r[0] == 0x12 && r[1] == 0x34); /* id echoed */
    CHECK(r[2] & 0x80);                  /* a response */
    CHECK(r[2] & 0x01);                  /* RD echoed */
    CHECK_EQ_INT(r[3] & 0x0F, 0);        /* NOERROR */
    CHECK_EQ_INT(r[7], 1);               /* one answer */
    CHECK(memcmp(r + 12, q + 12, n - 12) == 0);
    const uint8_t *a = r + n;
    CHECK(a[0] == 0xC0 && a[1] == 12); /* name points at the question */
    CHECK(a[3] == 1 && a[5] == 1);     /* A, IN */
    CHECK(a[11] == 4);
    CHECK(a[12] == 192 && a[13] == 168 && a[14] == 4 && a[15] == 1);
}

static void test_other_types_get_empty_answer(void)
{
    uint8_t q[64], r[128];
    size_t n = make_query(q, 28); /* AAAA */
    int len = captive_dns_reply(q, n, r, sizeof(r), 0xC0A80401u);
    CHECK_EQ_INT(len, (int)n);
    CHECK(r[2] & 0x80);
    CHECK_EQ_INT(r[7], 0);
    CHECK_EQ_INT(r[3] & 0x0F, 0);
}

static void test_refusals(void)
{
    uint8_t q[64], r[128];
    size_t n = make_query(q, 1);

    CHECK_EQ_INT(captive_dns_reply(q, 11, r, sizeof(r), 1), -1); /* short header */
    CHECK_EQ_INT(captive_dns_reply(q, n - 1, r, sizeof(r), 1), -1); /* truncated question */
    CHECK_EQ_INT(captive_dns_reply(q, n, r, n + 15, 1), -1); /* no room for the answer */

    uint8_t resp[64];
    memcpy(resp, q, n);
    resp[2] |= 0x80; /* already a response: answering would start a loop */
    CHECK_EQ_INT(captive_dns_reply(resp, n, r, sizeof(r), 1), -1);

    uint8_t two[64];
    memcpy(two, q, n);
    two[5] = 2; /* two questions */
    CHECK_EQ_INT(captive_dns_reply(two, n, r, sizeof(r), 1), -1);

    uint8_t ptr[64];
    memcpy(ptr, q, n);
    ptr[12] = 0xC0; /* compression pointer in a query */
    CHECK_EQ_INT(captive_dns_reply(ptr, n, r, sizeof(r), 1), -1);

    uint8_t runaway[64];
    memcpy(runaway, q, n);
    runaway[12] = 60; /* label runs past the end */
    CHECK_EQ_INT(captive_dns_reply(runaway, n, r, sizeof(r), 1), -1);
}

int main(void)
{
    RUN(test_a_record);
    RUN(test_other_types_get_empty_answer);
    RUN(test_refusals);
    TEST_MAIN_END("captive_dns");
}
