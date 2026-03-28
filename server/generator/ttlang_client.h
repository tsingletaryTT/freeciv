/***********************************************************************
 TT-Lang FreeCiv Bridge — Client Header
 Connects FreeCiv server to the persistent TT-Lang Python socket server
 that runs TT-Lang kernels on Tenstorrent P300C Blackhole hardware.
***********************************************************************/

#ifndef TTLANG_CLIENT_H
#define TTLANG_CLIENT_H

#include <stdbool.h>

/* Path to the Unix domain socket created by ttlang_server.py */
#define TTLANG_SOCKET_PATH "/tmp/ttlang_freeciv.sock"

/* Maximum number of terrain-event tiles returned per turn.
 * Python generates ~300 events/turn on a size-8 map; 512 captures all of them. */
#define TTLANG_MAX_EVENTS 512

/* Maximum tiles scored per call (matching typical FreeCiv map size) */
#define TTLANG_MAX_TILES  8192

/* ── Connection management ─────────────────────────────────────────────── */

/* Attempt to connect to the TT-Lang server.
 * Returns true on success, false if server is not available.
 * Safe to call multiple times; reconnects if disconnected. */
bool ttlang_connect(void);

/* Close the connection (called on server shutdown). */
void ttlang_disconnect(void);

/* Returns true if the server is reachable and the connection is live. */
bool ttlang_available(void);

/* ── Terrain generation ────────────────────────────────────────────────── */

/* Generate a height map of (w x h) tiles using TT hardware.
 * Fills height_map[0..w*h-1] with values in [0, 1000].
 * Returns true on success, false if server unavailable (caller uses fallback). */
bool ttlang_terrain_gen(int *height_map, int w, int h, int seed);

/* ── Per-turn terrain events ───────────────────────────────────────────── */

typedef struct {
  int  tile_idx;           /* flat tile index into height_map / map */
  char extra_name[32];     /* FreeCiv extra rule name e.g. "Gold", "Oil" */
} ttlang_event_t;

/* Ask TT hardware to generate terrain events for this turn.
 * Fills events[0..*n_events-1].  *n_events <= TTLANG_MAX_EVENTS.
 * Returns true on success. */
bool ttlang_turn_events(int turn, int w, int h,
                        ttlang_event_t *events, int *n_events);

/* ── AI tile scoring ───────────────────────────────────────────────────── */

/* Score all (w*h) tiles using TT hardware.
 * food[], shields[], trade[] are per-tile values (flat row-major order).
 * turn is the current game turn (passed to Python for weather computation).
 * scores_out[] is filled with score per tile (same layout).
 * top_tiles_out[] receives the top-N tile indices; *n_top is set on return.
 * disaster_active_out: set to 1 if a disaster is active this turn, else 0.
 * disaster_epi_out: flat tile index of the disaster epicenter (valid when active).
 * Returns true on success. */
bool ttlang_score_tiles(int w, int h, int turn,
                        const float *food,
                        const float *shields,
                        const float *trade,
                        float *scores_out,
                        int   *top_tiles_out,
                        int   *n_top,
                        int   *disaster_active_out,
                        int   *disaster_epi_out);

/* ── City territorial influence ────────────────────────────────────────── */

/* Maximum number of city entries that can be sent per call */
#define TTLANG_MAX_CITIES 512

/* One city entry: flat tile index + owning player index */
typedef struct {
  int tile_idx;    /* flat row-major tile index */
  int player_id;   /* player index (0-based) */
} ttlang_city_t;

/* Compute a per-tile territorial influence field using TT hardware.
 *
 * Each city radiates influence outward via 2D Gaussian diffusion
 * (smooth_height_map applied with a neighbour-shifted copy of the field).
 * Rival-city influence is returned as a negative modifier in [-0.6, 0].
 *
 * influence_out[] is filled with one float per tile (flat row-major).
 * Returns true on success; caller must pre-allocate w*h floats. */
bool ttlang_city_influence(int w, int h, int turn,
                           const ttlang_city_t *cities, int n_cities,
                           float *influence_out);

/* ── Per-settler movement recommendations ──────────────────────────────── */

/* Maximum number of settlers whose positions can be sent per call */
#define TTLANG_MAX_UNITS 64

/* One recommendation from TT hardware: settler → best destination tile */
typedef struct {
  int unit_id;     /* FreeCiv unit ID (use game_unit_by_number() to look up) */
  int best_tile;   /* flat tile index of TT-recommended destination */
} ttlang_unit_rec_t;

/* Ask TT hardware for per-settler optimal destinations.
 *
 * unit_ids[0..n_units-1]   : FreeCiv unit IDs of idle settlers
 * unit_tiles[0..n_units-1] : current flat tile index of each settler
 * n_units                  : number of settlers (≤ TTLANG_MAX_UNITS)
 *
 * On success:
 *   recs[0..*n_recs-1] are filled with {unit_id, best_tile} pairs.
 *   Returns true.
 *
 * Reuses the pathfield cached from the tile_score call earlier in begin_turn().
 * Must be called AFTER ttlang_score_tiles() in the same turn. */
bool ttlang_unit_eval(int w, int h, int turn,
                      const int *unit_ids, const int *unit_tiles, int n_units,
                      ttlang_unit_rec_t *recs, int *n_recs);

