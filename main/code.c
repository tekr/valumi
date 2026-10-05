#include "code.h"

#include <string.h>

static const char k_alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

void code_random(char *out, int len, uint32_t (*rnd)(void))
{
    for (int i = 0; i < len; i++) {
        out[i] = k_alphabet[rnd() % (sizeof(k_alphabet) - 1)];
    }
    out[len] = '\0';
}

bool code_valid(const char *s, int len)
{
    if ((int)strlen(s) != len) {
        return false;
    }
    for (int i = 0; i < len; i++) {
        if (strchr(k_alphabet, s[i]) == NULL) {
            return false;
        }
    }
    return true;
}
