#include <pebble.h>
#include "comms.h"
#include "feed.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Layout constants  (Emery: 200 × 228 px)
// ---------------------------------------------------------------------------
#define SCREEN_W         200
#define SCREEN_H         228
#define HEADER_H          24   // SEGOEUIB_18
#define LOGO_W            80
#define LOGO_H            11
#define ITEM_PAD           4
#define DIVIDER_H          1
#define LINE1_H           22   // SEGOEUIB_18
#define LINE2_H           25   // one line of SEGOEUIB_20 (list: ellipsised)
#define LINE3_H           21   // SEGOEUIB_18
#define NAME_LINE_H       25   // one line of SEGOEUIB_20
#define NAME_MAX_LINES     4   // detail view only — no row height to respect
#define TEXT_PAD_X         4
/* Line boxes are tight to the glyph heights so the three lines read as one
   block, with ITEM_PAD doing the separating between rows instead of loose
   leading inside them. List rows stay a fixed height: names are ellipsised
   here, and measuring every name on load was the slow part. Wrapping happens
   in the detail view. */
#define ITEM_H            (LINE1_H + LINE2_H + LINE3_H + ITEM_PAD * 2 + DIVIDER_H)
#define TEXT_W            (SCREEN_W - TEXT_PAD_X * 2)

#define DETAIL_IMG_MAX_W  200
#define DETAIL_IMG_MAX_H  120
#define DETAIL_SCROLL_STEP 30

// Persistent storage: one bool per feed, so the watch remembers which feeds
// are enabled across launches instead of waiting for Clay to push settings.
#define PERSIST_KEY_FEED_BASE  100

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------
static Window    *s_list_window;
static Window    *s_detail_window;
static Layer     *s_list_canvas;
static Layer     *s_detail_canvas;

static Recognizer *s_list_tap;
static Recognizer *s_list_pan;
static Recognizer *s_list_swipe;
static Recognizer *s_detail_pan;

static int16_t s_list_scroll   = 0;
static int16_t s_detail_scroll = 0;
static int      s_selected_item = 0;

static GBitmap *s_logo = NULL;

// True between sending a feed request and the payload (or an error) landing.
// Distinguishes "still loading" from "this feed is genuinely empty".
static bool s_waiting_for_feed = false;

// Last message from the phone (e.g. no user ID configured). Shown instead of
// "No items" so a fresh install explains itself rather than looking broken.
static char s_status_msg[48] = "";

static GFont s_font_header;
static GFont s_font_bold;
static GFont s_font_normal;
static GFont s_font_small;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void update_list(void);
static void update_detail(void);
static void request_active_feed(void);
static void open_detail(int item_index);
static void cycle_feed(void);
static void fling_stop(void);
static bool recognizer_always_simultaneous(const Recognizer *r1, const Recognizer *r2);
static void save_feed_settings(void);
static void load_feed_settings(void);

// ---------------------------------------------------------------------------
// Scroll helpers
// ---------------------------------------------------------------------------
static int list_max_scroll(void) {
  Feed *f = feed_get(feed_active());
  int content_h = f ? f->item_count * ITEM_H : 0;
  int visible_h = SCREEN_H - HEADER_H;
  return (content_h > visible_h) ? content_h - visible_h : 0;
}

static int16_t clamp_list_scroll(int val) {
  if (val < 0) val = 0;
  int mx = list_max_scroll();
  if (val > mx) val = mx;
  return (int16_t)val;
}

static int item_top(int index) {
  return index * ITEM_H;
}

static int item_screen_y(int index) {
  return HEADER_H + item_top(index) - s_list_scroll;
}

static int first_visible_item(void) {
  int first = s_list_scroll / ITEM_H;
  return (first < 0) ? 0 : first;
}

/* Measure just the selected item's name, at the width the detail view uses.
   Called when the detail window opens, so it costs one text layout rather
   than one per item in the feed. */
