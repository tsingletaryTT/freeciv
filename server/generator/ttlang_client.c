/***********************************************************************
 TT-Lang FreeCiv Bridge — C Client
 Provides a thin socket client so the FreeCiv server can call TT-Lang
 kernels running on Tenstorrent P300C Blackhole hardware.

 Architecture:
   FreeCiv server (C) ──Unix socket──► ttlang_server.py (Python)
                                              │
                                        4x P300C Blackhole

 Protocol: 4-byte little-endian length prefix + UTF-8 JSON body.
 All functions return false gracefully if the TT-Lang server is not
 running — FreeCiv falls back to its normal CPU behaviour.
***********************************************************************/

#ifdef HAVE_CONFIG_H
#  include <fc_config.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* utility */
#include "log.h"
#include "mem.h"

#include "ttlang_client.h"

/* ── Internal state ──────────────────────────────────────────────────────── */

static int  g_sock = -1;   /* connected socket fd, -1 = disconnected */

/* ── Low-level helpers ───────────────────────────────────────────────────── */

/* Send exactly n bytes; returns false on error. */
static bool send_all(int fd, const void *buf, size_t n)
{
  const char *p = (const char *)buf;
  while (n > 0) {
    ssize_t sent = send(fd, p, n, MSG_NOSIGNAL);
    if (sent <= 0) return false;
    p += sent;
    n -= (size_t)sent;
  }
  return true;
}

/* Receive exactly n bytes; returns false on disconnect. */
static bool recv_all(int fd, void *buf, size_t n)
{
  char *p = (char *)buf;
  while (n > 0) {
    ssize_t r = recv(fd, p, n, 0);
    if (r <= 0) return false;
    p += r;
    n -= (size_t)r;
  }
  return true;
}

/* Send a length-prefixed JSON string.  msg must be NUL-terminated. */
static bool send_msg(int fd, const char *msg)
{
  uint32_t len = (uint32_t)strlen(msg);
  uint32_t len_le;
  /* Store in little-endian byte order */
  len_le = (uint32_t)(
    ((len & 0xFF)      ) |
    ((len & 0xFF00)    ) |
    ((len & 0xFF0000)  ) |
    ((len & 0xFF000000))
  );
  /* Proper LE serialisation */
  unsigned char hdr[4];
  hdr[0] = (unsigned char)(len & 0xFF);
  hdr[1] = (unsigned char)((len >> 8)  & 0xFF);
  hdr[2] = (unsigned char)((len >> 16) & 0xFF);
  hdr[3] = (unsigned char)((len >> 24) & 0xFF);

  return send_all(fd, hdr, 4) && send_all(fd, msg, len);
}

/* Receive a length-prefixed JSON string.
 * Caller must free() the returned buffer.  Returns NULL on error. */
static char *recv_msg(int fd)
{
  unsigned char hdr[4];
  if (!recv_all(fd, hdr, 4)) return NULL;

  uint32_t len = (uint32_t)hdr[0]
               | ((uint32_t)hdr[1] << 8)
               | ((uint32_t)hdr[2] << 16)
               | ((uint32_t)hdr[3] << 24);

  if (len == 0 || len > 4 * 1024 * 1024) {
    /* Sanity check: reject messages > 4 MB */
    return NULL;
  }

  char *buf = (char *)fc_malloc(len + 1);
  if (!recv_all(fd, buf, len)) {
    free(buf);
    return NULL;
  }
  buf[len] = '\0';
  return buf;
}

/* ── Minimal JSON helpers ────────────────────────────────────────────────── */

/* Extract a numeric array from a JSON string like [..., ...].
 * Writes up to max_vals values into out[].  Returns count written. */
static int parse_int_array(const char *json, const char *key,
                           int *out, int max_vals)
{
  char search[128];
  snprintf(search, sizeof(search), "\"%s\":", key);
  const char *p = strstr(json, search);
  if (!p) return 0;
  p += strlen(search);
  while (*p == ' ') p++;
  if (*p != '[') return 0;
  p++;   /* skip '[' */

  int count = 0;
  while (*p && *p != ']' && count < max_vals) {
    while (*p == ' ' || *p == ',') p++;
    if (*p == ']' || *p == '\0') break;
    out[count++] = (int)strtol(p, (char **)&p, 10);
  }
  return count;
}

