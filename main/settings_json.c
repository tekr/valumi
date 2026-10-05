#include "settings_json.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "code.h"

/* ---- small helpers ------------------------------------------------------ */

bool settings_parse_hhmm(const char *str, int *out_min)
{
    if (str == NULL || strlen(str) != 5 || str[2] != ':') {
        return false;
    }
    for (int i = 0; i < 5; i++) {
        if (i != 2 && !isdigit((unsigned char)str[i])) {
            return false;
        }
    }
    int h = (str[0] - '0') * 10 + (str[1] - '0');
    int m = (str[3] - '0') * 10 + (str[4] - '0');
    if (h > 23 || m > 59) {
        return false;
    }
    *out_min = h * 60 + m;
    return true;
}

void settings_format_hhmm(int min, char *out, size_t n)
{
    snprintf(out, n, "%02d:%02d", (min / 60) % 24, min % 60);
}

/* A JSON number that is a whole number within int range. */
static bool get_int(const cJSON *item, int *out)
{
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    double d = item->valuedouble;
    if (d < -1e9 || d > 1e9 || d != (double)(int)d) {
        return false;
    }
    *out = (int)d;
    return true;
}

/* Copies a JSON string into a fixed buffer, refusing rather than truncating:
 * a silently shortened SSID or password is a network that never connects. */
static bool get_str(const cJSON *item, char *out, size_t n)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    size_t len = strlen(item->valuestring);
    if (len >= n) {
        return false;
    }
    memcpy(out, item->valuestring, len + 1);
    return true;
}

static void upcase(char *s)
{
    for (; *s; s++) {
        *s = (char)toupper((unsigned char)*s);
    }
}

static bool key_in(const char *key, const char *const *allowed)
{
    for (; *allowed; allowed++) {
        if (strcmp(key, *allowed) == 0) {
            return true;
        }
    }
    return false;
}

/* Unknown keys are an error, so a misspelt field cannot pass for success. */
static bool only_keys(const cJSON *obj, const char *const *allowed, const char *what, char *err,
                      size_t en)
{
    const cJSON *it;
    cJSON_ArrayForEach(it, obj)
    {
        if (it->string == NULL || !key_in(it->string, allowed)) {
            snprintf(err, en, "unknown field '%s' in %s", it->string ? it->string : "?", what);
            return false;
        }
    }
    return true;
}

static bool want_object(const cJSON *o, const char *const *keys, const char *what, char *err,
                        size_t en)
{
    if (!cJSON_IsObject(o)) {
        snprintf(err, en, "%s must be an object", what);
        return false;
    }
    return only_keys(o, keys, what, err, en);
}

static void add_ip(cJSON *o, const char *key, uint32_t ip, bool present)
{
    char buf[16] = "";
    if (present && ip != 0) {
        settings_format_ipv4(ip, buf, sizeof(buf));
    }
    cJSON_AddStringToObject(o, key, buf);
}

/* Indexed by SET_CHART_*. */
static const char *const k_chart_styles[] = {"close", "highs_lows", "band"};

/* ---- to JSON ------------------------------------------------------------ */

