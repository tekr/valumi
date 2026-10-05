#include "settings.h"
#include "settings_json.h"
#include "test.h"

static cJSON *parse(const char *s)
{
    cJSON *j = cJSON_Parse(s);
    if (j == NULL) {
        fprintf(stderr, "bad test JSON: %s\n", s);
        exit(2);
    }
    return j;
}

/* Apply, and hand back the flags; the error goes to err. */
static int apply(settings_t *s, const char *json, char *err)
{
    cJSON *j = parse(json);
    int r = settings_apply_json(s, j, err, 160);
    cJSON_Delete(j);
    return r;
}

/* @p json is the network list; the request wraps it. */
static int apply_nets(settings_t *s, const char *json, char *err)
{
    char req[1024];
    snprintf(req, sizeof(req), "{\"networks\":%s}", json);
    cJSON *j = parse(req);
    int r = settings_apply_wifi_json(s, j, err, 160);
    cJSON_Delete(j);
    return r;
}

/* Defaults plus the panel password the store would generate. */
static void fresh(settings_t *s)
{
    settings_defaults(s);
    strcpy(s->panel_pw, "AB3K9");
}

static void test_defaults_are_valid(void)
{
    settings_t s;
    char err[160];
    settings_defaults(&s);
    CHECK_EQ_INT(s.n_coins, 4);
    CHECK_EQ_STR(s.coins[0].inst_id, "OKB-USDT");
    CHECK_EQ_STR(s.coins[3].label, "SOL");
    CHECK_EQ_INT(s.dwell_s, 8);
    CHECK_EQ_INT(s.n_nets, 0);             /* no credentials compiled in */
    CHECK_EQ_INT(s.tz_offset_min, 480);    /* UTC+8 */
    CHECK(!s.orient_fixed);
    CHECK(!s.usb_left);                    /* USB on the right */
    CHECK_EQ_STR(s.panel_pw, ""); /* the store makes one; defaults cannot */
    CHECK(!settings_validate(&s, err, sizeof(err)));
    strcpy(s.panel_pw, "AB3K9");
    CHECK(settings_validate(&s, err, sizeof(err)));
    /* The rest of "defaults = the values it had before settings existed". */
    CHECK_EQ_INT(s.range, 1);       /* 7D */
    CHECK_EQ_INT(s.brightness, 30);
    CHECK_EQ_INT(s.transition_fade, 100); /* the transition as designed */
    CHECK(!s.night_on);
    CHECK_EQ_INT(s.night_start_min, 22 * 60);
    CHECK_EQ_INT(s.night_end_min, 7 * 60);
    CHECK_EQ_INT(s.night_brightness, 10);
}

static void test_inst_id_rules(void)
{
    CHECK(settings_valid_inst_id("BTC-USDT"));
    CHECK(settings_valid_inst_id("BTC-USDT-SWAP"));
    CHECK(settings_valid_inst_id("1INCH-USDT"));
    CHECK(!settings_valid_inst_id("BTCUSDT"));    /* no dash */
    CHECK(!settings_valid_inst_id("BTC--USDT"));
    CHECK(!settings_valid_inst_id("-BTC-USDT"));
    CHECK(!settings_valid_inst_id("BTC-USDT-"));
    CHECK(!settings_valid_inst_id("btc-usdt"));   /* upper-cased before this */
    CHECK(!settings_valid_inst_id("BTC-US?T"));   /* would go into a URL */
    CHECK(!settings_valid_inst_id("BTC-US&T"));

    char id[SET_INST_MAX + 1];
    CHECK(settings_parse_inst_id("btc-usdt", id));
    CHECK_EQ_STR(id, "BTC-USDT");
    CHECK(!settings_parse_inst_id("btc usdt", id));
    CHECK(!settings_parse_inst_id("ABCDEFGHIJKL-MNOPQRSTUVWX", id)); /* 25: too long */
}

static void test_default_label(void)
{
    char l[8];
    settings_default_label("BTC-USDT", l, sizeof(l));
    CHECK_EQ_STR(l, "BTC");
    settings_default_label("MATIC-USDT", l, sizeof(l));
    CHECK_EQ_STR(l, "MATI"); /* cut to the 4 the title line fits */
}