/* Extract "status":"ok" check — handles both "status":"ok" and "status": "ok" */
static bool is_ok(const char *json)
{
  return strstr(json, "\"status\":\"ok\"")  != NULL
      || strstr(json, "\"status\": \"ok\"") != NULL;
}

/* Extract a scalar double field.  Returns def if not found. */
static double get_double(const char *json, const char *key, double def)
{
  char search[128];
  snprintf(search, sizeof(search), "\"%s\":", key);
  const char *p = strstr(json, search);
  if (!p) return def;
  p += strlen(search);
  while (*p == ' ') p++;
  return strtod(p, NULL);
}

/* Extract a scalar int field. Returns def if not found. */
static int get_int(const char *json, const char *key, int def)
{
  return (int)get_double(json, key, (double)def);
}

/* ── Connection management ───────────────────────────────────────────────── */

bool ttlang_connect(void)
{
  if (g_sock >= 0) return true;   /* already connected */

  g_sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (g_sock < 0) return false;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, TTLANG_SOCKET_PATH, sizeof(addr.sun_path) - 1);

  if (connect(g_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(g_sock);
    g_sock = -1;
    return false;
  }

  log_verbose("TT-Lang: connected to %s", TTLANG_SOCKET_PATH);
  return true;
}

void ttlang_disconnect(void)
{
  if (g_sock >= 0) {
    close(g_sock);
    g_sock = -1;
  }
}

bool ttlang_available(void)
{
  if (g_sock >= 0) return true;
  return ttlang_connect();
}

/* Reset connection on error so next call retries. */
static void reset_connection(void)
{
  close(g_sock);
  g_sock = -1;
}

/* ── Terrain generation ──────────────────────────────────────────────────── */

bool ttlang_terrain_gen(int *height_map, int w, int h, int seed)
{
  if (!ttlang_available()) return false;

  /* Build JSON request */
  char req[256];
  snprintf(req, sizeof(req),
           "{\"cmd\":\"terrain_gen\",\"w\":%d,\"h\":%d,\"seed\":%d}",
           w, h, seed);

  if (!send_msg(g_sock, req)) { reset_connection(); return false; }

  char *resp = recv_msg(g_sock);
  if (!resp)                   { reset_connection(); return false; }

  if (!is_ok(resp)) {
    log_error("TT-Lang terrain_gen error: %s", resp);
    free(resp);
    return false;
  }

  double ms = get_double(resp, "kernel_ms", 0.0);

  /* Parse the "data" integer array */
  int count = parse_int_array(resp, "data", height_map, w * h);
  free(resp);

  if (count != w * h) {
    log_error("TT-Lang terrain_gen: expected %d values, got %d", w * h, count);
    return false;
  }

  log_verbose("TT-Lang: terrain_gen %dx%d seed=%d: %.3fms",
              w, h, seed, ms);
  return true;
}

/* ── Per-turn terrain events ─────────────────────────────────────────────── */

bool ttlang_turn_events(int turn, int w, int h,
                        ttlang_event_t *events, int *n_events)
{
  *n_events = 0;
  if (!ttlang_available()) return false;

  char req[256];
  snprintf(req, sizeof(req),
           "{\"cmd\":\"terrain_event\",\"w\":%d,\"h\":%d,\"turn\":%d}",
           w, h, turn);

  if (!send_msg(g_sock, req)) { reset_connection(); return false; }

  char *resp = recv_msg(g_sock);
  if (!resp)                   { reset_connection(); return false; }

  if (!is_ok(resp)) {
    log_error("TT-Lang terrain_event error: %s", resp);
    free(resp);
    return false;
  }

  double ms = get_double(resp, "kernel_ms", 0.0);

  /* Parse "events":[{"tile":N,"extra":"Gold"}, ...] */
  const char *p = strstr(resp, "\"events\":");
  if (!p) { free(resp); return true; }   /* empty response is ok */
  p = strchr(p, '[');
  if (!p) { free(resp); return true; }
  p++;  /* skip '[' */

  while (*p && *p != ']' && *n_events < TTLANG_MAX_EVENTS) {
    /* find "tile": */
    const char *tile_p = strstr(p, "\"tile\":");
    const char *extra_p = strstr(p, "\"extra\":\"");
    const char *next_obj = strstr(p, "},{");

    if (!tile_p || !extra_p) break;
    if (next_obj && tile_p > next_obj) { p = next_obj + 2; continue; }

    int tile_idx = (int)strtol(tile_p + 7, NULL, 10);

    /* Extract extra name */
    extra_p += 9;  /* skip past  "extra":"  */
    const char *end_q = strchr(extra_p, '"');
    if (!end_q) break;

    int ev = *n_events;
    events[ev].tile_idx = tile_idx;
    size_t name_len = (size_t)(end_q - extra_p);
    if (name_len >= sizeof(events[ev].extra_name))
      name_len = sizeof(events[ev].extra_name) - 1;
    strncpy(events[ev].extra_name, extra_p, name_len);
    events[ev].extra_name[name_len] = '\0';
    (*n_events)++;

    /* Advance past this object */
    if (next_obj) p = next_obj + 2;
    else break;
  }

  log_verbose("TT-Lang: turn %d events: %d tiles (%.3fms)",
              turn, *n_events, ms);
  free(resp);
  return true;
}

