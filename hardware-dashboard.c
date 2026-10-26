/* Native, event-driven telemetry display. Build with make. */
#include <gtk/gtk.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <glib-unix.h>
#include <math.h>
#include <signal.h>
#include <string.h>

#define STATUS_URL "http://127.0.0.1:8088/status.json"
#define HISTORY 90
#define MAX_BODY (2 * 1024 * 1024)

typedef struct {
    JsonParser *parser;
    gboolean connected;
    GtkWidget *window, *area;
    SoupSession *session;
    SoupMessage *message;
    GCancellable *cancel;
    GInputStream *stream;
    GByteArray *body;
    guint timer;
    gboolean pending, stopping;
} App;

static JsonNode *member(JsonObject *o, const char *key) {
    return o ? json_object_get_member(o, key) : NULL;
}
static JsonObject *object_node(JsonNode *n) {
    return n && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL;
}
static JsonObject *object(JsonObject *o, const char *key) {
    return object_node(member(o, key));
}
static JsonArray *array(JsonObject *o, const char *key) {
    JsonNode *n = member(o, key);
    return n && JSON_NODE_HOLDS_ARRAY(n) ? json_node_get_array(n) : NULL;
}
static double number(JsonObject *o, const char *key) {
    JsonNode *n = member(o, key);
    if (!n || !JSON_NODE_HOLDS_VALUE(n)) return NAN;
    GType type = json_node_get_value_type(n);
    double v = NAN;
    if (type == G_TYPE_DOUBLE || type == G_TYPE_INT64) v = json_node_get_double(n);
    else if (type == G_TYPE_STRING) {
        const char *s = json_node_get_string(n);
        char *end = NULL;
        v = g_ascii_strtod(s, &end);
        if (s == end || *end) return NAN;
    }
    return isfinite(v) ? v : NAN;
}
static const char *string(JsonObject *o, const char *key, const char *fallback) {
    JsonNode *n = member(o, key);
    if (n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING) {
        const char *s = json_node_get_string(n);
        if (s && *s) return s;
    }
    return fallback;
}
static gboolean boolean(JsonObject *o, const char *key) {
    JsonNode *n = member(o, key);
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_BOOLEAN
        && json_node_get_boolean(n);
}
static JsonObject *root(App *a) {
    return a->parser ? object_node(json_parser_get_root(a->parser)) : NULL;
}
/* Reject invalid roots without losing the last good snapshot. */
static gboolean accept_payload(App *a, const char *data, gssize length) {
    if (!data || length == 0) return FALSE;
    JsonParser *p = json_parser_new();
    GError *error = NULL;
    gboolean ok = json_parser_load_from_data(p, data, length, &error)
        && object_node(json_parser_get_root(p));
    g_clear_error(&error);
    if (!ok) { g_object_unref(p); return FALSE; }
    g_clear_object(&a->parser);
    a->parser = p;
    return TRUE;
}
/* One type scale for the whole page; rhythm comes from spacing, not variety.
   Data is tiered by how far away it is read from: hero, large, ledger, meta. */
#define T_META   17.0
#define T_LABEL  19.0
#define T_UNIT   16.0
#define T_LEDGER 24.0
#define T_LARGE  44.0
#define T_HERO   84.0
#define FONT     "Noto Sans CJK KR, sans-serif"

/* Three columns on the 1200pt canvas, 84pt margins. */
#define M    84.0
#define CW   1032.0
#define COL  316.0
#define C1   M
#define C2   (M + COL + 42.0)
#define C3   (M + 2 * (COL + 42.0))
/* The sensor band splits the right two columns in half. */
#define SW   320.0
#define S1   C2
#define S2   (C2 + 354.0)
#define SPARK_MAX 4

typedef struct { double r, g, b; } Rgb;
#define INK        ((Rgb){ .92, .92, .92 })
#define INK_DIM    ((Rgb){ .62, .62, .62 })
#define INK_FAINT  ((Rgb){ .46, .46, .46 })
#define INK_RULE   ((Rgb){ .20, .20, .20 })
#define INK_WIRE   ((Rgb){ .72, .72, .72 })
#define INK_WIRE2  ((Rgb){ .40, .40, .40 })
#define INK_ALERT  ((Rgb){ 1.0, 1.0, 1.0 })
#define INK_BG     ((Rgb){ .015, .015, .015 })
#define INK_PANEL  ((Rgb){ .055, .055, .055 })
#define INK_EDGE   ((Rgb){ .16, .16, .16 })

