#include <pebble.h>

#define CONVO_SIZE 4096
#define TEXT_PADDING 4
#define EMPTY_PROMPT "How can\nI help you?"

#define MAX_CONVS 20
#define ID_LEN 24
#define TITLE_LEN 40
#define LIST_BUF 2048

#define LIGHT_READ_MS 10000

typedef enum { SCROLL_NONE, SCROLL_ANCHOR, SCROLL_BOTTOM } ScrollMode;

static Window *s_window;
static ScrollLayer *s_scroll_layer;
static TextLayer *s_text_layer;
static ActionBarLayer *s_action_bar;
static GBitmap *s_mic_icon;
static GFont s_font;

// Empty-state: animated pulse rings + prompt text.
static Layer *s_spark_layer;
static TextLayer *s_prompt_layer;
static AppTimer *s_pulse_timer;
static int s_pulse_phase;

static Window *s_menu_window;
static MenuLayer *s_menu_layer;

#if defined(PBL_MICROPHONE)
static DictationSession *s_dictation;
#endif

static char s_convo[CONVO_SIZE];
static char s_display[CONVO_SIZE + 128];
static bool s_waiting;
static bool s_display_stream;
static ScrollMode s_scroll_mode;
static int s_anchor_index;       // byte offset of the latest "You:" turn

static int s_anim_frame;
static AppTimer *s_anim_timer;

static char s_transient[64];
static AppTimer *s_transient_timer;

static bool s_no_key;            // phone reports the API key is missing
static bool s_connected = true;  // phone Bluetooth connection state

static char s_model_name[40];    // short model name for the menu

// Conversation list mirror for the menu.
static char s_conv_ids[MAX_CONVS][ID_LEN];
static char s_conv_titles[MAX_CONVS][TITLE_LEN];
static int s_conv_count;
static int s_active_index;
static char s_list_buf[LIST_BUF];

static int s_pending_delete = -1;
static AppTimer *s_del_timer;

// ---- backlight ----------------------------------------------------------

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

// ---- empty-state spark --------------------------------------------------

#define PULSE_RINGS 2
#define PULSE_MAX_R 26

static void prv_spark_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  GPoint c = GPoint(b.size.w / 2, b.size.h / 2);

  for (int j = 0; j < PULSE_RINGS; j++) {
    int ph = (s_pulse_phase + j * (100 / PULSE_RINGS)) % 100;
    int r = 5 + ph * (PULSE_MAX_R - 5) / 100;
#ifdef PBL_COLOR
    uint8_t cr = (uint8_t)(0 + 255 * ph / 100);
    uint8_t cg = (uint8_t)(170 + (255 - 170) * ph / 100);
    graphics_context_set_stroke_color(ctx, GColorFromRGB(cr, cg, 255));
#else
    graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
    graphics_draw_circle(ctx, c, r);
    if (r > 6) graphics_draw_circle(ctx, c, r - 1);
  }

  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorVividCerulean, GColorBlack));
  graphics_fill_circle(ctx, c, 5);
}

static void prv_pulse_tick(void *context) {
  s_pulse_phase = (s_pulse_phase + 3) % 100;
  if (s_spark_layer) layer_mark_dirty(s_spark_layer);
  s_pulse_timer = app_timer_register(60, prv_pulse_tick, NULL);
}

// ---- chat view rendering ------------------------------------------------

static const char *const DOTS[] = { "", ".", "..", "..." };

