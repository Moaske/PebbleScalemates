#include "comms.h"
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static CommsMetaCallback     s_on_meta     = NULL;
static CommsImageCallback    s_on_image    = NULL;
static CommsErrorCallback    s_on_error    = NULL;
static CommsSettingsCallback s_on_settings = NULL;

// PNG reassembly buffer — allocated when first chunk arrives, freed after
// firing the callback (or on error / new item arriving before completion).
static uint8_t *s_png_buf      = NULL;
static size_t   s_png_buf_size = 0;   // total allocated
static size_t   s_png_received = 0;   // bytes received so far
static int      s_png_item     = -1;  // which item we're building
static int      s_png_chunks_n = 0;   // total chunks expected

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
// Inbox received
// ---------------------------------------------------------------------------

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *t;

  // --- Metadata message ---
  if ((t = dict_find(iter, KEY_FEED_NAME)) != NULL) {
    const char *feed_name = t->value->cstring;
    int item_index  = (t = dict_find(iter, KEY_ITEM_INDEX)) ? t->value->int32 : 0;
    int item_total  = (t = dict_find(iter, KEY_ITEM_TOTAL)) ? t->value->int32 : 0;
    const char *kit_no = (t = dict_find(iter, KEY_KIT_NO)) ? t->value->cstring : "";
    const char *brand  = (t = dict_find(iter, KEY_BRAND))  ? t->value->cstring : "";
    const char *name   = (t = dict_find(iter, KEY_NAME))   ? t->value->cstring : "";
    const char *scale  = (t = dict_find(iter, KEY_SCALE))  ? t->value->cstring : "";

    if (s_on_meta) {
      s_on_meta(feed_name, item_index, item_total, kit_no, brand, name, scale);
    }
    return;
  }

  // --- Image chunk message ---
  if ((t = dict_find(iter, KEY_IMG_DATA)) != NULL) {
    int  item_idx  = dict_find(iter, KEY_IMG_ITEM)    ? dict_find(iter, KEY_IMG_ITEM)->value->int32    : -1;
    int  chunk_i   = dict_find(iter, KEY_IMG_CHUNK_I) ? dict_find(iter, KEY_IMG_CHUNK_I)->value->int32 : 0;
    int  chunk_n   = dict_find(iter, KEY_IMG_CHUNK_N) ? dict_find(iter, KEY_IMG_CHUNK_N)->value->int32 : 1;
    uint8_t *data  = t->value->data;
    uint16_t dlen  = t->length;

    // If this is the first chunk for this item, (re)allocate buffer.
    // We don't know final size up front, so grow as needed.
    if (item_idx != s_png_item || chunk_i == 0) {
      free_png_buf();
      s_png_item     = item_idx;
      s_png_chunks_n = chunk_n;
      // Allocate generously; chunk_n * IMG_CHUNK_BYTES is a safe upper bound.
      s_png_buf_size = (size_t)chunk_n * IMG_CHUNK_BYTES;
      s_png_buf      = malloc(s_png_buf_size);
      if (!s_png_buf) {
        if (s_on_error) s_on_error("PNG buf alloc failed");
        return;
      }
    }

    // Append chunk data.
    if (s_png_received + dlen > s_png_buf_size) {
      // Shouldn't happen, but guard anyway.
      if (s_on_error) s_on_error("PNG buf overflow");
      free_png_buf();
      return;
    }
    memcpy(s_png_buf + s_png_received, data, dlen);
    s_png_received += dlen;

    // Last chunk — fire callback then free.
    if (chunk_i == chunk_n - 1) {
      if (s_on_image) {
        s_on_image(s_png_item, s_png_buf, s_png_received);
      }
      free_png_buf();
    }
    return;
  }

  // --- Clay settings message ---
  // Clay sends settings using the numeric keys from message_keys.auto.h.
  // Detect by presence of user_id or any feed toggle key.
  Tuple *t_uid = dict_find(iter, KEY_CLAY_USER_ID);
  if (t_uid != NULL) {
    const char *user_id = t_uid->value->cstring;
    int feed_enabled[5] = {0, 0, 0, 0, 0};
    Tuple *tf;
    tf = dict_find(iter, KEY_CLAY_FEED_STASH);
    if (tf) feed_enabled[0] = tf->value->int32;
    tf = dict_find(iter, KEY_CLAY_FEED_WISHLIST);
    if (tf) feed_enabled[1] = tf->value->int32;
    tf = dict_find(iter, KEY_CLAY_FEED_STARTED);
    if (tf) feed_enabled[2] = tf->value->int32;
    tf = dict_find(iter, KEY_CLAY_FEED_COMPLETED);
    if (tf) feed_enabled[3] = tf->value->int32;
    tf = dict_find(iter, KEY_CLAY_FEED_FORSALE);
    if (tf) feed_enabled[4] = tf->value->int32;
    if (s_on_settings) s_on_settings(user_id, feed_enabled);
    return;
  }

  // --- Error message ---
  if ((t = dict_find(iter, KEY_ERROR)) != NULL) {
    if (s_on_error) s_on_error(t->value->cstring);
    return;
  }
}

static void inbox_dropped(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "AppMsg dropped: %d", (int)reason);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void comms_init(CommsMetaCallback     on_meta,
                CommsImageCallback    on_image,
                CommsErrorCallback    on_error,
                CommsSettingsCallback on_settings) {
  s_on_meta     = on_meta;
  s_on_image    = on_image;
  s_on_error    = on_error;
  s_on_settings = on_settings;

  // 2 KB inbox — enough for a 512-byte PNG chunk plus metadata keys.
  // 256-byte outbox — requests are tiny.
  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_open(2048, 256);
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
