#include <pebble.h>

#define COMMAND_PREVIOUS 1
#define COMMAND_PLAY_PAUSE 2
#define COMMAND_NEXT 3

#define TITLE_BUFFER_SIZE 64
#define ARTIST_BUFFER_SIZE 56
#define ALBUM_BUFFER_SIZE 56
#define TIME_BUFFER_SIZE 32

static Window *s_main_window;

static TextLayer *s_title_layer;
static TextLayer *s_artist_layer;
static TextLayer *s_album_layer;
static TextLayer *s_time_layer;

static Layer *s_sidebar_layer;
static Layer *s_progress_layer;

static AppTimer *s_position_timer;

static char s_title[TITLE_BUFFER_SIZE] = "Music Remote";
static char s_artist[ARTIST_BUFFER_SIZE] = "Connecting...";
static char s_album[ALBUM_BUFFER_SIZE] = "";
static char s_time_text[TIME_BUFFER_SIZE] = "00:00 / 00:00";

static uint32_t s_position_seconds;
static uint32_t s_duration_seconds;

static bool s_is_playing;

static void update_time_text(void);
static void schedule_position_timer(void);

static void copy_tuple_string(
    Tuple *tuple,
    char *destination,
    size_t destination_size
) {
  if (!tuple || destination_size == 0) {
    return;
  }

  snprintf(
      destination,
      destination_size,
      "%s",
      tuple->value->cstring
  );
}

static void update_time_text(void) {
  uint32_t position_minutes = s_position_seconds / 60;
  uint32_t position_seconds = s_position_seconds % 60;
  uint32_t duration_minutes = s_duration_seconds / 60;
  uint32_t duration_seconds = s_duration_seconds % 60;

  if (position_minutes > 999) {
    position_minutes = 999;
    position_seconds = 59;
  }

  if (duration_minutes > 999) {
    duration_minutes = 999;
    duration_seconds = 59;
  }

  snprintf(
      s_time_text,
      sizeof(s_time_text),
      "%02lu:%02lu / %02lu:%02lu",
      (unsigned long)position_minutes,
      (unsigned long)position_seconds,
      (unsigned long)duration_minutes,
      (unsigned long)duration_seconds
  );

  if (s_time_layer) {
    text_layer_set_text(s_time_layer, s_time_text);
  }

  if (s_progress_layer) {
    layer_mark_dirty(s_progress_layer);
  }
}

static void draw_triangle(
    GContext *ctx,
    GPoint first,
    GPoint second,
    GPoint third
) {
  GPoint points[] = {
    first,
    second,
    third
  };

  GPathInfo path_info = {
    .num_points = 3,
    .points = points
  };

  GPath *path = gpath_create(&path_info);

  if (path) {
    gpath_draw_filled(ctx, path);
    gpath_destroy(path);
  }
}

static void sidebar_update_proc(
    Layer *layer,
    GContext *ctx
) {
  GRect bounds = layer_get_bounds(layer);

  int16_t width = bounds.size.w;
  int16_t height = bounds.size.h;
  int16_t center_x = width / 2;

  int16_t previous_y = height / 6;
  int16_t play_pause_y = height / 2;
  int16_t next_y = height - (height / 6);

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_context_set_stroke_color(ctx, GColorWhite);

  graphics_fill_rect(
      ctx,
      GRect(center_x - 8, previous_y - 7, 2, 14),
      0,
      GCornerNone
  );

  draw_triangle(
      ctx,
      GPoint(center_x + 7, previous_y - 8),
      GPoint(center_x + 7, previous_y + 8),
      GPoint(center_x - 5, previous_y)
  );

  if (s_is_playing) {
    graphics_fill_rect(
        ctx,
        GRect(center_x - 6, play_pause_y - 8, 4, 16),
        0,
        GCornerNone
    );

    graphics_fill_rect(
        ctx,
        GRect(center_x + 2, play_pause_y - 8, 4, 16),
        0,
        GCornerNone
    );
  } else {
    draw_triangle(
        ctx,
        GPoint(center_x - 5, play_pause_y - 9),
        GPoint(center_x - 5, play_pause_y + 9),
        GPoint(center_x + 8, play_pause_y)
    );
  }

  graphics_fill_rect(
      ctx,
      GRect(center_x + 6, next_y - 7, 2, 14),
      0,
      GCornerNone
  );

  draw_triangle(
      ctx,
      GPoint(center_x - 7, next_y - 8),
      GPoint(center_x - 7, next_y + 8),
      GPoint(center_x + 5, next_y)
  );
}