static void test_ipv4(void)
{
    uint32_t ip;
    CHECK(settings_parse_ipv4("192.168.1.20", &ip));
    CHECK_EQ_INT(ip, 0xC0A80114u);
    char buf[16];
    settings_format_ipv4(ip, buf, sizeof(buf));
    CHECK_EQ_STR(buf, "192.168.1.20");
    CHECK(settings_parse_ipv4("0.0.0.0", &ip));
    CHECK(!settings_parse_ipv4("192.168.1", &ip));
    CHECK(!settings_parse_ipv4("192.168.1.256", &ip));
    CHECK(!settings_parse_ipv4("192.168.01.1", &ip)); /* octal trap */
    CHECK(!settings_parse_ipv4("192.168.1.1 ", &ip));
    CHECK(!settings_parse_ipv4("1.2.3.4.5", &ip));
    CHECK(!settings_parse_ipv4("", &ip));
    CHECK(!settings_parse_ipv4("1..2.3", &ip));
}

static void test_apply_live_fields(void)
{
    settings_t s;
    char err[160] = "";
    fresh(&s);
    int r = apply(&s,
                  "{\"dwell_s\":15,\"brightness\":70,\"range\":2,"
                  "\"orientation\":{\"fixed\":true,\"usb_left\":true},"
                  "\"night\":{\"on\":true,\"start\":\"23:30\",\"end\":\"06:15\","
                  "\"brightness\":20},\"tz_offset_min\":-210}",
                  err);
    CHECK(r > 0 && (r & SET_CHG_ANY));
    CHECK(!(r & SET_CHG_NETS));
    CHECK(!(r & SET_CHG_COINS));
    CHECK_EQ_INT(s.dwell_s, 15);
    CHECK_EQ_INT(s.brightness, 70);
    CHECK_EQ_INT(s.range, 2);
    CHECK(s.orient_fixed && s.usb_left);
    CHECK(s.night_on);
    CHECK_EQ_INT(s.night_start_min, 23 * 60 + 30);
    CHECK_EQ_INT(s.night_end_min, 6 * 60 + 15);
    CHECK_EQ_INT(s.night_brightness, 20);
    CHECK_EQ_INT(s.tz_offset_min, -210);

    /* Applying the same values again changes nothing. */
    r = apply(&s, "{\"dwell_s\":15}", err);
    CHECK_EQ_INT(r, 0);
}

static void test_transition_fade(void)
{
    settings_t s;
    char err[160];
    fresh(&s);
    CHECK(apply(&s, "{\"transition_fade\":0}", err) > 0);
    CHECK_EQ_INT(s.transition_fade, 0);
    CHECK(apply(&s, "{\"transition_fade\":100}", err) > 0);
    CHECK_EQ_INT(apply(&s, "{\"transition_fade\":101}", err), -1);
    CHECK_EQ_INT(apply(&s, "{\"transition_fade\":-1}", err), -1);
    CHECK_EQ_INT(apply(&s, "{\"transition_fade\":50.5}", err), -1);
    cJSON *j = settings_to_json(&s);
    CHECK(cJSON_GetNumberValue(cJSON_GetObjectItem(j, "transition_fade")) == 100);
    CHECK_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(j, "transition_style")), "crossfade");
    cJSON_Delete(j);

    /* Style: crossfade by default (the transition as designed), or dip. */
    fresh(&s);
    CHECK_EQ_INT(s.transition_style, SET_STYLE_CROSSFADE);
    CHECK(apply(&s, "{\"transition_style\":\"dip\"}", err) > 0);
    CHECK_EQ_INT(s.transition_style, SET_STYLE_DIP);
    CHECK_EQ_INT(apply(&s, "{\"transition_style\":\"wipe\"}", err), -1);
    CHECK_EQ_INT(apply(&s, "{\"transition_style\":1}", err), -1);

    /* Chart: closes by default, as before; highs and lows, or a band. */
    fresh(&s);
    CHECK_EQ_INT(s.chart_style, SET_CHART_CLOSE);
    CHECK(apply(&s, "{\"chart_style\":\"highs_lows\"}", err) > 0);
    CHECK_EQ_INT(s.chart_style, SET_CHART_HIGHS_LOWS);
    CHECK(apply(&s, "{\"chart_style\":\"band\"}", err) > 0);
    CHECK_EQ_INT(s.chart_style, SET_CHART_BAND);
    j = settings_to_json(&s);
    CHECK_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(j, "chart_style")), "band");
    cJSON_Delete(j);
    CHECK_EQ_INT(apply(&s, "{\"chart_style\":\"candles\"}", err), -1);
    CHECK_EQ_INT(apply(&s, "{\"chart_style\":2}", err), -1);

    /* Time: 680 ms by default, as designed; 200-3000 ms. */
    fresh(&s);
    CHECK_EQ_INT(s.transition_ms, 680);
    CHECK(apply(&s, "{\"transition_ms\":200}", err) > 0);
    CHECK(apply(&s, "{\"transition_ms\":3000}", err) > 0);
    CHECK_EQ_INT(apply(&s, "{\"transition_ms\":199}", err), -1);
    CHECK_EQ_INT(apply(&s, "{\"transition_ms\":3001}", err), -1);
}