static void paint(cairo_t *cr, Rgb color) { cairo_set_source_rgb(cr, color.r, color.g, color.b); }
/* Pango anchors a layout by its top edge; shifting by the ascent makes the
   argument a real baseline, so vertical rhythm does not depend on font metrics. */
static void place(cairo_t *cr, PangoLayout *layout, double x, double y) {
    cairo_move_to(cr, x, y - pango_layout_get_baseline(layout) / (double) PANGO_SCALE);
}
static void text(cairo_t *cr, const char *s, double x, double y,
                 double size, Rgb color, double width, gboolean right) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_new();
    pango_font_description_set_family(font, FONT);
    pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, s, -1);
    pango_layout_set_width(layout, (int)(width * PANGO_SCALE));
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_single_paragraph_mode(layout, TRUE);
    pango_layout_set_alignment(layout, right ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
    paint(cr, color);
    place(cr, layout, x, y);
    /* Outline text avoids host-dependent glyph-cache artifacts. */
    pango_cairo_layout_path(cr, layout);
    cairo_fill(cr);
    pango_font_description_free(font);
    g_object_unref(layout);
}
/* Hairline on a half-pixel so it stays crisp at unit scale. */
static void rule(cairo_t *cr, double x, double y, double width) {
    paint(cr, INK_RULE);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, x, y + .5); cairo_line_to(cr, x + width, y + .5);
    cairo_stroke(cr);
}
static void panel(cairo_t *cr, double x, double y, double width, double height) {
    cairo_new_path(cr);
    cairo_rectangle(cr, x, y, width, height);
    paint(cr, INK_PANEL); cairo_fill_preserve(cr);
    paint(cr, INK_EDGE); cairo_set_line_width(cr, 1); cairo_stroke(cr);
}
/* Label left, reading right. `reserve` is the room held back for the reading,
   so a long sensor name is never clipped by the width of a short value. */
static void ledger(cairo_t *cr, double x, double y, double width, double reserve,
                   const char *label, const char *value) {
    text(cr, label, x, y, T_META, INK_FAINT, MAX(width - reserve, 40), FALSE);
    text(cr, value, x, y, T_LEDGER, INK, width, TRUE);
}
/* A reading is one number with its unit set small against it, recoloured when
   it crosses a limit. Returns the drawn width so a trend can follow it. */
static double reading(cairo_t *cr, double x, double y, double size, double width,
                      double value, const char *unit, double alert) {
    char shown[64];
    Rgb color = INK;
    if (isfinite(value)) {
        g_snprintf(shown, sizeof shown, "%.1f", value);
        if (alert > 0 && value >= alert) color = INK_ALERT;
    } else {
        g_strlcpy(shown, "—", sizeof shown);
        color = INK_FAINT;
        unit = NULL;
    }
    char *escaped = g_markup_escape_text(shown, -1);
    char *markup = unit
        ? g_strdup_printf("%s<span font_desc=\"%.0fpt\"> %s</span>", escaped, T_UNIT, unit)
        : g_strdup(escaped);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_new();
    pango_font_description_set_family(font, FONT);
    pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_markup(layout, markup, -1);
    pango_layout_set_width(layout, (int)(width * PANGO_SCALE));
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_single_paragraph_mode(layout, TRUE);
    paint(cr, color);
    place(cr, layout, x, y);
    pango_cairo_layout_path(cr, layout);
    cairo_fill(cr);
    int drawn = 0, height = 0;
    pango_layout_get_pixel_size(layout, &drawn, &height);
    pango_font_description_free(font);
    g_object_unref(layout);
    g_free(markup);
    g_free(escaped);
    return drawn;
}
static double spark_y(double v, double y, double height, double max) {
    return y + height - CLAMP(v / max, 0, 1) * height;
}
/* A trend line is only worth drawing against its own limit, so the limit is
   part of the plot. Each series is scaled to itself and capped with a dot, so
   a small series is not flattened by a large one and no trace reads as a rule. */
