#include <pebble.h>
#include "comms.h"
#include "feed.h"

// ---------------------------------------------------------------------------
// Layout constants  (Emery: 200 × 228 px)
// ---------------------------------------------------------------------------
#define SCREEN_W       200
#define SCREEN_H       228
#define HEADER_H        24    // feed label bar at top
#define THUMB_W        140
#define THUMB_H        140
#define THUMB_X        ((SCREEN_W - THUMB_W) / 2)   // = 30 — centred
#define THUMB_Y         (HEADER_H + 4)
#define TEXT_PAD_X       6
#define TEXT_PAD_Y       4
#define TEXT_AREA_Y     (THUMB_Y + THUMB_H + TEXT_PAD_Y)
#define LINE1_H         18    // kit_no · Brand
#define LINE2_H         18    // Name
#define LINE3_H         16    // Scale
#define ITEM_H          (THUMB_H + TEXT_PAD_Y + LINE1_H + LINE2_H + LINE3_H + 12)
//                            140     +4           +18       +18      +16    +12  = 208

// How many pixels from the bottom of the current item the next request fires
#define PREFETCH_PX     60

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------
static Window    *s_window;
static Layer     *s_canvas;

static Recognizer *s_swipe_h;   // horizontal — switches feeds
static Recognizer *s_pan_v;     // vertical   — scrolls items

// Scroll state
static int16_t s_scroll_base   = 0;   // committed y offset (px, positive = scroll down)
static int16_t s_scroll_offset = 0;   // live offset while panning

// Fonts
static GFont s_font_label;   // header
static GFont s_font_line1;   // kit_no · Brand (bold)
static GFont s_font_line2;   // Name
static GFont s_font_line3;   // Scale (small)

// Image-request tracking: don't re-request what's already on the way
static bool s_img_requested[MAX_ITEMS];

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void request_active_feed(void);
static void update_canvas(void);
static void activate_first_enabled_feed(void);

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// Returns y-coordinate (in content space) of the top of item[index].
static int item_y(int index) {
  return HEADER_H + index * ITEM_H;
}

// First item visible at current scroll offset
static int first_visible_item(void) {
  int first = (s_scroll_offset - HEADER_H) / ITEM_H;
  return (first < 0) ? 0 : first;
}

static void draw_header(GContext *ctx, const char *label) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(0, 0, SCREEN_W, HEADER_H), 0, GCornerNone);

  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, label,
                     s_font_label,
                     GRect(TEXT_PAD_X, 2, SCREEN_W - TEXT_PAD_X * 2, HEADER_H),
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
}

