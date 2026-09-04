// ---------------------------------------------------------------------------
// ScaleMates Stash Viewer — PebbleKit JS  (@rebble/clay edition)
// ---------------------------------------------------------------------------

var Clay = require('@rebble/clay');
var clayConfig = require('./clay-config');
var messageKeys = require('message_keys');

// Clay auto-handles showConfiguration and webviewclosed, sending settings
// to the watch as AppMessage using the numeric keys from message_keys.
var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

Pebble.addEventListener('showConfiguration', function() {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e || !e.response) return;

  // Get settings as a plain object (messageKey names as keys, not integers)
  var settings = clay.getSettings(e.response, false);

  // Extract values — getSettings(response, false) returns {key: {value: X}}
  var uid = settings.user_id ? settings.user_id.value : null;
  if (uid !== null && uid !== undefined) {
    s_userId = String(uid).trim();
    localStorage.setItem('user_id', s_userId);
  }
  var toggles = ['feed_stash','feed_wishlist','feed_started','feed_completed','feed_forsale'];
  toggles.forEach(function(key, i) {
    if (settings[key] !== undefined) {
      s_feeds_enabled[i] = !!settings[key].value;
      localStorage.setItem(key, s_feeds_enabled[i]);
    }
  });
  console.log('webviewclosed. user_id=' + s_userId + ' feeds=' + s_feeds_enabled.join(','));

  // Now send to watch using the numeric messageKey integers
  var dict = clay.getSettings(e.response);
  Pebble.sendAppMessage(dict,
    function() { console.log('Sent config data to Pebble'); },
    function(e) { console.log('Failed to send config: ' + JSON.stringify(e)); }
  );

  // Also trigger a feed fetch immediately without waiting for watch to re-request
  s_itemCache = {};
  handleFeedRequest(s_lastFeedIndex);
});

// ---------------------------------------------------------------------------
// AppMessage key constants — from message_keys module (auto-assigned by SDK)
// ---------------------------------------------------------------------------
var KEY_FEED_NAME   = messageKeys.FEED_NAME;
var KEY_ITEM_INDEX  = messageKeys.ITEM_INDEX;
var KEY_ITEM_TOTAL  = messageKeys.ITEM_TOTAL;
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

// Clay settings keys
var KEY_USER_ID        = messageKeys.user_id;
var KEY_FEED_STASH     = messageKeys.feed_stash;
var KEY_FEED_WISHLIST  = messageKeys.feed_wishlist;
var KEY_FEED_STARTED   = messageKeys.feed_started;
var KEY_FEED_COMPLETED = messageKeys.feed_completed;
var KEY_FEED_FORSALE   = messageKeys.feed_forsale;

var IMG_CHUNK_BYTES = 512;
var THUMB_SIZE      = 140;

var FEEDS = [
  { id: 0, label: 'Stash',     param: 'stash'     },
  { id: 1, label: 'Wishlist',  param: 'wishlist'  },
  { id: 2, label: 'Started',   param: 'started'   },
  { id: 3, label: 'Completed', param: 'completed' },
  { id: 4, label: 'For Sale',  param: 'forsale'   },
];

// ---------------------------------------------------------------------------
// Settings — stored locally after Clay delivers them via AppMessage
// ---------------------------------------------------------------------------
var s_userId   = '';
var s_feeds_enabled = [true, false, false, false, false];  // defaults

function applySettings(msg) {
  if (typeof msg[KEY_USER_ID] !== 'undefined') {
    s_userId = String(msg[KEY_USER_ID]).trim();
    localStorage.setItem('user_id', s_userId);
  }
  if (typeof msg[KEY_FEED_STASH]     !== 'undefined') { s_feeds_enabled[0] = !!msg[KEY_FEED_STASH];     localStorage.setItem('feed_stash',     s_feeds_enabled[0]); }
  if (typeof msg[KEY_FEED_WISHLIST]  !== 'undefined') { s_feeds_enabled[1] = !!msg[KEY_FEED_WISHLIST];  localStorage.setItem('feed_wishlist',  s_feeds_enabled[1]); }
  if (typeof msg[KEY_FEED_STARTED]   !== 'undefined') { s_feeds_enabled[2] = !!msg[KEY_FEED_STARTED];   localStorage.setItem('feed_started',   s_feeds_enabled[2]); }
  if (typeof msg[KEY_FEED_COMPLETED] !== 'undefined') { s_feeds_enabled[3] = !!msg[KEY_FEED_COMPLETED]; localStorage.setItem('feed_completed', s_feeds_enabled[3]); }
  if (typeof msg[KEY_FEED_FORSALE]   !== 'undefined') { s_feeds_enabled[4] = !!msg[KEY_FEED_FORSALE];   localStorage.setItem('feed_forsale',   s_feeds_enabled[4]); }
  console.log('Settings applied. user_id=' + s_userId + ' feeds=' + s_feeds_enabled.join(','));
}