static void spark(cairo_t *cr, JsonArray *history, const char *const *keys, guint count,
                  double x, double y, double width, double height, const double *alerts) {
    guint count_history = history ? json_array_get_length(history) : 0;
    guint start = count_history > HISTORY ? count_history - HISTORY : 0;
    guint span = count_history - start;
    if (span < 2 || count == 0 || count > SPARK_MAX || !(width > 0)) return;
    double maxima[SPARK_MAX] = { 0 };
    for (guint s = 0; s < count; s++) {
        double alert = alerts ? alerts[s] : 0;
        for (guint i = start; i < count_history; i++) {
            double v = number(object_node(json_array_get_element(history, i)), keys[s]);
            if (isfinite(v) && v > maxima[s]) maxima[s] = v;
        }
        maxima[s] = MAX(maxima[s] * 1.12, alert > 0 ? alert * 1.2 : 0);
    }
    for (guint s = 0; s < count; s++) {
        double alert = alerts ? alerts[s] : 0;
        if (!(maxima[s] > 0)) continue;
        if (alert > 0) {
            /* A short edge mark, not a second line: the trace stays the only
               thing spanning the width. */
            paint(cr, INK_RULE);
            cairo_set_line_width(cr, 1);
            double ty = spark_y(alert, y, height, maxima[s]) + .5;
            cairo_move_to(cr, x, ty); cairo_line_to(cr, x + MIN(28, width * .2), ty);
            cairo_stroke(cr);
        }
        paint(cr, s ? INK_WIRE2 : INK_WIRE);
        cairo_set_line_width(cr, 1.5);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        double last = NAN, last_x = x;
        gboolean drawing = FALSE;
        cairo_new_path(cr);
        for (guint i = start; i < count_history; i++) {
            double v = number(object_node(json_array_get_element(history, i)), keys[s]);
            if (!isfinite(v)) { drawing = FALSE; continue; }
            double px = x + (i - start) * width / (span - 1);
            double py = spark_y(v, y, height, maxima[s]);
            if (drawing) cairo_line_to(cr, px, py); else cairo_move_to(cr, px, py);
            drawing = TRUE;
            last = v; last_x = px;
        }
        cairo_stroke(cr);
        /* Every series is capped with a dot so no trace can be mistaken for a rule. */
        if (isfinite(last)) {
            paint(cr, s ? INK_WIRE2 : alert > 0 && last >= alert ? INK_ALERT : INK_DIM);
            cairo_arc(cr, last_x, spark_y(last, y, height, maxima[s]), 2.5, 0, 2 * G_PI);
            cairo_fill(cr);
        }
    }
}
/* Primary reading: what the screen is read for, from across the room. */
static void hero(cairo_t *cr, double x, double y, const char *label,
                 double value, const char *unit, JsonArray *history,
                 const char *key, double alert) {
    text(cr, label, x, y, T_LABEL, INK_DIM, COL, FALSE);
    reading(cr, x, y + 70, T_HERO, COL, value, unit, alert);
    const char *keys[] = { key };
    const double alerts[] = { alert };
    spark(cr, history, keys, 1, x, y + 94, COL, 90, alerts);
}
/* Secondary reading: a trend no wider than the space the value leaves. */
static void large(cairo_t *cr, double x, double y, const char *label,
                  double value, const char *unit, JsonArray *history,
                  const char *key, double alert) {
    text(cr, label, x, y, T_LABEL, INK_DIM, COL, FALSE);
    double used = reading(cr, x, y + 56, T_LARGE, COL, value, unit, alert);
    double room = COL - used - 22;
    if (room < 48) return;
    const char *keys[] = { key };
    const double alerts[] = { alert };
    spark(cr, history, keys, 1, x + used + 22, y + 28, room, 30, alerts);
}
static void section(cairo_t *cr, double y, const char *label,
                    const char *name, const char *status) {
    text(cr, label, M, y, 15, INK_FAINT, 160, FALSE);
    text(cr, name, M + 176, y, 25, INK, 360, FALSE);
    text(cr, status, M + 554, y, 16, INK_DIM, CW - 554, TRUE);
    rule(cr, M, y + 28, CW);
}
static void rate(char *s, gsize len, double v) {
    if (!isfinite(v) || v < 0) g_strlcpy(s, "—", len);
    /* Bytes/second to decimal bits/second: one multiply, no division. */
    else if (v >= 125000) g_snprintf(s, len, "%.1f Mbps", v * 0.000008);
    else g_snprintf(s, len, "%.1f Kbps", v * 0.008);
}
static void gib(char *s, gsize len, double v) {
    if (!isfinite(v) || v < 0) g_strlcpy(s, "—", len);
    else if (v >= 1099511627776.0) g_snprintf(s, len, "%.2f TB", v / 1099511627776.0);
    else if (v >= 1073741824.0) g_snprintf(s, len, "%.1f GB", v / 1073741824.0);
    else g_snprintf(s, len, "%.0f MB", v / 1048576.0);
}
static void capacity(char *s, gsize len, double used, double total) {
    if (!isfinite(used) || !isfinite(total)) { g_strlcpy(s, "—", len); return; }
    char u[32], t[32];
    gib(u, sizeof u, used);
    gib(t, sizeof t, total);
    g_snprintf(s, len, "%s / %s", u, t);
}
static void uptime(char *s, gsize len, double v) {
    if (!isfinite(v) || v < 0) { g_strlcpy(s, "—", len); return; }
    long total = (long)v;
    if (total >= 86400) g_snprintf(s, len, "%ld d %02ld h", total / 86400, total % 86400 / 3600);
    else if (total >= 3600) g_snprintf(s, len, "%ld h %02ld m", total / 3600, total % 3600 / 60);
    else g_snprintf(s, len, "%ld m", total / 60);
}
/* Timestamps arrive with microsecond precision. Trimmed only where a dot after
   the seconds field can only be a fraction, so odd labels pass through intact. */
