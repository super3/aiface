// AiFace — phone-side bridge. The phone is the source of truth for all
// conversations (list, titles, per-chat message history, and the watch display
// text); the watch is a thin client that sends commands and renders what it's
// told. Configure the API key/model in the app's settings page.

var Clay = require('pebble-clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig);

var DEFAULT_MODEL = 'anthropic/claude-haiku-4.5';
var SYSTEM_PROMPT =
  'You are a voice assistant on a Pebble smartwatch with a tiny screen. ' +
  'Reply in plain text only (no markdown). Keep replies under 60 words ' +
  'unless the user explicitly asks for detail.';

var CHUNK_SIZE = 180;   // bytes per AppMessage, well under the inbox limit
var MAX_HISTORY = 20;   // messages kept per conversation for LLM context
var MAX_CONVS = 20;     // conversations retained
var DISPLAY_CAP = 6000; // chars of rendered transcript kept per conversation
var STORE_KEY = 'aiface-store';

function getSettings() {
  try {
    return JSON.parse(localStorage.getItem('clay-settings')) || {};
  } catch (e) {
    return {};
  }
}

// ---- conversation store -------------------------------------------------

var store = loadStore();

function loadStore() {
  try {
    var s = JSON.parse(localStorage.getItem(STORE_KEY));
    if (s && s.convs && s.order) return s;
  } catch (e) {}
  return { active: null, order: [], convs: {} };
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

function activeConv() {
  return store.convs[store.active];
}

function moveFront(id) {
  store.order = [id].concat(store.order.filter(function(x) { return x !== id; }));
}

function deleteConversation(id) {
  if (!store.convs[id]) return;
  delete store.convs[id];
  store.order = store.order.filter(function(x) { return x !== id; });
  if (store.active === id) {
    store.active = null;
  }
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

// ---- serialized outbound queue -----------------------------------------
// AppMessage sends must not overlap, so funnel everything through one queue.

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
    // one retry, then drop so the queue can't wedge
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

function pushList() {
  enqueue({ CONV_LIST: serializeList() });
}

function serializeList() {
  return store.order.map(function(id) {
    var c = store.convs[id];
    if (!c) return null;
    return (id === store.active ? '*' : '') + id + '\t' + c.title;
  }).filter(Boolean).join('\n');
}

function sendStatus(msg) {
  enqueue({ STATUS: msg });
  enqueue({ FINAL: 1 });
}

// ---- events -------------------------------------------------------------

Pebble.addEventListener('ready', function() {
  ensureActive();
  pushDisplay();
  pushList();
});

Pebble.addEventListener('appmessage', function(e) {
  var p = e.payload;
  if (p.NEW_CHAT !== undefined) {
    newConversation();
    pushDisplay();
    pushList();
    return;
  }
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
  if (p.DELETE_CHAT !== undefined) {
    deleteConversation(p.DELETE_CHAT);
    pushDisplay();
    pushList();
    return;
  }
  if (p.SYNC !== undefined) {
    ensureActive();
    pushDisplay();
    pushList();
    return;
  }
  if (p.TRANSCRIPT) {
    ask(p.TRANSCRIPT);
    return;
  }
});

// ---- the LLM call -------------------------------------------------------

function ask(prompt) {
  var settings = getSettings();
  var apiKey = settings.APIKEY || '';
  var model = settings.MODEL || DEFAULT_MODEL;
  if (!apiKey) {
    sendStatus('[Set your API key in the app settings on your phone]');
    return;
  }

  ensureActive();
  var conv = activeConv();
  var firstMessage = conv.messages.length === 0;

  // Record the user turn. The watch already shows "You: ..." locally for
  // instant feedback, so we don't echo it back — only the reply is streamed.
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

  var convId = conv.id;
  var xhr = new XMLHttpRequest();
  xhr.open('POST', 'https://openrouter.ai/api/v1/chat/completions');
  xhr.setRequestHeader('Content-Type', 'application/json');
  xhr.setRequestHeader('Authorization', 'Bearer ' + apiKey);
  xhr.timeout = 30000;

  xhr.onload = function() {
    var resp;
    try {
      resp = JSON.parse(xhr.responseText);
    } catch (err) {
      sendStatus('[Bad response from OpenRouter]');
      return;
    }
    if (xhr.status !== 200) {
      var detail = resp.error && resp.error.message ? ': ' + resp.error.message : '';
      sendStatus('[Error ' + xhr.status + detail + ']');
      return;
    }
    var text = resp.choices[0].message.content.trim();
    var c = store.convs[convId];
    if (c) {
      appendDisplay(c, text);
      c.messages.push({ role: 'assistant', content: text });
      saveStore();
    }
    enqueueText(text);
    enqueue({ FINAL: 1 });
  };
  xhr.ontimeout = function() { sendStatus('[Timed out]'); };
  xhr.onerror = function() { sendStatus('[Network error]'); };

  xhr.send(JSON.stringify({
    model: model,
    max_tokens: 300,
    messages: [{ role: 'system', content: SYSTEM_PROMPT }].concat(conv.messages)
  }));
}