// ---------------------------------------------------------------------------
// HTML parser
// ---------------------------------------------------------------------------

function parseItems(html) {
  var items = [];
  var imgRe = /<img[^>]+src="(https:\/\/www\.scalemates\.com\/products\/img\/[^"]+)"[^>]+title="([^"]+)"[^>]*>/gi;
  var match;

  while ((match = imgRe.exec(html)) !== null) {
    var thumbUrl = match[1];
    var title    = match[2];

    var titleRe  = /^(1:\d+)\s+(.+?)\s+\(([^)]+)\)\s*$/;
    var tm       = titleRe.exec(title);
    var scale    = tm ? tm[1] : '';
    var name     = tm ? tm[2] : title;
    var rest     = tm ? tm[3] : '';

    var lastSpace = rest.lastIndexOf(' ');
    var brand  = lastSpace > 0 ? rest.substring(0, lastSpace)  : rest;
    var kit_no = lastSpace > 0 ? rest.substring(lastSpace + 1) : '';

    items.push({
      scale:    scale.substring(0, 47),
      name:     name.substring(0, 79),
      brand:    brand.substring(0, 47),
      kit_no:   kit_no.substring(0, 47),
      thumbUrl: thumbUrl,
    });
  }
  return items;
}

// ---------------------------------------------------------------------------
// Metadata sender
// ---------------------------------------------------------------------------

function sendMeta(feedLabel, itemIndex, itemTotal, item) {
  return new Promise(function(resolve, reject) {
    var msg = {};
    msg[KEY_FEED_NAME]  = feedLabel;
    msg[KEY_ITEM_INDEX] = itemIndex;
    msg[KEY_ITEM_TOTAL] = itemTotal;
    msg[KEY_KIT_NO]     = item.kit_no;
    msg[KEY_BRAND]      = item.brand;
    msg[KEY_NAME]       = item.name;
    msg[KEY_SCALE]      = item.scale;
    Pebble.sendAppMessage(msg,
      function()  { resolve(); },
      function(e) { reject(e); }
    );
  });
}

function sendAllMeta(feed, items) {
  var total = items.length;
  var index = 0;
  function sendNext() {
    if (index >= total) return;
    var i = index++;
    sendMeta(feed.label, i, total, items[i])
      .then(function()  { setTimeout(sendNext, 30); })
      .catch(function() { setTimeout(sendNext, 150); });
  }
  sendNext();
}

// ---------------------------------------------------------------------------
// Image pipeline
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