static void measure_detail_name(void) {
  StashItem *it = feed_get_item(feed_active(), s_selected_item);
  if (!it) return;
  GSize sz = graphics_text_layout_get_content_size(
    it->name, s_font_normal,
    GRect(0, 0, TEXT_W, NAME_LINE_H * NAME_MAX_LINES),
    GTextOverflowModeWordWrap, GTextAlignmentLeft);
  int h = sz.h;
  if (h < NAME_LINE_H) h = NAME_LINE_H;
  if (h > NAME_LINE_H * NAME_MAX_LINES) h = NAME_LINE_H * NAME_MAX_LINES;
  it->name_h = (int16_t)h;
}

static int detail_content_height(void) {
  StashItem *it = feed_get_item(feed_active(), s_selected_item);
  int name_h = (it && it->name_h > 0) ? it->name_h : NAME_LINE_H;
  return DETAIL_IMG_MAX_H + 4 + LINE1_H + name_h + LINE3_H + 8;
}

static int16_t clamp_detail_scroll(int val) {
  int mx = detail_content_height() - SCREEN_H;
  if (mx < 0) mx = 0;
  if (val < 0) val = 0;
  if (val > mx) val = mx;
  return (int16_t)val;
}

// ---------------------------------------------------------------------------
// List drawing
// ---------------------------------------------------------------------------
static void draw_header(GContext *ctx, const char *label, int current, int total) {
  graphics_context_set_fill_color(ctx, GColorIslamicGreen);
  graphics_fill_rect(ctx, GRect(0, 0, SCREEN_W, HEADER_H), 0, GCornerNone);

  /* Logo sits flush right, vertically centred in the bar.
     The default compositing mode is GCompOpAssign, which copies raw pixel
     values and paints the PNG's alpha-zero pixels as black — hence the black
     box behind the logo. GCompOpSet is the mode that honours alpha on colour
     displays. Restore the default afterwards so nothing else is affected. */
  if (s_logo) {
    graphics_context_set_compositing_mode(ctx, GCompOpSet);
    graphics_draw_bitmap_in_rect(ctx, s_logo,
      GRect(SCREEN_W - LOGO_W, (HEADER_H - LOGO_H) / 2, LOGO_W, LOGO_H));
    graphics_context_set_compositing_mode(ctx, GCompOpAssign);
  }

  char hdr[40];
  if (total > 0) snprintf(hdr, sizeof(hdr), "%s  %d/%d", label, current, total);
  else           snprintf(hdr, sizeof(hdr), "%s", label);

  // Text gets the space left of the logo
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, hdr, s_font_header,
    GRect(TEXT_PAD_X, 2, SCREEN_W - LOGO_W - TEXT_PAD_X * 2, HEADER_H - 2),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void draw_list_item(GContext *ctx, int index, StashItem *it) {
  int y = item_screen_y(index);
  if (y + ITEM_H <= HEADER_H || y >= SCREEN_H) return;

  if (index == s_selected_item) {
    graphics_context_set_fill_color(ctx, GColorLightGray);
    graphics_fill_rect(ctx, GRect(0, y, SCREEN_W, ITEM_H), 0, GCornerNone);
  }

  int ty = y + ITEM_PAD;

  // Line 1: kit no. + brand
  char line1[MAX_STR_LEN * 2 + 4];
  snprintf(line1, sizeof(line1), "%s · %s", it->kit_no, it->brand);
  graphics_context_set_text_color(ctx, GColorDarkGreen);
  graphics_draw_text(ctx, line1, s_font_bold,
    GRect(TEXT_PAD_X, ty, TEXT_W, LINE1_H),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  ty += LINE1_H;

  // Line 2: name — single line, ellipsised (wrapping lives in the detail view)
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, it->name, s_font_normal,
    GRect(TEXT_PAD_X, ty, TEXT_W, LINE2_H),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  ty += LINE2_H;

  // Line 3: scale
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, it->scale, s_font_small,
    GRect(TEXT_PAD_X, ty, TEXT_W, LINE3_H),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  int div_y = y + ITEM_H - 1;
  int div_x = (SCREEN_W - 180) / 2;
  graphics_context_set_stroke_color(ctx, GColorLightGray);
  graphics_draw_line(ctx, GPoint(div_x, div_y), GPoint(div_x + 180, div_y));
}

static void list_update_proc(Layer *layer, GContext *ctx) {
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, GRect(0, 0, SCREEN_W, SCREEN_H), 0, GCornerNone);

  Feed *f = feed_get(feed_active());
  if (!f || f->item_count == 0) {
    graphics_context_set_text_color(ctx, GColorDarkGray);
    const char *empty_msg = s_waiting_for_feed ? "Loading list..."
                          : (s_status_msg[0] ? s_status_msg : "No items");
    // Wrapped and vertically centred: the settings prompt is a full sentence
    // and would otherwise be cut off after a couple of words.
    GRect box = GRect(TEXT_PAD_X * 2, 0, SCREEN_W - TEXT_PAD_X * 4,
                      NAME_LINE_H * 3);
    GSize msz = graphics_text_layout_get_content_size(
      empty_msg, s_font_normal, box,
      GTextOverflowModeWordWrap, GTextAlignmentCenter);
    box.origin.y = (SCREEN_H - msz.h) / 2;
    box.size.h   = msz.h;
    graphics_draw_text(ctx, empty_msg, s_font_normal, box,
      GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    draw_header(ctx, f ? f->label : "---", 0, 0);
    return;
  }

  // Walk forward from the first visible row until we fall off the bottom
  for (int i = first_visible_item(); i < f->item_count; i++) {
    if (item_screen_y(i) >= SCREEN_H) break;
    StashItem *it = feed_get_item(feed_active(), i);
    if (it) draw_list_item(ctx, i, it);
  }

  draw_header(ctx, f->label, s_selected_item + 1, f->item_count);
}

static void update_list(void) {
  if (s_list_canvas) layer_mark_dirty(s_list_canvas);
}

// ---------------------------------------------------------------------------
// Detail drawing
// ---------------------------------------------------------------------------
static void detail_update_proc(Layer *layer, GContext *ctx) {
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, GRect(0, 0, SCREEN_W, SCREEN_H), 0, GCornerNone);

  StashItem *it = feed_get_item(feed_active(), s_selected_item);
  if (!it) return;

  int y = -s_detail_scroll;

  if (it->image_ready && it->bmp) {
    // The bitmap already arrives at its display size; draw_bitmap_in_rect
    // clips rather than scales, so draw at natural size, centred.
    GSize sz = gbitmap_get_bounds(it->bmp).size;
    int ix = (SCREEN_W - sz.w) / 2;
    if (ix < 0) ix = 0;
    graphics_draw_bitmap_in_rect(ctx, it->bmp, GRect(ix, y, sz.w, sz.h));
    y += sz.h;
  } else {
    graphics_context_set_fill_color(ctx, GColorLightGray);
    graphics_fill_rect(ctx, GRect(0, y, SCREEN_W, DETAIL_IMG_MAX_H), 0, GCornerNone);
    graphics_context_set_text_color(ctx, GColorDarkGray);
    graphics_draw_text(ctx, "Loading image...", s_font_small,
      GRect(0, y + DETAIL_IMG_MAX_H / 2 - 8, SCREEN_W, 16),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    y += DETAIL_IMG_MAX_H;
  }

  y += 4;
  graphics_context_set_text_color(ctx, GColorBlack);

  char line1[MAX_STR_LEN * 2 + 4];
  snprintf(line1, sizeof(line1), "%s · %s", it->kit_no, it->brand);
  graphics_context_set_text_color(ctx, GColorDarkGreen);
  graphics_draw_text(ctx, line1, s_font_bold,
    GRect(TEXT_PAD_X, y, TEXT_W, LINE1_H),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  y += LINE1_H;

  // Name wraps here too, using the height measured for the list
  int name_h = (it->name_h > 0) ? it->name_h : NAME_LINE_H;
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, it->name, s_font_normal,
    GRect(TEXT_PAD_X, y, TEXT_W, name_h),
    GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
  y += name_h;

  // Worst case is scale(47) + year(11) + type(27) + two 4-byte " · " + NUL.
  // The middot is 2 bytes in UTF-8, so 64 was not enough and snprintf would
  // have silently truncated the type on a long entry.
  char line3[96];
  snprintf(line3, sizeof(line3), "%s · %s · %s", it->scale, it->year, it->type);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, line3, s_font_small,
    GRect(TEXT_PAD_X, y, TEXT_W, LINE3_H),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void update_detail(void) {
  if (s_detail_canvas) layer_mark_dirty(s_detail_canvas);
}

// ---------------------------------------------------------------------------
// Feed request — debounced so rapid cycling sends only one REQ_FEED
// ---------------------------------------------------------------------------
static void request_active_feed(void) {
  s_waiting_for_feed = true;
  APP_LOG(APP_LOG_LEVEL_INFO, "Requesting feed %d (%s)",
          (int)feed_active(), feed_get(feed_active())->label);
  comms_request_feed((int)feed_active());
}

// ---------------------------------------------------------------------------
// Feed cycling
// ---------------------------------------------------------------------------
static void cycle_feed(void) {
  if (!feed_next()) {
    for (int i = 0; i < MAX_FEEDS; i++) {
      if (feed_is_enabled((FeedId)i)) { feed_set_active((FeedId)i); break; }
    }
  }
  fling_stop();
  APP_LOG(APP_LOG_LEVEL_INFO, "Cycled to feed %d (%s)",
          (int)feed_active(), feed_get(feed_active())->label);
  s_list_scroll   = 0;
  s_selected_item = 0;
  request_active_feed();
  update_list();
}

// ---------------------------------------------------------------------------
// Touch recognizers
// ---------------------------------------------------------------------------
static bool recognizer_always_simultaneous(const Recognizer *r1, const Recognizer *r2) {
  (void)r1; (void)r2;
  return true;
}

/* Momentum scrolling. A pan alone is strictly 1:1 with the finger, so a flick
   only ever moves as far as the finger did - hence the sticky feel. On
   liftoff we take the pan velocity and keep coasting, decaying each frame,
   which is what makes a big swing travel several items. */
#define FLING_MIN_VELOCITY   150   // px/s below which a lift is not a fling
#define FLING_FRAME_MS        33   // ~30 fps
#define FLING_DECAY_NUM       88   // velocity *= 88/100 each frame
#define FLING_DECAY_DEN      100
#define FLING_STOP_VELOCITY   40   // px/s at which coasting ends

static AppTimer *s_fling_timer = NULL;
static int       s_fling_vel   = 0;   // px/s, positive scrolls down

static void sync_selected_to_scroll(void) {
  Feed *f = feed_get(feed_active());
  if (!f || f->item_count == 0) return;
  int idx = first_visible_item();
  if (idx < 0) idx = 0;
  if (idx >= f->item_count) idx = f->item_count - 1;
  s_selected_item = idx;
}

static void fling_stop(void) {
  if (s_fling_timer) {
    app_timer_cancel(s_fling_timer);
    s_fling_timer = NULL;
  }
  s_fling_vel = 0;
}

static void fling_timer_cb(void *context) {
  s_fling_timer = NULL;

  int before = s_list_scroll;
  s_list_scroll = clamp_list_scroll(s_list_scroll +
                                    (s_fling_vel * FLING_FRAME_MS) / 1000);
  sync_selected_to_scroll();
  update_list();

  // Stop when we run out of speed or hit an end and stopped moving
  s_fling_vel = (s_fling_vel * FLING_DECAY_NUM) / FLING_DECAY_DEN;
  if (s_list_scroll == before) { s_fling_vel = 0; return; }
  if (s_fling_vel > FLING_STOP_VELOCITY || s_fling_vel < -FLING_STOP_VELOCITY) {
    s_fling_timer = app_timer_register(FLING_FRAME_MS, fling_timer_cb, NULL);
  }
}

static void list_pan_handler(const Recognizer *recognizer, RecognizerEvent event) {
  static int16_t pan_base = 0;

  switch (event) {
    case RecognizerEvent_Started:
      fling_stop();               // a new touch halts any coast in progress
      pan_base = s_list_scroll;
      break;

    case RecognizerEvent_Updated: {
      GPoint d = pan_recognizer_get_delta_since_start(recognizer);
      s_list_scroll = clamp_list_scroll(pan_base - d.y);
      sync_selected_to_scroll();
      update_list();
      break;
    }

    case RecognizerEvent_Completed: {
      /* No snap-to-item here. Snapping to item_top() of the first visible row
         always rounds *down*, so at the very bottom it pulled the view back up
         and hid the last item's final line. Free positioning also lets the end
         of the list rest exactly at max scroll. */
      GPoint v = pan_recognizer_get_velocity(recognizer);
      int vy = -v.y;              // finger up (negative y) scrolls list down
      if (vy > FLING_MIN_VELOCITY || vy < -FLING_MIN_VELOCITY) {
        s_fling_vel   = vy;
        s_fling_timer = app_timer_register(FLING_FRAME_MS, fling_timer_cb, NULL);
      }
      sync_selected_to_scroll();
      update_list();
      break;
    }

    case RecognizerEvent_Cancelled:
      s_list_scroll = clamp_list_scroll(pan_base);
      sync_selected_to_scroll();
      update_list();
      break;
  }
}

/* Tap picks the row under the finger. Tapping the already-selected row opens
   it, so a tap never opens something the user hasn't seen highlighted. */
static void list_tap_handler(const Recognizer *recognizer, RecognizerEvent event) {
  if (event != RecognizerEvent_Completed) return;
  fling_stop();

  Feed *f = feed_get(feed_active());
  if (!f || f->item_count == 0) return;

  GPoint p = tap_recognizer_get_tap_point(recognizer);
  if (p.y < HEADER_H) return;   // header bar is not a row

  int idx = (s_list_scroll + p.y - HEADER_H) / ITEM_H;
  if (idx < 0 || idx >= f->item_count) return;

  if (idx == s_selected_item) {
    open_detail(idx);
  } else {
    s_selected_item = idx;
    update_list();
  }
}

static void list_swipe_handler(const Recognizer *recognizer, RecognizerEvent event) {
  if (event != RecognizerEvent_Completed) return;
  SwipeDirection dir = swipe_recognizer_get_direction(recognizer);
  if (dir == SwipeDirection_Left || dir == SwipeDirection_Right) cycle_feed();
}

static void detail_pan_handler(const Recognizer *recognizer, RecognizerEvent event) {
  static int16_t pan_base = 0;
  switch (event) {
    case RecognizerEvent_Started:
      pan_base = s_detail_scroll;
      break;
    case RecognizerEvent_Updated: {
      GPoint d = pan_recognizer_get_delta_since_start(recognizer);
      s_detail_scroll = clamp_detail_scroll(pan_base - d.y);
      update_detail();
      break;
    }
    case RecognizerEvent_Completed:
    case RecognizerEvent_Cancelled:
      s_detail_scroll = clamp_detail_scroll(s_detail_scroll);
      update_detail();
      break;
  }
}

// ---------------------------------------------------------------------------
// List click handlers
// ---------------------------------------------------------------------------
static void list_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  fling_stop();
  Feed *f = feed_get(feed_active());
  if (!f || f->item_count == 0) return;
  if (s_selected_item > 0) {
    s_selected_item--;
    s_list_scroll = clamp_list_scroll(item_top(s_selected_item));
    update_list();
  }
}

static void list_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  fling_stop();
  Feed *f = feed_get(feed_active());
  if (!f || f->item_count == 0) return;
  if (s_selected_item < f->item_count - 1) {
    s_selected_item++;
    s_list_scroll = clamp_list_scroll(item_top(s_selected_item));
    update_list();
  }
}

static void list_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  fling_stop();
  open_detail(s_selected_item);
}

static void list_select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  cycle_feed();
}