static void stamp(char *s, gsize len, const char *raw) {
    if (!raw || !*raw) { g_strlcpy(s, "—", len); return; }
    const char *dot = strchr(raw, '.');
    if (dot && dot - raw >= 19 && (gsize)(dot - raw) < len) {
        memcpy(s, raw, (gsize)(dot - raw));
        s[dot - raw] = '\0';
        return;
    }
    g_strlcpy(s, raw, len);
}
static void ago(char *s, gsize len, double v) {
    if (!isfinite(v) || v < 0) g_strlcpy(s, "—", len);
    else if (v < 90) g_snprintf(s, len, "%.0fs ago", v);
    else if (v < 5400) g_snprintf(s, len, "%.0f min ago", v / 60);
    else g_snprintf(s, len, "%.1f h ago", v / 3600);
}
/* load_average is a bare array, so index it directly rather than by name. */
static void load_at(char *s, gsize len, JsonArray *a, guint index) {
    JsonNode *n = a && index < json_array_get_length(a) ? json_array_get_element(a, index) : NULL;
    double v = NAN;
    if (n && JSON_NODE_HOLDS_VALUE(n)) {
        GType t = json_node_get_value_type(n);
        if (t == G_TYPE_DOUBLE || t == G_TYPE_INT64) v = json_node_get_double(n);
    }
    if (!isfinite(v)) g_strlcpy(s, "—", len);
    else g_snprintf(s, len, "%.2f", v);
}
/* Sensors are sorted by name, not by value: a wall display that reorders its
   rows every three seconds is harder to read than one that is stable. */
typedef struct { const char *label; double celsius; } Sensor;
static gint sensor_order(const void *a, const void *b) {
    const Sensor *x = a, *y = b;
    return g_strcmp0(x->label, y->label);
}
/* Both hosts use the same coordinates and hierarchy. Only the middle row is
   device-specific: GPU readings on the workstation, load averages on server. */
static void host_card(cairo_t *cr, double top, JsonObject *host, JsonArray *history,
                      gboolean workstation, gboolean online, const char *status) {
    char value[96];
    panel(cr, 60, top, 1080, 610);
    section(cr, top + 42, workstation ? "WORKSTATION" : "SERVER",
            string(host, "machine", workstation ? "Main PC" : "Server"), status);
    JsonObject *memory = object(host, "memory");
    double temperature = workstation ? number(host, "cpu_temp_celsius")
        : number(object(host, "temperature"), "celsius");
    hero(cr, C1, top + 110, "CPU LOAD", number(host, "cpu_percent"), "%", history, "cpu", 0);
    hero(cr, C2, top + 110, "CPU TEMP", temperature, "°C", history,
         workstation ? "cpu_temp" : "temperature", workstation ? 85 : 75);
    hero(cr, C3, top + 110, "MEMORY", number(memory, "percent"), "%", history, "ram", 90);
    rule(cr, M, top + 312, CW);
    if (workstation) {
        large(cr, C1, top + 346, "GPU LOAD", number(host, "gpu_percent"), "%", history, "gpu", 0);
        large(cr, C2, top + 346, "GPU TEMP", number(host, "gpu_temp_celsius"), "°C", history, "gpu_temp", 80);
        large(cr, C3, top + 346, "GPU POWER", number(host, "gpu_power_watts"), "W", history, "gpu_power", 0);
    } else {
        JsonArray *loads = array(host, "load_average");
        const char *labels[] = {"LOAD / 1 MIN", "LOAD / 5 MIN", "LOAD / 15 MIN"};
        const double columns[] = {C1, C2, C3};
        for (guint i = 0; i < 3; i++) {
            text(cr, labels[i], columns[i], top + 346, T_LABEL, INK_DIM, COL, FALSE);
            load_at(value, sizeof value, loads, i);
            text(cr, value, columns[i], top + 402, T_LARGE, INK, COL, FALSE);
        }
    }
    rule(cr, M, top + 426, CW);
    capacity(value, sizeof value, number(memory, "used_bytes"), number(memory, "total_bytes"));
    ledger(cr, C1, top + 462, COL, 235, "USED", value);
    JsonObject *network = online ? object(host, "network") : NULL;
    rate(value, sizeof value, number(network, "received_bps"));
    ledger(cr, C2, top + 462, COL, 150, "DOWN", value);
    rate(value, sizeof value, number(network, "transmitted_bps"));
    ledger(cr, C3, top + 462, COL, 150, "UP", value);
    double used = number(memory, "percent");
    paint(cr, INK_RULE); cairo_rectangle(cr, C1, top + 488, COL, 4); cairo_fill(cr);
    if (isfinite(used)) {
        paint(cr, used >= 90 ? INK_ALERT : INK_WIRE);
        cairo_rectangle(cr, C1, top + 488, COL * CLAMP(used / 100, 0, 1), 4); cairo_fill(cr);
    }
    if (workstation) g_snprintf(value, sizeof value, "Agent %s", string(host, "agent_version", "—"));
    else { char duration[64]; uptime(duration, sizeof duration, number(host, "uptime_seconds"));
        g_snprintf(value, sizeof value, "Uptime  %s", duration); }
    text(cr, value, C1, top + 550, T_META, INK_FAINT, COL, FALSE);
    const char *down[] = {"network_down"}, *up[] = {"network_up"};
    spark(cr, history, down, 1, C2, top + 490, COL, 70, NULL);
    spark(cr, history, up, 1, C3, top + 490, COL, 70, NULL);
}