/* ── City production priority recommendations ──────────────────────────── */

/* Production category indices — must match Python's PROD_* constants */
#define TTLANG_PROD_MILITARY  0
#define TTLANG_PROD_GROWTH    1
#define TTLANG_PROD_SCIENCE   2

/* One production recommendation returned by TT hardware for an AI city */
typedef struct {
  int   city_tile;   /* flat tile index of the city */
  int   prod_cat;    /* TTLANG_PROD_MILITARY / GROWTH / SCIENCE */
  float urgency;     /* [0, 1] how strongly TT recommends this category */
} ttlang_city_prod_rec_t;

/* Ask TT hardware for city production priorities.
 *
 * TT runs ThreatFieldModel (8-pass diffusion from enemy positions) and
 * CityProdModel (6 smooth_height_map passes) on P300C hardware.
 * The computed threat field is cached in the server for combat_pos reuse.
 *
 * city_tiles[0..n_cities-1]         : flat tile index of each AI city
 * city_food/shields/trade/pop       : per-city yield values
 * n_cities                          : number of cities (≤ TTLANG_MAX_CITIES)
 * enemy_tiles/enemy_strengths       : visible enemy military unit positions
 * n_enemy                           : number of enemy units (≤ TTLANG_MAX_UNITS)
 * recs[0..*n_recs-1]                : filled with {city_tile, prod_cat, urgency}
 *
 * Must be called BEFORE ttlang_combat_pos() in the same turn (caches threat). */
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
                      ttlang_city_prod_rec_t *recs, int *n_recs);

/* ── Combat positioning orders ─────────────────────────────────────────── */

/* Action codes for each military unit */
#define TTLANG_ACTION_ADVANCE  0
#define TTLANG_ACTION_HOLD     1
#define TTLANG_ACTION_RETREAT  2

/* Maximum number of own military units sent per call */
#define TTLANG_MAX_COMBAT_UNITS 128

/* One combat positioning order from TT hardware */
typedef struct {
  int  unit_id;    /* FreeCiv unit ID (use game_unit_by_number() to look up) */
  int  action;     /* TTLANG_ACTION_ADVANCE / HOLD / RETREAT */
  int  goto_tile;  /* recommended destination tile; -1 for HOLD */
} ttlang_combat_order_t;

/* Ask TT hardware for advance/hold/retreat orders for own military units.
 *
 * TT diffuses an own-strength field (8 passes) then CPU computes:
 *   signal = own_strength[tile] - threat[tile]
 *   signal > +0.25 → ADVANCE toward enemy concentrations
 *   signal < -0.25 → RETREAT toward own strength
 *   otherwise      → HOLD
 *
 * Reuses the threat field cached from ttlang_city_prod() earlier this turn.
 * Must be called AFTER ttlang_city_prod() in the same turn.
 *
 * own_unit_ids[0..n_own-1]  : FreeCiv unit IDs of AI military units
 * own_unit_tiles[0..n_own-1]: current flat tile indices
 * own_strengths[0..n_own-1] : unit attack power (raw FreeCiv value)
 * n_own                     : number of units (≤ TTLANG_MAX_COMBAT_UNITS)
 * orders[0..*n_orders-1]    : filled with {unit_id, action, goto_tile} */
bool ttlang_combat_pos(int w, int h, int turn,
                       const int   *own_unit_ids,
                       const int   *own_unit_tiles,
                       const float *own_strengths,
                       int n_own,
                       ttlang_combat_order_t *orders, int *n_orders);

/* ── Civilization generation ────────────────────────────────────────────── */

/* Maximum number of civilizations that can be generated per call */
#define TTLANG_MAX_CIVS 8

/* Data for one TT-generated civilization */
typedef struct {
  int   index;           /* 0-based civ index */
  char  slug[24];        /* unique lowercase identifier e.g. "romai" */
  char  leader[48];      /* leader name e.g. "Augustus" */
  char  nation[48];      /* nation name e.g. "Romaia" */
  char  adjective[48];   /* adjectival form e.g. "Roman" */
  char  city1[48];       /* primary city name */
  char  city2[48];       /* secondary city name */
  char  city3[48];       /* tertiary city name */
  int   color_r;         /* primary color red component (0-255) */
  int   color_g;         /* primary color green component (0-255) */
  int   color_b;         /* primary color blue component (0-255) */
  char  flag_path[256];  /* absolute path to generated flag PNG */
  char  portrait_path[256]; /* absolute path to generated portrait PNG */
  char  ruleset_path[256];  /* absolute path to generated .ruleset file */
  /* AI personality (0-100 each) */
  int   aggression;
  int   expansion;
  int   military;
  int   science;
} ttlang_civ_t;

/* Ask TT hardware to generate n_civs complete civilizations for the given seed.
 *
 * On success:
 *   civs[0..n_civs-1] are filled with civ data.
 *   output_dir (if non-NULL, at least 256 bytes) receives the path to where
 *   flags, portraits, and ruleset files were written.
 *   Returns true.
 *
 * On failure (server not available, civgen not installed, etc.):
 *   Returns false; caller should use built-in FreeCiv nations.
 */
bool ttlang_gen_civs(int n_civs, int seed,
                     ttlang_civ_t *civs,
                     char *output_dir);

#endif /* TTLANG_CLIENT_H */