static void prv_refresh(void) {
  bool empty = (strlen(s_convo) == 0 && !s_waiting);
  layer_set_hidden(s_spark_layer, !empty);
  layer_set_hidden(text_layer_get_layer(s_prompt_layer), !empty);
  layer_set_hidden(scroll_layer_get_layer(s_scroll_layer), empty);

  if (empty) {
    if (!s_pulse_timer) s_pulse_timer = app_timer_register(60, prv_pulse_tick, NULL);
    const char *msg = s_transient[0] ? s_transient
                    : s_no_key ? "Set your API key\nin the phone app"
                    : EMPTY_PROMPT;
    text_layer_set_text(s_prompt_layer, msg);
    return;
  }
  if (s_pulse_timer) { app_timer_cancel(s_pulse_timer); s_pulse_timer = NULL; }

  snprintf(s_display, sizeof(s_display), "%s%s%s%s",
           s_convo,
           s_waiting ? DOTS[s_anim_frame] : "",
           s_transient[0] ? "\n\n" : "",
           s_transient);
  text_layer_set_text(s_text_layer, s_display);

  GRect rb = layer_get_bounds(window_get_root_layer(s_window));
  int16_t screen_h = rb.size.h;
  int16_t text_w = rb.size.w - ACTION_BAR_WIDTH - TEXT_PADDING * 2;
  text_layer_set_size(s_text_layer, GSize(text_w, CONVO_SIZE));
  GSize content = text_layer_get_content_size(s_text_layer);
  content.h += 12;
  text_layer_set_size(s_text_layer, GSize(text_w, content.h));
  scroll_layer_set_content_size(s_scroll_layer, GSize(text_w + TEXT_PADDING * 2, content.h));

  int16_t min_y = screen_h - content.h;
  if (min_y > 0) min_y = 0;
  if (s_scroll_mode == SCROLL_BOTTOM) {
    scroll_layer_set_content_offset(s_scroll_layer, GPoint(0, min_y), true);
  } else if (s_scroll_mode == SCROLL_ANCHOR) {
    // Put the latest "You:" turn at the top so the reply reads from its start.
    char saved = s_convo[s_anchor_index];
    s_convo[s_anchor_index] = '\0';
    int16_t ay = graphics_text_layout_get_content_size(
        s_convo, s_font, GRect(0, 0, text_w, CONVO_SIZE),
        GTextOverflowModeWordWrap, GTextAlignmentLeft).h;
    s_convo[s_anchor_index] = saved;
    int16_t off = -ay;
    if (off < min_y) off = min_y;
    if (off > 0) off = 0;
    scroll_layer_set_content_offset(s_scroll_layer, GPoint(0, off), true);
  }
  s_scroll_mode = SCROLL_NONE;
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

// ---- animation + transient ----------------------------------------------

static void prv_anim_tick(void *context) {
  if (s_waiting) {
    s_anim_frame = (s_anim_frame + 1) % 4;
    s_anim_timer = app_timer_register(350, prv_anim_tick, NULL);
    prv_refresh();
  } else {
    s_anim_timer = NULL;
  }
}

static void prv_start_anim(void) {
  s_anim_frame = 0;
  if (!s_anim_timer) {
    s_anim_timer = app_timer_register(350, prv_anim_tick, NULL);
  }
}

static void prv_clear_transient(void *context) {
  s_transient_timer = NULL;
  s_transient[0] = '\0';
  prv_refresh();
}

static void prv_flash_transient(const char *msg) {
  strncpy(s_transient, msg, sizeof(s_transient) - 1);
  s_transient[sizeof(s_transient) - 1] = '\0';
  if (s_transient_timer) {
    app_timer_reschedule(s_transient_timer, 2500);
  } else {
    s_transient_timer = app_timer_register(2500, prv_clear_transient, NULL);
  }
  prv_refresh();
}

// ---- outbound -----------------------------------------------------------

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
    prv_flash_transient("Didn't catch that — try again");
    return;
  }
  if (strlen(s_convo) > 0) prv_convo_append("\n\n");
  s_anchor_index = strlen(s_convo);
  prv_convo_append("You: ");
  prv_convo_append(transcription);
  prv_convo_append("\n\n");
  s_waiting = true;
  s_scroll_mode = SCROLL_ANCHOR;
  prv_start_anim();
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
  s_pending_delete = -1;
  if (s_menu_layer) menu_layer_reload_data(s_menu_layer);
}

static void prv_delete_disarm(void *context) {
  s_del_timer = NULL;
  s_pending_delete = -1;
  if (s_menu_layer) menu_layer_reload_data(s_menu_layer);
}

static int prv_model_row(void) { return s_conv_count + 1; }

static uint16_t prv_menu_num_rows(MenuLayer *menu, uint16_t section, void *ctx) {
  return s_conv_count + 2;  // New Chat + conversations + Model
}