static void list_click_config_provider(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP,   150, list_up_click_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 150, list_down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, list_select_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, list_select_long_click_handler, NULL);
}

// ---------------------------------------------------------------------------
// Detail click handlers
// ---------------------------------------------------------------------------
static void detail_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_detail_scroll = clamp_detail_scroll(s_detail_scroll - DETAIL_SCROLL_STEP);
  update_detail();
}

static void detail_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_detail_scroll = clamp_detail_scroll(s_detail_scroll + DETAIL_SCROLL_STEP);
  update_detail();
}

static void detail_click_config_provider(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP,   150, detail_up_click_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 150, detail_down_click_handler);
}

// ---------------------------------------------------------------------------
// Comms callbacks
// ---------------------------------------------------------------------------
static void on_feed(const char *feed_name, int item_total,
                    const char *payload) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Feed payload: %s, %d items", feed_name, item_total);
  s_waiting_for_feed = false;
  s_status_msg[0] = '\0';
  feed_set_payload(feed_name, item_total, payload);
  s_list_scroll   = 0;
  s_selected_item = 0;
  update_list();
}

/* Payload is a raw Pebble 8-bit bitmap:
   [w_hi, w_lo, h_hi, h_lo, pixels...] one GColor8 byte per pixel. */
static void on_image(int item_index, const uint8_t *data, size_t len) {
  if (len < 5) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Image payload too short: %d", (int)len);
    return;
  }

  int w = (data[0] << 8) | data[1];
  int h = (data[2] << 8) | data[3];
  const uint8_t *px = data + 4;

  if (w <= 0 || h <= 0 || w > DETAIL_IMG_MAX_W || h > DETAIL_IMG_MAX_H) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Bad image dims %dx%d", w, h);
    return;
  }
  if (len - 4 < (size_t)w * h) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Image short: got %d need %d",
            (int)(len - 4), w * h);
    return;
  }

  /* Drop the previous bitmap FIRST. The receive buffer (~24 KB) is still held
     by comms while this runs, so keeping the old bitmap alive as well meant
     peaking at old + buffer + new ≈ 72 KB and failing the allocation. */
  feed_clear_images();

  APP_LOG(APP_LOG_LEVEL_INFO, "Bitmap %dx%d for item %d, heap free %d",
          w, h, item_index, (int)heap_bytes_free());

  GBitmap *bmp = gbitmap_create_blank(GSize(w, h), GBitmapFormat8Bit);
  if (!bmp) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "gbitmap_create_blank failed (need %d, free %d)",
            w * h, (int)heap_bytes_free());
    return;
  }

  uint8_t *dst    = gbitmap_get_data(bmp);
  uint16_t stride = gbitmap_get_bytes_per_row(bmp);
  for (int y = 0; y < h; y++) {
    memcpy(dst + (size_t)y * stride, px + (size_t)y * w, w);
  }

  feed_set_image(item_index, bmp);
  update_detail();
}