/* ── AI tile scoring ─────────────────────────────────────────────────────── */

bool ttlang_score_tiles(int w, int h, int turn,
                        const float *food,
                        const float *shields,
                        const float *trade,
                        float *scores_out,
                        int   *top_tiles_out,
                        int   *n_top,
                        int   *disaster_active_out,
                        int   *disaster_epi_out)
{
  *n_top = 0;
  if (!ttlang_available()) return false;

  int map_tiles = w * h;

  /* Build JSON arrays for the three inputs */
  /* Each value is at most 8 chars ("-999.99,"), plus brackets and key */
  size_t arr_sz = (size_t)(map_tiles * 9 + 16);
  char *json    = (char *)fc_malloc(arr_sz * 3 + 256);
  char *food_s  = (char *)fc_malloc(arr_sz);
  char *shld_s  = (char *)fc_malloc(arr_sz);
  char *trad_s  = (char *)fc_malloc(arr_sz);

  /* Serialize food array */
  food_s[0] = '['; size_t fp = 1;
  shld_s[0] = '['; size_t sp = 1;
  trad_s[0] = '['; size_t tp = 1;
  for (int i = 0; i < map_tiles; i++) {
    const char *sep = (i < map_tiles - 1) ? "," : "";
    fp += (size_t)snprintf(food_s + fp, arr_sz - fp, "%.1f%s", food[i],    sep);
    sp += (size_t)snprintf(shld_s + sp, arr_sz - sp, "%.1f%s", shields[i], sep);
    tp += (size_t)snprintf(trad_s + tp, arr_sz - tp, "%.1f%s", trade[i],   sep);
  }
  food_s[fp] = ']'; food_s[fp+1] = '\0';
  shld_s[sp] = ']'; shld_s[sp+1] = '\0';
  trad_s[tp] = ']'; trad_s[tp+1] = '\0';

  snprintf(json, arr_sz * 3 + 256,
           "{\"cmd\":\"tile_score\",\"w\":%d,\"h\":%d,\"turn\":%d,"
           "\"food\":%s,\"shields\":%s,\"trade\":%s}",
           w, h, turn, food_s, shld_s, trad_s);
  free(food_s); free(shld_s); free(trad_s);

  if (!send_msg(g_sock, json)) { free(json); reset_connection(); return false; }
  free(json);

  char *resp = recv_msg(g_sock);
  if (!resp) { reset_connection(); return false; }

  if (!is_ok(resp)) {
    log_error("TT-Lang tile_score error: %s", resp);
    free(resp);
    return false;
  }

  double ms = get_double(resp, "kernel_ms", 0.0);


  /* Parse score array (floating point → stored as-is in JSON) */
  const char *p = strstr(resp, "\"scores\":");
  if (p) {
    p = strchr(p, '[');
    if (p) {
      p++;
      int i = 0;
      while (*p && *p != ']' && i < map_tiles) {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']' || !*p) break;
        scores_out[i++] = (float)strtod(p, (char **)&p);
      }
    }
  }

  /* Parse top_tiles array */
  int top_buf[32] = {0};
  int n = parse_int_array(resp, "top_tiles", top_buf, 32);
  for (int i = 0; i < n && i < 32; i++)
    top_tiles_out[i] = top_buf[i];
  *n_top = n;

  /* Parse disaster fields (active this turn + epicenter tile index) */
  *disaster_active_out = (int)get_double(resp, "disaster_active", 0.0);
  *disaster_epi_out    = (int)get_double(resp, "disaster_epi_tile", 0.0);

  if (n > 0) {
    int tx = top_buf[0] % w;
    int ty = top_buf[0] / w;
    log_verbose("TT-Lang: AI scoring %dx%d: top tile (%d,%d) score=%.1f (%.3fms)",
                w, h, tx, ty, (double)scores_out[top_buf[0]], ms);
  }

  free(resp);
  return true;
}