static void render(App *a, cairo_t *cr, int width, int height) {
    paint(cr, INK_BG); cairo_paint(cr);
    double scale = MIN(width / 1200.0, height / 1920.0);
    cairo_save(cr);
    cairo_translate(cr, (width - 1200 * scale) / 2, (height - 1920 * scale) / 2);
    cairo_scale(cr, scale, scale);
    JsonObject *r = root(a), *pc = object(r, "main_pc"), *srv = object(r, "server");
    gboolean live = a->connected && boolean(pc, "online");
    char s[128], value[64], status[160];

    text(cr, "RTK / HARDWARE TELEMETRY", M, 50, 14, INK_DIM, CW * .5, FALSE);
    /* Silence reads as healthy; only a lost feed is worth announcing. */
    stamp(value, sizeof value, string(r, "updated_at", "Waiting for telemetry"));
    text(cr, a->connected ? value : "CONNECTION LOST",
         M, 50, 14, a->connected ? INK_FAINT : INK_ALERT, CW, TRUE);
    rule(cr, M, 84, CW);

    if (!a->connected) g_strlcpy(status, "Stale data", sizeof status);
    else if (!live) g_strlcpy(status, "Offline · last readings", sizeof status);
    else {
        ago(value, sizeof value, number(pc, "age_seconds"));
        g_snprintf(status, sizeof status, "Online · %s", value);
    }
    host_card(cr, 104, pc, array(r, "history"), TRUE, live, status);
    host_card(cr, 742, srv, array(r, "server_history"), FALSE, a->connected && srv,
              !a->connected ? "Stale data" : srv ? "Online" : "No data");

    /* Room readings belong to the shared environment, outside either host. */
    panel(cr, 60, 1374, 1080, 86);
    text(cr, "ENVIRONMENT", C1, 1425, T_LABEL, INK_DIM, COL, FALSE);
    JsonObject *room = object(r, "room_temperature");
    double celsius = number(room, "celsius"), humidity = number(room, "humidity_percent");
    if (isfinite(celsius)) g_snprintf(value, sizeof value, "%.1f °C", celsius);
    else g_strlcpy(value, "—", sizeof value);
    ledger(cr, C2, 1425, COL, 110, "ROOM TEMP", value);
    if (isfinite(humidity)) g_snprintf(value, sizeof value, "%.0f %%", humidity);
    else g_strlcpy(value, "—", sizeof value);
    ledger(cr, C3, 1425, COL, 110, "HUMIDITY", value);

    /* ---- storage and every sensor the host reports ---- */
    panel(cr, 60, 1484, 1080, 372);
    text(cr, "STORAGE", C1, 1520, T_LABEL, INK_DIM, COL, FALSE);
    JsonArray *disks = array(srv, "filesystems");
    guint n = disks ? MIN(json_array_get_length(disks), 3) : 0;
    if (!n) text(cr, "—", C1, 1568, T_LEDGER, INK_FAINT, COL, FALSE);
    for (guint i = 0; i < n; i++) {
        JsonObject *d = object_node(json_array_get_element(disks, i));
        double v = number(d, "percent");
        if (isfinite(v)) g_snprintf(s, sizeof s, "%.1f%%", v);
        else g_strlcpy(s, "—", sizeof s);
        double row = 1568 + 94 * i;
        text(cr, string(d, "label", "Disk"), C1, row, T_LEDGER, INK, COL - 100, FALSE);
        text(cr, s, C1, row, T_LEDGER, v >= 90 ? INK_ALERT : INK, COL, TRUE);
        capacity(value, sizeof value, number(d, "used_bytes"), number(d, "total_bytes"));
        text(cr, value, C1, row + 30, T_META, INK_FAINT, COL, FALSE);
    }
    text(cr, "SENSORS", S1, 1520, T_LABEL, INK_DIM, CW - COL, FALSE);
    JsonArray *sensors = array(srv, "temperatures");
    guint total = sensors ? json_array_get_length(sensors) : 0;
    Sensor *list = g_new0(Sensor, MAX(total, 1u));
    guint found = 0;
    for (guint i = 0; i < total; i++) {
        JsonObject *t = object_node(json_array_get_element(sensors, i));
        if (!t) continue;
        list[found].label = string(t, "label", "sensor");
        list[found].celsius = number(t, "celsius");
        found++;
    }
    qsort(list, found, sizeof *list, sensor_order);
    guint slots = found > 12 ? 11 : found;
    for (guint i = 0; i < slots; i++) {
        double x = i % 2 ? S2 : S1, y = 1568 + 48 * (i / 2);
        if (isfinite(list[i].celsius)) g_snprintf(value, sizeof value, "%.1f °C", list[i].celsius);
        else g_strlcpy(value, "—", sizeof value);
        ledger(cr, x, y, SW, 100, list[i].label, value);
    }
    if (found > slots) {
        g_snprintf(s, sizeof s, "+%u more", found - slots);
        ledger(cr, S1 + (slots % 2 ? 354 : 0), 1568 + 48 * (slots / 2), SW, 100, s, "");
    }
    g_free(list);
    cairo_restore(cr);
}
static gboolean draw(GtkWidget *area, cairo_t *cr, gpointer data) {
    render(data, cr, gtk_widget_get_allocated_width(area), gtk_widget_get_allocated_height(area));
    return FALSE;
}
static void finish_request(App *a, gboolean ok) {
    gboolean changed = a->connected != ok;
    if (ok) {
        JsonParser *old = a->parser ? g_object_ref(a->parser) : NULL;
        ok = accept_payload(a, (char *)a->body->data, a->body->len);
        if (ok) changed |= !old || !json_node_equal(json_parser_get_root(old), json_parser_get_root(a->parser));
        g_clear_object(&old);
    }
    changed |= a->connected != ok;
    a->connected = ok;
    g_clear_object(&a->stream);
    g_clear_object(&a->message);
    g_clear_pointer(&a->body, g_byte_array_unref);
    a->pending = FALSE;
    if (changed && !a->stopping) gtk_widget_queue_draw(a->area);
}
static void read_chunk(GObject *source, GAsyncResult *result, gpointer data) {
    App *a = data;
    GError *error = NULL;
    GBytes *chunk = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, &error);
    g_clear_error(&error);
    if (!chunk) { finish_request(a, FALSE); return; }
    gsize size; const guint8 *bytes = g_bytes_get_data(chunk, &size);
    if (!size) { g_bytes_unref(chunk); finish_request(a, !a->stopping); return; }
    if (a->stopping || a->body->len + size > MAX_BODY) {
        g_bytes_unref(chunk); finish_request(a, FALSE); return;
    }
    g_byte_array_append(a->body, bytes, (guint)size);
    g_bytes_unref(chunk);
    g_input_stream_read_bytes_async(a->stream, 16384, G_PRIORITY_DEFAULT, a->cancel, read_chunk, a);
}
static void response(GObject *source, GAsyncResult *result, gpointer data) {
    App *a = data;
    GError *error = NULL;
    a->stream = soup_session_send_finish(SOUP_SESSION(source), result, &error);
    g_clear_error(&error);
    guint status = soup_message_get_status(a->message);
    if (!a->stream || status < 200 || status >= 300 || a->stopping) {
        finish_request(a, FALSE); return;
    }
    a->body = g_byte_array_new();
    g_input_stream_read_bytes_async(a->stream, 16384, G_PRIORITY_DEFAULT, a->cancel, read_chunk, a);
}
static gboolean refresh(gpointer data) {
    App *a = data;
    if (!a->pending && !a->stopping) {
        a->pending = TRUE;
        a->message = soup_message_new("GET", STATUS_URL);
        soup_session_send_async(a->session, a->message, G_PRIORITY_DEFAULT, a->cancel, response, a);
    }
    return G_SOURCE_CONTINUE;
}
static void realized(GtkWidget *widget, gpointer data) {
    App *a = data;
    GdkWindow *window = gtk_widget_get_window(widget);
    GdkCursor *cursor = gdk_cursor_new_for_display(gdk_window_get_display(window), GDK_BLANK_CURSOR);
    gdk_window_set_cursor(window, cursor); g_object_unref(cursor);
    gtk_window_maximize(GTK_WINDOW(widget));
    refresh(a);
    a->timer = g_timeout_add_seconds(3, refresh, a);
}
static gboolean stop(gpointer data) {
    App *a = data; a->stopping = TRUE; gtk_main_quit();
    return G_SOURCE_CONTINUE;
}
static void destroyed(GtkWidget *widget, gpointer data) { (void)widget; stop(data); }