static void test_apply_is_atomic(void)
{
    settings_t s, before;
    char err[160] = "";
    fresh(&s);
    before = s;
    /* The valid dwell must not land when the brightness in the same request
     * is out of range. */
    int r = apply(&s, "{\"dwell_s\":12,\"brightness\":500}", err);
    CHECK_EQ_INT(r, -1);
    CHECK(strstr(err, "brightness") != NULL);
    CHECK(memcmp(&s, &before, sizeof(s)) == 0);
}

static void test_apply_rejects_bad_input(void)
{
    settings_t s;
    char err[160];
    fresh(&s);
    CHECK_EQ_INT(apply(&s, "{\"dwel_s\":5}", err), -1); /* typo is an error */
    CHECK(strstr(err, "dwel_s") != NULL);
    CHECK_EQ_INT(apply(&s, "{\"dwell_s\":-1}", err), -1);
    CHECK(apply(&s, "{\"dwell_s\":0}", err) >= 0); /* the minimum: a continuous scroll */
    CHECK_EQ_INT(apply(&s, "{\"dwell_s\":16}", err), -1);
    CHECK(apply(&s, "{\"dwell_s\":15}", err) >= 0); /* the maximum */
    CHECK_EQ_INT(apply(&s, "{\"dwell_s\":8.5}", err), -1);
    CHECK_EQ_INT(apply(&s, "{\"dwell_s\":\"8\"}", err), -1);
    CHECK_EQ_INT(apply(&s, "{\"range\":3}", err), -1);
    CHECK_EQ_INT(apply(&s, "{\"tz_offset_min\":481}", err), -1);  /* not a 15 min step */
    CHECK_EQ_INT(apply(&s, "{\"tz_offset_min\":900}", err), -1);  /* beyond +14 */
    CHECK_EQ_INT(apply(&s, "{\"night\":{\"start\":\"24:00\"}}", err), -1);
    CHECK_EQ_INT(apply(&s, "{\"night\":{\"on\":true,\"start\":\"07:00\",\"end\":\"07:00\"}}",
                       err),
                 -1);
    CHECK_EQ_INT(apply(&s, "{\"orientation\":{\"fixed\":1}}", err), -1);
    CHECK_EQ_INT(apply(&s, "{\"name\":\"My Ticker\"}", err), -1);
    CHECK_EQ_INT(apply(&s, "{\"networks\":[]}", err), -1); /* not via this route */
    CHECK_EQ_INT(apply(&s, "[1,2]", err), -1);
}

