#include <pebble.h>

#define CONVO_SIZE 4096
#define TEXT_PADDING 4
#define GREETING "AiFace\n\nPress the mic to talk.\nHold to clear."

// Persist the tail of the conversation across launches. The watch gives each
// app 4 kB total with a 256-byte cap per value, so chunk a bounded tail.
#define PERSIST_KEY_NCHUNKS 1
#define PERSIST_KEY_CHUNK_BASE 10
#define PERSIST_CHUNK_BYTES 240
#define PERSIST_CONVO_MAX 2400

static Window *s_window;
static ScrollLayer *s_scroll_layer;
static TextLayer *s_text_layer;
static ActionBarLayer *s_action_bar;
static GBitmap *s_mic_icon;
#if defined(PBL_MICROPHONE)
static DictationSession *s_dictation;
#endif

static char s_convo[CONVO_SIZE];
static char s_display[CONVO_SIZE + 16];
static bool s_waiting;

// Keep the backlight on while a reply arrives and for a readable window after.
#define LIGHT_READ_MS 10000
static AppTimer *s_light_timer;

static void prv_light_off(void *context) {
  s_light_timer = NULL;
  light_enable(false);
}

static void prv_light_hold(uint32_t ms) {
  light_enable(true);
  if (s_light_timer) {
    app_timer_reschedule(s_light_timer, ms);
  } else {
    s_light_timer = app_timer_register(ms, prv_light_off, NULL);
  }
}

// Width available for conversation text, i.e. screen minus the action bar.
static int16_t prv_content_width(void) {
  return layer_get_bounds(window_get_root_layer(s_window)).size.w - ACTION_BAR_WIDTH;
}

static void prv_refresh(void) {
  snprintf(s_display, sizeof(s_display), "%s%s", s_convo, s_waiting ? " ..." : "");
  text_layer_set_text(s_text_layer, s_display);

  int16_t screen_h = layer_get_bounds(window_get_root_layer(s_window)).size.h;
  int16_t text_w = prv_content_width() - TEXT_PADDING * 2;
  // Give the layer full height first; content_size measures within the current
  // frame, so a short frame would clip the measurement of longer replies.
  text_layer_set_size(s_text_layer, GSize(text_w, CONVO_SIZE));
  GSize content = text_layer_get_content_size(s_text_layer);
  content.h += 12;
  text_layer_set_size(s_text_layer, GSize(text_w, content.h));
  scroll_layer_set_content_size(s_scroll_layer, GSize(prv_content_width(), content.h));

  int16_t overflow = content.h - screen_h;
  if (overflow > 0) {
    scroll_layer_set_content_offset(s_scroll_layer, GPoint(0, -overflow), true);
  }
}

static void prv_scroll(int16_t dy) {
  GPoint offset = scroll_layer_get_content_offset(s_scroll_layer);
  GSize content = scroll_layer_get_content_size(s_scroll_layer);
  int16_t screen_h = layer_get_bounds(window_get_root_layer(s_window)).size.h;
  int16_t min_y = screen_h - content.h;  // most-negative offset (bottom)
  if (min_y > 0) min_y = 0;
  offset.y += dy;
  if (offset.y > 0) offset.y = 0;
  if (offset.y < min_y) offset.y = min_y;
  scroll_layer_set_content_offset(s_scroll_layer, offset, true);
}

static void prv_convo_append(const char *str) {
  size_t cur = strlen(s_convo);
  size_t add = strlen(str);
  if (cur + add + 1 > CONVO_SIZE) {
    // Drop the oldest text to make room
    size_t drop = (cur + add + 1) - CONVO_SIZE + 512;
    if (drop > cur) drop = cur;
    memmove(s_convo, s_convo + drop, cur - drop + 1);
    cur -= drop;
  }
  strncat(s_convo, str, CONVO_SIZE - cur - 1);
}

static void prv_save_convo(void) {
  size_t len = strlen(s_convo);
  const char *src = s_convo;
  if (len > PERSIST_CONVO_MAX) {  // keep only the most recent text
    src += (len - PERSIST_CONVO_MAX);
    len = PERSIST_CONVO_MAX;
  }
  int chunks = 0;
  char buf[PERSIST_CHUNK_BYTES + 1];
  for (size_t off = 0; off < len; off += PERSIST_CHUNK_BYTES) {
    size_t n = len - off;
    if (n > PERSIST_CHUNK_BYTES) n = PERSIST_CHUNK_BYTES;
    memcpy(buf, src + off, n);
    buf[n] = '\0';
    persist_write_string(PERSIST_KEY_CHUNK_BASE + chunks, buf);
    chunks++;
  }
  persist_write_int(PERSIST_KEY_NCHUNKS, chunks);
}

static void prv_load_convo(void) {
  s_convo[0] = '\0';
  if (!persist_exists(PERSIST_KEY_NCHUNKS)) {
    return;
  }
  int chunks = persist_read_int(PERSIST_KEY_NCHUNKS);
  char buf[PERSIST_CHUNK_BYTES + 1];
  for (int i = 0; i < chunks; i++) {
    if (persist_exists(PERSIST_KEY_CHUNK_BASE + i)) {
      persist_read_string(PERSIST_KEY_CHUNK_BASE + i, buf, sizeof(buf));
      prv_convo_append(buf);
    }
  }
}

