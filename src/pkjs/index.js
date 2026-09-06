// ---------------------------------------------------------------------------
// ScaleMates Stash Viewer — PebbleKit JS  (@rebble/clay edition)
// ---------------------------------------------------------------------------

var Clay = require('@rebble/clay');
var clayConfig = require('./clay-config');
var messageKeys = require('message_keys');

var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

Pebble.addEventListener('showConfiguration', function() {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e || !e.response) return;
  var settings = clay.getSettings(e.response, false);

  var uid = settings.user_id ? settings.user_id.value : null;
  if (uid !== null && uid !== undefined) {
    s_userId = String(uid).trim();
    localStorage.setItem('user_id', s_userId);
  }
  var toggles = ['feed_stash','feed_wishlist','feed_started',
                 'feed_completed','feed_forsale','feed_onorder'];
  toggles.forEach(function(key, i) {
    if (settings[key] !== undefined) {
      s_feeds_enabled[i] = !!settings[key].value;
      localStorage.setItem(key, s_feeds_enabled[i]);
    }
  });
  console.log('webviewclosed. user_id=' + s_userId + ' feeds=' + s_feeds_enabled.join(','));

  var dict = clay.getSettings(e.response);
  Pebble.sendAppMessage(dict,
    function() { console.log('Sent config data to Pebble'); },
    function(e) { console.log('Failed to send config: ' + JSON.stringify(e)); }
  );

  s_itemCache = {};
  s_inFlight  = {};
  handleFeedRequest(s_lastFeedIndex);
});

// ---------------------------------------------------------------------------
// Message keys
// ---------------------------------------------------------------------------
var KEY_FEED_NAME   = messageKeys.FEED_NAME;
var KEY_ITEM_INDEX  = messageKeys.ITEM_INDEX;
var KEY_ITEM_TOTAL  = messageKeys.ITEM_TOTAL;
var KEY_ITEMS       = messageKeys.ITEMS;
var KEY_KIT_NO      = messageKeys.KIT_NO;
var KEY_BRAND       = messageKeys.BRAND;
var KEY_NAME        = messageKeys.NAME;
var KEY_SCALE       = messageKeys.SCALE;
var KEY_IMG_ITEM    = messageKeys.IMG_ITEM;
var KEY_IMG_CHUNK_I = messageKeys.IMG_CHUNK_I;
var KEY_IMG_CHUNK_N = messageKeys.IMG_CHUNK_N;
var KEY_IMG_DATA    = messageKeys.IMG_DATA;
var KEY_REQ_FEED    = messageKeys.REQ_FEED;
var KEY_REQ_IMAGE   = messageKeys.REQ_IMAGE;
var KEY_ERROR       = messageKeys.ERROR;
var KEY_YEAR        = messageKeys.YEAR;
var KEY_TYPE        = messageKeys.TYPE;
var KEY_USER_ID        = messageKeys.user_id;
var KEY_FEED_STASH     = messageKeys.feed_stash;
var KEY_FEED_WISHLIST  = messageKeys.feed_wishlist;
var KEY_FEED_STARTED   = messageKeys.feed_started;
var KEY_FEED_COMPLETED = messageKeys.feed_completed;
var KEY_FEED_FORSALE   = messageKeys.feed_forsale;
var KEY_FEED_ONORDER   = messageKeys.feed_onorder;

var IMG_CHUNK_BYTES = 2048;
var DETAIL_MAX_W    = 200;   // must match DETAIL_IMG_MAX_W in main.c
var DETAIL_MAX_H    = 120;   // must match DETAIL_IMG_MAX_H in main.c

var FEEDS = [
  { id: 0, label: 'Stash',     param: 'stash'     },
  { id: 1, label: 'Wishlist',  param: 'wishlist'  },
  { id: 2, label: 'Started',   param: 'started'   },
  { id: 3, label: 'Completed', param: 'completed' },
  { id: 4, label: 'For Sale',  param: 'forsale'   },
  { id: 5, label: 'On Order',  param: 'onorder'   },
];

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
var s_userId        = '';
var s_feeds_enabled = [true, false, false, false, false, false];