static void on_error(const char *message) {
  s_waiting_for_feed = false;
  strncpy(s_status_msg, message, sizeof(s_status_msg) - 1);
  s_status_msg[sizeof(s_status_msg) - 1] = '\0';
  APP_LOG(APP_LOG_LEVEL_ERROR, "Comms error: %s", message);
  update_list();
}

static void on_settings(const char *user_id, int feed_enabled[MAX_FEEDS]) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Settings: user_id=%s feeds=%d%d%d%d%d%d",
          user_id, feed_enabled[0], feed_enabled[1], feed_enabled[2],
          feed_enabled[3], feed_enabled[4], feed_enabled[5]);
  fling_stop();
  for (int i = 0; i < MAX_FEEDS; i++) {
    feed_set_enabled((FeedId)i, feed_enabled[i] != 0);
  }
  save_feed_settings();
  s_list_scroll   = 0;
  s_selected_item = 0;
  for (int i = 0; i < MAX_FEEDS; i++) {
    if (feed_is_enabled((FeedId)i)) { feed_set_active((FeedId)i); break; }
  }
  request_active_feed();
  update_list();
}

// ---------------------------------------------------------------------------
// Detail window
// ---------------------------------------------------------------------------
static void detail_window_load(Window *window) {
  s_detail_scroll = 0;
  measure_detail_name();
  Layer *root = window_get_root_layer(window);
  s_detail_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_detail_canvas, detail_update_proc);
  layer_add_child(root, s_detail_canvas);

  window_set_click_config_provider(window, detail_click_config_provider);

  // Opt out of the system recognizer set, otherwise the built-in gestures
  // consume touches before ours ever see them. Physical buttons are handled
  // by the click config provider and are unaffected.
  window_set_touch_bridge_disabled(window, true);
  s_detail_pan = pan_recognizer_create(detail_pan_handler, NULL, PanAxis_Vertical);
  window_attach_recognizer(window, s_detail_pan);

  comms_request_image(s_selected_item);
}

