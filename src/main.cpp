// JoshTerminal: a desk ticker for Reddit's stock (RDDT) on an ESP32-2432S028R
// "Cheap Yellow Display". Based on https://github.com/MaWe88/esp32-cyd-ticker
// (GPL-3.0).
#include <FS.h>
using fs::FS; // WiFiManager's WebServer.h needs FS in the global namespace
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>
#include <time.h>
#include <vector>

// ---------------------------------------------------------------- what it
// shows

const char *TICKER = "RDDT"; // Finnhub / Yahoo stock symbol
const char *GREETING = "Happy birthday, Josh";
const char *AP_SSID =
    "RDDT-Ticker"; // setup network, only up until WiFi is configured
const char *AP_PASS = "happybirthday"; // shown on the setup screen
const char *MARKET_TZ =
    "EST5EDT,M3.2.0,M11.1.0"; // "today" means today in New York

enum Range { R_1D, R_5D, R_3M, R_1Y, R_ALL, R_COUNT };
struct RangeInfo {
  const char *tab, *query,
      *phrase;         // phrase after the change, e.g. "+$8.20 past 3 months"
  unsigned long ttlMs; // how long a downloaded chart stays fresh
};
const RangeInfo RANGES[R_COUNT] = {
    {"1D", "range=1d&interval=5m", nullptr,
     5UL * 60 * 1000}, // phrase: "today" / "on Friday"
    {"5D", "range=5d&interval=30m", "past 5 days", 30UL * 60 * 1000},
    {"3M", "range=3mo&interval=1d", "past 3 months", 6UL * 3600 * 1000},
    {"1Y", "range=1y&interval=1d", "past year", 6UL * 3600 * 1000},
    {"All", "range=max&interval=1wk", "since IPO", 6UL * 3600 * 1000},
};
enum Page {
  PAGE_PRICE,
  PAGE_THESIS,
  PAGE_COUNT
}; // swipe left/right between them
const unsigned long IDLE_RETURN_MS =
    60000; // back to the price page and 1D after a minute untouched
const unsigned long FUND_TTL_MS =
    6UL * 3600 * 1000; // P/E etc. only change after earnings

// ---------------------------------------------------------------- design
// tokens

#define COL_UP 0xFF4500 // Reddit's upvote orangered: RDDT is up
#define COL_DOWN                                                               \
  0x5D74E6 // Reddit's downvote periwinkle, deepened so white text reads
#define COL_INK 0x262A3B  // boot and setup
#define OPA_SECONDARY 184 // secondary text: white at 72%
#define OPA_TRACK 90      // range bar track and the dim page dot
const lv_coord_t MARGIN = 16;
const lv_coord_t HEADER_BASE = 36; // text baselines
const lv_coord_t PRICE_BASE = 122;
const lv_coord_t CHANGE_BASE = 154;
const lv_coord_t CHART_TOP = 166, CHART_BOTTOM = 206;
const lv_coord_t TABS_Y = 211, TAB_H = 24;
const lv_coord_t STAT_BASE = 104, STAT_LABEL_BASE = 126; // thesis page
const lv_coord_t RANGE_Y = 158, RANGE_H = 6, RANGE_LABEL_BASE = 184,
                 BELOW_HIGH_BASE = 212;

LV_FONT_DECLARE(font_price_84); // Nimbus Sans Narrow Bold, digits only
LV_FONT_DECLARE(font_bold_36);  // Nimbus Sans Narrow Bold, numbers only
LV_FONT_DECLARE(font_bold_26);  // Nimbus Sans Narrow Bold
LV_FONT_DECLARE(font_bold_17);  // Nimbus Sans Narrow Bold, tab labels only
LV_FONT_DECLARE(font_text_17);  // Nimbus Sans Narrow Regular
LV_IMG_DECLARE(badge_30);
LV_IMG_DECLARE(badge_64);
LV_IMG_DECLARE(arrow_up);

// ---------------------------------------------------------------- hardware

#define XPT2046_IRQ 36 // touch has its own SPI bus on the CYD
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33
#define LED_RED 4 // RGB LED on the back, active low; kept off
#define LED_GREEN 16
#define LED_BLUE 17

static const uint16_t screenWidth = 320;
static const uint16_t screenHeight = 240;

SPIClass touchscreenSPI(VSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);
TFT_eSPI tft = TFT_eSPI();
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf;
static lv_indev_drv_t indev_drv;
lv_disp_t *disp;
WiFiClientSecure client;
Preferences prefs;

// ---------------------------------------------------------------- settings
// (from the setup page)

String finnhub_api_key_config = "";
bool auto_interval_enabled = true;
unsigned long switchInterval = 30000; // ms between price refreshes

// ---------------------------------------------------------------- state

struct Quote {
  float price = 0, change = 0, pct = 0; // change vs the previous close
  time_t tradeTime = 0;                 // last trade, unix seconds
  bool valid = false;                   // at least one successful fetch
};
Quote quote;
unsigned long lastRefresh = 0;

struct Series {
  std::vector<float> closes;
  float base = 0;              // close just before the range starts
  unsigned long fetchedAt = 0; // last attempt, millis
  bool ok = false;
};
Series series[R_COUNT];
Range selected = R_1D;
const int CHART_MAX = 260;
lv_coord_t chartY[CHART_MAX];

struct Fundamentals {
  float fwdPE = 0, revGrowth = 0, mcapM = 0, hi52 = 0,
        lo52 = 0;              // revGrowth: latest quarter y/y, %
  unsigned long fetchedAt = 0; // last attempt, millis
  bool ok = false;
};
Fundamentals fund;

