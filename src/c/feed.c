#include "feed.h"
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Static feed table
// ---------------------------------------------------------------------------

static Feed s_feeds[MAX_FEEDS] = {
  { FEED_STASH,     "Stash",     false, NULL, 0, 0, false },
  { FEED_WISHLIST,  "Wishlist",  false, NULL, 0, 0, false },
  { FEED_STARTED,   "Started",   false, NULL, 0, 0, false },
  { FEED_COMPLETED, "Completed", false, NULL, 0, 0, false },
  { FEED_FORSALE,   "For Sale",  false, NULL, 0, 0, false },
};

static FeedId s_active = FEED_STASH;

// The active feed's item index — stored here so feed switches reset it
static int s_active_item_index = 0;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static void free_feed_items(Feed *f) {
  if (!f->items) return;
  for (int i = 0; i < f->item_count; i++) {
    if (f->items[i].bmp) {
      gbitmap_destroy(f->items[i].bmp);
      f->items[i].bmp = NULL;
    }
  }
  free(f->items);
  f->items      = NULL;
  f->item_count = 0;
  f->item_total = 0;
  f->loading    = false;
}

static Feed *feed_by_name(const char *name) {
  for (int i = 0; i < MAX_FEEDS; i++) {
    if (strncmp(s_feeds[i].label, name, sizeof(s_feeds[i].label)) == 0) {
      return &s_feeds[i];
    }
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void feed_init(void) {
  for (int i = 0; i < MAX_FEEDS; i++) {
    s_feeds[i].items      = NULL;
    s_feeds[i].item_count = 0;
    s_feeds[i].item_total = 0;
    s_feeds[i].loading    = false;
    s_feeds[i].enabled    = false;
  }
}

void feed_deinit(void) {
  for (int i = 0; i < MAX_FEEDS; i++) {
    free_feed_items(&s_feeds[i]);
  }
}

void feed_set_enabled(FeedId id, bool enabled) {
  if (id >= MAX_FEEDS) return;
  s_feeds[id].enabled = enabled;
}

bool feed_is_enabled(FeedId id) {
  if (id >= MAX_FEEDS) return false;
  return s_feeds[id].enabled;
}

Feed *feed_get(FeedId id) {
  if (id >= MAX_FEEDS) return NULL;
  return &s_feeds[id];
}

StashItem *feed_get_item(FeedId id, int index) {
  Feed *f = feed_get(id);
  if (!f || index < 0 || index >= f->item_count) return NULL;
  return &f->items[index];
}

void feed_set_meta(const char *feed_name,
                   int item_index, int item_total,
                   const char *kit_no, const char *brand,
                   const char *name, const char *scale) {
  Feed *f = feed_by_name(feed_name);
  if (!f) return;

  f->item_total = item_total;
  f->loading    = true;

  // Allocate item array on first item for this feed
  if (item_index == 0 || !f->items) {
    free_feed_items(f);
    int alloc = (item_total < MAX_ITEMS) ? item_total : MAX_ITEMS;
    if (alloc == 0) alloc = 1;
    f->items = malloc(sizeof(StashItem) * alloc);
    if (!f->items) return;
    memset(f->items, 0, sizeof(StashItem) * alloc);
  }

  if (item_index >= MAX_ITEMS) return;  // cap silently

  StashItem *it = &f->items[item_index];
  strncpy(it->kit_no, kit_no, MAX_STR_LEN - 1);
  strncpy(it->brand,  brand,  MAX_STR_LEN - 1);
  strncpy(it->name,   name,   MAX_NAME_LEN - 1);
  strncpy(it->scale,  scale,  MAX_STR_LEN - 1);
  it->image_ready = false;
  it->bmp         = NULL;

  if (item_index + 1 > f->item_count) {
    f->item_count = item_index + 1;
  }
  // When the last item arrives, mark loading done
  if (item_index + 1 >= item_total) {
    f->loading = false;
  }
}

void feed_set_image(int item_index, GBitmap *bmp) {
  // Image belongs to whichever active feed sent the request
  Feed *f = feed_get(s_active);
  if (!f || item_index < 0 || item_index >= f->item_count) {
    gbitmap_destroy(bmp);
    return;
  }
  StashItem *it = &f->items[item_index];
  if (it->bmp) gbitmap_destroy(it->bmp);
  it->bmp         = bmp;
  it->image_ready = true;
}

// ---------------------------------------------------------------------------
// Active feed navigation
// ---------------------------------------------------------------------------

FeedId feed_active(void) {
  return s_active;
}

void feed_set_active(FeedId id) {
  if (id >= MAX_FEEDS) return;
  s_active            = id;
  s_active_item_index = 0;
}

bool feed_next(void) {
  for (int i = s_active + 1; i < MAX_FEEDS; i++) {
    if (s_feeds[i].enabled) {
      feed_set_active((FeedId)i);
      return true;
    }
  }
  return false;  // already on last enabled feed
}

bool feed_prev(void) {
  for (int i = s_active - 1; i >= 0; i--) {
    if (s_feeds[i].enabled) {
      feed_set_active((FeedId)i);
      return true;
    }
  }
  return false;  // already on first enabled feed
}