static void progress_update_proc(
    Layer *layer,
    GContext *ctx
) {
  GRect bounds = layer_get_bounds(layer);

  int16_t horizontal_margin = 2;
  int16_t bar_height = 8;
  int16_t bar_y = (bounds.size.h - bar_height) / 2;
  int16_t bar_width =
      bounds.size.w - (horizontal_margin * 2);

  GRect outline = GRect(
      horizontal_margin,
      bar_y,
      bar_width,
      bar_height
  );

  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_draw_rect(ctx, outline);

  if (
    s_duration_seconds == 0 ||
    s_position_seconds == 0
  ) {
    return;
  }

  uint32_t clamped_position = s_position_seconds;

  if (clamped_position > s_duration_seconds) {
    clamped_position = s_duration_seconds;
  }

  int16_t inner_width = bar_width - 2;

  int16_t filled_width = (int16_t)(
      ((uint64_t)inner_width * clamped_position) /
      s_duration_seconds
  );

  if (filled_width < 1 && clamped_position > 0) {
    filled_width = 1;
  }

  graphics_context_set_fill_color(ctx, GColorBlack);

  graphics_fill_rect(
      ctx,
      GRect(
          horizontal_margin + 1,
          bar_y + 1,
          filled_width,
          bar_height - 2
      ),
      0,
      GCornerNone
  );
}

static void position_timer_callback(void *context) {
  s_position_timer = NULL;

  if (!s_is_playing) {
    return;
  }

  if (
    s_duration_seconds == 0 ||
    s_position_seconds < s_duration_seconds
  ) {
    s_position_seconds += 1;
  }

  if (
    s_duration_seconds > 0 &&
    s_position_seconds > s_duration_seconds
  ) {
    s_position_seconds = s_duration_seconds;
  }

  update_time_text();
  schedule_position_timer();
}

static void schedule_position_timer(void) {
  if (!s_is_playing || s_position_timer != NULL) {
    return;
  }

  s_position_timer = app_timer_register(
      1000,
      position_timer_callback,
      NULL
  );
}

static void update_position_timer(void) {
  if (s_is_playing) {
    schedule_position_timer();
    return;
  }

  if (s_position_timer) {
    app_timer_cancel(s_position_timer);
    s_position_timer = NULL;
  }
}

static void send_command(int command) {
  DictionaryIterator *iterator = NULL;

  AppMessageResult result =
      app_message_outbox_begin(&iterator);

  if (result != APP_MSG_OK || !iterator) {
    APP_LOG(
        APP_LOG_LEVEL_ERROR,
        "Unable to begin AppMessage: %d",
        result
    );
    return;
  }

  dict_write_int32(
      iterator,
      MESSAGE_KEY_COMMAND,
      command
  );

  result = app_message_outbox_send();

  if (result != APP_MSG_OK) {
    APP_LOG(
        APP_LOG_LEVEL_ERROR,
        "Unable to send command: %d",
        result
    );
  }
}

static void up_click_handler(
    ClickRecognizerRef recognizer,
    void *context
) {
  send_command(COMMAND_PREVIOUS);
}

static void select_click_handler(
    ClickRecognizerRef recognizer,
    void *context
) {
  send_command(COMMAND_PLAY_PAUSE);
}

static void down_click_handler(
    ClickRecognizerRef recognizer,
    void *context
) {
  send_command(COMMAND_NEXT);
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(
      BUTTON_ID_UP,
      up_click_handler
  );

  window_single_click_subscribe(
      BUTTON_ID_SELECT,
      select_click_handler
  );

  window_single_click_subscribe(
      BUTTON_ID_DOWN,
      down_click_handler
  );
}