cJSON *settings_to_json(const settings_t *s)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    char buf[8];

    cJSON *coins = cJSON_AddArrayToObject(root, "coins");
    for (int i = 0; i < s->n_coins; i++) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "inst_id", s->coins[i].inst_id);
        cJSON_AddStringToObject(c, "label", s->coins[i].label);
        cJSON_AddItemToArray(coins, c);
    }

    cJSON_AddNumberToObject(root, "dwell_s", s->dwell_s);

    cJSON *o = cJSON_AddObjectToObject(root, "orientation");
    cJSON_AddBoolToObject(o, "fixed", s->orient_fixed);
    cJSON_AddBoolToObject(o, "usb_left", s->usb_left);

    cJSON_AddNumberToObject(root, "brightness", s->brightness);
    cJSON_AddNumberToObject(root, "transition_fade", s->transition_fade);
    cJSON_AddNumberToObject(root, "transition_ms", s->transition_ms);
    cJSON_AddStringToObject(root, "transition_style",
                            s->transition_style == SET_STYLE_DIP ? "dip" : "crossfade");
    cJSON_AddNumberToObject(root, "range", s->range);
    cJSON_AddStringToObject(root, "chart_style", k_chart_styles[s->chart_style]);

    cJSON *night = cJSON_AddObjectToObject(root, "night");
    cJSON_AddBoolToObject(night, "on", s->night_on);
    settings_format_hhmm(s->night_start_min, buf, sizeof(buf));
    cJSON_AddStringToObject(night, "start", buf);
    settings_format_hhmm(s->night_end_min, buf, sizeof(buf));
    cJSON_AddStringToObject(night, "end", buf);
    cJSON_AddNumberToObject(night, "brightness", s->night_brightness);

    cJSON_AddNumberToObject(root, "tz_offset_min", s->tz_offset_min);

    cJSON *nets = cJSON_AddArrayToObject(root, "networks");
    for (int i = 0; i < s->n_nets; i++) {
        const set_net_t *n = &s->nets[i];
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid", n->ssid);
        cJSON_AddBoolToObject(e, "has_password", n->password[0] != '\0');
        cJSON_AddBoolToObject(e, "static_ip", n->static_ip);
        add_ip(e, "ip", n->ip, n->static_ip);
        add_ip(e, "mask", n->mask, n->static_ip);
        add_ip(e, "gateway", n->gw, n->static_ip);
        add_ip(e, "dns", n->dns, n->static_ip);
        cJSON_AddItemToArray(nets, e);
    }

    return root;
}

/* ---- live fields -------------------------------------------------------- */

static bool apply_coins(settings_t *s, const cJSON *arr, char *err, size_t en)
{
    if (!cJSON_IsArray(arr)) {
        snprintf(err, en, "coins must be a list");
        return false;
    }
    int n = cJSON_GetArraySize(arr);
    if (n < 1 || n > SET_MAX_COINS) {
        snprintf(err, en, "between 1 and %d coins", SET_MAX_COINS);
        return false;
    }
    static const char *const keys[] = {"inst_id", "label", NULL};
    for (int i = 0; i < n; i++) {
        const cJSON *c = cJSON_GetArrayItem(arr, i);
        if (!want_object(c, keys, "coin", err, en)) {
            return false;
        }
        set_coin_t *dst = &s->coins[i];
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(c, "inst_id");
        if (!cJSON_IsString(id) || !settings_parse_inst_id(id->valuestring, dst->inst_id)) {
            snprintf(err, en, "coin %d is not a valid instrument id (like BTC-USDT)", i + 1);
            return false;
        }
        const cJSON *label = cJSON_GetObjectItemCaseSensitive(c, "label");
        if (label == NULL || (cJSON_IsString(label) && label->valuestring[0] == '\0')) {
            settings_default_label(dst->inst_id, dst->label, sizeof(dst->label));
        } else if (!get_str(label, dst->label, sizeof(dst->label))) {
            snprintf(err, en, "label for %s must be 1-%d characters", dst->inst_id,
                     SET_LABEL_MAX);
            return false;
        }
        upcase(dst->label);
    }
    s->n_coins = n;
    return true;
}

static bool apply_orientation(settings_t *s, const cJSON *o, char *err, size_t en)
{
    static const char *const keys[] = {"fixed", "usb_left", NULL};
    if (!want_object(o, keys, "orientation", err, en)) {
        return false;
    }
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(o, "fixed");
    const cJSON *l = cJSON_GetObjectItemCaseSensitive(o, "usb_left");
    if ((f && !cJSON_IsBool(f)) || (l && !cJSON_IsBool(l))) {
        snprintf(err, en, "orientation fields must be true or false");
        return false;
    }
    if (f) {
        s->orient_fixed = cJSON_IsTrue(f);
    }
    if (l) {
        s->usb_left = cJSON_IsTrue(l);
    }
    return true;
}