/* ── Civilization generation ─────────────────────────────────────────────── */

/* Extract a string field value from JSON.  Writes up to max_len-1 chars.
 * Looks for "key":"value" and copies value into out.  Returns false if not found. */
static bool get_string(const char *json, const char *key,
                       char *out, int max_len)
{
  char search[128];
  snprintf(search, sizeof(search), "\"%s\":\"", key);
  const char *p = strstr(json, search);
  if (!p) return false;
  p += strlen(search);
  const char *end = strchr(p, '"');
  if (!end) return false;
  int len = (int)(end - p);
  if (len >= max_len) len = max_len - 1;
  strncpy(out, p, (size_t)len);
  out[len] = '\0';
  return true;
}

bool ttlang_gen_civs(int n_civs, int seed,
                     ttlang_civ_t *civs,
                     char *output_dir)
{
  if (!ttlang_available()) return false;

  /* Build JSON request */
  char req[256];
  snprintf(req, sizeof(req),
           "{\"cmd\":\"gen_civs\",\"n_civs\":%d,\"seed\":%d}",
           n_civs, seed);

  if (!send_msg(g_sock, req)) { reset_connection(); return false; }

  char *resp = recv_msg(g_sock);
  if (!resp)                   { reset_connection(); return false; }

  if (!is_ok(resp)) {
    log_error("TT-Lang gen_civs error: %s", resp);
    free(resp);
    return false;
  }

  double ms = get_double(resp, "kernel_ms", 0.0);

  /* Extract output_dir if caller wants it */
  if (output_dir) {
    get_string(resp, "output_dir", output_dir, 256);
  }

  /* Parse "civs":[{...}, ...] array.
   * Find each object between { and } within the "civs" array and extract fields. */
  const char *arr_start = strstr(resp, "\"civs\":");
  if (!arr_start) { free(resp); return false; }
  arr_start = strchr(arr_start, '[');
  if (!arr_start) { free(resp); return false; }
  arr_start++;  /* skip '[' */

  int parsed = 0;
  const char *p = arr_start;

  while (*p && *p != ']' && parsed < n_civs && parsed < TTLANG_MAX_CIVS) {
    /* Find the next object '{' */
    const char *obj_start = strchr(p, '{');
    if (!obj_start) break;

    /* Find the matching '}' (assumes no nested objects in civ dict) */
    const char *obj_end = strchr(obj_start, '}');
    if (!obj_end) break;

    /* Copy this object into a temporary NUL-terminated buffer */
    int obj_len = (int)(obj_end - obj_start + 1);
    char *obj   = (char *)fc_malloc((size_t)obj_len + 1);
    strncpy(obj, obj_start, (size_t)obj_len);
    obj[obj_len] = '\0';

    ttlang_civ_t *civ = &civs[parsed];
    memset(civ, 0, sizeof(*civ));

    civ->index = get_int(obj, "index", parsed);
    get_string(obj, "slug",          civ->slug,          sizeof(civ->slug));
    get_string(obj, "leader",        civ->leader,        sizeof(civ->leader));
    get_string(obj, "nation",        civ->nation,        sizeof(civ->nation));
    get_string(obj, "adjective",     civ->adjective,     sizeof(civ->adjective));
    get_string(obj, "city1",         civ->city1,         sizeof(civ->city1));
    get_string(obj, "city2",         civ->city2,         sizeof(civ->city2));
    get_string(obj, "city3",         civ->city3,         sizeof(civ->city3));
    get_string(obj, "flag_path",     civ->flag_path,     sizeof(civ->flag_path));
    get_string(obj, "portrait_path", civ->portrait_path, sizeof(civ->portrait_path));
    get_string(obj, "ruleset_path",  civ->ruleset_path,  sizeof(civ->ruleset_path));

    civ->aggression = (int)(get_double(obj, "aggression", 0.5) * 100);
    civ->expansion  = (int)(get_double(obj, "expansion",  0.5) * 100);
    civ->military   = (int)(get_double(obj, "military",   0.5) * 100);
    civ->science    = (int)(get_double(obj, "science",    0.5) * 100);

    /* Parse color_rgb array [r, g, b] using the int array parser */
    int rgb[3] = {128, 128, 128};
    parse_int_array(obj, "color_rgb", rgb, 3);
    civ->color_r = rgb[0];
    civ->color_g = rgb[1];
    civ->color_b = rgb[2];

    free(obj);
    parsed++;
    p = obj_end + 1;
  }

  log_verbose("TT-Lang: gen_civs %d civs seed=%d: %.3fms", parsed, seed, ms);
  free(resp);
  return (parsed == n_civs);
}