static void test_coins(void)
{
    settings_t s;
    char err[160];
    fresh(&s);
    settings_t before = s;
    int r = apply(&s,
                  "{\"coins\":[{\"inst_id\":\"btc-usdt\"},{\"inst_id\":\"DOGE-USDT\","
                  "\"label\":\"dog\"},{\"inst_id\":\"PEPE-USDT\",\"label\":\"\"}]}",
                  err);
    CHECK(r > 0 && (r & SET_CHG_COINS));
    CHECK_EQ_INT(s.n_coins, 3);
    CHECK_EQ_STR(s.coins[0].inst_id, "BTC-USDT"); /* upper-cased */
    CHECK_EQ_STR(s.coins[0].label, "BTC");        /* derived */
    CHECK_EQ_STR(s.coins[1].label, "DOG");        /* upper-cased */
    CHECK_EQ_STR(s.coins[2].label, "PEPE");       /* empty means derive */

    int idx[8];
    int n = settings_new_coins(&before, &s, idx, 8);
    CHECK_EQ_INT(n, 2); /* DOGE and PEPE; BTC was already there */
    CHECK_EQ_INT(idx[0], 1);
    CHECK_EQ_INT(idx[1], 2);

    CHECK_EQ_INT(apply(&s, "{\"coins\":[]}", err), -1);
    CHECK_EQ_INT(apply(&s,
                       "{\"coins\":[{\"inst_id\":\"BTC-USDT\"},{\"inst_id\":\"BTC-USDT\"}]}",
                       err),
                 -1);
    CHECK(strstr(err, "twice") != NULL);
    CHECK_EQ_INT(apply(&s, "{\"coins\":[{\"inst_id\":\"BTC-USDT\",\"label\":\"TOOLONG\"}]}",
                       err),
                 -1);
    CHECK_EQ_INT(apply(&s, "{\"coins\":[{\"inst_id\":\"BTC-USDT\",\"label\":\"B.C\"}]}", err),
                 -1);
    CHECK_EQ_INT(apply(&s, "{\"coins\":[{\"inst_id\":\"BTC-USDT\",\"colour\":\"red\"}]}",
                       err),
                 -1);

    /* Fifteen is the most; sixteen is one too many. */
    char many[1024];
    for (int count = 15; count <= 16; count++) {
        int k = snprintf(many, sizeof(many), "{\"coins\":[");
        for (int c = 0; c < count; c++) {
            k += snprintf(many + k, sizeof(many) - k, "%s{\"inst_id\":\"C%d-USDT\"}",
                          c ? "," : "", c);
        }
        snprintf(many + k, sizeof(many) - k, "]}");
        if (count == 15) {
            CHECK(apply(&s, many, err) > 0);
            CHECK_EQ_INT(s.n_coins, 15);
        } else {
            CHECK_EQ_INT(apply(&s, many, err), -1);
        }
    }

    /* Re-ordering is a coin change even though the set is the same. */
    fresh(&s);
    r = apply(&s,
              "{\"coins\":[{\"inst_id\":\"BTC-USDT\"},{\"inst_id\":\"OKB-USDT\"},"
              "{\"inst_id\":\"ETH-USDT\"},{\"inst_id\":\"SOL-USDT\"}]}",
              err);
    CHECK(r > 0 && (r & SET_CHG_COINS));
}