static const char *sample =
    "{\"main_pc\":{\"machine\":\"Main PC\",\"online\":true,\"agent_version\":\"2.1.2\",\"age_seconds\":4.3,"
    "\"cpu_percent\":42.7,\"memory\":{\"total_bytes\":68618899456,\"used_bytes\":13693820928,\"percent\":58.2},"
    "\"gpu_percent\":36.4,\"cpu_temp_celsius\":54.2,\"gpu_temp_celsius\":49.0,\"gpu_power_watts\":112.0,"
    "\"network\":{\"received_bps\":2400000,\"transmitted_bps\":180000}},"
    "\"server\":{\"machine\":\"eruinserver\",\"cpu_percent\":12.8,"
    "\"memory\":{\"total_bytes\":15957516288,\"used_bytes\":5794422784,\"percent\":23.6},"
    "\"temperature\":{\"label\":\"coretemp Package id 0\",\"celsius\":39.0},"
    "\"temperatures\":[{\"label\":\"acpitz\",\"celsius\":27.8},{\"label\":\"acpitz temp1_input\",\"celsius\":27.8},"
    "{\"label\":\"coretemp Core 0\",\"celsius\":38.0},{\"label\":\"coretemp Core 1\",\"celsius\":36.0},"
    "{\"label\":\"coretemp Core 2\",\"celsius\":38.0},{\"label\":\"coretemp Package id 0\",\"celsius\":39.0},"
    "{\"label\":\"nvme Composite\",\"celsius\":40.9},{\"label\":\"x86_pkg_temp\",\"celsius\":40.0}],"
    "\"filesystems\":[{\"key\":\"storage\",\"label\":\"MAIN DATA\",\"total_bytes\":337059803136,\"used_bytes\":73099104256,\"percent\":21.7},"
    "{\"key\":\"backup\",\"label\":\"BACKUP\",\"total_bytes\":235148750848,\"used_bytes\":59986968576,\"percent\":25.5}],"
    "\"network\":{\"received_bps\":3430000,\"transmitted_bps\":307020},"
    "\"load_average\":[1.72,1.45,0.89],\"uptime_seconds\":195846},"
    "\"room_temperature\":{\"celsius\":24.3,\"humidity_percent\":47},"
    "\"updated_at\":\"2026-09-28T11:37:47+00:00\"}";