/* ── ttlang_city_influence ───────────────────────────────────────────────── */

bool ttlang_city_influence(int w, int h, int turn,
                           const ttlang_city_t *cities, int n_cities,
                           float *influence_out)
{
  if (!ttlang_connect() || n_cities <= 0) return false;

  /* Build JSON: {"cmd":"city_influence","w":W,"h":H,"turn":T,
   *              "cities":[{"t":idx,"p":pid},...]} */
  /* Each city entry is at most ~30 chars; add generous header room. */
  int city_buf_sz = n_cities * 32 + 128;
  char *city_s = (char *)fc_malloc(city_buf_sz);
  int  pos = 0;
  city_s[pos++] = '[';
  for (int i = 0; i < n_cities; i++) {
    pos += snprintf(city_s + pos, city_buf_sz - pos,
                    "%s{\"t\":%d,\"p\":%d}",
                    (i > 0 ? "," : ""),
                    cities[i].tile_idx,
                    cities[i].player_id);
  }
  city_s[pos++] = ']';
  city_s[pos]   = '\0';

  int total_sz = city_buf_sz + 128;
  char *json = (char *)fc_malloc(total_sz);
  snprintf(json, total_sz,
           "{\"cmd\":\"city_influence\",\"w\":%d,\"h\":%d,\"turn\":%d,"
           "\"cities\":%s}",
           w, h, turn, city_s);
  free(city_s);

  if (!send_msg(g_sock, json)) {
    free(json);
    g_sock = -1;
    return false;
  }
  free(json);

  char *resp = recv_msg(g_sock);
  if (!resp || !is_ok(resp)) {
    free(resp);
    g_sock = -1;
    return false;
  }

  /* Parse "influence":[float, ...] */
  const char *p = strstr(resp, "\"influence\":");
  if (!p) { free(resp); return false; }
  p += strlen("\"influence\":");
  while (*p == ' ') p++;
  if (*p != '[') { free(resp); return false; }
  p++;

  int n_tiles = w * h;
  int i = 0;
  while (*p && *p != ']' && i < n_tiles) {
    while (*p == ' ' || *p == ',') p++;
    if (*p == ']' || *p == '\0') break;
    influence_out[i++] = (float)strtod(p, (char **)&p);
  }

  double ms = get_double(resp, "kernel_ms", 0.0);
  log_verbose("TT-Lang: city_influence %d cities %dx%d: %.3fms",
              n_cities, w, h, ms);
  free(resp);
  return (i == n_tiles);
}