function applySettings(msg) {
  // msg here is a name-keyed inbound payload
  if (typeof msg.user_id !== 'undefined') {
    s_userId = String(msg.user_id).trim();
    localStorage.setItem('user_id', s_userId);
  }
  var names = ['feed_stash','feed_wishlist','feed_started',
               'feed_completed','feed_forsale','feed_onorder'];
  for (var i = 0; i < names.length; i++) {
    if (typeof msg[names[i]] !== 'undefined') {
      s_feeds_enabled[i] = !!msg[names[i]];
      localStorage.setItem(names[i], s_feeds_enabled[i]);
    }
  }
  console.log('Settings applied. user_id=' + s_userId +
              ' feeds=' + s_feeds_enabled.join(','));
}

// ---------------------------------------------------------------------------
// HTML parser
// ---------------------------------------------------------------------------
function parseItems(html) {
  var items = [];
  var m;

  // Type comes from the nearest preceding <h5> heading
  var h5Re = /<h5>([^<]+)<\/h5>/gi;
  var h5Positions = [];
  while ((m = h5Re.exec(html)) !== null) {
    h5Positions.push({ pos: m.index, type: m[1].trim() });
  }
  function typeAtPos(pos) {
    var type = '';
    for (var i = 0; i < h5Positions.length; i++) {
      if (h5Positions[i].pos < pos) type = h5Positions[i].type;
      else break;
    }
    return type;
  }

  /* Year cells. These sit inside the h4/h5 block, which puts them BEFORE the
     product image in document order - so scanning forward from the image
     never finds the right one. Collect every year cell with its position
     instead, then pair each image with the nearest one that falls inside its
     own item. Direction-agnostic, so it survives a markup reshuffle. */
  var yearRe = /<div[^>]*class="[^"]*\bbl\b[^"]*"[^>]*>\s*(\d{3}[\dxX])\s*<\/div>/gi;
  var years = [];
  while ((m = yearRe.exec(html)) !== null) {
    years.push({ pos: m.index, year: m[1] });
  }

  // Collect the images first so each one knows its neighbours
  var imgRe = /<img[^>]+src="(\/products\/img\/[^"]+?-t240\.[a-z]+)"[^>]+title="([^"]+)"/gi;
  var imgs = [];
  while ((m = imgRe.exec(html)) !== null) {
    imgs.push({ pos: m.index, url: m[1], title: m[2] });
  }

  console.log('parse: ' + imgs.length + ' images, ' + years.length + ' year cells');

  /* Assign each year cell to its single nearest image, rather than letting
     each image go looking for a year. One-to-one means an item with no year
     of its own stays blank instead of borrowing a neighbour's. */
  var yearFor = new Array(imgs.length);
  var yearDist = new Array(imgs.length);
  for (var k = 0; k < years.length; k++) {
    var nearest = -1, nearestDist = Infinity;
    for (var j = 0; j < imgs.length; j++) {
      var dist = Math.abs(years[k].pos - imgs[j].pos);
      if (dist < nearestDist) { nearestDist = dist; nearest = j; }
    }
    if (nearest >= 0 &&
        (yearFor[nearest] === undefined || nearestDist < yearDist[nearest])) {
      yearFor[nearest]  = years[k].year;
      yearDist[nearest] = nearestDist;
    }
  }

  for (var i = 0; i < imgs.length; i++) {
    var title = imgs[i].title;

    // title is "SCALE NAME (BRAND KITNO)"
    var tm    = /^(1:\d+)\s+(.+?)\s+\(([^)]+)\)\s*$/.exec(title);
    var scale = tm ? tm[1] : '';
    var name  = tm ? tm[2] : title;
    var rest  = tm ? tm[3] : '';

    var lastSpace = rest.lastIndexOf(' ');
    var brand  = lastSpace > 0 ? rest.substring(0, lastSpace)  : rest;
    var kit_no = lastSpace > 0 ? rest.substring(lastSpace + 1) : '';

    var type = typeAtPos(imgs[i].pos);
    if      (/kit/i.test(type))      type = 'Kit';
    else if (/accessor/i.test(type)) type = 'Accessory';
    else if (/decal/i.test(type))    type = 'Decals';
    else if (/book/i.test(type))     type = 'Book';
    else if (/tool/i.test(type))     type = 'Tool';

    items.push({
      scale:    scale.substring(0, 47),
      name:     name.substring(0, 79),
      brand:    brand.substring(0, 47),
      kit_no:   kit_no.substring(0, 47),
      year:     (yearFor[i] || "").substring(0, 8),
      type:     type.substring(0, 24),
      thumbUrl: 'https://www.scalemates.com' + imgs[i].url,
    });
  }
  return items;
}