static void load_sample(App *a) {
    accept_payload(a, sample, -1); a->connected = TRUE;
    const char *names[] = {"history", "server_history"};
    const char *keys[] = {"cpu", "ram", "gpu", "cpu_temp", "gpu_temp", "gpu_power",
                          "temperature", "network_down", "network_up"};
    const double bases[] = {35, 35, 35, 45, 45, 80, 38, 800, 60};
    const double sways[] = {10, 10, 10, 10, 10, 30, 10, 400, 40};
    for (guint j = 0; j < G_N_ELEMENTS(names); j++) {
        JsonArray *h = json_array_new();
        for (int i = 0; i < HISTORY; i++) {
            JsonObject *p = json_object_new();
            for (guint k = 0; k < G_N_ELEMENTS(keys); k++)
                json_object_set_double_member(p, keys[k], bases[k] + sways[k] * sin(i / (6.0 + k)));
            json_array_add_object_element(h, p);
        }
        json_object_set_array_member(root(a), names[j], h);
    }
}
static gboolean preview(App *a, const char *path) {
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1200, 1920);
    cairo_t *cr = cairo_create(surface);
    render(a, cr, 1200, 1920);
    cairo_status_t result = cairo_surface_write_to_png(surface, path);
    cairo_destroy(cr); cairo_surface_destroy(surface);
    if (result != CAIRO_STATUS_SUCCESS) g_printerr("Preview: %s\n", cairo_status_to_string(result));
    return result == CAIRO_STATUS_SUCCESS;
}
static int self_test(void) {
    App a = {0};
    g_assert_true(accept_payload(&a, "{\"n\":\"4.2\",\"bad\":true,\"null\":null}", -1));
    g_assert_cmpfloat(number(root(&a), "n"), ==, 4.2);
    g_assert_true(isnan(number(root(&a), "bad")));
    g_assert_true(isnan(number(root(&a), "null")));
    g_assert_true(isnan(number(root(&a), "missing")));
    g_assert_false(accept_payload(&a, NULL, 0));
    g_assert_false(accept_payload(&a, "", 0));
    g_assert_false(accept_payload(&a, "[]", -1));
    g_assert_false(accept_payload(&a, "{", -1));
    g_assert_cmpfloat(number(root(&a), "n"), ==, 4.2);
    const char *cases[] = {"{}", "{\"main_pc\":false,\"server\":[],\"history\":[null,1,{}]}",
        "{\"main_pc\":{\"machine\":\"긴 이름 한국어 workstation\",\"cpu_percent\":null,\"memory\":false},"
        "\"server\":{\"filesystems\":[null,{},false],\"network\":null}}"};
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1200, 1920);
    cairo_t *cr = cairo_create(s);
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_assert_true(accept_payload(&a, cases[i], -1)); render(&a, cr, 1200, 1920);
        g_assert_cmpint(cairo_status(cr), ==, CAIRO_STATUS_SUCCESS);
    }
    load_sample(&a); render(&a, cr, 1200, 1920);
    a.connected = FALSE; render(&a, cr, 600, 960);
    g_assert_cmpint(cairo_status(cr), ==, CAIRO_STATUS_SUCCESS);
    cairo_destroy(cr); cairo_surface_destroy(s); g_clear_object(&a.parser);
    g_print("Self-test passed: parsing, malformed data, retained snapshot, rendering.\n");
    return 0;
}
int main(int argc, char **argv) {
    App a = {0};
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0) return self_test();
    if (argc == 3 && strcmp(argv[1], "--preview") == 0) {
        load_sample(&a); gboolean ok = preview(&a, argv[2]); g_clear_object(&a.parser); return ok ? 0 : 1;
    }
    if (argc != 1) {
        g_printerr("Usage: %s [--self-test | --preview FILE.png]\n", argv[0]); return 2;
    }
    if (!gtk_init_check(&argc, &argv)) { g_printerr("No graphical display available.\n"); return 1; }
    a.session = soup_session_new_with_options("timeout", 2, NULL);
    a.cancel = g_cancellable_new();
    a.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(a.window), "Eruin Hardware Dashboard");
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_window_set_wmclass(GTK_WINDOW(a.window), "eruin-hardware-dashboard", "EruinHardwareDashboard");
    G_GNUC_END_IGNORE_DEPRECATIONS
    gtk_window_set_decorated(GTK_WINDOW(a.window), FALSE);
    gtk_window_set_keep_above(GTK_WINDOW(a.window), TRUE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(a.window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(a.window), TRUE);
    gtk_window_set_accept_focus(GTK_WINDOW(a.window), FALSE);
    gtk_window_set_focus_on_map(GTK_WINDOW(a.window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(a.window), 1200, 1920);
    a.area = gtk_drawing_area_new(); gtk_container_add(GTK_CONTAINER(a.window), a.area);
    g_signal_connect(a.area, "draw", G_CALLBACK(draw), &a);
    g_signal_connect(a.window, "realize", G_CALLBACK(realized), &a);
    g_signal_connect(a.window, "delete-event", G_CALLBACK(gtk_true), NULL);
    g_signal_connect(a.window, "destroy", G_CALLBACK(destroyed), &a);
    guint term = g_unix_signal_add(SIGTERM, stop, &a), intr = g_unix_signal_add(SIGINT, stop, &a);
    gtk_widget_show_all(a.window); gtk_main();
    a.stopping = TRUE;
    if (a.timer) g_source_remove(a.timer);
    g_source_remove(term); g_source_remove(intr);
    g_cancellable_cancel(a.cancel); soup_session_abort(a.session);
    while (a.pending) g_main_context_iteration(NULL, TRUE);
    g_clear_object(&a.cancel); g_clear_object(&a.session); g_clear_object(&a.parser);
    return 0;
}