function resizeToThumbPng(arrayBuffer) {
  return new Promise(function(resolve, reject) {
    var blob = new Blob([arrayBuffer]);
    var url  = URL.createObjectURL(blob);
    var img  = new Image();
    img.onload = function() {
      URL.revokeObjectURL(url);
      var canvas = document.createElement('canvas');
      canvas.width = canvas.height = THUMB_SIZE;
      canvas.getContext('2d').drawImage(img, 0, 0, THUMB_SIZE, THUMB_SIZE);
      var b64    = canvas.toDataURL('image/png').split(',')[1];
      var binary = atob(b64);
      var bytes  = new Uint8Array(binary.length);
      for (var i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
      resolve(bytes);
    };
    img.onerror = function() { URL.revokeObjectURL(url); reject(new Error('img load failed')); };
    img.src = url;
  });
}

function sendPngChunks(itemIndex, pngBytes) {
  var totalChunks = Math.ceil(pngBytes.length / IMG_CHUNK_BYTES);
  function sendChunk(i) {
    if (i >= totalChunks) return;
    var start = i * IMG_CHUNK_BYTES;
    var chunk = Array.from(pngBytes.subarray(start, Math.min(start + IMG_CHUNK_BYTES, pngBytes.length)));
    var msg = {};
    msg[KEY_IMG_ITEM]    = itemIndex;
    msg[KEY_IMG_CHUNK_I] = i;
    msg[KEY_IMG_CHUNK_N] = totalChunks;
    msg[KEY_IMG_DATA]    = chunk;
    Pebble.sendAppMessage(msg,
      function() { setTimeout(function() { sendChunk(i + 1); }, 20); },
      function() { setTimeout(function() { sendChunk(i); },     200); }
    );
  }
  sendChunk(0);
}

// ---------------------------------------------------------------------------
// Feed cache
// ---------------------------------------------------------------------------

var s_itemCache     = {};
var s_lastFeedIndex = 0;

function cacheKey(feedParam) { return feedParam + ':' + s_userId; }

// ---------------------------------------------------------------------------
// Request handlers
// ---------------------------------------------------------------------------

function handleFeedRequest(feedIndex) {
  console.log('handleFeedRequest: feedIndex=' + feedIndex + ' userId="' + s_userId + '" enabled=' + s_feeds_enabled[feedIndex]);

  if (!s_userId) {
    var errMsg = {};
    errMsg[KEY_ERROR] = 'No user ID — open settings';
    Pebble.sendAppMessage(errMsg);
    return;
  }

  var feed = FEEDS[feedIndex];
  if (!feed || !s_feeds_enabled[feedIndex]) {
    console.log('handleFeedRequest: bailing — feed=' + JSON.stringify(feed) + ' enabled=' + s_feeds_enabled[feedIndex]);
    return;
  }

  var key = cacheKey(feed.param);
  if (s_itemCache[key]) {
    sendAllMeta(feed, s_itemCache[key]);
    return;
  }

  var url = 'https://www.scalemates.com/profiles/mate.php?id=' +
            s_userId + '&p=' + feed.param;
  console.log('Fetching: ' + url);

  fetch(url)
    .then(function(r)    { return r.text(); })
    .then(function(html) {
      console.log('HTML preview: ' + html.substring(0, 500));
      var items = parseItems(html);
      console.log(feed.label + ': ' + items.length + ' items parsed from ' + html.length + ' bytes');
      s_itemCache[key] = items;
      sendAllMeta(feed, items);
    })
    .catch(function(e) {
      console.log('Fetch error: ' + e);
      var errMsg = {};
      errMsg[KEY_ERROR] = 'Fetch failed';
      Pebble.sendAppMessage(errMsg);
    });
}

function handleImageRequest(itemIndex) {
  var feed  = FEEDS[s_lastFeedIndex];
  if (!feed || !s_userId) return;
  var items = s_itemCache[cacheKey(feed.param)];
  if (!items || itemIndex >= items.length) return;

  fetchBinary(items[itemIndex].thumbUrl)
    .then(function(buf)      { return resizeToThumbPng(buf); })
    .then(function(pngBytes) {
      console.log('Item ' + itemIndex + ': ' + pngBytes.length + 'B, ' +
                  Math.ceil(pngBytes.length / IMG_CHUNK_BYTES) + ' chunks');
      sendPngChunks(itemIndex, pngBytes);
    })
    .catch(function(e) { console.log('Image error: ' + e); });
}

// ---------------------------------------------------------------------------
// PebbleKit JS events
// ---------------------------------------------------------------------------

Pebble.addEventListener('ready', function() {
  // Clay persists settings to localStorage under each messageKey name.
  // Load them now so the first KEY_REQ_FEED from the watch can be served.
  var stored = localStorage.getItem('user_id');
  if (stored) s_userId = String(stored).trim();

  var stash     = localStorage.getItem('feed_stash');
  var wishlist  = localStorage.getItem('feed_wishlist');
  var started   = localStorage.getItem('feed_started');
  var completed = localStorage.getItem('feed_completed');
  var forsale   = localStorage.getItem('feed_forsale');

  if (stash     !== null) s_feeds_enabled[0] = stash     === 'true' || stash     === '1';
  if (wishlist  !== null) s_feeds_enabled[1] = wishlist  === 'true' || wishlist  === '1';
  if (started   !== null) s_feeds_enabled[2] = started   === 'true' || started   === '1';
  if (completed !== null) s_feeds_enabled[3] = completed === 'true' || completed === '1';
  if (forsale   !== null) s_feeds_enabled[4] = forsale   === 'true' || forsale   === '1';

  console.log('JS ready. user_id=' + s_userId + ' feeds=' + s_feeds_enabled.join(','));
});

Pebble.addEventListener('appmessage', function(e) {
  var msg = e.payload;

  // Debug: log everything that arrives
  console.log('appmessage keys: ' + JSON.stringify(Object.keys(msg)));
  console.log('appmessage values: ' + JSON.stringify(msg));

  // Clay settings arrive here after the user saves config.
  var isSettings = typeof msg[KEY_USER_ID]    !== 'undefined' ||
                   typeof msg[KEY_FEED_STASH] !== 'undefined';
  if (isSettings) {
    applySettings(msg);
    s_itemCache = {};
    console.log('Requesting feed ' + s_lastFeedIndex + ' after settings, userId=' + s_userId);
    handleFeedRequest(s_lastFeedIndex);
    return;
  }

  if (typeof msg[KEY_REQ_FEED] !== 'undefined') {
    s_lastFeedIndex = parseInt(msg[KEY_REQ_FEED], 10);
    handleFeedRequest(s_lastFeedIndex);
  }

  if (typeof msg[KEY_REQ_IMAGE] !== 'undefined') {
    handleImageRequest(parseInt(msg[KEY_REQ_IMAGE], 10));
  }
});

// NOTE: showConfiguration and webviewclosed are handled by @rebble/clay.
// Clay sends the settings dict to the watch, which arrives in appmessage above.
