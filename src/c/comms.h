#pragma once
#include <pebble.h>
#include "message_keys.auto.h"

// App protocol keys — aliases for the auto-generated MESSAGE_KEY_* names
// appinfo.json lists "FEED_NAME" → SDK generates MESSAGE_KEY_FEED_NAME
#define KEY_FEED_NAME    MESSAGE_KEY_FEED_NAME
#define KEY_ITEM_INDEX   MESSAGE_KEY_ITEM_INDEX
#define KEY_ITEM_TOTAL   MESSAGE_KEY_ITEM_TOTAL
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

// Clay settings keys
#define KEY_CLAY_USER_ID        MESSAGE_KEY_user_id
#define KEY_CLAY_FEED_STASH     MESSAGE_KEY_feed_stash
#define KEY_CLAY_FEED_WISHLIST  MESSAGE_KEY_feed_wishlist
#define KEY_CLAY_FEED_STARTED   MESSAGE_KEY_feed_started
#define KEY_CLAY_FEED_COMPLETED MESSAGE_KEY_feed_completed
#define KEY_CLAY_FEED_FORSALE   MESSAGE_KEY_feed_forsale

// Chunk size in bytes — leaves room for other keys in the same message.
// AppMessage inbox is opened at 2048 bytes; PNG chunks of 512 bytes each
// give us ~4 messages per KB of image data.
#define IMG_CHUNK_BYTES  512

// Callbacks the rest of the app subscribes to
typedef void (*CommsMetaCallback)(const char *feed_name,
                                  int item_index, int item_total,
                                  const char *kit_no, const char *brand,
                                  const char *name, const char *scale);

typedef void (*CommsImageCallback)(int item_index, const uint8_t *png_data,
                                   size_t png_len);

typedef void (*CommsErrorCallback)(const char *message);

// Called when Clay settings arrive from the phone after the user saves config.
// user_id is the Scalemates user ID string; feed_enabled is a 5-element array
// indexed by FeedId (0=Stash … 4=ForSale), value 1=enabled, 0=disabled.
typedef void (*CommsSettingsCallback)(const char *user_id,
                                      int feed_enabled[5]);

void comms_init(CommsMetaCallback     on_meta,
                CommsImageCallback    on_image,
                CommsErrorCallback    on_error,
                CommsSettingsCallback on_settings);

void comms_deinit(void);

// Outbound requests
void comms_request_feed(int feed_index);
void comms_request_image(int item_index);
