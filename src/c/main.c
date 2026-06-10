#include <pebble.h>

#define CONVO_SIZE 4096
#define TEXT_PADDING 4
#define GREETING "AiFace\n\nPress the mic to talk.\nHold for chats."

#define MAX_CONVS 20
#define ID_LEN 24
#define TITLE_LEN 40
#define LIST_BUF 2048

static Window *s_window;
static ScrollLayer *s_scroll_layer;
static TextLayer *s_text_layer;
static ActionBarLayer *s_action_bar;
static GBitmap *s_mic_icon;

static Window *s_menu_window;
static MenuLayer *s_menu_layer;

#if defined(PBL_MICROPHONE)
static DictationSession *s_dictation;
#endif

static char s_convo[CONVO_SIZE];
static char s_display[CONVO_SIZE + 16];
static bool s_waiting;
static bool s_display_stream;  // true while the phone is streaming a switched convo

// Conversation list mirror (titles + ids) pushed from the phone for the menu.
static char s_conv_ids[MAX_CONVS][ID_LEN];
static char s_conv_titles[MAX_CONVS][TITLE_LEN];
static int s_conv_count;
static int s_active_index;
static char s_list_buf[LIST_BUF];

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

// ---- chat view ----------------------------------------------------------

static void prv_refresh(void) {
  const char *shown = (strlen(s_convo) == 0) ? GREETING : s_convo;
  snprintf(s_display, sizeof(s_display), "%s%s", shown, s_waiting ? " ..." : "");
  text_layer_set_text(s_text_layer, s_display);

  int16_t screen_h = layer_get_bounds(window_get_root_layer(s_window)).size.h;
  int16_t text_w = layer_get_bounds(window_get_root_layer(s_window)).size.w
                   - ACTION_BAR_WIDTH - TEXT_PADDING * 2;
  // Give the layer full height first; content_size measures within the current
  // frame, so a short frame would clip the measurement of longer replies.
  text_layer_set_size(s_text_layer, GSize(text_w, CONVO_SIZE));
  GSize content = text_layer_get_content_size(s_text_layer);
  content.h += 12;
  text_layer_set_size(s_text_layer, GSize(text_w, content.h));
  scroll_layer_set_content_size(s_scroll_layer, GSize(text_w + TEXT_PADDING * 2, content.h));

  int16_t overflow = content.h - screen_h;
  if (overflow > 0) {
    scroll_layer_set_content_offset(s_scroll_layer, GPoint(0, -overflow), true);
  }
}

static void prv_scroll(int16_t dy) {
  GPoint offset = scroll_layer_get_content_offset(s_scroll_layer);
  GSize content = scroll_layer_get_content_size(s_scroll_layer);
  int16_t screen_h = layer_get_bounds(window_get_root_layer(s_window)).size.h;
  int16_t min_y = screen_h - content.h;
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
    size_t drop = (cur + add + 1) - CONVO_SIZE + 512;
    if (drop > cur) drop = cur;
    memmove(s_convo, s_convo + drop, cur - drop + 1);
    cur -= drop;
  }
  strncat(s_convo, str, CONVO_SIZE - cur - 1);
}

// ---- outbound commands --------------------------------------------------

static void prv_send_u8(uint32_t key) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) return;
  dict_write_uint8(out, key, 1);
  app_message_outbox_send();
}

static void prv_send_str(uint32_t key, const char *val) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) return;
  dict_write_cstring(out, key, val);
  app_message_outbox_send();
}

// ---- dictation ----------------------------------------------------------

#if defined(PBL_MICROPHONE)
static void prv_dictation_handler(DictationSession *session, DictationSessionStatus status,
                                  char *transcription, void *context) {
  if (status != DictationSessionStatusSuccess) {
    prv_convo_append("\n\n[didn't catch that — press mic to retry]");
    prv_refresh();
    return;
  }
  prv_convo_append(strlen(s_convo) == 0 ? "You: " : "\n\nYou: ");
  prv_convo_append(transcription);
  prv_convo_append("\n\nAI:");
  s_waiting = true;
  prv_refresh();
  prv_send_str(MESSAGE_KEY_TRANSCRIPT, transcription);
}
#endif

// ---- conversation menu --------------------------------------------------

static void prv_parse_list(const char *s) {
  s_conv_count = 0;
  s_active_index = -1;
  size_t n = strlen(s);
  if (n >= LIST_BUF) n = LIST_BUF - 1;
  memcpy(s_list_buf, s, n);
  s_list_buf[n] = '\0';

  char *p = s_list_buf;
  while (*p && s_conv_count < MAX_CONVS) {
    char *line = p;
    char *nl = line;
    while (*nl && *nl != '\n') nl++;
    if (*nl == '\n') { *nl = '\0'; p = nl + 1; } else { p = nl; }

    bool active = false;
    if (line[0] == '*') { active = true; line++; }
    char *tab = line;
    while (*tab && *tab != '\t') tab++;
    if (*tab == '\t') {
      *tab = '\0';
      strncpy(s_conv_ids[s_conv_count], line, ID_LEN - 1);
      s_conv_ids[s_conv_count][ID_LEN - 1] = '\0';
      strncpy(s_conv_titles[s_conv_count], tab + 1, TITLE_LEN - 1);
      s_conv_titles[s_conv_count][TITLE_LEN - 1] = '\0';
      if (active) s_active_index = s_conv_count;
      s_conv_count++;
    }
  }
  if (s_menu_layer) {
    menu_layer_reload_data(s_menu_layer);
  }
}