/***********************************************************************
  Ask TT hardware for per-settler optimal destination tiles.

  Sends idle settler positions to ttlang_server.py which runs:
    1. Per-settler 4-pass diffusion from each settler's position (TT).
    2. CPU argmax(pathfield × reach_field) per settler.

  The pathfield used here was cached inside the server from the
  tile_score call earlier in begin_turn() — no extra kernel cost.

  JSON request:
    {"cmd":"unit_eval","w":W,"h":H,"turn":T,
     "units":[{"id":N,"tile":M,"is_settler":1},...]}

  JSON response:
    {"status":"ok","recommendations":[{"unit_id":N,"best_tile":M},...],
     "kernel_ms":...}
***********************************************************************/
bool ttlang_unit_eval(int w, int h, int turn,
                      const int *unit_ids, const int *unit_tiles, int n_units,
                      ttlang_unit_rec_t *recs, int *n_recs)
{
  if (g_sock < 0 || n_units <= 0 || n_units > TTLANG_MAX_UNITS) return false;

  /* Build units JSON array: [{"id":N,"tile":M,"is_settler":1}, ...] */
  /* Each entry: ~30 chars; total array ≤ 64*30 + 4 = ~1924 chars */
  size_t arr_sz  = (size_t)n_units * 48 + 8;
  char  *units_s = (char *)fc_malloc(arr_sz);
  units_s[0]     = '[';
  units_s[1]     = '\0';
  for (int i = 0; i < n_units; i++) {
    char entry[64];
    snprintf(entry, sizeof(entry),
             "%s{\"id\":%d,\"tile\":%d,\"is_settler\":1}",
             i == 0 ? "" : ",",
             unit_ids[i], unit_tiles[i]);
    /* strncat is safe here: arr_sz is sized for all entries */
    strncat(units_s, entry, arr_sz - strlen(units_s) - 1);
  }
  strncat(units_s, "]", arr_sz - strlen(units_s) - 1);

  size_t total_sz = arr_sz + 128;
  char  *json     = (char *)fc_malloc(total_sz);
  snprintf(json, total_sz,
           "{\"cmd\":\"unit_eval\",\"w\":%d,\"h\":%d,\"turn\":%d,"
           "\"units\":%s}",
           w, h, turn, units_s);
  free(units_s);

  if (!send_msg(g_sock, json)) {
    free(json);
    g_sock = -1;
    return false;
  }
  free(json);

  char *resp = recv_msg(g_sock);
  if (!resp || !is_ok(resp)) {
    free(resp);
    g_sock = -1;
    return false;
  }

  /* Parse "recommendations":[{"unit_id":N,"best_tile":M}, ...] */
  const char *p = strstr(resp, "\"recommendations\":");
  if (!p) { free(resp); *n_recs = 0; return true; } /* empty list is valid */
  p += strlen("\"recommendations\":");
  while (*p == ' ') p++;
  if (*p != '[') { free(resp); *n_recs = 0; return true; }
  p++;

  int count = 0;
  while (*p && *p != ']' && count < TTLANG_MAX_UNITS) {
    /* Skip whitespace / commas between objects */
    while (*p == ' ' || *p == ',' || *p == '\n') p++;
    if (*p != '{') break;

    /* Extract unit_id */
    const char *uid_p = strstr(p, "\"unit_id\":");
    const char *bt_p  = strstr(p, "\"best_tile\":");
    const char *end_p = strchr(p, '}');
    if (!uid_p || !bt_p || !end_p) break;

    recs[count].unit_id   = (int)strtol(uid_p  + strlen("\"unit_id\":"),
                                        NULL, 10);
    recs[count].best_tile = (int)strtol(bt_p   + strlen("\"best_tile\":"),
                                        NULL, 10);
    count++;
    p = end_p + 1;
  }

  *n_recs = count;
  double ms = get_double(resp, "kernel_ms", 0.0);
  log_verbose("TT-Lang: unit_eval %d settlers → %d recs %dx%d: %.3fms",
              n_units, count, w, h, ms);
  free(resp);
  return true;
}

