#pragma once
#include <pebble.h>

#define MAX_FEEDS        5
#define MAX_ITEMS        50   // max items cached per feed
#define MAX_STR_LEN      48   // kit_no, brand, scale
#define MAX_NAME_LEN     80   // kit name can be longer

// Feed identifiers — must match order sent from JS / Clay checkboxes
typedef enum {
  FEED_STASH     = 0,
  FEED_WISHLIST  = 1,
  FEED_STARTED   = 2,
  FEED_COMPLETED = 3,
  FEED_FORSALE   = 4,
} FeedId;

typedef struct {
  char kit_no[MAX_STR_LEN];
  char brand[MAX_STR_LEN];
  char name[MAX_NAME_LEN];
  char scale[MAX_STR_LEN];
  bool image_ready;          // true once PNG has been decoded into bmp
  GBitmap *bmp;              // NULL until image arrives
} StashItem;

typedef struct {
  FeedId      id;
  char        label[16];     // "Stash", "Wishlist", etc.
  bool        enabled;       // set from Clay settings
  StashItem  *items;         // heap-allocated array, length = item_count
  int         item_count;
  int         item_total;    // total reported by phone (may exceed MAX_ITEMS)
  bool        loading;       // phone is still sending items
} Feed;

// Initialise all feeds (disabled by default; call feed_set_enabled to enable).
void feed_init(void);
void feed_deinit(void);

void feed_set_enabled(FeedId id, bool enabled);
bool feed_is_enabled(FeedId id);

Feed      *feed_get(FeedId id);
StashItem *feed_get_item(FeedId id, int index);

// Called by comms callbacks to populate data
void feed_set_meta(const char *feed_name,
                   int item_index, int item_total,
                   const char *kit_no, const char *brand,
                   const char *name, const char *scale);

void feed_set_image(int item_index, GBitmap *bmp);

// Active feed tracking (which feed is currently shown)
FeedId feed_active(void);
void   feed_set_active(FeedId id);

// Navigate to next/prev enabled feed; returns true if feed changed.
bool feed_next(void);
bool feed_prev(void);