static void test_networks(void)
{
    settings_t s;
    char err[160] = "";
    fresh(&s);
    int r = apply_nets(&s,
                       "[{\"ssid\":\"Home\",\"password\":\"secret123\"},"
                       "{\"ssid\":\"Phone\",\"password\":\"\",\"static_ip\":true,"
                       "\"ip\":\"172.20.10.5\",\"mask\":\"255.255.255.240\","
                       "\"gateway\":\"172.20.10.1\"}]",
                       err);
    CHECK(r > 0 && (r & SET_CHG_NETS));
    CHECK_EQ_INT(s.n_nets, 2);
    CHECK_EQ_STR(s.nets[0].password, "secret123");
    CHECK(!s.nets[0].static_ip);
    CHECK(s.nets[1].static_ip);
    CHECK_EQ_INT(s.nets[1].ip, 0xAC140A05u);
    CHECK_EQ_INT(s.nets[1].dns, 0); /* optional: gateway is used */

    /* Reorder and keep both passwords without resending them. */
    r = apply_nets(&s,
                   "[{\"ssid\":\"Phone\",\"keep_password\":true},"
                   "{\"ssid\":\"Home\",\"keep_password\":true}]",
                   err);
    CHECK(r > 0);
    CHECK_EQ_STR(s.nets[0].ssid, "Phone");
    CHECK_EQ_STR(s.nets[1].password, "secret123");
    CHECK(!s.nets[0].static_ip); /* the static address was not resent, so it is gone */

    /* keep_password for a network that has none saved is refused. */
    settings_t before = s;
    CHECK_EQ_INT(apply_nets(&s, "[{\"ssid\":\"Cafe\",\"keep_password\":true}]", err), -1);
    CHECK(strstr(err, "no saved password") != NULL);
    CHECK(memcmp(&s, &before, sizeof(s)) == 0);

    /* Neither password nor keep: the panel must say which it means. */
    CHECK_EQ_INT(apply_nets(&s, "[{\"ssid\":\"Cafe\"}]", err), -1);

    CHECK_EQ_INT(apply_nets(&s, "[{\"ssid\":\"Cafe\",\"password\":\"short\"}]", err), -1);
    CHECK_EQ_INT(apply_nets(&s,
                            "[{\"ssid\":\"A\",\"password\":\"\"},"
                            "{\"ssid\":\"A\",\"password\":\"\"}]",
                            err),
                 -1);
    CHECK_EQ_INT(apply_nets(&s,
                            "[{\"ssid\":\"A\",\"password\":\"\"},{\"ssid\":\"B\",\"password\":\"\"},"
                            "{\"ssid\":\"C\",\"password\":\"\"},{\"ssid\":\"D\",\"password\":\"\"},"
                            "{\"ssid\":\"E\",\"password\":\"\"}]",
                            err),
                 -1);
    CHECK_EQ_INT(apply_nets(&s, "[{\"ssid\":\"\",\"password\":\"\"}]", err), -1);
    CHECK_EQ_INT(apply_nets(&s, "[{\"ssid\":\"123456789012345678901234567890123\","
                                "\"password\":\"\"}]",
                            err),
                 -1); /* 33 characters: refused, never truncated */

    /* Static addressing is checked for sense. */
    CHECK_EQ_INT(apply_nets(&s,
                            "[{\"ssid\":\"A\",\"password\":\"\",\"static_ip\":true,"
                            "\"ip\":\"192.168.1.20\",\"mask\":\"255.255.255.0\","
                            "\"gateway\":\"10.0.0.1\"}]",
                            err),
                 -1);
    CHECK(strstr(err, "gateway") != NULL);
    CHECK_EQ_INT(apply_nets(&s,
                            "[{\"ssid\":\"A\",\"password\":\"\",\"static_ip\":true,"
                            "\"ip\":\"192.168.1.0\",\"mask\":\"255.255.255.0\","
                            "\"gateway\":\"192.168.1.1\"}]",
                            err),
                 -1); /* network address */
    CHECK_EQ_INT(apply_nets(&s,
                            "[{\"ssid\":\"A\",\"password\":\"\",\"static_ip\":true,"
                            "\"ip\":\"192.168.1.20\",\"mask\":\"255.0.255.0\","
                            "\"gateway\":\"192.168.1.1\"}]",
                            err),
                 -1);
    CHECK_EQ_INT(apply_nets(&s,
                            "[{\"ssid\":\"A\",\"password\":\"\",\"static_ip\":true,"
                            "\"ip\":\"192.168.1.20\",\"mask\":\"255.255.255.0\"}]",
                            err),
                 -1); /* gateway missing */

    /* An empty list is allowed: it sends the ticker back to setup mode. */
    CHECK(apply_nets(&s, "[]", err) >= 0);
    CHECK_EQ_INT(s.n_nets, 0);
}

static void test_json_never_carries_passwords(void)
{
    settings_t s;
    char err[160];
    fresh(&s);
    CHECK(apply_nets(&s, "[{\"ssid\":\"Home\",\"password\":\"hunter2hunter2\"}]", err) > 0);

    cJSON *j = settings_to_json(&s);
    char *text = cJSON_PrintUnformatted(j);
    CHECK(strstr(text, "hunter2") == NULL);
    CHECK(strstr(text, "\"has_password\":true") != NULL);
    CHECK(strstr(text, "AB3K9") == NULL);
    free(text);
    cJSON_Delete(j);
}

