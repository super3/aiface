#include <pebble.h>

#define CONVO_SIZE 4096
#define GREETING "AiFace\nPress Select and speak.\nHold Select to clear."

static Window *s_window;
static ScrollLayer *s_scroll_layer;
static TextLayer *s_text_layer;
#if defined(PBL_MICROPHONE)
static DictationSession *s_dictation;
#endif

static char s_convo[CONVO_SIZE];
static char s_display[CONVO_SIZE + 16];
static bool s_waiting;

static void prv_refresh(void) {
  snprintf(s_display, sizeof(s_display), "%s%s", s_convo, s_waiting ? " ..." : "");
  text_layer_set_text(s_text_layer, s_display);

  GRect bounds = layer_get_bounds(window_get_root_layer(s_window));
  // Give the layer full height first; content_size measures within the current
  // frame, so a short frame would clip the measurement of longer replies.
  text_layer_set_size(s_text_layer, GSize(bounds.size.w - 8, CONVO_SIZE));
  GSize content = text_layer_get_content_size(s_text_layer);
  content.h += 12;
  text_layer_set_size(s_text_layer, GSize(bounds.size.w - 8, content.h));
  scroll_layer_set_content_size(s_scroll_layer, GSize(bounds.size.w, content.h));

  int16_t overflow = content.h - bounds.size.h;
  if (overflow > 0) {
    scroll_layer_set_content_offset(s_scroll_layer, GPoint(0, -overflow), true);
  }
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
}

static void prv_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, prv_select_long_handler, NULL);
}

static void prv_inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *chunk = dict_find(iter, MESSAGE_KEY_CHUNK);
  if (chunk) {
    prv_convo_append(chunk->value->cstring);
  }
  Tuple *status = dict_find(iter, MESSAGE_KEY_STATUS);
  if (status) {
    prv_convo_append(" ");
    prv_convo_append(status->value->cstring);
  }
  if (dict_find(iter, MESSAGE_KEY_FINAL)) {
    s_waiting = false;
    vibes_short_pulse();
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

  s_scroll_layer = scroll_layer_create(bounds);
  scroll_layer_set_shadow_hidden(s_scroll_layer, true);
  scroll_layer_set_click_config_onto_window(s_scroll_layer, window);
  scroll_layer_set_callbacks(s_scroll_layer, (ScrollLayerCallbacks) {
    .click_config_provider = prv_click_config,
  });

  s_text_layer = text_layer_create(GRect(4, 0, bounds.size.w - 8, 2000));
  text_layer_set_font(s_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24));
  text_layer_set_text_color(s_text_layer, GColorBlack);
  text_layer_set_background_color(s_text_layer, GColorClear);

  scroll_layer_add_child(s_scroll_layer, text_layer_get_layer(s_text_layer));
  layer_add_child(root, scroll_layer_get_layer(s_scroll_layer));

  prv_convo_append(GREETING);
  prv_refresh();
}

static void prv_window_unload(Window *window) {
  text_layer_destroy(s_text_layer);
  scroll_layer_destroy(s_scroll_layer);
}

static void prv_init(void) {
  s_window = window_create();
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
#endif

  window_stack_push(s_window, true);
}

static void prv_deinit(void) {
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