static bool apply_night(settings_t *s, const cJSON *o, char *err, size_t en)
{
    static const char *const keys[] = {"on", "start", "end", "brightness", NULL};
    if (!want_object(o, keys, "night", err, en)) {
        return false;
    }
    const cJSON *it;
    if ((it = cJSON_GetObjectItemCaseSensitive(o, "on"))) {
        if (!cJSON_IsBool(it)) {
            snprintf(err, en, "night.on must be true or false");
            return false;
        }
        s->night_on = cJSON_IsTrue(it);
    }
    if ((it = cJSON_GetObjectItemCaseSensitive(o, "start"))) {
        if (!cJSON_IsString(it) || !settings_parse_hhmm(it->valuestring, &s->night_start_min)) {
            snprintf(err, en, "night start must be HH:MM");
            return false;
        }
    }
    if ((it = cJSON_GetObjectItemCaseSensitive(o, "end"))) {
        if (!cJSON_IsString(it) || !settings_parse_hhmm(it->valuestring, &s->night_end_min)) {
            snprintf(err, en, "night end must be HH:MM");
            return false;
        }
    }
    if ((it = cJSON_GetObjectItemCaseSensitive(o, "brightness"))) {
        if (!get_int(it, &s->night_brightness)) {
            snprintf(err, en, "night brightness must be a whole number");
            return false;
        }
    }
    return true;
}

static bool coins_equal(const settings_t *a, const settings_t *b)
{
    if (a->n_coins != b->n_coins) {
        return false;
    }
    for (int i = 0; i < a->n_coins; i++) {
        if (strcmp(a->coins[i].inst_id, b->coins[i].inst_id) != 0 ||
            strcmp(a->coins[i].label, b->coins[i].label) != 0) {
            return false;
        }
    }
    return true;
}

static bool nets_equal(const settings_t *a, const settings_t *b)
{
    if (a->n_nets != b->n_nets) {
        return false;
    }
    return memcmp(a->nets, b->nets, sizeof(a->nets[0]) * (size_t)a->n_nets) == 0;
}

static int diff_flags(const settings_t *before, const settings_t *after)
{
    int flags = 0;
    if (memcmp(before, after, sizeof(*before)) != 0) {
        flags |= SET_CHG_ANY;
    }
    if (!nets_equal(before, after)) {
        flags |= SET_CHG_NETS;
    }
    if (!coins_equal(before, after)) {
        flags |= SET_CHG_COINS;
    }
    return flags;
}

/* Applies onto @p work without validating; callers validate the result.
 * Networks are only accepted from an import: the panel sets them through
 * POST /api/wifi, which restarts. */