// Touch input. Taps and swipes are only recorded in LVGL events and acted on in
// loop(), so no network call ever runs inside an LVGL event.
Page page = PAGE_PRICE;
int pendingRange = -1, pendingPage = -1;
unsigned long lastTouch = 0, touchPressStart = 0;
bool touchPressed = false, swipedThisPress = false, holdHintShown = false,
     resetInProgress = false;

// Widgets of the current view (null when that view isn't showing)
lv_obj_t *status_label, *price_label, *pct_label, *chg_label, *arrow_img,
    *chart, *chart_note;
lv_obj_t *tab_pill[R_COUNT], *tab_label[R_COUNT];
lv_chart_series_t *chart_ser;
lv_obj_t *stat_val[3], *range_marker, *range_lo, *range_hi, *below_label;
bool tickerShown = false, thesisShown = false;
String lastStatus = "";
uint32_t fieldHex = COL_INK;

// ---------------------------------------------------------------- display +
// touch drivers

void flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area,
              lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  tft.endWrite();
  lv_disp_flush_ready(disp_drv);
}

void touchscreen_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data) {
  if (touchscreen.tirqTouched() && touchscreen.touched()) {
    TS_Point p = touchscreen.getPoint();
    data->point.x =
        constrain(map(p.x, 200, 3800, 0, screenWidth - 1), 0, screenWidth - 1);
    data->point.y = constrain(map(p.y, 200, 4000, 0, screenHeight - 1), 0,
                              screenHeight - 1);
    data->state = LV_INDEV_STATE_PR;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

// Every press reaches the screen: swipes change page, a 10 s hold resets WiFi
// (in loop()).
void screenEvent(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED && !resetInProgress) {
    lastTouch = touchPressStart = millis();
    touchPressed = true;
    swipedThisPress = false;
    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);
    Serial.printf("Touch at %d,%d\n", p.x,
                  p.y); // for checking the touch calibration
  } else if (code == LV_EVENT_GESTURE) {
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_LEFT && page + 1 < PAGE_COUNT)
      pendingPage = page + 1;
    if (dir == LV_DIR_RIGHT && page > 0)
      pendingPage = page - 1;
    swipedThisPress = true;
    touchPressed = false; // a swipe isn't a reset hold
  } else if (code == LV_EVENT_RELEASED) {
    touchPressed = false;
    if (holdHintShown && status_label)
      lv_label_set_text(status_label, lastStatus.c_str());
    holdHintShown = false;
  }
}

void setupLVGL() {
  lv_init();
  lv_log_register_print_cb([](const char *msg) {
    Serial.print(msg);
  }); // LVGL warnings/asserts to serial
  tft.begin();
  tft.setRotation(1); // landscape: ILI9341 driver is 240x320 portrait natively
  tft.fillScreen(TFT_BLACK);

  uint32_t bufSize = screenWidth * 40;
  buf = (lv_color_t *)malloc(bufSize * sizeof(lv_color_t));
  if (!buf)
    return;
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, bufSize);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = flush_cb;
  disp_drv.draw_buf = &draw_buf;
  disp = lv_disp_drv_register(&disp_drv);

  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(scr, lv_color_hex(COL_INK), 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(scr, screenEvent, LV_EVENT_ALL, NULL);
}

// ---------------------------------------------------------------- UI helpers

lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font,
                    lv_opa_t opa = LV_OPA_COVER) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_white(), 0);
  lv_obj_set_style_text_opa(l, opa, 0);
  return l;
}

// LVGL positions a label by the top of its line box; place it by its text
// baseline instead.
lv_coord_t lineTop(const lv_font_t *f, lv_coord_t baseline) {
  return baseline - (f->line_height - f->base_line);
}

void atBaseline(lv_obj_t *l, lv_coord_t x, lv_coord_t baseline) {
  lv_obj_set_pos(l, x, lineTop(lv_obj_get_style_text_font(l, 0), baseline));
}

// Height of a glyph's top above the baseline.
int glyphTop(const lv_font_t *f, uint32_t ch) {
  lv_font_glyph_dsc_t g;
  lv_font_get_glyph_dsc(f, &g, ch, 0);
  return g.ofs_y + g.box_h;
}