static void prv_menu_draw_row(GContext *gctx, const Layer *cell, MenuIndex *idx, void *ctx) {
  if (idx->row == 0) {
    menu_cell_basic_draw(gctx, cell, "+ New Chat", NULL, NULL);
  } else if (idx->row == prv_model_row()) {
    menu_cell_basic_draw(gctx, cell, "Model", s_model_name[0] ? s_model_name : "default", NULL);
  } else {
    int i = idx->row - 1;
    const char *subtitle = (idx->row == s_pending_delete) ? "hold again to delete"
                         : (i == s_active_index) ? "current" : NULL;
    menu_cell_basic_draw(gctx, cell, s_conv_titles[i], subtitle, NULL);
  }
}

static void prv_menu_select(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  s_pending_delete = -1;
  if (idx->row == 0) {
    prv_send_u8(MESSAGE_KEY_NEW_CHAT);
    window_stack_pop(true);
  } else if (idx->row == prv_model_row()) {
    prv_send_u8(MESSAGE_KEY_NEXT_MODEL);  // cycles; phone pushes back MODEL_NAME
  } else {
    prv_send_str(MESSAGE_KEY_SWITCH_CHAT, s_conv_ids[idx->row - 1]);
    window_stack_pop(true);
  }
}

static void prv_menu_select_long(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  if (idx->row < 1 || idx->row >= prv_model_row()) return;  // conversations only
  if (s_pending_delete == idx->row) {
    prv_send_str(MESSAGE_KEY_DELETE_CHAT, s_conv_ids[idx->row - 1]);
    s_pending_delete = -1;
    if (s_del_timer) { app_timer_cancel(s_del_timer); s_del_timer = NULL; }
    vibes_double_pulse();
  } else {
    s_pending_delete = idx->row;
    vibes_short_pulse();
    if (s_del_timer) {
      app_timer_reschedule(s_del_timer, 3000);
    } else {
      s_del_timer = app_timer_register(3000, prv_delete_disarm, NULL);
    }
    menu_layer_reload_data(menu);
  }
}

static void prv_menu_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_menu_layer = menu_layer_create(layer_get_bounds(root));
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
  if (!s_connected) { prv_flash_transient("Connect your phone"); return; }
  if (s_no_key) { prv_flash_transient("Set your API key in the phone app"); return; }
#if defined(PBL_MICROPHONE)
  dictation_session_start(s_dictation);
#else
  prv_flash_transient("No microphone on this watch");
#endif
}

static void prv_select_long_handler(ClickRecognizerRef recognizer, void *context) {
  prv_open_menu();
}

static void prv_back_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_waiting) {
    s_waiting = false;
    prv_send_u8(MESSAGE_KEY_CANCEL);
    prv_flash_transient("Canceled");
  } else {
    window_stack_pop(true);  // exit the app
  }
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
  window_single_click_subscribe(BUTTON_ID_BACK, prv_back_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 100, prv_up_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, prv_down_handler);
}

// ---- inbound ------------------------------------------------------------