static uint16_t prv_menu_num_rows(MenuLayer *menu, uint16_t section, void *ctx) {
  return s_conv_count + 1;  // row 0 is "New Chat"
}

static void prv_menu_draw_row(GContext *gctx, const Layer *cell, MenuIndex *idx, void *ctx) {
  if (idx->row == 0) {
    menu_cell_basic_draw(gctx, cell, "+ New Chat", NULL, NULL);
  } else {
    int i = idx->row - 1;
    menu_cell_basic_draw(gctx, cell, s_conv_titles[i],
                         (i == s_active_index) ? "current" : NULL, NULL);
  }
}

static void prv_menu_select(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  if (idx->row == 0) {
    prv_send_u8(MESSAGE_KEY_NEW_CHAT);
  } else {
    prv_send_str(MESSAGE_KEY_SWITCH_CHAT, s_conv_ids[idx->row - 1]);
  }
  window_stack_pop(true);  // back to chat; phone streams the new display
}

static void prv_menu_select_long(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  if (idx->row >= 1) {
    prv_send_str(MESSAGE_KEY_DELETE_CHAT, s_conv_ids[idx->row - 1]);
    vibes_short_pulse();  // phone will push an updated list
  }
}

static void prv_menu_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_rows = prv_menu_num_rows,
    .draw_row = prv_menu_draw_row,
    .select_click = prv_menu_select,
    .select_long_click = prv_menu_select_long,
  });
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_menu_layer));
}

static void prv_menu_window_unload(Window *window) {
  menu_layer_destroy(s_menu_layer);
  s_menu_layer = NULL;
}

static void prv_open_menu(void) {
  if (!s_menu_window) {
    s_menu_window = window_create();
    window_set_window_handlers(s_menu_window, (WindowHandlers) {
      .load = prv_menu_window_load,
      .unload = prv_menu_window_unload,
    });
  }
  window_stack_push(s_menu_window, true);
}

// ---- chat clicks --------------------------------------------------------

static void prv_select_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_waiting) return;
#if defined(PBL_MICROPHONE)
  dictation_session_start(s_dictation);
#else
  prv_convo_append("\n[no microphone on this watch]");
  prv_refresh();
#endif
}

static void prv_select_long_handler(ClickRecognizerRef recognizer, void *context) {
  prv_open_menu();
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

// ---- inbound messages ---------------------------------------------------

static void prv_inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *list = dict_find(iter, MESSAGE_KEY_CONV_LIST);
  if (list) {
    prv_parse_list(list->value->cstring);
  }
  if (dict_find(iter, MESSAGE_KEY_DISPLAY_RESET)) {
    s_convo[0] = '\0';
    s_waiting = false;
    s_display_stream = true;
    prv_refresh();
  }
  Tuple *chunk = dict_find(iter, MESSAGE_KEY_CHUNK);
  if (chunk) {
    prv_convo_append(chunk->value->cstring);
    if (!s_display_stream) prv_light_hold(LIGHT_READ_MS);
    prv_refresh();
  }
  Tuple *status = dict_find(iter, MESSAGE_KEY_STATUS);
  if (status) {
    prv_convo_append(" ");
    prv_convo_append(status->value->cstring);
    prv_refresh();
  }
  if (dict_find(iter, MESSAGE_KEY_FINAL)) {
    if (s_display_stream) {
      s_display_stream = false;  // a switched/launch display finished loading
    } else {
      s_waiting = false;
      vibes_short_pulse();
      prv_light_hold(LIGHT_READ_MS);
    }
    prv_refresh();
  }
}

static void prv_inbox_dropped(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "inbox dropped: %d", (int)reason);
}

static void prv_outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "outbox failed: %d", (int)reason);
}

// ---- chat window --------------------------------------------------------

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
  app_message_open(2048, 256);

#if defined(PBL_MICROPHONE)
  s_dictation = dictation_session_create(512, prv_dictation_handler, NULL);
  // Skip the confirm/edit screen so the transcript sends as soon as STT returns.
  dictation_session_enable_confirmation(s_dictation, false);
  dictation_session_enable_error_dialogs(s_dictation, false);
#endif

  window_stack_push(s_window, true);
  prv_send_u8(MESSAGE_KEY_SYNC);  // ask the phone for current state (best-effort)
}

static void prv_deinit(void) {
#if defined(PBL_MICROPHONE)
  if (s_dictation) {
    dictation_session_destroy(s_dictation);
  }
#endif
  if (s_menu_window) {
    window_destroy(s_menu_window);
  }
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