static bool apply_live_fields(settings_t *work, const cJSON *j, bool import, char *err, size_t en)
{
    static const char *const keys[] = {"coins",           "dwell_s",       "orientation",
                                       "brightness",      "transition_fade", "transition_ms",
                                       "transition_style", "range",        "chart_style",
                                       "night",           "tz_offset_min", "networks",
                                       NULL};
    if (!want_object(j, keys, "settings", err, en)) {
        return false;
    }
    if (!import && cJSON_GetObjectItemCaseSensitive(j, "networks")) {
        snprintf(err, en, "networks are set through Wi-Fi settings");
        return false;
    }

    const cJSON *it;
    if ((it = cJSON_GetObjectItemCaseSensitive(j, "coins")) && !apply_coins(work, it, err, en)) {
        return false;
    }
    if ((it = cJSON_GetObjectItemCaseSensitive(j, "dwell_s")) && !get_int(it, &work->dwell_s)) {
        snprintf(err, en, "time per coin must be a whole number of seconds");
        return false;
    }
    if ((it = cJSON_GetObjectItemCaseSensitive(j, "orientation")) &&
        !apply_orientation(work, it, err, en)) {
        return false;
    }
    if ((it = cJSON_GetObjectItemCaseSensitive(j, "brightness")) &&
        !get_int(it, &work->brightness)) {
        snprintf(err, en, "brightness must be a whole number");
        return false;
    }
    if ((it = cJSON_GetObjectItemCaseSensitive(j, "transition_fade")) &&
        !get_int(it, &work->transition_fade)) {
        snprintf(err, en, "transition fade must be a whole number");
        return false;
    }
    if ((it = cJSON_GetObjectItemCaseSensitive(j, "transition_ms")) &&
        !get_int(it, &work->transition_ms)) {
        snprintf(err, en, "transition time must be a whole number of ms");
        return false;
    }
    if ((it = cJSON_GetObjectItemCaseSensitive(j, "transition_style"))) {
        if (cJSON_IsString(it) && strcmp(it->valuestring, "crossfade") == 0) {
            work->transition_style = SET_STYLE_CROSSFADE;
        } else if (cJSON_IsString(it) && strcmp(it->valuestring, "dip") == 0) {
            work->transition_style = SET_STYLE_DIP;
        } else {
            snprintf(err, en, "transition style must be \"crossfade\" or \"dip\"");
            return false;
        }
    }
    if ((it = cJSON_GetObjectItemCaseSensitive(j, "chart_style"))) {
        int k = SET_CHART_BAND;
        while (k >= 0 && !(cJSON_IsString(it) && strcmp(it->valuestring, k_chart_styles[k]) == 0)) {
            k--;
        }
        if (k < 0) {
            snprintf(err, en, "chart style must be \"close\", \"highs_lows\" or \"band\"");
            return false;
        }
        work->chart_style = k;
    }
    if ((it = cJSON_GetObjectItemCaseSensitive(j, "range")) && !get_int(it, &work->range)) {
        snprintf(err, en, "range must be a whole number");
        return false;
    }
    if ((it = cJSON_GetObjectItemCaseSensitive(j, "night")) && !apply_night(work, it, err, en)) {
        return false;
    }
    if ((it = cJSON_GetObjectItemCaseSensitive(j, "tz_offset_min")) &&
        !get_int(it, &work->tz_offset_min)) {
        snprintf(err, en, "time zone offset must be a whole number of minutes");
        return false;
    }
    return true;
}

int settings_apply_json(settings_t *s, const cJSON *j, char *err, size_t en)
{
    settings_t work = *s;
    int flags = -1;
    if (apply_live_fields(&work, j, false, err, en) && settings_validate(&work, err, en)) {
        flags = diff_flags(s, &work);
        *s = work;
    }
    memset(&work, 0, sizeof(work)); /* the copy carries every password */
    return flags;
}

/* ---- networks ----------------------------------------------------------- */

static bool get_ip_field(const cJSON *e, const char *key, uint32_t *out, bool required,
                         const char *ssid, char *err, size_t en)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(e, key);
    *out = 0;
    if (it == NULL || (cJSON_IsString(it) && it->valuestring[0] == '\0')) {
        if (required) {
            snprintf(err, en, "'%s': %s is required for a static address", ssid, key);
            return false;
        }
        return true;
    }
    if (!cJSON_IsString(it) || !settings_parse_ipv4(it->valuestring, out)) {
        snprintf(err, en, "'%s': %s is not an address like 192.168.1.20", ssid, key);
        return false;
    }
    return true;
}

/* Import mode (@p import): the list comes from an export, which carries each
 * network's password. A network without one -- a file from before exports
 * did, or edited by hand -- takes the password this device already has for
 * that SSID; one it has no password for is SKIPPED with a warning rather
 * than failing the whole import. One marked has_password:false is open and
 * imports as such.
 *
 * Panel mode: every entry must say which it means, a password (empty for an
 * open network) or keep_password. Anything else is an error. */