static void prv_inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *list = dict_find(iter, MESSAGE_KEY_CONV_LIST);
  if (list) prv_parse_list(list->value->cstring);

  Tuple *model = dict_find(iter, MESSAGE_KEY_MODEL_NAME);
  if (model) {
    strncpy(s_model_name, model->value->cstring, sizeof(s_model_name) - 1);
    s_model_name[sizeof(s_model_name) - 1] = '\0';
    if (s_menu_layer) menu_layer_reload_data(s_menu_layer);
  }

  Tuple *nokey = dict_find(iter, MESSAGE_KEY_NO_KEY);
  if (nokey) {
    s_no_key = (nokey->value->uint8 != 0);
    prv_refresh();
  }

  if (dict_find(iter, MESSAGE_KEY_DISPLAY_RESET)) {
    s_convo[0] = '\0';
    s_waiting = false;
    s_display_stream = true;
    s_scroll_mode = SCROLL_BOTTOM;
    prv_refresh();
  }
  Tuple *chunk = dict_find(iter, MESSAGE_KEY_CHUNK);
  if (chunk) {
    prv_convo_append(chunk->value->cstring);
    if (s_display_stream) {
      s_scroll_mode = SCROLL_BOTTOM;
    } else {
      s_waiting = false;  // reply text arriving; stop the thinking dots
      prv_light_hold(LIGHT_READ_MS);
    }
    prv_refresh();
  }
  Tuple *status = dict_find(iter, MESSAGE_KEY_STATUS);
  if (status) {
    s_waiting = false;
    prv_flash_transient(status->value->cstring);
  }
  if (dict_find(iter, MESSAGE_KEY_FINAL)) {
    if (s_display_stream) {
      s_display_stream = false;
      s_scroll_mode = SCROLL_BOTTOM;
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

// ---- connection ---------------------------------------------------------

static void prv_conn_handler(bool connected) {
  s_connected = connected;
}

// ---- chat window --------------------------------------------------------

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_font = fonts_get_system_font(FONT_KEY_GOTHIC_24);

  GRect scroll_frame = GRect(0, 0, bounds.size.w - ACTION_BAR_WIDTH, bounds.size.h);
  s_scroll_layer = scroll_layer_create(scroll_frame);
  scroll_layer_set_shadow_hidden(s_scroll_layer, false);  // overflow affordance

  s_text_layer = text_layer_create(GRect(TEXT_PADDING, 2,
                                         scroll_frame.size.w - TEXT_PADDING * 2, 2000));
  text_layer_set_font(s_text_layer, s_font);
  text_layer_set_text_color(s_text_layer, GColorBlack);
  text_layer_set_background_color(s_text_layer, GColorClear);
  scroll_layer_add_child(s_scroll_layer, text_layer_get_layer(s_text_layer));
  layer_add_child(root, scroll_layer_get_layer(s_scroll_layer));

  int16_t content_w = bounds.size.w - ACTION_BAR_WIDTH;
  int16_t spark_top = bounds.size.h / 5;
  s_spark_layer = layer_create(GRect(content_w / 2 - 30, spark_top, 60, 60));
  layer_set_update_proc(s_spark_layer, prv_spark_update);
  layer_add_child(root, s_spark_layer);

  s_prompt_layer = text_layer_create(GRect(0, spark_top + 64, content_w, 64));
  text_layer_set_font(s_prompt_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_text_color(s_prompt_layer, GColorBlack);
  text_layer_set_background_color(s_prompt_layer, GColorClear);
  text_layer_set_text_alignment(s_prompt_layer, GTextAlignmentCenter);
  text_layer_set_text(s_prompt_layer, EMPTY_PROMPT);
  layer_add_child(root, text_layer_get_layer(s_prompt_layer));

  s_mic_icon = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_MIC);
  s_action_bar = action_bar_layer_create();
  action_bar_layer_set_background_color(s_action_bar, GColorBlack);
  action_bar_layer_set_icon_animated(s_action_bar, BUTTON_ID_SELECT, s_mic_icon, true);
  action_bar_layer_set_click_config_provider(s_action_bar, prv_click_config);
  action_bar_layer_add_to_window(s_action_bar, window);

  prv_refresh();
}

static void prv_window_unload(Window *window) {
  if (s_pulse_timer) { app_timer_cancel(s_pulse_timer); s_pulse_timer = NULL; }
  action_bar_layer_destroy(s_action_bar);
  gbitmap_destroy(s_mic_icon);
  text_layer_destroy(s_prompt_layer);
  layer_destroy(s_spark_layer);
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

  s_connected = connection_service_peek_pebble_app_connection();
  connection_service_subscribe((ConnectionHandlers) {
    .pebble_app_connection_handler = prv_conn_handler,
  });

#if defined(PBL_MICROPHONE)
  s_dictation = dictation_session_create(512, prv_dictation_handler, NULL);
  dictation_session_enable_confirmation(s_dictation, false);
  dictation_session_enable_error_dialogs(s_dictation, false);
#endif

  window_stack_push(s_window, true);
  prv_send_u8(MESSAGE_KEY_SYNC);
}

static void prv_deinit(void) {
#if defined(PBL_MICROPHONE)
  if (s_dictation) dictation_session_destroy(s_dictation);
#endif
  if (s_menu_window) window_destroy(s_menu_window);
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