static void detail_window_unload(Window *window) {
  layer_destroy(s_detail_canvas);
  s_detail_canvas = NULL;

  // The image is only ever shown here, so hand its ~24 KB back to the heap
  // rather than carrying it around while browsing the list.
  feed_clear_images();

  /* window_deinit() only calls layer_remove_child_layers() on the root layer,
     never layer_deinit(), and layer_deinit() is the only thing that destroys
     attached recognizers. So a recognizer attached to the *window* outlives
     window_destroy() and leaks. Detach first: recognizer_destroy() no-ops
     while the recognizer is still owned by a list. */
  if (s_detail_pan) {
    window_detach_recognizer(window, s_detail_pan);
    recognizer_destroy(s_detail_pan);
    s_detail_pan = NULL;
  }

  /* open_detail() creates a fresh Window each time, so it has to be freed
     here or every visit to a detail page leaks one. The firmware explicitly
     allows a window to free itself from its own unload handler
     (see window_unload() in PebbleOS window.c). */
  window_destroy(window);
  if (s_detail_window == window) s_detail_window = NULL;
}

static void open_detail(int item_index) {
  s_selected_item = item_index;
  s_detail_window = window_create();
  window_set_window_handlers(s_detail_window, (WindowHandlers){
    .load   = detail_window_load,
    .unload = detail_window_unload,
  });
  window_stack_push(s_detail_window, true);
}