/***********************************************************************
  Ask TT hardware for city production priority scores.

  Runs ThreatFieldModel (8-pass diffusion from enemy unit positions) and
  CityProdModel (6 smooth_height_map passes across Military/Growth/Science)
  on P300C Blackhole hardware.  The computed threat field is cached inside
  the server for ttlang_combat_pos() reuse later in the same turn.

  JSON request:
    {"cmd":"city_prod","w":W,"h":H,"turn":T,
     "cities":[{"t":N,"f":F,"s":S,"r":R,"p":P},...],
     "enemy":[{"t":N,"s":S},...]}

  JSON response:
    {"status":"ok",
     "recommendations":[{"city_tile":N,"prod_cat":0|1|2,"urgency":U},...],
     "kernel_ms":...}
***********************************************************************/
bool ttlang_city_prod(int w, int h, int turn,
                      const int   *city_tiles,
                      const float *city_food,
                      const float *city_shields,
                      const float *city_trade,
                      const float *city_pop,
                      int n_cities,
                      const int   *enemy_tiles,
                      const float *enemy_strengths,
                      int n_enemy,
                      ttlang_city_prod_rec_t *recs, int *n_recs)
{
  *n_recs = 0;
  if (g_sock < 0 || n_cities <= 0) return false;

  /* Build city JSON array: [{"t":N,"f":F,"s":S,"r":R,"p":P}, ...] */
  size_t city_buf_sz = (size_t)n_cities * 80 + 8;
  char  *city_s      = (char *)fc_malloc(city_buf_sz);
  city_s[0] = '['; city_s[1] = '\0';
  for (int i = 0; i < n_cities; i++) {
    char entry[128];
    snprintf(entry, sizeof(entry),
             "%s{\"t\":%d,\"f\":%.1f,\"s\":%.1f,\"r\":%.1f,\"p\":%.1f}",
             i == 0 ? "" : ",",
             city_tiles[i],
             (double)city_food[i], (double)city_shields[i],
             (double)city_trade[i], (double)city_pop[i]);
    strncat(city_s, entry, city_buf_sz - strlen(city_s) - 1);
  }
  strncat(city_s, "]", city_buf_sz - strlen(city_s) - 1);

  /* Build enemy JSON array: [{"t":N,"s":S}, ...] */
  size_t enemy_buf_sz = (size_t)(n_enemy + 1) * 32 + 8;
  char  *enemy_s      = (char *)fc_malloc(enemy_buf_sz);
  enemy_s[0] = '['; enemy_s[1] = '\0';
  for (int i = 0; i < n_enemy; i++) {
    char entry[64];
    snprintf(entry, sizeof(entry),
             "%s{\"t\":%d,\"s\":%.1f}",
             i == 0 ? "" : ",",
             enemy_tiles[i], (double)enemy_strengths[i]);
    strncat(enemy_s, entry, enemy_buf_sz - strlen(enemy_s) - 1);
  }
  strncat(enemy_s, "]", enemy_buf_sz - strlen(enemy_s) - 1);

  size_t total_sz = city_buf_sz + enemy_buf_sz + 128;
  char  *json     = (char *)fc_malloc(total_sz);
  snprintf(json, total_sz,
           "{\"cmd\":\"city_prod\",\"w\":%d,\"h\":%d,\"turn\":%d,"
           "\"cities\":%s,\"enemy\":%s}",
           w, h, turn, city_s, enemy_s);
  free(city_s); free(enemy_s);

  if (!send_msg(g_sock, json)) {
    free(json); g_sock = -1; return false;
  }
  free(json);

  char *resp = recv_msg(g_sock);
  if (!resp || !is_ok(resp)) { free(resp); g_sock = -1; return false; }

  /* Parse "recommendations":[{"city_tile":N,"prod_cat":C,"urgency":U},...] */
  const char *p = strstr(resp, "\"recommendations\":");
  if (!p) { free(resp); return true; }   /* empty list is valid */
  p += strlen("\"recommendations\":");
  while (*p == ' ') p++;
  if (*p != '[') { free(resp); return true; }
  p++;

  int count = 0;
  while (*p && *p != ']' && count < n_cities) {
    while (*p == ' ' || *p == ',' || *p == '\n') p++;
    if (*p != '{') break;
    const char *ct_p  = strstr(p, "\"city_tile\":");
    const char *pc_p  = strstr(p, "\"prod_cat\":");
    const char *urg_p = strstr(p, "\"urgency\":");
    const char *end_p = strchr(p, '}');
    if (!ct_p || !pc_p || !end_p) break;
    recs[count].city_tile = (int)strtol(ct_p  + strlen("\"city_tile\":"), NULL, 10);
    recs[count].prod_cat  = (int)strtol(pc_p  + strlen("\"prod_cat\":"),  NULL, 10);
    recs[count].urgency   = urg_p
                            ? (float)strtod(urg_p + strlen("\"urgency\":"), NULL)
                            : 0.5f;
    count++;
    p = end_p + 1;
  }
  *n_recs = count;

  double ms = get_double(resp, "kernel_ms", 0.0);
  log_verbose("TT-Lang: city_prod %d cities → %d recs %dx%d: %.3fms",
              n_cities, count, w, h, ms);
  free(resp);
  return true;
}

