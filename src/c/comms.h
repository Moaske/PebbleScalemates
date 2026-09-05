#pragma once
#include <pebble.h>
#include "message_keys.auto.h"
#include "feed.h"   // MAX_FEEDS

// App protocol keys — aliases for the auto-generated MESSAGE_KEY_* names
#define KEY_FEED_NAME    MESSAGE_KEY_FEED_NAME
#define KEY_ITEM_INDEX   MESSAGE_KEY_ITEM_INDEX
#define KEY_ITEM_TOTAL   MESSAGE_KEY_ITEM_TOTAL
#define KEY_ITEMS        MESSAGE_KEY_ITEMS
#define KEY_KIT_NO       MESSAGE_KEY_KIT_NO
#define KEY_BRAND        MESSAGE_KEY_BRAND
#define KEY_NAME         MESSAGE_KEY_NAME
#define KEY_SCALE        MESSAGE_KEY_SCALE
#define KEY_IMG_ITEM     MESSAGE_KEY_IMG_ITEM
#define KEY_IMG_CHUNK_I  MESSAGE_KEY_IMG_CHUNK_I
#define KEY_IMG_CHUNK_N  MESSAGE_KEY_IMG_CHUNK_N
#define KEY_IMG_DATA     MESSAGE_KEY_IMG_DATA
#define KEY_REQ_FEED     MESSAGE_KEY_REQ_FEED
#define KEY_REQ_IMAGE    MESSAGE_KEY_REQ_IMAGE
#define KEY_ERROR        MESSAGE_KEY_ERROR
#define KEY_YEAR         MESSAGE_KEY_YEAR
#define KEY_TYPE         MESSAGE_KEY_TYPE

// Clay settings keys
#define KEY_CLAY_USER_ID        MESSAGE_KEY_user_id
#define KEY_CLAY_FEED_STASH     MESSAGE_KEY_feed_stash
#define KEY_CLAY_FEED_WISHLIST  MESSAGE_KEY_feed_wishlist
#define KEY_CLAY_FEED_STARTED   MESSAGE_KEY_feed_started
#define KEY_CLAY_FEED_COMPLETED MESSAGE_KEY_feed_completed
#define KEY_CLAY_FEED_FORSALE   MESSAGE_KEY_feed_forsale
#define KEY_CLAY_FEED_ONORDER   MESSAGE_KEY_feed_onorder

// Chunk size in bytes — leaves room for other keys in the same message.
// AppMessage inbox is opened at 2048 bytes; PNG chunks of 512 bytes each
// give us ~4 messages per KB of image data.
#define IMG_CHUNK_BYTES  2048

// Upper bound on a received image payload (raw 8-bit pixels + 4-byte header).
// 200 x 120 + 4 = 24004, rounded up to a chunk boundary.
#define PNG_MAX_BYTES    32768

// Callbacks the rest of the app subscribes to
// Called once per feed with the whole packed payload.
// payload format: kit_no|brand|name|scale|year|type  (newline separated)
typedef void (*CommsFeedCallback)(const char *feed_name, int item_total,
                                  const char *payload);

typedef void (*CommsImageCallback)(int item_index, const uint8_t *png_data,
                                   size_t png_len);

typedef void (*CommsErrorCallback)(const char *message);

// Called when Clay settings arrive from the phone after the user saves config.
// user_id is the Scalemates user ID string; feed_enabled is a 5-element array
// indexed by FeedId (0=Stash … 4=ForSale), value 1=enabled, 0=disabled.
typedef void (*CommsSettingsCallback)(const char *user_id,
                                      int feed_enabled[MAX_FEEDS]);

void comms_init(CommsFeedCallback     on_feed,
                CommsImageCallback    on_image,
                CommsErrorCallback    on_error,
                CommsSettingsCallback on_settings);

void comms_deinit(void);

// Outbound requests
void comms_request_feed(int feed_index);
void comms_request_image(int item_index);
