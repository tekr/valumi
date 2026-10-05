/* The pure half of captive_dns.h, kept apart so the host tests can build it
 * without lwIP. */
#include "captive_dns.h"

#include <string.h>

#define HDR_LEN 12
#define TYPE_A 1
#define CLASS_IN 1
#define TTL_S 60

int captive_dns_reply(const uint8_t *q, size_t qlen, uint8_t *out, size_t cap, uint32_t ip)
{
    if (qlen < HDR_LEN) {
        return -1;
    }
    uint16_t flags = (uint16_t)(q[2] << 8 | q[3]);
    uint16_t qdcount = (uint16_t)(q[4] << 8 | q[5]);
    if ((flags & 0x8000) != 0) {
        return -1; /* a response, not a query: never answer those */
    }
    if (((flags >> 11) & 0x0F) != 0 || qdcount != 1) {
        return -1; /* only standard queries with exactly one question */
    }

    /* Walk the QNAME. Queries never use compression, so a pointer here is
     * malformed and refused rather than followed. */
    size_t p = HDR_LEN;
    for (;;) {
        if (p >= qlen) {
            return -1;
        }
        uint8_t len = q[p];
        if (len == 0) {
            p++;
            break;
        }
        if (len & 0xC0) {
            return -1;
        }
        p += 1 + (size_t)len;
    }
    if (p + 4 > qlen) {
        return -1;
    }
    uint16_t qtype = (uint16_t)(q[p] << 8 | q[p + 1]);
    uint16_t qclass = (uint16_t)(q[p + 2] << 8 | q[p + 3]);
    size_t qend = p + 4; /* header + the one question, echoed back */

    bool answer = qtype == TYPE_A && qclass == CLASS_IN;
    size_t need = qend + (answer ? 16 : 0);
    if (need > cap) {
        return -1;
    }

    memcpy(out, q, qend);
    /* QR=1, opcode 0, AA=1, keep RD from the query, RA=0, RCODE 0. */
    out[2] = (uint8_t)(0x80 | 0x04 | (q[2] & 0x01));
    out[3] = 0x00;
    out[6] = 0;
    out[7] = answer ? 1 : 0; /* ANCOUNT */
    out[8] = out[9] = 0;     /* NSCOUNT */
    out[10] = out[11] = 0;   /* ARCOUNT: any EDNS record in the query is dropped */

    if (!answer) {
        return (int)qend;
    }
    uint8_t *a = out + qend;
    a[0] = 0xC0; /* name: pointer to the question's name at offset 12 */
    a[1] = HDR_LEN;
    a[2] = 0;
    a[3] = TYPE_A;
    a[4] = 0;
    a[5] = CLASS_IN;
    a[6] = 0;
    a[7] = 0;
    a[8] = 0;
    a[9] = TTL_S;
    a[10] = 0;
    a[11] = 4;
    a[12] = (uint8_t)(ip >> 24);
    a[13] = (uint8_t)(ip >> 16);
    a[14] = (uint8_t)(ip >> 8);
    a[15] = (uint8_t)ip;
    return (int)need;
}