static void draw_item(GContext *ctx, int index, StashItem *it) {
  // y in screen space
  int y = item_y(index) - s_scroll_offset;

  // --- Thumbnail ---
  if (it->image_ready && it->bmp) {
    graphics_draw_bitmap_in_rect(ctx, it->bmp,
                                 GRect(THUMB_X, y, THUMB_W, THUMB_H));
  } else {
    // Placeholder: grey rectangle with centred "…"
    graphics_context_set_fill_color(ctx, GColorLightGray);
    graphics_fill_rect(ctx, GRect(THUMB_X, y, THUMB_W, THUMB_H), 0, GCornerNone);
    graphics_context_set_text_color(ctx, GColorDarkGray);
    graphics_draw_text(ctx, "Loading...",
                       s_font_line3,
                       GRect(THUMB_X, y + THUMB_H / 2 - 8, THUMB_W, 16),
                       GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentCenter, NULL);
  }

  int ty = y + THUMB_H + TEXT_PAD_Y;

  // --- Line 1: Kit No. · Brand (bold) ---
  char line1[MAX_STR_LEN * 2 + 4];
  snprintf(line1, sizeof(line1), "%s · %s", it->kit_no, it->brand);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, line1,
                     s_font_line1,
                     GRect(TEXT_PAD_X, ty, SCREEN_W - TEXT_PAD_X * 2, LINE1_H),
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
  ty += LINE1_H;

  // --- Line 2: Name ---
  graphics_draw_text(ctx, it->name,
                     s_font_line2,
                     GRect(TEXT_PAD_X, ty, SCREEN_W - TEXT_PAD_X * 2, LINE2_H),
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
  ty += LINE2_H;

  // --- Line 3: Scale ---
  graphics_context_set_text_color(ctx, GColorDarkGray);
  graphics_draw_text(ctx, it->scale,
                     s_font_line3,
                     GRect(TEXT_PAD_X, ty, SCREEN_W - TEXT_PAD_X * 2, LINE3_H),
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);

  // Thin divider below item
  int div_y = y + ITEM_H - 2;
  if (div_y > HEADER_H && div_y < SCREEN_H) {
    graphics_context_set_stroke_color(ctx, GColorLightGray);
    graphics_draw_line(ctx, GPoint(0, div_y), GPoint(SCREEN_W, div_y));
  }
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
  // White background
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, GRect(0, 0, SCREEN_W, SCREEN_H), 0, GCornerNone);

  Feed *f = feed_get(feed_active());
  if (!f || f->item_count == 0) {
    // Empty state
    graphics_context_set_text_color(ctx, GColorDarkGray);
    graphics_draw_text(ctx, f && f->loading ? "Loading..." : "No items",
                       s_font_line2,
                       GRect(0, SCREEN_H / 2 - 10, SCREEN_W, 20),
                       GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentCenter, NULL);
    draw_header(ctx, f ? f->label : "---");
    return;
  }

  // Draw visible items (plus one above/below for smooth pan feel)
  int first = first_visible_item();
  int last  = first + (SCREEN_H / ITEM_H) + 2;
  if (last >= f->item_count) last = f->item_count - 1;

  for (int i = first; i <= last; i++) {
    StashItem *it = feed_get_item(feed_active(), i);
    if (!it) continue;

    // Prefetch image if approaching and not yet requested
    int screen_y = item_y(i) - s_scroll_offset;
    if (!it->image_ready && !s_img_requested[i] &&
        screen_y < SCREEN_H + PREFETCH_PX) {
      s_img_requested[i] = true;
      comms_request_image(i);
    }

    // Only actually draw if on screen
    if (screen_y + ITEM_H > HEADER_H && screen_y < SCREEN_H) {
      draw_item(ctx, i, it);
    }
  }

  // Header drawn last so it always sits on top
  draw_header(ctx, f->label);
}

// ---------------------------------------------------------------------------
// Swipe handler — switches feeds horizontally
// ---------------------------------------------------------------------------
static void swipe_handler(const Recognizer *recognizer, RecognizerEvent event) {
  if (event != RecognizerEvent_Completed) return;

  SwipeDirection dir = swipe_recognizer_get_direction(recognizer);
  bool changed = false;

  if (dir == SwipeDirection_Left)  changed = feed_next();
  if (dir == SwipeDirection_Right) changed = feed_prev();

  if (changed) {
    // Reset scroll and image-request tracking
    s_scroll_base   = 0;
    s_scroll_offset = 0;
    memset(s_img_requested, 0, sizeof(s_img_requested));
    request_active_feed();
    update_canvas();
  }
}

// ---------------------------------------------------------------------------
// Pan handler — scrolls vertically within the active feed
// ---------------------------------------------------------------------------
static void pan_handler(const Recognizer *recognizer, RecognizerEvent event) {
  Feed *f = feed_get(feed_active());
  int content_h = f ? f->item_count * ITEM_H : 0;
  int max_scroll = (content_h > SCREEN_H) ? content_h - SCREEN_H : 0;

  switch (event) {
    case RecognizerEvent_Started:
      // delta_since_start is (0,0) here — nothing to do
      break;

    case RecognizerEvent_Updated: {
      GPoint d = pan_recognizer_get_delta_since_start(recognizer);
      int16_t candidate = s_scroll_base - d.y;   // drag up → positive offset
      if (candidate < 0)          candidate = 0;
      if (candidate > max_scroll) candidate = max_scroll;
      s_scroll_offset = candidate;
      layer_mark_dirty(s_canvas);
      break;
    }

    case RecognizerEvent_Completed:
      s_scroll_base = s_scroll_offset;
      break;

    case RecognizerEvent_Cancelled:
      s_scroll_offset = s_scroll_base;
      layer_mark_dirty(s_canvas);
      break;
  }
}

// ---------------------------------------------------------------------------
// Comms callbacks
// ---------------------------------------------------------------------------

static void on_meta(const char *feed_name,
                    int item_index, int item_total,
                    const char *kit_no, const char *brand,
                    const char *name, const char *scale) {
  feed_set_meta(feed_name, item_index, item_total, kit_no, brand, name, scale);
  update_canvas();
}