// ---------------------------------------------------------------------------
// AppMessage packing — FuelWatch pattern: ONE message, all items packed
// Format per item: kit_no|brand|name|scale|year|type   (newline separated)
// ---------------------------------------------------------------------------
function packItem(it) {
  function clean(v, len) {
    return String(v || '').substring(0, len).replace(/[|\n]/g, ' ');
  }
  return [
    clean(it.kit_no, 24),
    clean(it.brand,  24),
    clean(it.name,   40),
    clean(it.scale,  10),
    clean(it.year,    8),
    clean(it.type,   16)
  ].join('|');
}

function sendFeedToWatch(feed, items) {
  var packed = items.map(packItem).join('\n');
  var msg = {};
  msg[KEY_FEED_NAME]  = feed.label;
  msg[KEY_ITEM_TOTAL] = items.length;
  msg[KEY_ITEMS]      = packed;
  console.log('Sending ' + feed.label + ': ' + items.length + ' items, ' +
              packed.length + ' bytes');
  Pebble.sendAppMessage(msg,
    function()  { console.log('Feed sent OK'); },
    function(e) { console.log('Feed send failed: ' + JSON.stringify(e)); }
  );
}

// ---------------------------------------------------------------------------
// Feed cache
// ---------------------------------------------------------------------------
var s_itemCache     = {};
// Feeds currently being fetched, so a duplicate request while one is already
// in flight is ignored instead of starting a second identical fetch.
var s_inFlight      = {};
var s_lastFeedIndex = 0;

function cacheKey(feedParam) { return feedParam + ':' + s_userId; }

function handleFeedRequest(feedIndex) {
  console.log('handleFeedRequest: ' + feedIndex + ' user=' + s_userId +
              ' enabled=' + s_feeds_enabled[feedIndex]);
  if (!s_userId) {
    var errMsg = {}; errMsg[KEY_ERROR] = 'Go to settings first to set your ID';
    Pebble.sendAppMessage(errMsg); return;
  }
  var feed = FEEDS[feedIndex];
  if (!feed || !s_feeds_enabled[feedIndex]) return;

  s_lastFeedIndex = feedIndex;

  var key = cacheKey(feed.param);
  if (s_itemCache[key]) {
    sendFeedToWatch(feed, s_itemCache[key]);
    return;
  }

  if (s_inFlight[key]) {
    console.log(feed.label + ' already fetching, ignoring duplicate request');
    return;
  }

  var url = 'https://www.scalemates.com/profiles/mate.php?id=' +
            s_userId + '&p=' + feed.param;
  console.log('Fetching: ' + url);
  s_inFlight[key] = true;

  fetch(url)
    .then(function(r)    { return r.text(); })
    .then(function(html) {
      s_inFlight[key] = false;
      var items = parseItems(html);
      console.log(feed.label + ': ' + items.length + ' items');
      if (items.length) console.log('first item: ' + JSON.stringify(items[0]));
      s_itemCache[key] = items;
      sendFeedToWatch(feed, items);
    })
    .catch(function(e) {
      s_inFlight[key] = false;
      console.log('Fetch error: ' + e);
      var errMsg = {}; errMsg[KEY_ERROR] = 'Fetch failed';
      Pebble.sendAppMessage(errMsg);
    });
}