static void test_export_import_round_trip(void)
{
    settings_t a, b;
    char err[160] = "";
    fresh(&a);
    CHECK(apply(&a,
                "{\"dwell_s\":12,\"coins\":[{\"inst_id\":\"ETH-USDT\"}],"
                "\"night\":{\"on\":true,\"start\":\"21:00\",\"end\":\"06:00\"}}",
                err) > 0);
    CHECK(apply_nets(&a,
                     "[{\"ssid\":\"Home\",\"password\":\"secret123\",\"static_ip\":true,"
                     "\"ip\":\"192.168.1.20\",\"mask\":\"255.255.255.0\","
                     "\"gateway\":\"192.168.1.1\",\"dns\":\"1.1.1.1\"}]",
                     err) > 0);

    cJSON *exported = settings_to_json(&a);

    /* Into a device that already knows the network: everything restored, and
     * the password kept from the target's own store. */
    fresh(&b);
    CHECK(apply_nets(&b, "[{\"ssid\":\"Home\",\"password\":\"secret123\"}]", err) > 0);
    int r = settings_import_json(&b, exported, NULL, err, sizeof(err));
    CHECK(r > 0);
    CHECK_EQ_INT(b.dwell_s, 12);
    CHECK_EQ_STR(b.coins[0].inst_id, "ETH-USDT");
    CHECK(b.night_on);
    CHECK_EQ_STR(b.nets[0].password, "secret123");
    CHECK(b.nets[0].static_ip);
    CHECK_EQ_INT(b.nets[0].dns, 0x01010101u);

    /* Into a blank device -- the case export exists for. The live settings
     * move; the secured network cannot come without its password, so it is
     * skipped with a warning, and the ticker is left with the networks it had
     * (none) rather than one that can never connect. */
    settings_t blank;
    fresh(&blank);
    cJSON *warnings = cJSON_CreateArray();
    r = settings_import_json(&blank, exported, warnings, err, sizeof(err));
    CHECK(r > 0);
    CHECK_EQ_INT(blank.dwell_s, 12);
    CHECK_EQ_INT(blank.n_nets, 0);
    CHECK_EQ_INT(cJSON_GetArraySize(warnings), 2); /* the skip, and "unchanged" */
    char *wtext = cJSON_PrintUnformatted(warnings);
    CHECK(strstr(wtext, "Home") != NULL);
    free(wtext);
    cJSON_Delete(warnings);

    cJSON_Delete(exported);
}

/* A backup carries the Wi-Fi passwords, so a blank ticker restored from it
 * joins the same networks; the panel password stays with each ticker. */
static void test_export_carries_wifi_passwords(void)
{
    settings_t a, blank;
    char err[160] = "";
    fresh(&a);
    CHECK(apply_nets(&a, "[{\"ssid\":\"Home\",\"password\":\"secret123\"},"
                         "{\"ssid\":\"Cafe\",\"password\":\"\"}]",
                     err) > 0);
    cJSON *exported = settings_to_export_json(&a);
    char *text = cJSON_PrintUnformatted(exported);
    CHECK(strstr(text, "secret123") != NULL);
    CHECK(strstr(text, "AB3K9") == NULL);
    free(text);

    fresh(&blank);
    strcpy(blank.panel_pw, "K7RM3");
    cJSON *w = cJSON_CreateArray();
    CHECK(settings_import_json(&blank, exported, w, err, sizeof(err)) > 0);
    CHECK_EQ_INT(blank.n_nets, 2);
    CHECK_EQ_STR(blank.nets[0].password, "secret123");
    CHECK_EQ_STR(blank.nets[1].password, "");
    CHECK_EQ_INT(cJSON_GetArraySize(w), 0);
    CHECK_EQ_STR(blank.panel_pw, "K7RM3");
    cJSON_Delete(w);
    cJSON_Delete(exported);
}