static void inbox_received_handler(
    DictionaryIterator *iterator,
    void *context
) {
  Tuple *title_tuple = dict_find(
      iterator,
      MESSAGE_KEY_TITLE
  );

  Tuple *artist_tuple = dict_find(
      iterator,
      MESSAGE_KEY_ARTIST
  );

  Tuple *album_tuple = dict_find(
      iterator,
      MESSAGE_KEY_ALBUM
  );

  Tuple *position_tuple = dict_find(
      iterator,
      MESSAGE_KEY_POSITION_SECONDS
  );

  Tuple *duration_tuple = dict_find(
      iterator,
      MESSAGE_KEY_DURATION_SECONDS
  );

  Tuple *playing_tuple = dict_find(
      iterator,
      MESSAGE_KEY_IS_PLAYING
  );

  if (title_tuple) {
    copy_tuple_string(
        title_tuple,
        s_title,
        sizeof(s_title)
    );

    text_layer_set_text(s_title_layer, s_title);
  }

  if (artist_tuple) {
    copy_tuple_string(
        artist_tuple,
        s_artist,
        sizeof(s_artist)
    );

    text_layer_set_text(s_artist_layer, s_artist);
  }

  if (album_tuple) {
    copy_tuple_string(
        album_tuple,
        s_album,
        sizeof(s_album)
    );

    text_layer_set_text(s_album_layer, s_album);
  }

  if (position_tuple) {
    s_position_seconds =
        position_tuple->value->uint32;
  }

  if (duration_tuple) {
    s_duration_seconds =
        duration_tuple->value->uint32;
  }

  if (playing_tuple) {
    s_is_playing =
        playing_tuple->value->int32 != 0;

    update_position_timer();

    if (s_sidebar_layer) {
      layer_mark_dirty(s_sidebar_layer);
    }
  }

  if (
    position_tuple ||
    duration_tuple
  ) {
    if (
      s_duration_seconds > 0 &&
      s_position_seconds > s_duration_seconds
    ) {
      s_position_seconds = s_duration_seconds;
    }

    update_time_text();
  }
}

static void inbox_dropped_handler(
    AppMessageResult reason,
    void *context
) {
  APP_LOG(
      APP_LOG_LEVEL_ERROR,
      "AppMessage inbox dropped: %d",
      reason
  );
}

static void outbox_sent_handler(
    DictionaryIterator *iterator,
    void *context
) {
  APP_LOG(
      APP_LOG_LEVEL_DEBUG,
      "Command sent"
  );
}

static void outbox_failed_handler(
    DictionaryIterator *iterator,
    AppMessageResult reason,
    void *context
) {
  APP_LOG(
      APP_LOG_LEVEL_ERROR,
      "Command send failed: %d",
      reason
  );
}

static TextLayer *create_text_layer(
    GRect frame,
    GFont font,
    GTextAlignment alignment
) {
  TextLayer *layer = text_layer_create(frame);

  text_layer_set_background_color(
      layer,
      GColorClear
  );

  text_layer_set_text_color(
      layer,
      GColorBlack
  );

  text_layer_set_font(layer, font);
  text_layer_set_text_alignment(layer, alignment);

  text_layer_set_overflow_mode(
      layer,
      GTextOverflowModeTrailingEllipsis
  );

  return layer;
}

static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  int16_t screen_width = bounds.size.w;
  int16_t screen_height = bounds.size.h;

#if defined(PBL_ROUND)
  int16_t sidebar_width = 42;
  int16_t content_left = 14;
  int16_t content_right_margin = 4;
  int16_t title_y = 18;
  int16_t progress_y = screen_height - 28;
#else
  int16_t sidebar_width = 36;
  int16_t content_left = 5;
  int16_t content_right_margin = 5;
  int16_t title_y = 4;
  int16_t progress_y = screen_height - 18;