// ---------------------------------------------------------------------------
// Image pipeline — same official pattern: success callback drives next chunk
// ---------------------------------------------------------------------------
function fetchBinary(url) {
  return new Promise(function(resolve, reject) {
    var xhr = new XMLHttpRequest();
    xhr.open('GET', url, true);
    xhr.responseType = 'arraybuffer';
    xhr.onload  = function() {
      xhr.status === 200 ? resolve(xhr.response) : reject(new Error('HTTP ' + xhr.status));
    };
    xhr.onerror = function() { reject(new Error('XHR error')); };
    xhr.send();
  });
}

/* Produce a raw Pebble 8-bit bitmap instead of a PNG.
   Pebble's PNG decoder expands a truecolour PNG to its source format first
   (200x120 RGBA = 96 KB in one contiguous block), which no amount of
   compression avoids. Sending pixels in Pebble's own GColor8 byte format
   means the watch just memcpy's them into a blank GBitmap — no decoder,
   no expansion, fully predictable memory.

   GColor8 byte layout: aarrggbb, 2 bits per channel. Opaque = 0xC0.
   Payload: [w_hi, w_lo, h_hi, h_lo, pixels...] */
function resizeToPebbleBitmap(arrayBuffer) {
  return new Promise(function(resolve, reject) {
    var blob = new Blob([arrayBuffer]);
    var url  = URL.createObjectURL(blob);
    var img  = new Image();
    img.onload = function() {
      URL.revokeObjectURL(url);

      var sw = img.naturalWidth  || img.width  || 1;
      var sh = img.naturalHeight || img.height || 1;

      var w = DETAIL_MAX_W;
      var h = Math.round(w * sh / sw);
      if (h > DETAIL_MAX_H) {
        h = DETAIL_MAX_H;
        w = Math.round(h * sw / sh);
      }

      var canvas = document.createElement('canvas');
      canvas.width  = w;
      canvas.height = h;
      var ctx = canvas.getContext('2d');
      ctx.drawImage(img, 0, 0, w, h);

      var d = ctx.getImageData(0, 0, w, h).data;

      /* Floyd-Steinberg dithering. Snapping each channel to 0/85/170/255
         on its own leaves visible banding across the gradients in box art;
         diffusing the rounding error into neighbouring pixels trades that
         banding for fine noise, which reads as far more detail at this size.
         Costs nothing in payload - the result is still one byte per pixel. */
      var err = new Float32Array(w * h * 3);
      for (var i = 0, e = 0; i < d.length; i += 4, e += 3) {
        err[e]     = d[i];
        err[e + 1] = d[i + 1];
        err[e + 2] = d[i + 2];
      }

      function diffuse(idx, amount) {
        var v = err[idx] + amount;
        err[idx] = v < 0 ? 0 : (v > 255 ? 255 : v);
      }

      var bytes = new Uint8Array(4 + w * h);
      bytes[0] = (w >> 8) & 0xFF;
      bytes[1] =  w       & 0xFF;
      bytes[2] = (h >> 8) & 0xFF;
      bytes[3] =  h       & 0xFF;

      var o = 4;
      for (var y = 0; y < h; y++) {
        for (var x = 0; x < w; x++) {
          var base = (y * w + x) * 3;
          var lvl  = [0, 0, 0];

          for (var c = 0; c < 3; c++) {
            var oldv = err[base + c];
            var q    = (oldv / 85 + 0.5) | 0;      // nearest of 0,85,170,255
            if (q < 0) q = 0; else if (q > 3) q = 3;
            lvl[c] = q;
            var qe = oldv - q * 85;                // rounding error

            // Distribute: right 7/16, below-left 3/16, below 5/16, below-right 1/16
            if (x + 1 < w)              diffuse(base + 3 + c,             qe * 7 / 16);
            if (y + 1 < h) {
              var below = ((y + 1) * w + x) * 3 + c;
              if (x > 0)                diffuse(below - 3,                qe * 3 / 16);
                                        diffuse(below,                    qe * 5 / 16);
              if (x + 1 < w)            diffuse(below + 3,                qe * 1 / 16);
            }
          }

          bytes[o++] = 0xC0 | (lvl[0] << 4) | (lvl[1] << 2) | lvl[2];
        }
      }

      console.log('Bitmap ' + w + 'x' + h + ' -> ' + bytes.length + ' bytes raw');
      resolve(bytes);
    };
    img.onerror = function() {
      URL.revokeObjectURL(url);
      reject(new Error('img load failed'));
    };
    img.src = url;
  });
}