static void test_import_open_and_mixed_networks(void)
{
    char err[160] = "";
    /* An export with an open network and a secured one. */
    settings_t a;
    fresh(&a);
    CHECK(apply_nets(&a,
                     "[{\"ssid\":\"Cafe\",\"password\":\"\"},"
                     "{\"ssid\":\"Home\",\"password\":\"secret123\"}]",
                     err) > 0);
    cJSON *exported = settings_to_json(&a);

    /* A target that knows neither: the open one comes across, open; the
     * secured one is skipped. */
    settings_t b;
    fresh(&b);
    cJSON *w = cJSON_CreateArray();
    int r = settings_import_json(&b, exported, w, err, sizeof(err));
    CHECK(r > 0 && (r & SET_CHG_NETS));
    CHECK_EQ_INT(b.n_nets, 1);
    CHECK_EQ_STR(b.nets[0].ssid, "Cafe");
    CHECK_EQ_STR(b.nets[0].password, "");
    CHECK_EQ_INT(cJSON_GetArraySize(w), 1);
    cJSON_Delete(w);

    /* A target that knows Home: both come across, Home with its own password. */
    settings_t c;
    fresh(&c);
    CHECK(apply_nets(&c, "[{\"ssid\":\"Home\",\"password\":\"secret123\"}]", err) > 0);
    w = cJSON_CreateArray();
    r = settings_import_json(&c, exported, w, err, sizeof(err));
    CHECK(r > 0);
    CHECK_EQ_INT(c.n_nets, 2);
    CHECK_EQ_STR(c.nets[1].password, "secret123");
    CHECK_EQ_INT(cJSON_GetArraySize(w), 0);
    cJSON_Delete(w);
    cJSON_Delete(exported);

    /* An explicit empty list in an import leaves the networks alone. */
    w = cJSON_CreateArray();
    cJSON *empty = cJSON_Parse("{\"networks\":[]}");
    r = settings_import_json(&c, empty, w, err, sizeof(err));
    CHECK(r >= 0);
    CHECK_EQ_INT(c.n_nets, 2);
    CHECK_EQ_INT(cJSON_GetArraySize(w), 1);
    cJSON_Delete(empty);
    cJSON_Delete(w);

    /* The panel route is stricter: keep_password with nothing saved is an
     * error, never a silent skip. */
    settings_t d;
    fresh(&d);
    CHECK_EQ_INT(apply_nets(&d, "[{\"ssid\":\"Cafe\",\"keep_password\":true}]", err), -1);
}

static void test_json_bounded(void)
{
    const char *flat = "{\"a\":[1,{\"b\":2}]}";
    CHECK(settings_json_bounded(flat, strlen(flat), 3, 100));
    CHECK(!settings_json_bounded(flat, strlen(flat), 2, 100));
    /* Brackets inside strings, escaped quotes included, do not count. */
    const char *str = "{\"p\":\"[[[[\\\"[[[[,,::\"}";
    CHECK(settings_json_bounded(str, strlen(str), 1, 2));
    char deep[512];
    memset(deep, '[', 200);
    memset(deep + 200, ']', 200);
    deep[400] = '\0';
    CHECK(!settings_json_bounded(deep, 400, 8, 1000));
    CHECK(settings_json_bounded(deep + 192, 16, 8, 1000)); /* 8 open, 8 close */

    /* A flat array of many small items: shallow, but every item is a cJSON
     * node on the heap. */
    char wide[4096];
    int k = snprintf(wide, sizeof(wide), "[0");
    for (int i = 0; i < 1000; i++) {
        k += snprintf(wide + k, sizeof(wide) - k, ",0");
    }
    snprintf(wide + k, sizeof(wide) - k, "]");
    CHECK(!settings_json_bounded(wide, strlen(wide), 8, 256));
    CHECK(settings_json_bounded(wide, strlen(wide), 8, 2000));
}

static void test_panel_password_rules(void)
{
    settings_t s;
    char err[160];
    fresh(&s);
    strcpy(s.panel_pw, "AB3K9");
    CHECK(settings_validate(&s, err, sizeof(err)));
    strcpy(s.panel_pw, "AB3");
    CHECK(!settings_validate(&s, err, sizeof(err)));

    /* Never in the JSON. */
    strcpy(s.panel_pw, "AB3K9");
    cJSON *j = settings_to_json(&s);
    char *text = cJSON_PrintUnformatted(j);
    CHECK(strstr(text, "AB3K9") == NULL);
    free(text);
    cJSON_Delete(j);
}