static void prv_send_clear(void) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) {
    return;
  }
  dict_write_uint8(out, MESSAGE_KEY_CLEAR, 1);
  app_message_outbox_send();
}

static void prv_send_transcript(const char *text) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) {
    prv_convo_append("\n[outbox busy]");
    s_waiting = false;
    prv_refresh();
    return;
  }
  dict_write_cstring(out, MESSAGE_KEY_TRANSCRIPT, text);
  app_message_outbox_send();
}

#if defined(PBL_MICROPHONE)
static void prv_dictation_handler(DictationSession *session, DictationSessionStatus status,
                                  char *transcription, void *context) {
  if (status != DictationSessionStatusSuccess) {
    prv_convo_append("\n\n[didn't catch that — press mic to retry]");
    prv_refresh();
    return;
  }
  prv_convo_append("\n\nYou: ");
  prv_convo_append(transcription);
  prv_convo_append("\n\nAI:");
  s_waiting = true;
  prv_refresh();
  prv_send_transcript(transcription);
}
#endif

static void prv_select_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_waiting) {
    return;
  }
#if defined(PBL_MICROPHONE)
  dictation_session_start(s_dictation);
#else
  prv_convo_append("\n[no microphone on this watch]");
  prv_refresh();
#endif
}

static void prv_select_long_handler(ClickRecognizerRef recognizer, void *context) {
  s_convo[0] = '\0';
  prv_convo_append(GREETING);
  s_waiting = false;
  prv_refresh();
  prv_save_convo();
  prv_send_clear();  // reset the phone-side LLM context too
}

static void prv_up_handler(ClickRecognizerRef recognizer, void *context) {
  prv_scroll(layer_get_bounds(window_get_root_layer(s_window)).size.h - 24);
}

static void prv_down_handler(ClickRecognizerRef recognizer, void *context) {
  prv_scroll(-(layer_get_bounds(window_get_root_layer(s_window)).size.h - 24));
}

static void prv_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, prv_select_long_handler, NULL);
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 100, prv_up_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, prv_down_handler);
}

static void prv_inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *chunk = dict_find(iter, MESSAGE_KEY_CHUNK);
  if (chunk) {
    prv_convo_append(chunk->value->cstring);
    prv_light_hold(LIGHT_READ_MS);
  }
  Tuple *status = dict_find(iter, MESSAGE_KEY_STATUS);
  if (status) {
    prv_convo_append(" ");
    prv_convo_append(status->value->cstring);
  }
  if (dict_find(iter, MESSAGE_KEY_FINAL)) {
    s_waiting = false;
    vibes_short_pulse();
    prv_light_hold(LIGHT_READ_MS);  // keep it lit so the reply is readable
    prv_save_convo();  // persist completed exchanges as they arrive
  }
  prv_refresh();
}

static void prv_inbox_dropped(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "inbox dropped: %d", (int)reason);
}

static void prv_outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "outbox failed: %d", (int)reason);
}

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  GRect scroll_frame = GRect(0, 0, bounds.size.w - ACTION_BAR_WIDTH, bounds.size.h);
  s_scroll_layer = scroll_layer_create(scroll_frame);
  scroll_layer_set_shadow_hidden(s_scroll_layer, true);

  s_text_layer = text_layer_create(GRect(TEXT_PADDING, 2,
                                         scroll_frame.size.w - TEXT_PADDING * 2, 2000));
  text_layer_set_font(s_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24));
  text_layer_set_text_color(s_text_layer, GColorBlack);
  text_layer_set_background_color(s_text_layer, GColorClear);

  scroll_layer_add_child(s_scroll_layer, text_layer_get_layer(s_text_layer));
  layer_add_child(root, scroll_layer_get_layer(s_scroll_layer));

  s_mic_icon = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_MIC);
  s_action_bar = action_bar_layer_create();
  action_bar_layer_set_background_color(s_action_bar, GColorBlack);
  action_bar_layer_set_icon_animated(s_action_bar, BUTTON_ID_SELECT, s_mic_icon, true);
  action_bar_layer_set_click_config_provider(s_action_bar, prv_click_config);
  action_bar_layer_add_to_window(s_action_bar, window);

  prv_load_convo();
  if (strlen(s_convo) == 0) {
    prv_convo_append(GREETING);
  }
  prv_refresh();
}

static void prv_window_unload(Window *window) {
  action_bar_layer_destroy(s_action_bar);
  gbitmap_destroy(s_mic_icon);
  text_layer_destroy(s_text_layer);
  scroll_layer_destroy(s_scroll_layer);
}

static void prv_init(void) {
  s_window = window_create();
  window_set_background_color(s_window, GColorWhite);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });

  app_message_register_inbox_received(prv_inbox_received);
  app_message_register_inbox_dropped(prv_inbox_dropped);
  app_message_register_outbox_failed(prv_outbox_failed);
  app_message_open(1024, 512);

#if defined(PBL_MICROPHONE)
  s_dictation = dictation_session_create(512, prv_dictation_handler, NULL);
  // Skip the confirm/edit screen so the transcript sends as soon as STT returns.
  dictation_session_enable_confirmation(s_dictation, false);
  dictation_session_enable_error_dialogs(s_dictation, false);
#endif

  window_stack_push(s_window, true);
}

static void prv_deinit(void) {
  prv_save_convo();
#if defined(PBL_MICROPHONE)
  if (s_dictation) {
    dictation_session_destroy(s_dictation);
  }
#endif
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