// ---------------------------------------------------------------------------
// List window
// ---------------------------------------------------------------------------
static void list_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_list_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_list_canvas, list_update_proc);
  layer_add_child(root, s_list_canvas);

  window_set_click_config_provider(window, list_click_config_provider);

  // Attaching is not enough on its own: the window has to opt out of the
  // global system recognizer set or those consume the touch stream first.
  window_set_touch_bridge_disabled(window, true);

  s_list_tap   = tap_recognizer_create(list_tap_handler, NULL);
  s_list_pan   = pan_recognizer_create(list_pan_handler, NULL, PanAxis_Vertical);
  s_list_swipe = swipe_recognizer_create(list_swipe_handler, NULL,
                   SwipeDirection_Left | SwipeDirection_Right);

  /* No fail_after chain here. A recognizer waiting on another drops *every*
     touch event until that other one reaches Failed (see prv_should_handle_touches
     in recognizer.c) — and a pan only fails at liftoff. Chaining the tap behind
     the pan therefore starved it of its own Touchdown, so it had nothing to
     complete and never fired. The tap guards itself anyway: it fails on any
     movement past its slop and on an over-long press. */
  recognizer_set_simultaneous_with(s_list_tap,   recognizer_always_simultaneous);
  recognizer_set_simultaneous_with(s_list_pan,   recognizer_always_simultaneous);
  recognizer_set_simultaneous_with(s_list_swipe, recognizer_always_simultaneous);

  window_attach_recognizer(window, s_list_pan);
  window_attach_recognizer(window, s_list_swipe);
  window_attach_recognizer(window, s_list_tap);
}