static bool build_networks(settings_t *work, const settings_t *old, const cJSON *arr, bool import,
                           cJSON *warnings, char *err, size_t en)
{
    if (!cJSON_IsArray(arr)) {
        snprintf(err, en, "networks must be a list");
        return false;
    }
    int n = cJSON_GetArraySize(arr);
    if (n > SET_MAX_NETS) {
        snprintf(err, en, "at most %d networks", SET_MAX_NETS);
        return false;
    }
    static const char *const keys[] = {"ssid", "password", "keep_password", "has_password",
                                       "static_ip", "ip", "mask", "gateway", "dns", NULL};
    set_net_t nets[SET_MAX_NETS];
    int out = 0;
    bool ok = false;
    for (int i = 0; i < n; i++) {
        const cJSON *e = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(e)) {
            snprintf(err, en, "each network must be an object");
            goto done;
        }
        if (!only_keys(e, keys, "network", err, en)) {
            goto done;
        }
        set_net_t *d = &nets[out];
        memset(d, 0, sizeof(*d));
        if (!get_str(cJSON_GetObjectItemCaseSensitive(e, "ssid"), d->ssid, sizeof(d->ssid)) ||
            d->ssid[0] == '\0') {
            snprintf(err, en, "network %d: name missing or longer than %d characters", i + 1,
                     SET_SSID_MAX);
            goto done;
        }

        const cJSON *pw = cJSON_GetObjectItemCaseSensitive(e, "password");
        const cJSON *keep = cJSON_GetObjectItemCaseSensitive(e, "keep_password");
        const cJSON *has = cJSON_GetObjectItemCaseSensitive(e, "has_password");
        if (pw != NULL) {
            if (!get_str(pw, d->password, sizeof(d->password))) {
                snprintf(err, en, "'%s': password longer than %d characters", d->ssid,
                         SET_WIFI_PASS_MAX);
                goto done;
            }
        } else if (cJSON_IsTrue(keep) || (import && keep == NULL)) {
            const set_net_t *prev = NULL;
            for (int k = 0; k < old->n_nets; k++) {
                if (strcmp(old->nets[k].ssid, d->ssid) == 0) {
                    prev = &old->nets[k];
                }
            }
            if (prev != NULL) {
                memcpy(d->password, prev->password, sizeof(d->password));
            } else if (import && cJSON_IsFalse(has)) {
                d->password[0] = '\0'; /* exported as an open network */
            } else if (import) {
                if (warnings) {
                    char w[96];
                    snprintf(w, sizeof(w), "'%s' skipped: this ticker has no password for it",
                             d->ssid);
                    cJSON_AddItemToArray(warnings, cJSON_CreateString(w));
                }
                continue;
            } else {
                snprintf(err, en, "'%s': no saved password to keep - enter one", d->ssid);
                goto done;
            }
        } else {
            snprintf(err, en, "'%s': enter a password, or leave it empty for an open network",
                     d->ssid);
            goto done;
        }

        const cJSON *st = cJSON_GetObjectItemCaseSensitive(e, "static_ip");
        if (st != NULL && !cJSON_IsBool(st)) {
            snprintf(err, en, "'%s': static_ip must be true or false", d->ssid);
            goto done;
        }
        d->static_ip = cJSON_IsTrue(st);
        if (d->static_ip) {
            if (!get_ip_field(e, "ip", &d->ip, true, d->ssid, err, en) ||
                !get_ip_field(e, "mask", &d->mask, true, d->ssid, err, en) ||
                !get_ip_field(e, "gateway", &d->gw, true, d->ssid, err, en) ||
                !get_ip_field(e, "dns", &d->dns, false, d->ssid, err, en)) {
                goto done;
            }
        }
        out++;
    }
    /* An import never leaves the ticker with no networks, which would strand
     * it in setup mode on its next restart: with none importable, its own
     * stay. */
    if (import && out == 0) {
        if (warnings) {
            cJSON_AddItemToArray(warnings,
                                 cJSON_CreateString("no network could be imported - this "
                                                    "ticker's own networks are unchanged"));
        }
        ok = true;
        goto done;
    }
    memcpy(work->nets, nets, sizeof(nets[0]) * (size_t)out);
    memset(work->nets + out, 0, sizeof(nets[0]) * (size_t)(SET_MAX_NETS - out));
    work->n_nets = out;
    ok = true;
done:
    memset(nets, 0, sizeof(nets)); /* passwords */
    return ok;
}

