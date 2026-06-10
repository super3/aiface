// AiFace — phone-side bridge: watch transcript -> OpenRouter -> chunked reply
// Configure the API key and model in the app's settings page (Pebble phone app).

var Clay = require('pebble-clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig);

var DEFAULT_MODEL = 'anthropic/claude-haiku-4.5';

function getSettings() {
  try {
    return JSON.parse(localStorage.getItem('clay-settings')) || {};
  } catch (e) {
    return {};
  }
}

var SYSTEM_PROMPT =
  'You are a voice assistant on a Pebble smartwatch with a tiny screen. ' +
  'Reply in plain text only (no markdown). Keep replies under 60 words ' +
  'unless the user explicitly asks for detail.';

var CHUNK_SIZE = 180; // bytes per AppMessage, well under the inbox limit
var MAX_HISTORY = 20; // messages kept for conversational context

var messages = [];

Pebble.addEventListener('ready', function() {
  console.log('AiFace PKJS ready');
});

Pebble.addEventListener('appmessage', function(e) {
  var transcript = e.payload.TRANSCRIPT;
  if (transcript) {
    ask(transcript);
  }
});

function ask(prompt) {
  var settings = getSettings();
  var apiKey = settings.APIKEY || '';
  var model = settings.MODEL || DEFAULT_MODEL;
  if (!apiKey) {
    sendStatus('[Set your API key in the app settings on your phone]');
    return;
  }

  messages.push({ role: 'user', content: prompt });
  if (messages.length > MAX_HISTORY) {
    messages.splice(0, messages.length - MAX_HISTORY);
  }

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
    messages.push({ role: 'assistant', content: text });
    sendChunks(' ' + text);
  };
  xhr.ontimeout = function() { sendStatus('[Timed out]'); };
  xhr.onerror = function() { sendStatus('[Network error]'); };

  xhr.send(JSON.stringify({
    model: model,
    max_tokens: 300,
    messages: [{ role: 'system', content: SYSTEM_PROMPT }].concat(messages)
  }));
}

function sendChunks(text) {
  var offset = 0;
  var retried = false;

  function sendNext() {
    if (offset >= text.length) {
      Pebble.sendAppMessage({ FINAL: 1 });
      return;
    }
    var piece = text.slice(offset, offset + CHUNK_SIZE);
    Pebble.sendAppMessage({ CHUNK: piece }, function() {
      offset += CHUNK_SIZE;
      retried = false;
      sendNext();
    }, function(e) {
      if (retried) {
        Pebble.sendAppMessage({ STATUS: '[transfer failed]', FINAL: 1 });
        return;
      }
      retried = true;
      setTimeout(sendNext, 200);
    });
  }

  sendNext();
}

function sendStatus(msg) {
  Pebble.sendAppMessage({ STATUS: msg, FINAL: 1 });
}