function sendChunk(itemIndex, pngBytes, index, totalChunks) {
  var start = index * IMG_CHUNK_BYTES;
  var chunk = Array.from(pngBytes.subarray(start,
                Math.min(start + IMG_CHUNK_BYTES, pngBytes.length)));
  var msg = {};
  msg[KEY_IMG_ITEM]    = itemIndex;
  msg[KEY_IMG_CHUNK_I] = index;
  msg[KEY_IMG_CHUNK_N] = totalChunks;
  msg[KEY_IMG_DATA]    = chunk;

  Pebble.sendAppMessage(msg,
    function() {
      if (index + 1 < totalChunks) {
        sendChunk(itemIndex, pngBytes, index + 1, totalChunks);
      } else {
        console.log('Image ' + itemIndex + ' complete (' + pngBytes.length + 'B)');
      }
    },
    function(e) {
      console.log('Chunk ' + index + ' failed, retrying');
      setTimeout(function() { sendChunk(itemIndex, pngBytes, index, totalChunks); }, 500);
    }
  );
}

function handleImageRequest(itemIndex) {
  var feed  = FEEDS[s_lastFeedIndex];
  if (!feed || !s_userId) return;
  var items = s_itemCache[cacheKey(feed.param)];
  if (!items || itemIndex >= items.length) return;

  console.log('Image request for item ' + itemIndex);
  fetchBinary(items[itemIndex].thumbUrl)
    .then(function(buf)      { return resizeToPebbleBitmap(buf); })
    .then(function(pngBytes) {
      var totalChunks = Math.ceil(pngBytes.length / IMG_CHUNK_BYTES);
      console.log('Sending image ' + itemIndex + ': ' + pngBytes.length + 'B, ' + totalChunks + ' chunks');
      sendChunk(itemIndex, pngBytes, 0, totalChunks);
    })
    .catch(function(e) { console.log('Image error: ' + e); });
}

// ---------------------------------------------------------------------------
// PebbleKit JS events
// ---------------------------------------------------------------------------
Pebble.addEventListener('ready', function() {
  var stored = localStorage.getItem('user_id');
  if (stored) s_userId = String(stored).trim();

  var keys = ['feed_stash','feed_wishlist','feed_started',
              'feed_completed','feed_forsale','feed_onorder'];
  keys.forEach(function(key, i) {
    var val = localStorage.getItem(key);
    if (val !== null) s_feeds_enabled[i] = val === 'true' || val === '1';
  });

  console.log('JS ready. user_id=' + s_userId + ' feeds=' + s_feeds_enabled.join(','));

  if (s_userId) handleFeedRequest(s_lastFeedIndex);
});

/* Inbound payloads from the watch are keyed by message key NAME
   (e.g. "REQ_FEED"), not by the numeric messageKeys value used when
   sending. Outbound still uses the numeric keys. */
Pebble.addEventListener('appmessage', function(e) {
  var msg = e.payload;
  console.log('appmessage in: ' + JSON.stringify(msg));

  if (typeof msg.user_id !== 'undefined' ||
      typeof msg.feed_stash !== 'undefined') {
    applySettings(msg);
    s_itemCache = {};
    s_inFlight  = {};
    handleFeedRequest(s_lastFeedIndex);
    return;
  }

  if (typeof msg.REQ_FEED !== 'undefined') {
    var feedIndex = parseInt(msg.REQ_FEED, 10);
    console.log('REQ_FEED -> ' + feedIndex);
    handleFeedRequest(feedIndex);
    return;
  }

  if (typeof msg.REQ_IMAGE !== 'undefined') {
    var itemIndex = parseInt(msg.REQ_IMAGE, 10);
    console.log('REQ_IMAGE -> ' + itemIndex);
    handleImageRequest(itemIndex);
    return;
  }
});