#endif

  int16_t sidebar_x = screen_width - sidebar_width;

  int16_t content_width =
      sidebar_x -
      content_left -
      content_right_margin;

  s_title_layer = create_text_layer(
      GRect(
          content_left,
          title_y,
          content_width,
          28
      ),
      fonts_get_system_font(
          FONT_KEY_GOTHIC_24_BOLD
      ),
      GTextAlignmentCenter
  );

  s_artist_layer = create_text_layer(
      GRect(
          content_left,
          title_y + 29,
          content_width,
          22
      ),
      fonts_get_system_font(
          FONT_KEY_GOTHIC_18_BOLD
      ),
      GTextAlignmentCenter
  );

  s_album_layer = create_text_layer(
      GRect(
          content_left,
          title_y + 51,
          content_width,
          22
      ),
      fonts_get_system_font(
          FONT_KEY_GOTHIC_18
      ),
      GTextAlignmentCenter
  );

  s_time_layer = create_text_layer(
      GRect(
          content_left,
          progress_y - 25,
          content_width,
          24
      ),
      fonts_get_system_font(
          FONT_KEY_GOTHIC_18_BOLD
      ),
      GTextAlignmentCenter
  );

  s_progress_layer = layer_create(
      GRect(
          content_left,
          progress_y,
          content_width,
          16
      )
  );

  layer_set_update_proc(
      s_progress_layer,
      progress_update_proc
  );

  s_sidebar_layer = layer_create(
      GRect(
          sidebar_x,
          0,
          sidebar_width,
          screen_height
      )
  );

  layer_set_update_proc(
      s_sidebar_layer,
      sidebar_update_proc
  );

  text_layer_set_text(s_title_layer, s_title);
  text_layer_set_text(s_artist_layer, s_artist);
  text_layer_set_text(s_album_layer, s_album);
  text_layer_set_text(s_time_layer, s_time_text);

  layer_add_child(
      window_layer,
      text_layer_get_layer(s_title_layer)
  );

  layer_add_child(
      window_layer,
      text_layer_get_layer(s_artist_layer)
  );

  layer_add_child(
      window_layer,
      text_layer_get_layer(s_album_layer)
  );

  layer_add_child(
      window_layer,
      text_layer_get_layer(s_time_layer)
  );

  layer_add_child(
      window_layer,
      s_progress_layer
  );

  layer_add_child(
      window_layer,
      s_sidebar_layer
  );
}

static void main_window_unload(Window *window) {
  if (s_position_timer) {
    app_timer_cancel(s_position_timer);
    s_position_timer = NULL;
  }

  text_layer_destroy(s_title_layer);
  text_layer_destroy(s_artist_layer);
  text_layer_destroy(s_album_layer);
  text_layer_destroy(s_time_layer);

  layer_destroy(s_progress_layer);
  layer_destroy(s_sidebar_layer);

  s_title_layer = NULL;
  s_artist_layer = NULL;
  s_album_layer = NULL;
  s_time_layer = NULL;
  s_progress_layer = NULL;
  s_sidebar_layer = NULL;
}

static void init(void) {
  s_main_window = window_create();

  window_set_background_color(
      s_main_window,
      GColorWhite
  );

  window_set_click_config_provider(
      s_main_window,
      click_config_provider
  );

  window_set_window_handlers(
      s_main_window,
      (WindowHandlers) {
        .load = main_window_load,
        .unload = main_window_unload
      }
  );

  app_message_register_inbox_received(
      inbox_received_handler
  );

  app_message_register_inbox_dropped(
      inbox_dropped_handler
  );

  app_message_register_outbox_sent(
      outbox_sent_handler
  );

  app_message_register_outbox_failed(
      outbox_failed_handler
  );

  AppMessageResult result = app_message_open(
      256,
      64
  );

  if (result != APP_MSG_OK) {
    APP_LOG(
        APP_LOG_LEVEL_ERROR,
        "Unable to open AppMessage: %d",
        result
    );
  }

  window_stack_push(s_main_window, true);
}

static void deinit(void) {
  if (s_position_timer) {
    app_timer_cancel(s_position_timer);
    s_position_timer = NULL;
  }

  app_message_deregister_callbacks();

  window_destroy(s_main_window);
  s_main_window = NULL;
}

int main(void) {
  init();
  app_event_loop();
  deinit();

  return 0;
}
