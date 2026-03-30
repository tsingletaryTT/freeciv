/***********************************************************************
 TT-Lang FreeCiv Bridge — Per-Turn Tile Score Cache
 See tt_tile_cache.h for the full description.
***********************************************************************/

#ifdef HAVE_CONFIG_H
#  include <fc_config.h>
#endif

#include <string.h>

#include "tt_tile_cache.h"

/* ── Cache state ─────────────────────────────────────────────────────────── */

/* Static flat array; zero-initialised at program start. */
static float g_scores[TT_TILE_CACHE_MAX];
static int   g_n_tiles = 0;   /* 0 = not populated */

/* ── Public API ──────────────────────────────────────────────────────────── */

void tt_tile_cache_update(const float *scores, int n_tiles)
{
  if (n_tiles <= 0 || n_tiles > TT_TILE_CACHE_MAX) return;
  memcpy(g_scores, scores, (size_t)n_tiles * sizeof(float));
  g_n_tiles = n_tiles;
}

float tt_tile_cache_get(int tile_idx)
{
  if (tile_idx < 0 || tile_idx >= g_n_tiles) return 0.0f;
  return g_scores[tile_idx];
}

bool tt_tile_cache_valid(void)
{
  return g_n_tiles > 0;
}

void tt_tile_cache_blend_influence(const float *influence, int n_tiles)
{
  int i;
  if (n_tiles <= 0 || n_tiles > g_n_tiles) return;
  for (i = 0; i < n_tiles; i++) {
    /* influence[i] ∈ [-0.6, 0]; clamp before multiplying */
    float mod = influence[i];
    if (mod < -0.6f) mod = -0.6f;
    if (mod >  0.0f) mod =  0.0f;
    g_scores[i] *= (1.0f + mod);
  }
}
