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
  { FEED_ONORDER,   "On Order",  false, NULL, 0, 0, false },
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

/* Parse one packed line: kit_no|brand|name|scale|year|type
   strchr field splitting, as in FuelWatch. */
static bool parse_item_line(char *line, StashItem *out) {
  char *p = line;
  char *sep;
  int field = 0;

  while (field < 6) {
    sep = strchr(p, '|');
    if (sep) *sep = '\0';

    switch (field) {
      case 0: strncpy(out->kit_no, p, MAX_STR_LEN - 1);
              out->kit_no[MAX_STR_LEN - 1] = '\0'; break;
      case 1: strncpy(out->brand,  p, MAX_STR_LEN - 1);
              out->brand[MAX_STR_LEN - 1]  = '\0'; break;
      case 2: strncpy(out->name,   p, MAX_NAME_LEN - 1);
              out->name[MAX_NAME_LEN - 1]  = '\0'; break;
      case 3: strncpy(out->scale,  p, MAX_STR_LEN - 1);
              out->scale[MAX_STR_LEN - 1]  = '\0'; break;
      case 4: strncpy(out->year,   p, sizeof(out->year) - 1);
              out->year[sizeof(out->year) - 1] = '\0'; break;
      case 5: strncpy(out->type,   p, sizeof(out->type) - 1);
              out->type[sizeof(out->type) - 1] = '\0'; break;
    }

    field++;
    if (!sep) break;
    p = sep + 1;
  }

  return (field == 6);
}

void feed_set_payload(const char *feed_name, int item_total,
                      const char *payload) {
  Feed *f = feed_by_name(feed_name);
  if (!f) return;

  /* Only the feed being shown needs its items in memory. Keeping every feed
     resident costs ~272 bytes per item, so five full feeds would be ~66 KB —
     over half the heap, before the image bitmap and receive buffer. The phone
     caches the parsed items, so re-entering a feed costs one small message,
     not a refetch. */
  for (int i = 0; i < MAX_FEEDS; i++) {
    if (&s_feeds[i] != f) free_feed_items(&s_feeds[i]);
  }

  free_feed_items(f);

  int alloc = (item_total < MAX_ITEMS) ? item_total : MAX_ITEMS;
  if (alloc <= 0) {
    f->item_total = 0;
    f->loading    = false;
    return;
  }

  f->items = malloc(sizeof(StashItem) * alloc);
  if (!f->items) return;
  memset(f->items, 0, sizeof(StashItem) * alloc);
  f->item_total = item_total;

  char *buf = malloc(strlen(payload) + 1);
  if (!buf) return;
  strcpy(buf, payload);

  char *p = buf;
  while (p && *p && f->item_count < alloc) {
    char *nl = strchr(p, '\n');
    if (nl) *nl = '\0';

    StashItem it;
    memset(&it, 0, sizeof(it));
    if (parse_item_line(p, &it)) {
      f->items[f->item_count++] = it;
    }
    p = nl ? nl + 1 : NULL;
  }

  free(buf);
  f->loading = false;
  APP_LOG(APP_LOG_LEVEL_INFO, "Parsed %d items for %s", f->item_count, f->label);
}

/* Only one image is ever on screen (the detail view), so keep exactly one
   decoded bitmap alive. Holding every image visited would grow the heap
   until PNG decoding of the next one fails. */
void feed_clear_images(void) {
  Feed *f = feed_get(s_active);
  if (!f || !f->items) return;
  for (int i = 0; i < f->item_count; i++) {
    if (f->items[i].bmp) {
      gbitmap_destroy(f->items[i].bmp);
      f->items[i].bmp = NULL;
    }
    f->items[i].image_ready = false;
  }
}

void feed_set_image(int item_index, GBitmap *bmp) {
  Feed *f = feed_get(s_active);
  if (!f || item_index < 0 || item_index >= f->item_count) {
    if (bmp) gbitmap_destroy(bmp);
    return;
  }

  for (int i = 0; i < f->item_count; i++) {
    if (f->items[i].bmp) {
      gbitmap_destroy(f->items[i].bmp);
      f->items[i].bmp = NULL;
    }
    f->items[i].image_ready = false;
  }

  f->items[item_index].bmp         = bmp;
  f->items[item_index].image_ready = (bmp != NULL);
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
