// AiFace — phone-side bridge. The phone is the source of truth for all
// conversations (list, titles, per-chat message history, rendered display) and
// the selected model; the watch is a thin client. Configure the API key in the
// app's settings page.

var Clay = require('pebble-clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig);

var DEFAULT_MODEL = 'anthropic/claude-haiku-4.5';

// Models the on-watch switcher cycles through. Edit to taste — any OpenRouter
// model id works (https://openrouter.ai/models).
var PRESET_MODELS = [
  'anthropic/claude-haiku-4.5',
  'anthropic/claude-sonnet-4.6',
  'openai/gpt-4o-mini',
  'google/gemini-2.0-flash-001'
];

var SYSTEM_PROMPT =
  'You are a voice assistant on a Pebble smartwatch with a tiny screen. ' +
  'Reply in plain text only (no markdown). Keep replies under 60 words ' +
  'unless the user explicitly asks for detail.';

var CHUNK_SIZE = 180;
var MAX_HISTORY = 20;
var MAX_CONVS = 20;
var DISPLAY_CAP = 6000;
var STORE_KEY = 'aiface-store';

function getSettings() {
  try {
    return JSON.parse(localStorage.getItem('clay-settings')) || {};
  } catch (e) {
    return {};
  }
}

function hasKey() {
  return !!(getSettings().APIKEY || '');
}

// ---- store --------------------------------------------------------------

var store = loadStore();

function loadStore() {
  try {
    var s = JSON.parse(localStorage.getItem(STORE_KEY));
    if (s && s.convs && s.order) return s;
  } catch (e) {}
  return { active: null, order: [], convs: {}, model: null };
}

function saveStore() {
  try {
    localStorage.setItem(STORE_KEY, JSON.stringify(store));
  } catch (e) {}
}

function genId() {
  return 'c' + Date.now().toString(36) + Math.floor(Math.random() * 1296).toString(36);
}

function newConversation() {
  var id = genId();
  store.convs[id] = { id: id, title: 'New Chat', messages: [], display: '' };
  store.order.unshift(id);
  while (store.order.length > MAX_CONVS) {
    delete store.convs[store.order.pop()];
  }
  store.active = id;
  saveStore();
  return id;
}

function ensureActive() {
  if (!store.active || !store.convs[store.active]) {
    store.active = store.order.length ? store.order[0] : newConversation();
  }
}

function activeConv() { return store.convs[store.active]; }

function moveFront(id) {
  store.order = [id].concat(store.order.filter(function(x) { return x !== id; }));
}

function deleteConversation(id) {
  if (!store.convs[id]) return;
  delete store.convs[id];
  store.order = store.order.filter(function(x) { return x !== id; });
  if (store.active === id) store.active = null;
  ensureActive();
  saveStore();
}

function makeTitle(s) {
  var t = s.replace(/\s+/g, ' ').trim();
  if (t.length > 24) t = t.slice(0, 24) + '…';
  return t || 'New Chat';
}

function appendDisplay(conv, str) {
  conv.display += str;
  if (conv.display.length > DISPLAY_CAP) {
    conv.display = conv.display.slice(conv.display.length - DISPLAY_CAP);
  }
}

// ---- model --------------------------------------------------------------

function currentModel() {
  return store.model || getSettings().MODEL || DEFAULT_MODEL;
}

function modelShort(m) {
  var i = m.indexOf('/');
  return i >= 0 ? m.slice(i + 1) : m;
}

function nextModel() {
  var idx = PRESET_MODELS.indexOf(currentModel());
  store.model = PRESET_MODELS[(idx + 1) % PRESET_MODELS.length];
  saveStore();
  pushModelName();
}

// ---- serialized outbound queue ------------------------------------------

var outQ = [];
var sending = false;

function enqueue(msg) {
  outQ.push(msg);
  pump();
}

function pump() {
  if (sending || !outQ.length) return;
  sending = true;
  var msg = outQ.shift();
  Pebble.sendAppMessage(msg, function() {
    sending = false;
    pump();
  }, function() {
    Pebble.sendAppMessage(msg, function() {
      sending = false;
      pump();
    }, function() {
      sending = false;
      pump();
    });
  });
}

function enqueueText(text) {
  for (var off = 0; off < text.length; off += CHUNK_SIZE) {
    enqueue({ CHUNK: text.slice(off, off + CHUNK_SIZE) });
  }
}

function pushDisplay() {
  var conv = activeConv();
  enqueue({ DISPLAY_RESET: 1 });
  if (conv && conv.display) enqueueText(conv.display);
  enqueue({ FINAL: 1 });
}

function serializeList() {
  return store.order.map(function(id) {
    var c = store.convs[id];
    if (!c) return null;
    return (id === store.active ? '*' : '') + id + '\t' + c.title;
  }).filter(Boolean).join('\n');
}

function pushList() { enqueue({ CONV_LIST: serializeList() }); }
function pushModelName() { enqueue({ MODEL_NAME: modelShort(currentModel()) }); }
function pushNoKey() { enqueue({ NO_KEY: hasKey() ? 0 : 1 }); }
function sendStatus(msg) { enqueue({ STATUS: msg }); enqueue({ FINAL: 1 }); }

// ---- events -------------------------------------------------------------

Pebble.addEventListener('ready', function() {
  ensureActive();
  pushNoKey();
  pushModelName();
  pushDisplay();
  pushList();
});

// Re-check the key after the settings page closes.
Pebble.addEventListener('webviewclosed', function() {
  setTimeout(function() {
    pushNoKey();
    pushModelName();
  }, 150);
});

Pebble.addEventListener('appmessage', function(e) {
  var p = e.payload;
  if (p.NEW_CHAT !== undefined) { newConversation(); pushDisplay(); pushList(); return; }
  if (p.SWITCH_CHAT !== undefined) {
    if (store.convs[p.SWITCH_CHAT]) {
      store.active = p.SWITCH_CHAT;
      moveFront(store.active);
      saveStore();
      pushDisplay();
      pushList();
    }
    return;
  }
  if (p.DELETE_CHAT !== undefined) { deleteConversation(p.DELETE_CHAT); pushDisplay(); pushList(); return; }
  if (p.NEXT_MODEL !== undefined) { nextModel(); return; }
  if (p.CANCEL !== undefined) { cancelRequest(); return; }
  if (p.SYNC !== undefined) { ensureActive(); pushNoKey(); pushModelName(); pushDisplay(); pushList(); return; }
  if (p.TRANSCRIPT) { ask(p.TRANSCRIPT); return; }
});

// ---- the LLM call (streaming) -------------------------------------------

var currentXhr = null;
var canceled = false;

function cancelRequest() {
  canceled = true;
  if (currentXhr) {
    try { currentXhr.abort(); } catch (e) {}
    currentXhr = null;
  }
}

function ask(prompt) {
  if (!hasKey()) {
    pushNoKey();
    sendStatus('[Set your API key in the phone app]');
    return;
  }
  ensureActive();
  var conv = activeConv();
  var firstMessage = conv.messages.length === 0;

  appendDisplay(conv, (conv.display.length ? '\n\n' : '') + 'You: ' + prompt + '\n\n');
  conv.messages.push({ role: 'user', content: prompt });
  if (conv.messages.length > MAX_HISTORY) {
    conv.messages.splice(0, conv.messages.length - MAX_HISTORY);
  }
  if (firstMessage || conv.title === 'New Chat') {
    conv.title = makeTitle(prompt);
    pushList();
  }
  moveFront(conv.id);
  saveStore();

  sendRequest(conv.id, currentModel(), conv.messages.slice(), 0);
}

function sendRequest(convId, model, msgs, attempt) {
  canceled = false;
  var xhr = new XMLHttpRequest();
  currentXhr = xhr;
  xhr.open('POST', 'https://openrouter.ai/api/v1/chat/completions');
  xhr.setRequestHeader('Content-Type', 'application/json');
  xhr.setRequestHeader('Authorization', 'Bearer ' + (getSettings().APIKEY || ''));
  xhr.timeout = 45000;

  var processed = 0;  // bytes of responseText parsed
  var full = '';      // accumulated reply text
  var emitted = 0;    // chars already sent to the watch

  function consume() {
    var text = xhr.responseText || '';
    var lastNL = text.lastIndexOf('\n');
    if (lastNL + 1 <= processed) return;  // no complete new line yet
    var slice = text.slice(processed, lastNL + 1);
    processed = lastNL + 1;
    var lines = slice.split('\n');
    for (var i = 0; i < lines.length; i++) {
      var line = lines[i].trim();
      if (line.indexOf('data:') !== 0) continue;
      var data = line.slice(5).trim();
      if (!data || data === '[DONE]') continue;
      try {
        var obj = JSON.parse(data);
        var ch = obj.choices && obj.choices[0];
        var delta = ch && ((ch.delta && ch.delta.content) || (ch.message && ch.message.content));
        if (delta) full += delta;
      } catch (err) {}
    }
    if (full.length > emitted) {
      enqueueText(full.slice(emitted));
      emitted = full.length;
    }
  }

  function finalize() {
    var c = store.convs[convId];
    if (c && full) {
      appendDisplay(c, full);
      c.messages.push({ role: 'assistant', content: full });
      saveStore();
    }
    enqueue({ FINAL: 1 });
    currentXhr = null;
  }

  function fail(errmsg) {
    if (canceled) { currentXhr = null; return; }
    if (emitted > 0) { finalize(); return; }     // already streamed partial; keep it
    if (attempt < 1) { sendRequest(convId, model, msgs, attempt + 1); return; }  // one retry
    sendStatus(errmsg);
    currentXhr = null;
  }

  xhr.onprogress = function() { if (!canceled) consume(); };
  xhr.onload = function() {
    if (canceled) { currentXhr = null; return; }
    if (xhr.status !== 200) {
      var detail = '';
      try {
        var r = JSON.parse(xhr.responseText);
        if (r.error && r.error.message) detail = ': ' + r.error.message;
      } catch (e) {}
      sendStatus('[Error ' + xhr.status + detail + ']');
      currentXhr = null;
      return;
    }
    consume();
    finalize();
  };
  xhr.ontimeout = function() { fail('[Timed out]'); };
  xhr.onerror = function() { fail('[Network error]'); };

  xhr.send(JSON.stringify({
    model: model,
    max_tokens: 300,
    stream: true,
    messages: [{ role: 'system', content: SYSTEM_PROMPT }].concat(msgs)
  }));
}