static void list_window_unload(Window *window) {
  fling_stop();   // never leave a timer running against a destroyed layer
  layer_destroy(s_list_canvas);
  s_list_canvas = NULL;

  // Same reason as the detail window: window-attached recognizers are not
  // torn down by window_destroy(). Only leaks once here, but keep it tidy.
  if (s_list_tap) {
    window_detach_recognizer(window, s_list_tap);
    recognizer_destroy(s_list_tap);
    s_list_tap = NULL;
  }
  if (s_list_pan) {
    window_detach_recognizer(window, s_list_pan);
    recognizer_destroy(s_list_pan);
    s_list_pan = NULL;
  }
  if (s_list_swipe) {
    window_detach_recognizer(window, s_list_swipe);
    recognizer_destroy(s_list_swipe);
    s_list_swipe = NULL;
  }
}

// ---------------------------------------------------------------------------
// App lifecycle
// ---------------------------------------------------------------------------
static void save_feed_settings(void) {
  for (int i = 0; i < MAX_FEEDS; i++) {
    persist_write_bool(PERSIST_KEY_FEED_BASE + i, feed_is_enabled((FeedId)i));
  }
}

static void load_feed_settings(void) {
  if (persist_exists(PERSIST_KEY_FEED_BASE)) {
    for (int i = 0; i < MAX_FEEDS; i++) {
      feed_set_enabled((FeedId)i,
                       persist_read_bool(PERSIST_KEY_FEED_BASE + i));
    }
  } else {
    feed_set_enabled(FEED_STASH, true);   // first run
  }

  // Never end up with nothing selectable
  bool any = false;
  for (int i = 0; i < MAX_FEEDS; i++) {
    if (feed_is_enabled((FeedId)i)) { any = true; break; }
  }
  if (!any) feed_set_enabled(FEED_STASH, true);

  for (int i = 0; i < MAX_FEEDS; i++) {
    if (feed_is_enabled((FeedId)i)) { feed_set_active((FeedId)i); break; }
  }
}