/* What the device saves: everything, passwords included, and it reads back
 * to the same settings. */
static void test_stored_round_trip(void)
{
    settings_t a, b;
    char err[160];
    fresh(&a);
    CHECK(apply_nets(&a, "[{\"ssid\":\"Home\",\"password\":\"secret123\"},"
                         "{\"ssid\":\"Cafe\",\"password\":\"\"}]",
                     err) > 0);
    CHECK(apply(&a, "{\"dwell_s\":0,\"chart_style\":\"band\",\"coins\":[{\"inst_id\":\"ETH-USDT\"}]}",
                err) > 0);
    strcpy(a.panel_pw, "K7RM3");
    cJSON *j = settings_to_stored_json(&a);
    fresh(&b);
    settings_from_stored_json(&b, j);
    cJSON_Delete(j);
    CHECK(memcmp(&a, &b, sizeof(a)) == 0);
}

/* Saved by other firmware: unknown fields are ignored, missing ones keep
 * their defaults, and a bad one costs only itself. */
static void test_stored_from_other_firmware(void)
{
    settings_t s;
    fresh(&s);
    cJSON *j = parse("{\"dwell_s\":12,\"brightness\":500,\"sparkles\":true,"
                     "\"coins\":[{\"inst_id\":\"ETH-USDT\"}],\"panel_password\":\"K7RM3\","
                     "\"networks\":[{\"ssid\":\"Home\",\"password\":\"secret123\","
                     "\"has_password\":true}]}");
    settings_from_stored_json(&s, j);
    cJSON_Delete(j);
    CHECK_EQ_INT(s.dwell_s, 12);
    CHECK_EQ_INT(s.brightness, 30); /* out of range: the default */
    CHECK_EQ_STR(s.coins[0].inst_id, "ETH-USDT");
    CHECK_EQ_INT(s.n_coins, 1);
    CHECK_EQ_INT(s.chart_style, SET_CHART_CLOSE); /* not saved: the default */
    CHECK_EQ_STR(s.panel_pw, "K7RM3");
    CHECK_EQ_INT(s.n_nets, 1);
    CHECK_EQ_STR(s.nets[0].password, "secret123");

    /* An invalid saved password leaves the one already there. */
    j = parse("{\"panel_password\":\"abc\"}");
    settings_from_stored_json(&s, j);
    cJSON_Delete(j);
    CHECK_EQ_STR(s.panel_pw, "K7RM3");
}

static void test_hhmm(void)
{
    int m;
    CHECK(settings_parse_hhmm("00:00", &m) && m == 0);
    CHECK(settings_parse_hhmm("23:59", &m) && m == 1439);
    CHECK(!settings_parse_hhmm("7:00", &m));
    CHECK(!settings_parse_hhmm("07:60", &m));
    CHECK(!settings_parse_hhmm("07-00", &m));
    char buf[8];
    settings_format_hhmm(7 * 60 + 5, buf, sizeof(buf));
    CHECK_EQ_STR(buf, "07:05");
}

int main(void)
{
    RUN(test_defaults_are_valid);
    RUN(test_inst_id_rules);
    RUN(test_default_label);
    RUN(test_ipv4);
    RUN(test_apply_live_fields);
    RUN(test_transition_fade);
    RUN(test_apply_is_atomic);
    RUN(test_apply_rejects_bad_input);
    RUN(test_coins);
    RUN(test_networks);
    RUN(test_json_never_carries_passwords);
    RUN(test_export_import_round_trip);
    RUN(test_export_carries_wifi_passwords);
    RUN(test_import_open_and_mixed_networks);
    RUN(test_json_bounded);
    RUN(test_panel_password_rules);
    RUN(test_stored_round_trip);
    RUN(test_stored_from_other_firmware);
    RUN(test_hhmm);
    TEST_MAIN_END("settings");
}