/***********************************************************************
  Ask TT hardware for advance/hold/retreat orders for own military units.

  TT diffuses an own-strength field (8 smooth_height_map passes) from
  own unit positions, then CPU computes per-unit signal:
    signal = own_strength[tile] - threat[tile]
    signal > +0.25 → ADVANCE toward enemy concentrations
    signal < -0.25 → RETREAT toward friendly strength
    otherwise      → HOLD

  Reuses the threat field cached inside the server from the
  ttlang_city_prod() call earlier in the same turn.
  Must be called AFTER ttlang_city_prod().

  JSON request:
    {"cmd":"combat_pos","w":W,"h":H,"turn":T,
     "own_units":[{"id":N,"tile":M,"strength":S,"moves":1},...]}

  JSON response:
    {"status":"ok",
     "orders":[{"unit_id":N,"action":"advance"|"hold"|"retreat",
                "goto_tile":M},...],
     "kernel_ms":...}
***********************************************************************/
bool ttlang_combat_pos(int w, int h, int turn,
                       const int   *own_unit_ids,
                       const int   *own_unit_tiles,
                       const float *own_strengths,
                       int n_own,
                       ttlang_combat_order_t *orders, int *n_orders)
{
  *n_orders = 0;
  if (g_sock < 0 || n_own <= 0 || n_own > TTLANG_MAX_COMBAT_UNITS) return false;

  /* Build own_units JSON array */
  size_t arr_sz = (size_t)n_own * 64 + 8;
  char  *own_s  = (char *)fc_malloc(arr_sz);
  own_s[0] = '['; own_s[1] = '\0';
  for (int i = 0; i < n_own; i++) {
    char entry[80];
    snprintf(entry, sizeof(entry),
             "%s{\"id\":%d,\"tile\":%d,\"strength\":%.1f,\"moves\":1}",
             i == 0 ? "" : ",",
             own_unit_ids[i], own_unit_tiles[i], (double)own_strengths[i]);
    strncat(own_s, entry, arr_sz - strlen(own_s) - 1);
  }
  strncat(own_s, "]", arr_sz - strlen(own_s) - 1);

  size_t total_sz = arr_sz + 128;
  char  *json     = (char *)fc_malloc(total_sz);
  snprintf(json, total_sz,
           "{\"cmd\":\"combat_pos\",\"w\":%d,\"h\":%d,\"turn\":%d,"
           "\"own_units\":%s}",
           w, h, turn, own_s);
  free(own_s);

  if (!send_msg(g_sock, json)) {
    free(json); g_sock = -1; return false;
  }
  free(json);

  char *resp = recv_msg(g_sock);
  if (!resp || !is_ok(resp)) { free(resp); g_sock = -1; return false; }

  /* Parse "orders":[{"unit_id":N,"action":"advance","goto_tile":M},...] */
  const char *p = strstr(resp, "\"orders\":");
  if (!p) { free(resp); return true; }   /* empty list is valid */
  p += strlen("\"orders\":");
  while (*p == ' ') p++;
  if (*p != '[') { free(resp); return true; }
  p++;

  int count = 0;
  while (*p && *p != ']' && count < n_own) {
    while (*p == ' ' || *p == ',' || *p == '\n') p++;
    if (*p != '{') break;
    const char *uid_p = strstr(p, "\"unit_id\":");
    const char *act_p = strstr(p, "\"action\":\"");
    const char *gt_p  = strstr(p, "\"goto_tile\":");
    const char *end_p = strchr(p, '}');
    if (!uid_p || !end_p) break;

    orders[count].unit_id   = (int)strtol(uid_p + strlen("\"unit_id\":"), NULL, 10);
    orders[count].goto_tile = gt_p
                              ? (int)strtol(gt_p + strlen("\"goto_tile\":"), NULL, 10)
                              : -1;
    orders[count].action    = TTLANG_ACTION_HOLD;   /* default */
    if (act_p && act_p < end_p) {
      act_p += strlen("\"action\":\"");
      if      (strncmp(act_p, "advance", 7) == 0) orders[count].action = TTLANG_ACTION_ADVANCE;
      else if (strncmp(act_p, "retreat", 7) == 0) orders[count].action = TTLANG_ACTION_RETREAT;
    }
    count++;
    p = end_p + 1;
  }
  *n_orders = count;

  double ms = get_double(resp, "kernel_ms", 0.0);
  log_verbose("TT-Lang: combat_pos %d units → %d orders %dx%d: %.3fms",
              n_own, count, w, h, ms);
  free(resp);
  return true;
}
