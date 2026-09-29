#include "hostname.h"
#include <string.h>
#include <stdio.h>

static bool is_alnum_ascii(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

size_t hostname_sanitize(const char* name, char* out, size_t out_len, size_t max_len) {
    if (!out || out_len == 0) return 0;
    if (max_len > out_len - 1) max_len = out_len - 1;
    size_t n = 0;
    bool pending_dash = false;   // a '-' seen after at least one kept character
    for (const unsigned char* p = (const unsigned char*)(name ? name : ""); *p; p++) {
        unsigned char c = *p;
        if (c == ' ' || c == '_' || c == '-') {
            if (n > 0) pending_dash = true;   // never leading; collapsed; emitted before the next character
            continue;
        }
        if (!is_alnum_ascii(c)) continue;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if (pending_dash) {
            if (n + 1 >= max_len) break;      // no room for '-' plus this character
            out[n++] = '-';
            pending_dash = false;
        }
        if (n >= max_len) break;
        out[n++] = (char)c;
    }
    out[n] = '\0';
    return n;
}

size_t hostname_build(const char* name, const uint8_t mac[6], char* out, size_t out_len) {
    if (!out || out_len == 0) return 0;
    char label[HOSTNAME_MAX_LEN + 1];
    size_t max_label = HOSTNAME_MAX_LEN - HOSTNAME_SUFFIX_LEN;
    if (hostname_sanitize(name, label, sizeof(label), max_label) == 0) {
        strncpy(label, HOSTNAME_FALLBACK, sizeof(label) - 1);
        label[sizeof(label) - 1] = '\0';
    }
    int n = snprintf(out, out_len, "%s-%02X%02X%02X", label, mac[3], mac[4], mac[5]);
    if (n < 0) { out[0] = '\0'; return 0; }
    return (size_t)n < out_len ? (size_t)n : out_len - 1;
}