static void on_image(int item_index, const uint8_t *png_data, size_t png_len) {
  // Decode PNG → GBitmap natively
  GBitmap *bmp = gbitmap_create_from_png_data(png_data, png_len);
  if (bmp) {
    feed_set_image(item_index, bmp);
    update_canvas();
  } else {
    APP_LOG(APP_LOG_LEVEL_WARNING, "PNG decode failed for item %d", item_index);
    s_img_requested[item_index] = false;   // allow retry
  }
}

static void on_error(const char *message) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Comms error: %s", message);
}

static void on_settings(const char *user_id, int feed_enabled[5]) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Settings received. user_id=%s feeds=%d%d%d%d%d",
          user_id,
          feed_enabled[0], feed_enabled[1], feed_enabled[2],
          feed_enabled[3], feed_enabled[4]);

  // Update enabled feeds
  for (int i = 0; i < MAX_FEEDS; i++) {
    feed_set_enabled((FeedId)i, feed_enabled[i] != 0);
  }

  // Reset scroll and image state, switch to first enabled feed, reload
  s_scroll_base   = 0;
  s_scroll_offset = 0;
  memset(s_img_requested, 0, sizeof(s_img_requested));
  activate_first_enabled_feed();
  update_canvas();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void update_canvas(void) {
  layer_mark_dirty(s_canvas);
}

static void request_active_feed(void) {
  comms_request_feed((int)feed_active());
}

// Finds the first enabled feed and makes it active, then requests it.
static void activate_first_enabled_feed(void) {
  for (int i = 0; i < MAX_FEEDS; i++) {
    if (feed_is_enabled((FeedId)i)) {
      feed_set_active((FeedId)i);
      request_active_feed();
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// Window lifecycle
// ---------------------------------------------------------------------------

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_canvas = layer_create(bounds);
  layer_set_update_proc(s_canvas, canvas_update_proc);
  layer_add_child(root, s_canvas);

  // Load fonts
  s_font_label = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  s_font_line1 = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  s_font_line2 = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  s_font_line3 = fonts_get_system_font(FONT_KEY_GOTHIC_14);

  // Opt this window out of the system touch bridge so our recognizers
  // receive touches instead of the system back-swipe / scroll handlers.
  window_set_touch_bridge_disabled(window, true);

  // Horizontal swipe — switches feeds (left = next feed, right = prev feed)
  s_swipe_h = swipe_recognizer_create(swipe_handler, NULL,
                                      SwipeDirection_Left | SwipeDirection_Right);
  window_attach_recognizer(window, s_swipe_h);

  // Vertical pan — scrolls items within the current feed
  s_pan_v = pan_recognizer_create(pan_handler, NULL, PanAxis_Vertical);
  window_attach_recognizer(window, s_pan_v);

  // Let the pan and swipe recognizers run simultaneously (finger moving
  // diagonally shouldn't lock out one of them immediately).
  recognizer_set_simultaneous_with(s_pan_v, NULL);
  recognizer_set_simultaneous_with(s_swipe_h, NULL);
}

static void window_unload(Window *window) {
  // Recognizers are owned by the window and destroyed with it.
  layer_destroy(s_canvas);
}

// ---------------------------------------------------------------------------
// App lifecycle
// ---------------------------------------------------------------------------

static void init(void) {
  feed_init();
  memset(s_img_requested, 0, sizeof(s_img_requested));

  // Read Clay settings from persistent storage
  // Key layout: 0 = user_id (unused on watch), 1-5 = feed enabled flags
  // The phone side reads Clay and sends feed_enabled flags via the first
  // KEY_REQ_FEED response; here we default to Stash enabled.
  // Full Clay-driven enable/disable is handled in app.js.
  feed_set_enabled(FEED_STASH,     true);
  feed_set_enabled(FEED_WISHLIST,  false);
  feed_set_enabled(FEED_STARTED,   false);
  feed_set_enabled(FEED_COMPLETED, false);
  feed_set_enabled(FEED_FORSALE,   false);

  comms_init(on_meta, on_image, on_error, on_settings);

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load   = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  // Kick off the first feed load once JS is ready.
  // JS sends a ready ping; until then we show "Loading..."
  activate_first_enabled_feed();
}

static void deinit(void) {
  comms_deinit();
  feed_deinit();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
