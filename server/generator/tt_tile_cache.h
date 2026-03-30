/***********************************************************************
 TT-Lang FreeCiv Bridge — Per-Turn Tile Score Cache
 Stores TT-hardware-computed tile scores so the FreeCiv AI can query
 them synchronously from city_tile_value() without making a socket call.

 Updated once per turn by ttlang_score_tiles(); read many times per turn
 by the city advisor and settler AI.
***********************************************************************/

#ifndef TT_TILE_CACHE_H
#define TT_TILE_CACHE_H

#include <stdbool.h>

/* Maximum map size we support. FreeCiv default maps are well under 8192. */
#define TT_TILE_CACHE_MAX 16384

/* TT score weight relative to FreeCiv's own FOOD/SHIELD/TRADE weightings.
 * FOOD_WEIGHTING=30, so a 3-food tile scores ~105 base.
 * TT_SCORE_WEIGHT=25 lets TT add 0–1500 points — strongly biasing AI
 * toward TT-preferred land, creating clearly visible settlement patterns. */
#define TT_SCORE_WEIGHT 25

/* Update the cache with a new set of per-tile scores.
 * scores[] must have n_tiles entries, indexed by tile_index().
 * Safe to call from begin_turn(); the cache is write-once-per-turn. */
void tt_tile_cache_update(const float *scores, int n_tiles);

/* Blend a city-influence penalty into the existing cache scores.
 * influence[] must have n_tiles entries in [-0.6, 0].
 * Each cache entry is multiplied by (1 + influence[i]) so tiles near
 * rival cities become less attractive to the AI. */
void tt_tile_cache_blend_influence(const float *influence, int n_tiles);

/* Return the TT score for a tile index (0-based flat index).
 * Returns 0.0 if the cache has not been populated or index is out of range. */
float tt_tile_cache_get(int tile_index);

/* True if the cache has been populated this game session. */
bool tt_tile_cache_valid(void);

#endif /* TT_TILE_CACHE_H */