/* Custom fonts are resources, not system fonts: fonts_get_system_font() only
   accepts the built-in FONT_KEY_* constants. Load once here rather than in
   window_load, because measure_detail_name() needs s_font_normal and the
   detail window can outlive a list reload. Each returns NULL if the resource
   is missing, so fall back to a system font rather than rendering nothing. */
static GFont load_font(uint32_t resource_id, const char *fallback_key) {
  GFont f = fonts_load_custom_font(resource_get_handle(resource_id));
  if (!f) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Font %d missing, using fallback",
            (int)resource_id);
    return fonts_get_system_font(fallback_key);
  }
  return f;
}

/* Three distinct faces. fonts_load_custom_font() mallocs a fresh FontInfo on
   every call with no caching, so each face is loaded exactly once here and the
   role variables alias them — otherwise the regular 18 would be paid for twice. */
static GFont s_font_reg_18;    // segoeui.ttf   (regular)
static GFont s_font_bold_18;   // segoeuib.ttf  (bold)
static GFont s_font_bold_20;   // segoeuib.ttf  (bold)

static void load_fonts(void) {
  s_font_reg_18  = load_font(RESOURCE_ID_FONT_SEGOEUI_18,  FONT_KEY_GOTHIC_18);
  s_font_bold_18 = load_font(RESOURCE_ID_FONT_SEGOEUIB_18, FONT_KEY_GOTHIC_18_BOLD);
  s_font_bold_20 = load_font(RESOURCE_ID_FONT_SEGOEUIB_20, FONT_KEY_GOTHIC_24_BOLD);

  s_font_header = s_font_reg_18;    // header bar        (regular)
  s_font_bold   = s_font_bold_18;   // kit no. + brand   (bold)
  s_font_normal = s_font_bold_20;   // name, wraps in detail view
  s_font_small  = s_font_bold_18;   // scale / meta

  APP_LOG(APP_LOG_LEVEL_INFO, "Fonts loaded, heap free %d",
          (int)heap_bytes_free());
}

static void unload_fonts(void) {
  // Unload the three real handles, not the four aliases — freeing the same
  // pointer more than once would be a double free.
  fonts_unload_custom_font(s_font_reg_18);
  fonts_unload_custom_font(s_font_bold_18);
  fonts_unload_custom_font(s_font_bold_20);
  s_font_reg_18 = s_font_bold_18 = s_font_bold_20 = NULL;
}

static void init(void) {
  feed_init();
  load_fonts();

  /* Third-party apps are opted OUT of touch navigation by default — the
     firmware only marks system apps as participating, so without this call
     no touch events ever reach our recognizers, however they are attached.
     See PebbleOS app_state.c: touch_nav_participating is set from
     app_install_id_from_system(). */
  app_touch_navigation_enable(true);

  s_logo = gbitmap_create_with_resource(RESOURCE_ID_SCM_LOGO);

  load_feed_settings();

  comms_init(on_feed, on_image, on_error, on_settings);

  s_list_window = window_create();
  window_set_window_handlers(s_list_window, (WindowHandlers){
    .load   = list_window_load,
    .unload = list_window_unload,
  });
  window_stack_push(s_list_window, true);

  request_active_feed();
}

static void deinit(void) {
  unload_fonts();
  if (s_logo) gbitmap_destroy(s_logo);
  comms_deinit();
  feed_deinit();
  window_destroy(s_list_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