int settings_apply_wifi_json(settings_t *s, const cJSON *req, char *err, size_t en)
{
    static const char *const keys[] = {"networks", NULL};
    settings_t work = *s;
    int flags = -1;
    if (want_object(req, keys, "request", err, en) &&
        build_networks(&work, s, cJSON_GetObjectItemCaseSensitive(req, "networks"), false, NULL,
                       err, en) &&
        settings_validate(&work, err, en)) {
        flags = diff_flags(s, &work);
        *s = work;
    }
    memset(&work, 0, sizeof(work));
    return flags;
}

int settings_import_json(settings_t *s, const cJSON *j, cJSON *warnings, char *err, size_t en)
{
    settings_t work = *s;
    int flags = -1;
    const cJSON *nets = cJSON_GetObjectItemCaseSensitive(j, "networks");
    if (apply_live_fields(&work, j, true, err, en) &&
        (nets == NULL || build_networks(&work, s, nets, true, warnings, err, en)) &&
        settings_validate(&work, err, en)) {
        flags = diff_flags(s, &work);
        *s = work;
    }
    memset(&work, 0, sizeof(work));
    return flags;
}

cJSON *settings_to_export_json(const settings_t *s)
{
    cJSON *j = settings_to_json(s);
    const cJSON *nets = cJSON_GetObjectItemCaseSensitive(j, "networks");
    for (int i = 0; j != NULL && i < s->n_nets; i++) {
        cJSON *e = cJSON_GetArrayItem(nets, i);
        cJSON_DeleteItemFromObjectCaseSensitive(e, "has_password");
        cJSON_AddStringToObject(e, "password", s->nets[i].password);
    }
    return j;
}

cJSON *settings_to_stored_json(const settings_t *s)
{
    cJSON *j = settings_to_export_json(s);
    cJSON_AddStringToObject(j, "panel_password", s->panel_pw);
    return j;
}

void settings_from_stored_json(settings_t *s, const cJSON *j)
{
    const cJSON *pw = cJSON_GetObjectItemCaseSensitive(j, "panel_password");
    if (cJSON_IsString(pw) && code_valid(pw->valuestring, SET_PANEL_PW_LEN)) {
        strcpy(s->panel_pw, pw->valuestring);
    }
    char err[160];
    const cJSON *it;
    cJSON_ArrayForEach(it, j)
    {
        settings_t work = *s;
        bool ok = false;
        if (strcmp(it->string, "networks") == 0) {
            ok = build_networks(&work, s, it, false, NULL, err, sizeof(err));
        } else if (strcmp(it->string, "panel_password") != 0) {
            /* One field at a time, so a bad one costs only itself. */
            cJSON *one = cJSON_CreateObject();
            ok = one && cJSON_AddItemReferenceToObject(one, it->string, (cJSON *)it) &&
                 apply_live_fields(&work, one, false, err, sizeof(err));
            cJSON_Delete(one);
        }
        if (ok && settings_validate(&work, err, sizeof(err))) {
            *s = work;
        }
        memset(&work, 0, sizeof(work));
    }
}

/* Brackets, commas and colons inside strings (escapes included) don't
 * count. Every element cJSON allocates is preceded by one of [ { , : so their
 * count bounds the nodes it will create. */
bool settings_json_bounded(const char *text, size_t len, int max_depth, int max_elements)
{
    int depth = 0, elements = 0;
    bool in_str = false, esc = false;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
        } else if (c == '[' || c == '{') {
            if (++depth > max_depth || ++elements > max_elements) {
                return false;
            }
        } else if (c == ']' || c == '}') {
            depth--;
        } else if ((c == ',' || c == ':') && ++elements > max_elements) {
            return false;
        }
    }
    return true;
}

int settings_new_coins(const settings_t *before, const settings_t *after, int *idx, int max)
{
    int n = 0;
    for (int i = 0; i < after->n_coins && n < max; i++) {
        bool known = false;
        for (int k = 0; k < before->n_coins && !known; k++) {
            known = strcmp(before->coins[k].inst_id, after->coins[i].inst_id) == 0;
        }
        if (!known) {
            idx[n++] = i;
        }
    }
    return n;
}
