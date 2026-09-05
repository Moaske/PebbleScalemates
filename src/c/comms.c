#include "comms.h"
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static CommsFeedCallback     s_on_feed     = NULL;
static CommsImageCallback    s_on_image    = NULL;
static CommsErrorCallback    s_on_error    = NULL;
static CommsSettingsCallback s_on_settings = NULL;

// PNG reassembly buffer
static uint8_t *s_png_buf      = NULL;
static size_t   s_png_buf_size = 0;
static size_t   s_png_received = 0;
static int      s_png_item     = -1;
static int      s_png_chunks_n = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void free_png_buf(void) {
  if (s_png_buf) {
    free(s_png_buf);
    s_png_buf      = NULL;
    s_png_buf_size = 0;
    s_png_received = 0;
    s_png_item     = -1;
    s_png_chunks_n = 0;
  }
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *t;

  // --- Clay settings ---
  Tuple *t_uid = dict_find(iter, KEY_CLAY_USER_ID);
  if (t_uid != NULL) {
    const char *user_id = t_uid->value->cstring;
    int feed_enabled[MAX_FEEDS] = {0};
    Tuple *tf;
    tf = dict_find(iter, KEY_CLAY_FEED_STASH);     if (tf) feed_enabled[0] = tf->value->int32;
    tf = dict_find(iter, KEY_CLAY_FEED_WISHLIST);  if (tf) feed_enabled[1] = tf->value->int32;
    tf = dict_find(iter, KEY_CLAY_FEED_STARTED);   if (tf) feed_enabled[2] = tf->value->int32;
    tf = dict_find(iter, KEY_CLAY_FEED_COMPLETED); if (tf) feed_enabled[3] = tf->value->int32;
    tf = dict_find(iter, KEY_CLAY_FEED_FORSALE);   if (tf) feed_enabled[4] = tf->value->int32;
    tf = dict_find(iter, KEY_CLAY_FEED_ONORDER);   if (tf) feed_enabled[5] = tf->value->int32;
    if (s_on_settings) s_on_settings(user_id, feed_enabled);
    return;
  }

  // --- Feed payload (one message, all items packed) ---
  if ((t = dict_find(iter, KEY_FEED_NAME)) != NULL) {
    const char *feed_name = t->value->cstring;
    int item_total = (t = dict_find(iter, KEY_ITEM_TOTAL)) ? t->value->int32 : 0;
    Tuple *items_t = dict_find(iter, KEY_ITEMS);
    const char *payload = (items_t && items_t->type == TUPLE_CSTRING)
                          ? items_t->value->cstring : "";
    if (s_on_feed) s_on_feed(feed_name, item_total, payload);
    return;
  }

  // --- Image chunk ---
  if ((t = dict_find(iter, KEY_IMG_DATA)) != NULL) {
    int  item_idx = dict_find(iter, KEY_IMG_ITEM)    ? dict_find(iter, KEY_IMG_ITEM)->value->int32    : -1;
    int  chunk_i  = dict_find(iter, KEY_IMG_CHUNK_I) ? dict_find(iter, KEY_IMG_CHUNK_I)->value->int32 : 0;
    int  chunk_n  = dict_find(iter, KEY_IMG_CHUNK_N) ? dict_find(iter, KEY_IMG_CHUNK_N)->value->int32 : 1;
    uint8_t *data = t->value->data;
    uint16_t dlen = t->length;

    if (item_idx != s_png_item || chunk_i == 0) {
      free_png_buf();
      s_png_item     = item_idx;
      s_png_chunks_n = chunk_n;
      /* chunk_n * IMG_CHUNK_BYTES rounds up to a chunk boundary. The real
         payload is a 4-byte header plus w*h pixels, and the header is in the
         first chunk, so trim to it when we can — every byte here competes
         with the bitmap allocated while this buffer is still held. */
      s_png_buf_size = (size_t)chunk_n * IMG_CHUNK_BYTES;
      if (chunk_i == 0 && dlen >= 4) {
        size_t exact = 4 + (size_t)((data[0] << 8) | data[1])
                         * (size_t)((data[2] << 8) | data[3]);
        if (exact > 0 && exact <= s_png_buf_size) s_png_buf_size = exact;
      }
      // Decoding needs the PNG plus the output bitmap in heap at once,
      // so refuse anything we clearly cannot decode rather than faulting.
      if (s_png_buf_size > PNG_MAX_BYTES) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "PNG too large: %d bytes",
                (int)s_png_buf_size);
        if (s_on_error) s_on_error("Image too large");
        s_png_buf_size = 0;
        s_png_item     = -1;
        return;
      }
      s_png_buf = malloc(s_png_buf_size);
      if (!s_png_buf) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "PNG buf alloc failed (%d bytes)",
                (int)s_png_buf_size);
        if (s_on_error) s_on_error("PNG buf alloc failed");
        s_png_buf_size = 0;
        s_png_item     = -1;
        return;
      }
    }

    if (!s_png_buf) return;   // allocation was refused for this image
    if (s_png_received + dlen > s_png_buf_size) {
      if (s_on_error) s_on_error("PNG buf overflow");
      free_png_buf();
      return;
    }
    memcpy(s_png_buf + s_png_received, data, dlen);
    s_png_received += dlen;

    if (chunk_i == chunk_n - 1) {
      if (s_on_image) s_on_image(s_png_item, s_png_buf, s_png_received);
      free_png_buf();
    }
    return;
  }

  // --- Error ---
  if ((t = dict_find(iter, KEY_ERROR)) != NULL) {
    if (s_on_error) s_on_error(t->value->cstring);
    return;
  }
}

static void inbox_dropped(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "AppMsg inbox dropped: %d", (int)reason);
}

static void outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "AppMsg outbox failed: %d", (int)reason);
}

static void outbox_sent(DictionaryIterator *iter, void *context) {
  // Intentionally empty — main.c uses this implicitly via the ack timing
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void comms_init(CommsFeedCallback     on_feed,
                CommsImageCallback    on_image,
                CommsErrorCallback    on_error,
                CommsSettingsCallback on_settings) {
  s_on_feed     = on_feed;
  s_on_image    = on_image;
  s_on_error    = on_error;
  s_on_settings = on_settings;

  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_register_outbox_failed(outbox_failed);
  app_message_register_outbox_sent(outbox_sent);

  // Use maximum available buffers as recommended by the official docs
  app_message_open(app_message_inbox_size_maximum(),
                   app_message_outbox_size_maximum());
}

void comms_deinit(void) {
  free_png_buf();
}

void comms_request_feed(int feed_index) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) return;
  dict_write_int32(out, KEY_REQ_FEED, feed_index);
  dict_write_end(out);
  app_message_outbox_send();
}

void comms_request_image(int item_index) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) return;
  dict_write_int32(out, KEY_REQ_IMAGE, item_index);
  dict_write_end(out);
  app_message_outbox_send();
}