lv_obj_t *makeBox(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

lv_obj_t *makeDot(lv_obj_t *parent, lv_coord_t size, lv_opa_t opa) {
  lv_obj_t *d = makeBox(parent);
  lv_obj_set_size(d, size, size);
  lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(d, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(d, opa, 0);
  return d;
}

// Background colour; animated so a flip between up and down is noticeable from
// across the room.
lv_color_t fieldFrom, fieldTo;
void fieldAnim(void *scr, int32_t v) {
  lv_obj_set_style_bg_color((lv_obj_t *)scr,
                            lv_color_mix(fieldTo, fieldFrom, v), 0);
}
void setField(uint32_t hex, bool animate) {
  fieldHex = hex;
  lv_obj_t *scr = lv_scr_act();
  lv_color_t target = lv_color_hex(hex);
  lv_anim_del(scr, fieldAnim);
  lv_color_t now = lv_obj_get_style_bg_color(scr, 0);
  if (now.full == target.full)
    return;
  if (!animate) {
    lv_obj_set_style_bg_color(scr, target, 0);
    return;
  }
  fieldFrom = now;
  fieldTo = target;
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, scr);
  lv_anim_set_exec_cb(&a, fieldAnim);
  lv_anim_set_values(&a, 0, 255);
  lv_anim_set_time(&a, 900);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

void setStatus(const String &s) {
  lastStatus = s;
  if (status_label && !holdHintShown)
    lv_label_set_text(status_label, s.c_str());
}

void clearView() {
  lv_obj_clean(lv_scr_act());
  status_label = price_label = pct_label = chg_label = arrow_img = chart =
      chart_note = nullptr;
  range_marker = range_lo = range_hi = below_label = nullptr;
  tickerShown = thesisShown = false;
}

// Presses must reach the screen (swipes, reset hold); only the tab buttons take
// taps.
void passTouchesToScreen() {
  lv_obj_t *scr = lv_scr_act();
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(scr); i++)
    lv_obj_clear_flag(lv_obj_get_child(scr, i), LV_OBJ_FLAG_CLICKABLE);
}

// Badge, ticker, status line and the page dots: shared by the price and thesis
// pages.
void buildHeader(lv_obj_t *scr, Page current) {
  lv_obj_t *badge = lv_img_create(scr);
  lv_img_set_src(badge, &badge_30);
  lv_obj_set_pos(badge, MARGIN, 13);
  lv_obj_t *sym = makeLabel(scr, &font_bold_26);
  lv_label_set_text(sym, TICKER);
  atBaseline(sym, MARGIN + 30 + 8, HEADER_BASE);

  status_label = makeLabel(scr, &font_text_17, OPA_SECONDARY);
  lv_label_set_text(status_label, lastStatus.c_str());
  lv_obj_align(status_label, LV_ALIGN_TOP_RIGHT, -MARGIN,
               lineTop(&font_text_17, HEADER_BASE));

  for (int i = 0; i < PAGE_COUNT; i++)
    lv_obj_set_pos(makeDot(scr, 6, i == current ? LV_OPA_COVER : OPA_TRACK),
                   screenWidth / 2 - 9 + i * 12, 4);
}

// ---------------------------------------------------------------- message +
// setup views

// Ink screen with the big badge: shown while connecting and before the first
// price.
void showMessageView(const String &status) {
  clearView();
  setField(COL_INK, false);
  lv_obj_t *scr = lv_scr_act();
  lv_obj_t *badge = lv_img_create(scr);
  lv_img_set_src(badge, &badge_64);
  lv_obj_set_pos(badge, MARGIN, MARGIN);

  // Text column pinned to the bottom, so a two-line status pushes the greeting
  // up
  lv_obj_t *col = makeBox(scr);
  lv_obj_set_size(col, screenWidth - 2 * MARGIN, screenHeight - 100);
  lv_obj_align(col, LV_ALIGN_BOTTOM_LEFT, MARGIN, -12);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(col, 2, 0);

  lv_label_set_text(makeLabel(col, &font_bold_26), GREETING);
  status_label = makeLabel(col, &font_text_17, OPA_SECONDARY);
  lv_obj_set_width(status_label, lv_pct(100));
  lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
  setStatus(status);

  passTouchesToScreen();
  lv_refr_now(disp);
}

// First-run instructions while the setup network is up.
void showSetupView(const char *ssid) {
  clearView();
  setField(COL_INK, false);
  lv_obj_t *scr = lv_scr_act();
  lv_obj_t *badge = lv_img_create(scr);
  lv_img_set_src(badge, &badge_64);
  lv_obj_set_pos(badge, MARGIN, MARGIN);

  lv_obj_t *g = makeLabel(scr, &font_bold_26);
  lv_label_set_text(g, GREETING);
  atBaseline(g, MARGIN, 130);

  struct Step {
    const char *num;
    String text;
    lv_opa_t opa;
  };
  Step steps[] = {{"1", String("Join the WiFi network ") + ssid, LV_OPA_COVER},
                  {"", String("Password: ") + AP_PASS, OPA_SECONDARY},
                  {"2", "Open 192.168.4.1 and choose your WiFi", LV_OPA_COVER}};
  lv_coord_t base = 160;
  for (const Step &s : steps) {
    if (*s.num) {
      lv_obj_t *n = makeLabel(scr, &font_text_17);
      lv_label_set_text(n, s.num);
      atBaseline(n, MARGIN, base);
    }
    lv_obj_t *t = makeLabel(scr, &font_text_17, s.opa);
    lv_label_set_text(t, s.text.c_str());
    atBaseline(t, MARGIN + 16, base);
    base += 22;
  }
  passTouchesToScreen();
  lv_refr_now(disp);
}

// ---------------------------------------------------------------- price page

// Faded white area under the chart line (LVGL's lv_example_chart_2).
void chartFillCb(lv_event_t *e) {
  lv_obj_t *obj = lv_event_get_target(e);
  lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
  if (dsc->part != LV_PART_ITEMS || !dsc->p1 || !dsc->p2)
    return;

  lv_draw_mask_line_param_t line_mask;
  lv_draw_mask_line_points_init(&line_mask, dsc->p1->x, dsc->p1->y, dsc->p2->x,
                                dsc->p2->y, LV_DRAW_MASK_LINE_SIDE_BOTTOM);
  int16_t line_id = lv_draw_mask_add(&line_mask, NULL);
  lv_draw_mask_fade_param_t fade_mask;
  lv_draw_mask_fade_init(&fade_mask, &obj->coords, LV_OPA_COVER, obj->coords.y1,
                         LV_OPA_TRANSP, obj->coords.y2);
  int16_t fade_id = lv_draw_mask_add(&fade_mask, NULL);

  lv_draw_rect_dsc_t rect;
  lv_draw_rect_dsc_init(&rect);
  rect.bg_opa = LV_OPA_20;
  rect.bg_color = lv_color_white();
  lv_area_t a;
  a.x1 = dsc->p1->x;
  a.x2 = dsc->p2->x - 1;
  a.y1 = LV_MIN(dsc->p1->y, dsc->p2->y);
  a.y2 = obj->coords.y2;
  lv_draw_rect(dsc->draw_ctx, &rect, &a);

  lv_draw_mask_free_param(&line_mask);
  lv_draw_mask_free_param(&fade_mask);
  lv_draw_mask_remove_id(line_id);
  lv_draw_mask_remove_id(fade_id);
}

void tabClicked(lv_event_t *e) {
  if (!swipedThisPress)
    pendingRange = (int)(intptr_t)lv_event_get_user_data(e);
}

// Selected tab: white pill with text in the background colour. Others: plain
// white text.
void styleTabs() {
  for (int r = 0; r < R_COUNT; r++) {
    bool sel = r == selected;
    lv_obj_set_style_bg_opa(tab_pill[r], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_font(tab_label[r],
                               sel ? &font_bold_17 : &font_text_17, 0);
    lv_obj_set_style_text_color(
        tab_label[r], sel ? lv_color_hex(fieldHex) : lv_color_white(), 0);
    lv_obj_set_style_text_opa(tab_label[r], sel ? LV_OPA_COVER : OPA_SECONDARY,
                              0);
  }
}

// Draws the selected range's chart, scaled to its own low and high.
void renderChart() {
  if (!chart)
    return;
  const Series &s = series[selected];
  if (!s.ok) {
    lv_obj_add_flag(chart, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(chart_note,
                      s.fetchedAt ? "Chart unavailable" : "Loading chart");
    lv_obj_clear_flag(chart_note, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  int n = s.closes.size();
  float lo = s.closes[0], hi = s.closes[0];
  for (float c : s.closes) {
    lo = min(lo, c);
    hi = max(hi, c);
  }
  for (int i = 0; i < n; i++)
    chartY[i] = hi - lo < 0.005f
                    ? 500
                    : (lv_coord_t)(60 + 880 * (s.closes[i] - lo) / (hi - lo));
  lv_chart_set_point_count(chart, n);
  lv_chart_set_ext_y_array(chart, chart_ser, chartY);
  lv_chart_refresh(chart);
  lv_obj_add_flag(chart_note, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(chart, LV_OBJ_FLAG_HIDDEN);
}

void showTickerView() {
  clearView();
  lv_obj_t *scr = lv_scr_act();
  buildHeader(scr, PAGE_PRICE);

  // Small "$" top-aligned with the digits, the way prices are set in print
  int digitTop = glyphTop(&font_price_84, '0');
  lv_obj_t *dollar = makeLabel(scr, &font_bold_26);
  lv_label_set_text(dollar, "$");
  atBaseline(dollar, MARGIN,
             PRICE_BASE - digitTop + glyphTop(&font_bold_26, '$'));
  price_label = makeLabel(scr, &font_price_84);
  lv_label_set_text(price_label, "--.--");
  atBaseline(price_label,
             MARGIN + lv_font_get_glyph_width(&font_bold_26, '$', 0) + 2,
             PRICE_BASE);

  arrow_img = lv_img_create(scr);
  lv_img_set_src(arrow_img, &arrow_up);
  lv_obj_set_pos(arrow_img, screenWidth - MARGIN - arrow_up.header.w,
                 PRICE_BASE - digitTop +
                     (digitTop - (int)arrow_up.header.h) / 2);
  lv_obj_add_flag(arrow_img, LV_OBJ_FLAG_HIDDEN);

  pct_label = makeLabel(scr, &font_bold_26);
  lv_label_set_text(pct_label, "");
  atBaseline(pct_label, MARGIN, CHANGE_BASE);
  chg_label = makeLabel(scr, &font_text_17, OPA_SECONDARY);
  lv_label_set_text(chg_label, "");

  chart = lv_chart_create(scr);
  lv_obj_clear_flag(chart,
                    LV_OBJ_FLAG_SCROLLABLE); // a drag here should be a swipe
  lv_obj_set_size(chart, screenWidth, CHART_BOTTOM - CHART_TOP);
  lv_obj_set_pos(chart, 0, CHART_TOP);
  lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(chart, 0, 0);
  lv_obj_set_style_radius(chart, 0, 0);
  lv_obj_set_style_pad_all(chart, 0, 0);
  lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
  lv_obj_set_style_line_opa(chart, 235, LV_PART_ITEMS);
  lv_obj_set_style_line_rounded(chart, true, LV_PART_ITEMS);
  lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR); // no dots on the points
  lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
  lv_chart_set_div_line_count(chart, 0, 0);
  lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
  chart_ser =
      lv_chart_add_series(chart, lv_color_white(), LV_CHART_AXIS_PRIMARY_Y);
  lv_obj_add_event_cb(chart, chartFillCb, LV_EVENT_DRAW_PART_BEGIN, NULL);

  chart_note = makeLabel(scr, &font_text_17, OPA_SECONDARY);
  atBaseline(chart_note, MARGIN, (CHART_TOP + CHART_BOTTOM) / 2 + 6);

  // Tabs: tall touch cells, each with a pill behind its label. The cells reach
  // up over the chart because this panel's touch reads 15-30 px high near the
  // bottom edge.
  const lv_coord_t hitTop = 176;
  lv_obj_t *row = makeBox(scr);
  lv_obj_set_size(row, screenWidth - 2 * MARGIN, screenHeight - hitTop);
  lv_obj_set_pos(row, MARGIN, hitTop);
  lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_coord_t cellW = (screenWidth - 2 * MARGIN) / R_COUNT;
  for (int r = 0; r < R_COUNT; r++) {
    lv_obj_t *cell = makeBox(row);
    lv_obj_set_size(cell, cellW, lv_pct(100));
    lv_obj_set_pos(cell, r * cellW, 0);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(cell,
                    LV_OBJ_FLAG_EVENT_BUBBLE); // the screen still sees presses
    lv_obj_add_event_cb(cell, tabClicked, LV_EVENT_CLICKED,
                        (void *)(intptr_t)r);

    tab_pill[r] = makeBox(cell);
    lv_obj_set_size(tab_pill[r], cellW - 8, TAB_H);
    lv_obj_align(tab_pill[r], LV_ALIGN_TOP_MID, 0, TABS_Y - hitTop);
    lv_obj_set_style_radius(tab_pill[r], TAB_H / 2, 0);
    lv_obj_set_style_bg_color(tab_pill[r], lv_color_white(), 0);
    tab_label[r] = makeLabel(tab_pill[r], &font_text_17);
    lv_label_set_text(tab_label[r], RANGES[r].tab);
    lv_obj_center(tab_label[r]);
  }

  passTouchesToScreen();
  tickerShown = true;
  styleTabs();
  renderChart();
}

// "today" when the last trade was today in New York, otherwise the weekday ("on
// Friday").
String dayWord(time_t trade) {
  time_t now = time(nullptr);
  if (trade == 0 || now < 1700000000)
    return "today"; // clock not synced yet
  struct tm t, n;
  localtime_r(&trade, &t);
  localtime_r(&now, &n);
  if (t.tm_year == n.tm_year && t.tm_yday == n.tm_yday)
    return "today";
  static const char *days[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                               "Thursday", "Friday", "Saturday"};
  return String("on ") + days[t.tm_wday];
}

// "+" or a true minus (U+2212, same width as "+")
const char *signOf(float v) { return v >= 0 ? "+" : "\xE2\x88\x92"; }

// Price, change over the selected range, arrow and background colour.
void renderQuote() {
  if (!tickerShown)
    showTickerView();
  char buf[32];
  // snprintf, not lv_label_set_text_fmt: LVGL's printf has no %f
  snprintf(buf, sizeof(buf), quote.price >= 1000 ? "%.0f" : "%.2f",
           quote.price);
  lv_label_set_text(price_label, buf);

  const Series &s = series[selected];
  float chg = quote.change, pct = quote.pct;
  String phrase = dayWord(quote.tradeTime);
  if (selected != R_1D && s.ok) {
    chg = quote.price - s.base;
    pct = chg / s.base * 100;
    phrase = RANGES[selected].phrase;
  }
  bool up = chg >= 0;
  snprintf(buf, sizeof(buf), "%s%.2f%%", signOf(chg), fabsf(pct));
  lv_label_set_text(pct_label, buf);
  lv_label_set_text(chg_label, (String(signOf(chg)) + "$" +
                                String(fabsf(chg), 2) + " " + phrase)
                                   .c_str());
  lv_obj_update_layout(pct_label);
  atBaseline(chg_label, MARGIN + lv_obj_get_width(pct_label) + 10, CHANGE_BASE);

  lv_img_set_angle(arrow_img,
                   up ? 0 : 1800); // the same arrow, turned for a downvote
  lv_obj_clear_flag(arrow_img, LV_OBJ_FLAG_HIDDEN);
  setField(up ? COL_UP : COL_DOWN, true);
  styleTabs();
}

// ---------------------------------------------------------------- thesis page

void showThesisView() {
  clearView();
  lv_obj_t *scr = lv_scr_act();
  buildHeader(scr, PAGE_THESIS);

  const char *labels[3] = {"Forward P/E", "Revenue growth", "Market cap"};
  const lv_coord_t cols[3] = {MARGIN, 100, 210};
  for (int i = 0; i < 3; i++) {
    stat_val[i] = makeLabel(scr, &font_bold_36);
    lv_label_set_text(stat_val[i], "--");
    atBaseline(stat_val[i], cols[i], STAT_BASE);
    lv_obj_t *l = makeLabel(scr, &font_text_17, OPA_SECONDARY);
    lv_label_set_text(l, labels[i]);
    atBaseline(l, cols[i], STAT_LABEL_BASE);
  }

  lv_obj_t *track = makeBox(scr);
  lv_obj_set_size(track, screenWidth - 2 * MARGIN, RANGE_H);
  lv_obj_set_pos(track, MARGIN, RANGE_Y);
  lv_obj_set_style_radius(track, RANGE_H / 2, 0);
  lv_obj_set_style_bg_color(track, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(track, OPA_TRACK, 0);
  range_marker = makeDot(scr, 14, LV_OPA_COVER);
  lv_obj_add_flag(range_marker, LV_OBJ_FLAG_HIDDEN);

  range_lo = makeLabel(scr, &font_text_17, OPA_SECONDARY);
  lv_label_set_text(range_lo, "");
  atBaseline(range_lo, MARGIN, RANGE_LABEL_BASE);
  lv_obj_t *mid = makeLabel(scr, &font_text_17, OPA_SECONDARY);
  lv_label_set_text(mid, "52-week range");
  lv_obj_align(mid, LV_ALIGN_TOP_MID, 0,
               lineTop(&font_text_17, RANGE_LABEL_BASE));
  range_hi = makeLabel(scr, &font_text_17, OPA_SECONDARY);
  lv_label_set_text(range_hi, "");
  lv_obj_align(range_hi, LV_ALIGN_TOP_RIGHT, -MARGIN,
               lineTop(&font_text_17, RANGE_LABEL_BASE));

  below_label = makeLabel(scr, &font_text_17);
  lv_label_set_text(below_label, "");
  atBaseline(below_label, MARGIN, BELOW_HIGH_BASE);

  passTouchesToScreen();
  thesisShown = true;
}

// Fundamentals plus where today's price sits in the 52-week range.
void renderThesis() {
  if (!thesisShown)
    return;
  setField(quote.change >= 0 ? COL_UP : COL_DOWN,
           true); // colour follows today, like the price page
  if (!fund.ok) {
    for (lv_obj_t *v : stat_val)
      lv_label_set_text(v, "--");
    lv_label_set_text(range_lo, "");
    lv_label_set_text(range_hi, "");
    lv_obj_add_flag(range_marker, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(below_label, fund.fetchedAt
                                       ? "Can't load the numbers right now"
                                       : "Loading the numbers");
    return;
  }
  char buf[40]; // fits "100% below its 52-week high"
  if (fund.fwdPE > 0)
    snprintf(buf, sizeof(buf), "%.0f", fund.fwdPE);
  else
    strcpy(buf, "--"); // no forward earnings estimate
  lv_label_set_text(stat_val[0], buf);
  snprintf(buf, sizeof(buf), "%s%.0f%%", signOf(fund.revGrowth),
           fabsf(fund.revGrowth));
  lv_label_set_text(stat_val[1], buf);
  float billions = fund.mcapM / 1000; // Finnhub reports market cap in millions
  if (billions >= 1000)
    snprintf(buf, sizeof(buf), "$%.1fT", billions / 1000);
  else
    snprintf(buf, sizeof(buf), "$%.1fB", billions);
  lv_label_set_text(stat_val[2], buf);

  // A new high or low today stretches the range rather than pinning the dot to
  // an end
  float lo = fund.lo52, hi = fund.hi52, price = quote.valid ? quote.price : lo;
  if (quote.valid) {
    lo = min(lo, price);
    hi = max(hi, price);
  }
  snprintf(buf, sizeof(buf), "$%.2f", lo);
  lv_label_set_text(range_lo, buf);
  snprintf(buf, sizeof(buf), "$%.2f", hi);
  lv_label_set_text(range_hi, buf);
  float t = hi > lo ? (price - lo) / (hi - lo) : 1;
  lv_obj_set_pos(range_marker,
                 MARGIN + (lv_coord_t)(t * (screenWidth - 2 * MARGIN)) - 7,
                 RANGE_Y + RANGE_H / 2 - 7);
  lv_obj_clear_flag(range_marker, LV_OBJ_FLAG_HIDDEN);

  float below = (1 - price / hi) * 100;
  if (below < 0.5f)
    strcpy(buf, "At its 52-week high");
  else
    snprintf(buf, sizeof(buf), "%.0f%% below its 52-week high", below);
  lv_label_set_text(below_label, buf);
}

// ---------------------------------------------------------------- data

// Live price from Finnhub. Returns the HTTP status (200 = ok), 0 if Finnhub
// couldn't be reached, or -1 for a reply without a usable price.
int fetchQuote(Quote &q) {
  if (!client.connect("finnhub.io", 443))
    return 0;
  client.print(String("GET /api/v1/quote?symbol=") + TICKER +
               "&token=" + finnhub_api_key_config +
               " HTTP/1.1\r\nHost: finnhub.io\r\nConnection: close\r\n\r\n");

  unsigned long deadline = millis() + 10000;
  while (!client.available() && millis() < deadline) {
    delay(10);
    lv_timer_handler(); // keep animations moving while we wait
  }
  if (!client.available()) {
    client.stop();
    return 0;
  }
  String statusLine = client.readStringUntil('\n'); // "HTTP/1.1 200 OK"
  int code = statusLine.substring(9, 12).toInt();
  while (client.available())
    if (client.readStringUntil('\n') == "\r")
      break; // end of headers
  String body = "";
  while (client.available())
    body += client.readString();
  client.stop();
  if (code != 200)
    return code;

  JsonDocument doc;
  if (deserializeJson(doc, body) || !(doc["c"].as<float>() > 0))
    return -1;
  q.price = doc["c"];
  q.change = doc["d"];
  q.pct = doc["dp"];
  q.tradeTime = doc["t"].as<long>();
  q.valid = true;
  return 200;
}

// HTTPS GET, returning the whole body. It's read in full before parsing:
// parsing straight off the TLS stream gives up (IncompleteInput) whenever a
// packet takes longer than the 1 s stream timeout.
String httpsGet(const String &url, int &code) {
  HTTPClient http;
  http.useHTTP10(true); // plain Content-Length reply, no chunked encoding
  http.setUserAgent(
      "JoshTerminal/1.0"); // Yahoo answers 429 to requests without one
  http.setTimeout(10000);
  if (!http.begin(client, url)) {
    code = 0;
    return "";
  }
  code = http.GET();
  String body = code == HTTP_CODE_OK ? http.getString() : "";
  http.end();
  return body;
}

// Chart history from Yahoo Finance's chart endpoint (unofficial, no key). Only
// the charts depend on it: if it stops working the price keeps updating from
// Finnhub.
bool fetchSeries(Range r) {
  Series &s = series[r];
  s.fetchedAt = millis();
  int code;
  String body =
      httpsGet(String("https://query1.finance.yahoo.com/v8/finance/chart/") +
                   TICKER + "?" + RANGES[r].query,
               code);
  if (code != HTTP_CODE_OK) {
    Serial.printf("Chart %s: HTTP %d\n", RANGES[r].tab, code);
    return s.ok = false;
  }
  JsonDocument filter; // keep only what the chart needs (1Y is ~27 KB of JSON)
  filter["chart"]["result"][0]["meta"]["chartPreviousClose"] = true;
  filter["chart"]["result"][0]["indicators"]["quote"][0]["close"] = true;
  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("Chart %s: %s after %u bytes\n", RANGES[r].tab, err.c_str(),
                  body.length());
    return s.ok = false;
  }
  JsonObject res = doc["chart"]["result"][0];
  s.closes.clear();
  JsonArray closes = res["indicators"]["quote"][0]["close"];
  for (JsonVariant v : closes)
    if (!v.isNull())
      s.closes.push_back(v.as<float>()); // nulls are minutes without trades
  if (s.closes.size() > CHART_MAX)
    s.closes.erase(s.closes.begin(), s.closes.end() - CHART_MAX);
  s.base = res["meta"]["chartPreviousClose"] | 0.0f;
  s.ok = s.closes.size() >= 2 && s.base > 0;
  Serial.printf("Chart %s: %u points\n", RANGES[r].tab,
                (unsigned)s.closes.size());
  return s.ok;
}

// Forward P/E, latest-quarter revenue growth, market cap and the 52-week range
// from Finnhub.
bool fetchFundamentals() {
  fund.fetchedAt = millis();
  int code;
  String body =
      httpsGet(String("https://finnhub.io/api/v1/stock/metric?symbol=") +
                   TICKER + "&metric=all&token=" + finnhub_api_key_config,
               code);
  if (code != HTTP_CODE_OK) {
    Serial.printf("Fundamentals: HTTP %d\n", code);
    return fund.ok = false;
  }
  JsonDocument filter; // the reply is ~27 KB; keep five numbers
  for (const char *k : {"forwardPE", "revenueGrowthQuarterlyYoy",
                        "marketCapitalization", "52WeekHigh", "52WeekLow"})
    filter["metric"][k] = true;
  JsonDocument doc;
  if (DeserializationError err =
          deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
    Serial.printf("Fundamentals: %s\n", err.c_str());
    return fund.ok = false;
  }
  JsonObject m = doc["metric"];
  fund.fwdPE = m["forwardPE"] | 0.0f;
  fund.revGrowth = m["revenueGrowthQuarterlyYoy"] | 0.0f;
  fund.mcapM = m["marketCapitalization"] | 0.0f;
  fund.hi52 = m["52WeekHigh"] | 0.0f;
  fund.lo52 = m["52WeekLow"] | 0.0f;
  fund.ok = fund.hi52 > 0 && fund.lo52 > 0 && fund.mcapM > 0;
  Serial.printf(
      "Fundamentals: P/E %.1f, revenue %+.1f%%, cap $%.0fM, 52w %.2f-%.2f\n",
      fund.fwdPE, fund.revGrowth, fund.mcapM, fund.lo52, fund.hi52);
  return fund.ok;
}

bool seriesStale(Range r) {
  const Series &s = series[r];
  return !s.fetchedAt ||
         millis() - s.fetchedAt >= (s.ok ? RANGES[r].ttlMs : 60000UL);
}

bool fundStale() {
  return !fund.fetchedAt ||
         millis() - fund.fetchedAt >= (fund.ok ? FUND_TTL_MS : 60000UL);
}

void selectRange(Range r) {
  selected = r;
  if (!tickerShown)
    return;
  styleTabs();
  if (seriesStale(r) && WiFi.status() == WL_CONNECTED) {
    if (!series[r].ok) { // show the tab change and "Loading chart" before the
                         // download blocks
      renderChart();
      lv_refr_now(disp);
    }
    fetchSeries(r);
  }
  renderChart();
  if (quote.valid)
    renderQuote();
}

void showPage(Page p) {
  page = p;
  if (p == PAGE_THESIS) {
    showThesisView();
    renderThesis();
    if (fundStale() && WiFi.status() == WL_CONNECTED) {
      lv_refr_now(disp); // show the page with "Loading the numbers" before the
                         // download blocks
      fetchFundamentals();
      renderThesis();
    }
  } else {
    showTickerView();
    if (quote.valid)
      renderQuote();
  }
}

String problemText(int code) {
  if (code == 0)
    return "Can't reach Finnhub";
  if (code == 401 || code == 403)
    return "Finnhub rejected the API key";
  if (code == 429)
    return "Finnhub rate limit hit";
  if (code < 0)
    return "Finnhub sent no price";
  return String("Finnhub error ") + code;
}

void refreshQuote() {
  if (WiFi.status() != WL_CONNECTED) { // the ESP32 reconnects on its own
    setStatus("Reconnecting to WiFi");
    return;
  }
  int code = fetchQuote(quote);
  Serial.printf("Quote: HTTP %d, $%.2f\n", code, quote.price);
  if (quote.valid) {
    setStatus(code == 200
                  ? ""
                  : problemText(code)); // a failed refresh keeps the last price
    if (page == PAGE_THESIS)
      renderThesis();
    else
      renderQuote();
  } else {
    String retry = String(". Trying again in ") + switchInterval / 1000 + " s.";
    if (code == 401 || code == 403)
      retry = ". Hold the screen for 10 s to enter it again.";
    setStatus(problemText(code) + retry);
  }
}

// ---------------------------------------------------------------- WiFi + setup
// page

void connectWiFi() {
  WiFiManager wm;
  wm.setTitle("RDDT Ticker");

  prefs.begin("config", true);
  String current_finnhub = prefs.getString("finnhub_api", "");
  prefs.end();

  WiFiManagerParameter custom_finnhub_api("finnhub_api", "Finnhub API Key",
                                          current_finnhub.c_str(), 64);
  String dropdownHTML =
      "<br><b>Refresh Interval Configuration:</b><br><br>"
      "<label for='interval_mode'>Refresh Interval Mode:</label><br>"
      "<select id='interval_mode' name='interval_mode' "
      "onchange='toggleManualField()'>"
      "<option value='AUTO'" +
      String(auto_interval_enabled ? " selected" : "") +
      ">AUTO (Recommended)</option>"
      "<option value='MANUAL'" +
      String(!auto_interval_enabled ? " selected" : "") +
      ">MANUAL</option>"
      "</select><br><br>"
      "<div id='manual_div' style='display:" +
      String(auto_interval_enabled ? "none" : "block") +
      ";'>"
      "<label for='manual_interval'>Manual Interval (30-600 "
      "seconds):</label><br>"
      "<input type='number' id='manual_interval' name='manual_interval' "
      "min='30' max='600' value='" +
      String(auto_interval_enabled ? 60 : switchInterval / 1000) +
      "'><br><br>"
      "</div>"
      "<div style='background-color:#f0f0f0; padding:8px; border-radius:4px; "
      "font-size:12px;'>"
      "<b>Info:</b><br>"
      "&bull; AUTO mode: refreshes RDDT every 30 seconds<br>"
      "&bull; MANUAL mode: You set the interval (30-600 seconds)"
      "</div>"
      "<script>function "
      "toggleManualField(){document.getElementById('manual_div').style.display="
      "document.getElementById('interval_mode').value==='MANUAL'?'block':'none'"
      ";}</script>";
  WiFiManagerParameter custom_interval_controls(dropdownHTML.c_str());

  wm.addParameter(&custom_finnhub_api);
  wm.addParameter(&custom_interval_controls);

  wm.setAPCallback(
      [](WiFiManager *m) { showSetupView(m->getConfigPortalSSID().c_str()); });
  wm.setConfigPortalTimeout(300);
  wm.setBreakAfterConfig(
      true); // save the Finnhub key even if the WiFi join fails

  wm.setSaveConfigCallback([&wm, &custom_finnhub_api]() {
    finnhub_api_key_config = custom_finnhub_api.getValue();
    auto_interval_enabled = wm.server->arg("interval_mode") == "AUTO";

    prefs.begin("config", false);
    prefs.putString("finnhub_api", finnhub_api_key_config);
    prefs.putBool("auto_interval", auto_interval_enabled);
    if (!auto_interval_enabled) {
      switchInterval =
          constrain(wm.server->arg("manual_interval").toInt(), 30, 600) * 1000;
      prefs.putULong("switch_interval", switchInterval);
    }
    prefs.end();
    Serial.println(String("Settings saved. Finnhub key ") +
                   (finnhub_api_key_config.isEmpty() ? "missing" : "set"));
    showMessageView("Connecting to WiFi");
  });

  // No API key yet: show the setup page even if a WiFi network is already saved
  bool connected = finnhub_api_key_config.length() == 0
                       ? wm.startConfigPortal(AP_SSID, AP_PASS)
                       : wm.autoConnect(AP_SSID, AP_PASS);
  if (!connected) {
    Serial.println("Failed to connect and hit timeout");
    ESP.restart();
  }

  configTzTime(MARKET_TZ, "pool.ntp.org",
               "time.nist.gov"); // for "today" vs "on Friday"
  client.setInsecure();
  client.setHandshakeTimeout(
      15); // seconds; default 120 freezes the UI on a stalled connection
}

// ---------------------------------------------------------------- main

void setup() {
  Serial.begin(115200);
  for (int pin : {LED_RED, LED_GREEN, LED_BLUE}) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH); // off
  }

  prefs.begin("config", true);
  finnhub_api_key_config = prefs.getString("finnhub_api", "");
  auto_interval_enabled = prefs.getBool("auto_interval", true);
  switchInterval = prefs.getULong("switch_interval", 30000);
  prefs.end();
  if (auto_interval_enabled)
    switchInterval = 30000; // free tier allows 60 calls/min
  Serial.println(String("Finnhub API key: ") +
                 (finnhub_api_key_config.isEmpty() ? "(none)" : "(set)"));

  setupLVGL();
  touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchscreenSPI);
  touchscreen.setRotation(1);
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touchscreen_read;
  lv_indev_drv_register(&indev_drv);

  showMessageView("Connecting to WiFi");
  connectWiFi();
  showMessageView(String("Getting the latest ") + TICKER + " price");
  refreshQuote();
  lastRefresh = millis();
}

void loop() {
  lv_timer_handler();

  if (touchPressed && !resetInProgress) {
    unsigned long held = millis() - touchPressStart;
    if (held >= 10000) {
      resetInProgress = true;
      Serial.println("10 s hold: clearing WiFi and settings");
      showMessageView("Clearing WiFi and settings. Restarting.");
      delay(1500);
      WiFi.disconnect(true);
      WiFiManager wm;
      wm.resetSettings();
      prefs.begin("config", false);
      prefs.clear();
      prefs.end();
      ESP.restart();
    } else if (held >= 2000 && !holdHintShown && status_label) {
      lv_label_set_text(status_label, "Keep holding to reset WiFi");
      holdHintShown = true;
    }
  }

  bool onTicker = tickerShown || thesisShown; // past boot and setup
  if (pendingPage >= 0) {
    Page p = (Page)pendingPage;
    pendingPage = -1;
    if (onTicker && p != page)
      showPage(p);
  } else if (pendingRange >= 0) {
    Range r = (Range)pendingRange;
    pendingRange = -1;
    selectRange(r);
  } else if (onTicker && millis() - lastTouch >=
                             IDLE_RETURN_MS) { // drift back to price + 1D
    if (page != PAGE_PRICE)
      showPage(PAGE_PRICE);
    else if (selected != R_1D)
      selectRange(R_1D);
  }

  if (millis() - lastRefresh >= switchInterval) {
    refreshQuote();
    lastRefresh = millis();
  }
  // Background downloads, so tapping 1D or swiping to the thesis page is
  // instant
  if (onTicker && WiFi.status() == WL_CONNECTED) {
    if (seriesStale(R_1D)) {
      fetchSeries(R_1D);
      if (tickerShown && selected == R_1D)
        renderChart();
    } else if (fundStale()) {
      fetchFundamentals();
      renderThesis(); // no-op unless the thesis page is showing
    }
  }
  delay(5);
}
