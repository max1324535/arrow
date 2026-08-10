/* ================================================================
   arrowOS v0.1  -  CORE BUILD (single file)
   A cooperative multitasking OS for the Arduino UNO R3
   ----------------------------------------------------------------
   HARDWARE
     Elegoo UNO R3          ATmega328P, 32K flash, 2K SRAM
     SSD1306 128x64 OLED    GND->GND  VCC->5V  SCL->A5  SDA->A4
     KY-023 joystick        GND->GND  +5V->5V  VRx->A0  VRy->A1  SW->D2
     Leave A3 empty - it is read as a random seed.

   LIBRARY
     Install "U8g2" by oliver  (Tools > Manage Libraries)
     Do NOT use Adafruit_SSD1306 - it eats 1024 bytes of SRAM.

   CONTROLS
     stick        navigate / play
     click        select / confirm
     hold 0.6s    HOME - back to the Shell from anywhere

   THE ONE RULE
     onDraw() runs 8x per frame, once per 8-pixel page.
     Logic goes in onLoop(). Pixels go in onDraw(). Never mix them.
   ================================================================ */

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

/* ================================================================
   CONFIG
   ================================================================ */
#define AOS_VERSION   "0.1"
#define AOS_MAX_TASKS 8
#define AOS_NAME_LEN  10

#define PIN_JOY_X       A0
#define PIN_JOY_Y       A1
#define PIN_JOY_SW      2

#define JOY_DEADZONE    250   /* counts away from 512 before it registers */
#define JOY_REPEAT_MS   160   /* auto-repeat while held                   */
#define JOY_DELAY_MS    400   /* pause before auto-repeat kicks in        */
#define LONGPRESS_MS    600   /* hold this long = HOME                    */
#define JOY_INVERT_Y    1     /* set to 0 if up/down feel backwards       */

#define FRAME_MS        50    /* ~20 fps redraw cap */

/* ================================================================
   TYPES  (must stay above the first function definition)
   ================================================================ */
enum : uint8_t {
  EV_NONE = 0,
  EV_UP, EV_DOWN, EV_LEFT, EV_RIGHT,
  EV_CLICK,
  EV_HOME
};

enum : uint8_t {
  TS_EMPTY = 0,
  TS_READY,
  TS_SLEEPING,
  TS_SUSPENDED
};

typedef void (*TaskFn)();
typedef void (*EventFn)(uint8_t ev);

/* 16 bytes per task. 8 tasks = 128 bytes of your 2048. */
struct Task {
  const char *name;      /* pointer into PROGMEM, not SRAM */
  TaskFn      onStart;
  TaskFn      onLoop;
  TaskFn      onDraw;
  EventFn     onEvent;
  uint32_t    wakeAt;
  uint8_t     state;
  uint8_t     flags;
};

#define TF_NONE       0x00
#define TF_BACKGROUND 0x01   /* hidden from the Shell menu */

/* ================================================================
   GLOBALS
   ================================================================ */
Task    kTasks[AOS_MAX_TASKS];
uint8_t kTaskCount = 0;

static int8_t   curPid    = -1;   /* who is running right now */
static int8_t   focusPid  = 0;    /* who owns the screen      */
static uint32_t ticks     = 0;
static uint32_t lastFrame = 0;

/* The "_1_" means 1 page buffer = 128 bytes of SRAM.
   If you have an SH1106 (1.3" panel) swap this for:
   U8G2_SH1106_128X64_NONAME_1_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE); */
U8G2_SSD1306_128X64_NONAME_1_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);

static bool     btnDown    = false;
static bool     longFired  = false;
static uint32_t pressedAt  = 0;
static bool     centered   = true;
static uint32_t nextRepeat = 0;

/* task names live in flash */
const char nShell[] PROGMEM = "Shell";
const char nClock[] PROGMEM = "Clock";
const char nSys[]   PROGMEM = "SysMon";
const char nTask[]  PROGMEM = "Tasks";
const char nSnake[] PROGMEM = "Snake";
const char nBeat[]  PROGMEM = "Heartbeat";

/* ================================================================
   DISPLAY
   ================================================================ */
static void display_init() {
  oled.setBusClock(400000);   /* must come before begin() */
  oled.begin();
  oled.setFontMode(1);
  oled.setDrawColor(1);
}
static void display_begin() { oled.firstPage(); }
static bool display_next()  { return oled.nextPage(); }

/* ================================================================
   INPUT  -  KY-023 joystick to kernel events
   ================================================================ */
static void input_init() {
  pinMode(PIN_JOY_SW, INPUT_PULLUP);   /* switch is active LOW */
}

static uint8_t input_poll() {
  uint32_t now = millis();

  /* ---- button ---- */
  bool down = (digitalRead(PIN_JOY_SW) == LOW);

  if (down && !btnDown) {
    btnDown   = true;
    longFired = false;
    pressedAt = now;
  } else if (down && btnDown && !longFired && (now - pressedAt) >= LONGPRESS_MS) {
    longFired = true;
    return EV_HOME;
  } else if (!down && btnDown) {
    btnDown = false;
    if (!longFired) return EV_CLICK;
  }

  /* ---- stick ---- */
  int x = analogRead(PIN_JOY_X) - 512;
  int y = analogRead(PIN_JOY_Y) - 512;
#if JOY_INVERT_Y
  y = -y;
#endif

  uint8_t dir = EV_NONE;
  if (abs(x) > JOY_DEADZONE || abs(y) > JOY_DEADZONE) {
    if (abs(x) > abs(y)) dir = (x > 0) ? EV_RIGHT : EV_LEFT;
    else                 dir = (y > 0) ? EV_UP    : EV_DOWN;
  }

  if (dir == EV_NONE) { centered = true; return EV_NONE; }

  if (centered) {                    /* first flick fires immediately */
    centered   = false;
    nextRepeat = now + JOY_DELAY_MS;
    return dir;
  }
  if ((int32_t)(now - nextRepeat) >= 0) {
    nextRepeat = now + JOY_REPEAT_MS;
    return dir;
  }
  return EV_NONE;
}

/* ================================================================
   KERNEL
   ================================================================ */
static void kernel_init() {
  for (uint8_t i = 0; i < AOS_MAX_TASKS; i++) kTasks[i].state = TS_EMPTY;
  kTaskCount = 0;
  ticks = 0;
}

static int8_t kernel_spawn(const char *nameP, TaskFn onStart, TaskFn onLoop,
                           TaskFn onDraw, EventFn onEvent, uint8_t flags) {
  if (kTaskCount >= AOS_MAX_TASKS) return -1;

  Task &t   = kTasks[kTaskCount];
  t.name    = nameP;
  t.onStart = onStart;
  t.onLoop  = onLoop;
  t.onDraw  = onDraw;
  t.onEvent = onEvent;
  t.wakeAt  = 0;
  t.state   = TS_READY;
  t.flags   = flags;

  int8_t pid = (int8_t)kTaskCount++;
  if (t.onStart) { curPid = pid; t.onStart(); curPid = -1; }
  return pid;
}

static void kernel_focus(int8_t pid) {
  if (pid >= 0 && pid < (int8_t)kTaskCount) focusPid = pid;
}
static int8_t   kernel_focused() { return focusPid; }
static int8_t   kernel_self()    { return curPid;   }
static uint32_t kernel_ticks()   { return ticks;    }

static void task_sleep(uint16_t ms) {
  if (curPid < 0) return;
  kTasks[curPid].wakeAt = millis() + ms;
  kTasks[curPid].state  = TS_SLEEPING;
}

static void task_name(int8_t pid, char *out) {
  if (pid < 0 || pid >= (int8_t)kTaskCount) { out[0] = 0; return; }
  strncpy_P(out, kTasks[pid].name, AOS_NAME_LEN);
  out[AOS_NAME_LEN] = 0;
}

static uint8_t task_state(int8_t pid) {
  if (pid < 0 || pid >= (int8_t)kTaskCount) return TS_EMPTY;
  return kTasks[pid].state;
}

/* Gap between the top of the heap and the current stack pointer. */
extern int  __heap_start;
extern int *__brkval;
static int freeRam() {
  int v;
  return (int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);
}

/* One scheduler pass. */
static void kernel_run() {
  ticks++;
  uint32_t now = millis();

  /* ---- 1. input ---- */
  uint8_t ev = input_poll();
  if (ev == EV_HOME) {
    kernel_focus(0);
  } else if (ev != EV_NONE) {
    Task &f = kTasks[focusPid];
    if (f.onEvent) { curPid = focusPid; f.onEvent(ev); curPid = -1; }
  }

  /* ---- 2. round robin ---- */
  for (uint8_t i = 0; i < kTaskCount; i++) {
    Task &t = kTasks[i];

    /* signed compare so millis() rollover cannot strand a task */
    if (t.state == TS_SLEEPING && (int32_t)(now - t.wakeAt) >= 0)
      t.state = TS_READY;

    if (t.state != TS_READY || !t.onLoop) continue;

    curPid = (int8_t)i;
    t.onLoop();
    curPid = -1;
  }

  /* ---- 3. render the focused task, page by page ---- */
  if ((uint32_t)(now - lastFrame) >= FRAME_MS) {
    lastFrame = now;
    Task &f = kTasks[focusPid];
    if (f.onDraw) {
      curPid = focusPid;
      display_begin();
      do { f.onDraw(); } while (display_next());
      curPid = -1;
    }
  }
}

/* ================================================================
   APP: SHELL   (pid 0)
   ================================================================ */
static uint8_t shSel = 0;

static uint8_t shellCount() {
  uint8_t n = 0;
  for (uint8_t i = 1; i < kTaskCount; i++)
    if (!(kTasks[i].flags & TF_BACKGROUND)) n++;
  return n;
}

static int8_t shellPidAt(uint8_t idx) {
  uint8_t n = 0;
  for (uint8_t i = 1; i < kTaskCount; i++) {
    if (kTasks[i].flags & TF_BACKGROUND) continue;
    if (n == idx) return (int8_t)i;
    n++;
  }
  return -1;
}

static void shellDraw() {
  char buf[AOS_NAME_LEN + 1];

  oled.setFont(u8g2_font_5x7_tf);
  oled.drawStr(2, 7, "arrowOS " AOS_VERSION);
  oled.drawHLine(0, 10, 128);

  oled.setFont(u8g2_font_6x10_tf);
  uint8_t n = shellCount();
  for (uint8_t i = 0; i < n && i < 5; i++) {
    task_name(shellPidAt(i), buf);
    uint8_t y = 22 + i * 10;
    if (i == shSel) {
      oled.drawBox(0, y - 8, 128, 10);
      oled.setDrawColor(0);
      oled.drawTriangle(3, y - 7, 3, y - 1, 8, y - 4);   /* the mark */
      oled.drawStr(12, y, buf);
      oled.setDrawColor(1);
    } else {
      oled.drawStr(12, y, buf);
    }
  }
}

static void shellEvent(uint8_t ev) {
  uint8_t n = shellCount();
  if (!n) return;
  if      (ev == EV_DOWN) shSel = (uint8_t)((shSel + 1) % n);
  else if (ev == EV_UP)   shSel = (uint8_t)((shSel + n - 1) % n);
  else if (ev == EV_CLICK || ev == EV_RIGHT) {
    int8_t pid = shellPidAt(shSel);
    if (pid > 0) kernel_focus(pid);
  }
}

/* ================================================================
   APP: CLOCK
   ================================================================ */
static uint32_t clkOffset = 0;   /* seconds */

static void clockDraw() {
  uint32_t t = (millis() / 1000UL) + clkOffset;
  unsigned s = (unsigned)(t % 60);
  unsigned m = (unsigned)((t / 60) % 60);
  unsigned h = (unsigned)((t / 3600) % 24);

  char buf[9];
  snprintf(buf, sizeof(buf), "%02u:%02u:%02u", h, m, s);

  oled.setFont(u8g2_font_logisoso20_tn);
  oled.drawStr(4, 40, buf);

  oled.setFont(u8g2_font_5x7_tf);
  oled.drawStr(2, 62, "U/D hr  L/R min  clk=0");
}

static void clockEvent(uint8_t ev) {
  switch (ev) {
    case EV_UP:    clkOffset = (clkOffset + 3600) % 86400UL;           break;
    case EV_DOWN:  clkOffset = (clkOffset + 86400UL - 3600) % 86400UL; break;
    case EV_RIGHT: clkOffset = (clkOffset + 60) % 86400UL;             break;
    case EV_LEFT:  clkOffset = (clkOffset + 86400UL - 60) % 86400UL;   break;
    case EV_CLICK: clkOffset = 0;                                      break;
  }
}

/* ================================================================
   APP: SYSMON  -  free SRAM, scheduler rate, uptime
   ================================================================ */
static int      sysFree    = 0;
static uint16_t sysRate    = 0;
static uint32_t sysLastMs  = 0;
static uint32_t sysLastTick = 0;

static void sysLoop() {
  uint32_t now = millis();
  if ((uint32_t)(now - sysLastMs) < 1000) return;
  uint32_t tk = kernel_ticks();
  sysRate     = (uint16_t)(tk - sysLastTick);
  sysLastTick = tk;
  sysLastMs   = now;
  sysFree     = freeRam();
}

static void sysDraw() {
  char line[22];

  oled.setFont(u8g2_font_5x7_tf);
  oled.drawStr(2, 7, "SYSMON");
  oled.drawHLine(0, 10, 128);

  snprintf(line, sizeof(line), "SRAM free: %d B", sysFree);
  oled.drawStr(2, 22, line);

  uint8_t w = (uint8_t)((long)sysFree * 124L / 2048L);
  if (w > 124) w = 124;
  oled.drawFrame(2, 26, 124, 8);
  oled.drawBox(2, 26, w, 8);

  snprintf(line, sizeof(line), "sched: %u pass/s", sysRate);
  oled.drawStr(2, 46, line);

  snprintf(line, sizeof(line), "uptime: %lus", (unsigned long)(millis() / 1000UL));
  oled.drawStr(2, 56, line);
}

/* ================================================================
   APP: TASKS  -  live task table
   ================================================================ */
static void tasksDraw() {
  char name[AOS_NAME_LEN + 1];
  char line[24];

  oled.setFont(u8g2_font_5x7_tf);
  oled.drawStr(2, 7, "PID NAME        ST");
  oled.drawHLine(0, 10, 128);

  for (uint8_t i = 0; i < kTaskCount && i < 7; i++) {
    task_name((int8_t)i, name);
    char st;
    switch (task_state((int8_t)i)) {
      case TS_READY:     st = 'R'; break;
      case TS_SLEEPING:  st = 'S'; break;
      case TS_SUSPENDED: st = 'X'; break;
      default:           st = '-'; break;
    }
    snprintf(line, sizeof(line), " %u  %-10s  %c", i, name, st);
    oled.drawStr(2, 19 + i * 7, line);
  }
}

/* ================================================================
   APP: SNAKE  -  16x8 grid of 8px cells
   ================================================================ */
#define SN_W    16
#define SN_H     8
#define SN_MAX  48

static uint8_t snBody[SN_MAX];
static uint8_t snLen, snDir, snFood;
static bool    snDead;
static int8_t  snPid = -1;

static uint8_t snPack(uint8_t x, uint8_t y) { return (uint8_t)(y * SN_W + x); }
static uint8_t snX(uint8_t p)               { return (uint8_t)(p % SN_W);     }
static uint8_t snY(uint8_t p)               { return (uint8_t)(p / SN_W);     }

static void snPlaceFood() {
  bool clash;
  do {
    snFood = snPack((uint8_t)random(SN_W), (uint8_t)random(SN_H));
    clash = false;
    for (uint8_t i = 0; i < snLen; i++)
      if (snBody[i] == snFood) { clash = true; break; }
  } while (clash);
}

static void snakeStart() {
  randomSeed(analogRead(A3));   /* A3 floats -> decent entropy */
  snLen  = 3;
  snDir  = EV_RIGHT;
  snDead = false;
  snBody[0] = snPack(8, 4);
  snBody[1] = snPack(7, 4);
  snBody[2] = snPack(6, 4);
  snPlaceFood();
}

static void snakeLoop() {
  /* only tick while the player is actually looking at us */
  if (kernel_focused() != snPid || snDead) { task_sleep(120); return; }

  uint8_t hx = snX(snBody[0]);
  uint8_t hy = snY(snBody[0]);
  switch (snDir) {
    case EV_UP:   hy = (uint8_t)((hy + SN_H - 1) % SN_H); break;
    case EV_DOWN: hy = (uint8_t)((hy + 1) % SN_H);        break;
    case EV_LEFT: hx = (uint8_t)((hx + SN_W - 1) % SN_W); break;
    default:      hx = (uint8_t)((hx + 1) % SN_W);        break;
  }
  uint8_t head = snPack(hx, hy);

  for (uint8_t i = 0; i < snLen; i++)
    if (snBody[i] == head) { snDead = true; return; }

  for (uint8_t i = snLen; i > 0; i--) snBody[i] = snBody[i - 1];
  snBody[0] = head;

  if (head == snFood) {
    if (snLen < SN_MAX - 1) snLen++;
    snPlaceFood();
  }
  task_sleep(170);
}

static void snakeDraw() {
  for (uint8_t i = 0; i < snLen; i++)
    oled.drawBox(snX(snBody[i]) * 8 + 1, snY(snBody[i]) * 8 + 1, 6, 6);

  oled.drawFrame(snX(snFood) * 8 + 1, snY(snFood) * 8 + 1, 6, 6);

  if (snDead) {
    char line[20];
    oled.setDrawColor(0);
    oled.drawBox(10, 22, 108, 20);
    oled.setDrawColor(1);
    oled.drawFrame(10, 22, 108, 20);
    oled.setFont(u8g2_font_5x7_tf);
    snprintf(line, sizeof(line), "DEAD  len %u  clk", snLen);
    oled.drawStr(16, 35, line);
  }
}

static void snakeEvent(uint8_t ev) {
  if (snDead) { if (ev == EV_CLICK) snakeStart(); return; }
  if (ev == EV_UP    && snDir != EV_DOWN)  snDir = EV_UP;
  if (ev == EV_DOWN  && snDir != EV_UP)    snDir = EV_DOWN;
  if (ev == EV_LEFT  && snDir != EV_RIGHT) snDir = EV_LEFT;
  if (ev == EV_RIGHT && snDir != EV_LEFT)  snDir = EV_RIGHT;
}

/* ================================================================
   APP: HEARTBEAT  -  background task, proves the OS multitasks.
   Blinks pin 13 forever no matter which app you are in.
   ================================================================ */
static bool beatOn = false;

static void beatStart() { pinMode(LED_BUILTIN, OUTPUT); }

static void beatLoop() {
  beatOn = !beatOn;
  digitalWrite(LED_BUILTIN, beatOn ? HIGH : LOW);
  task_sleep(500);
}

/* ================================================================
   REGISTRY  -  order here is the order in the Shell menu
   ================================================================ */
static void apps_register() {
  kernel_spawn(nShell, NULL, NULL,    shellDraw, shellEvent, TF_NONE);  /* pid 0 */
  kernel_spawn(nClock, NULL, NULL,    clockDraw, clockEvent, TF_NONE);
  kernel_spawn(nSys,   NULL, sysLoop, sysDraw,   NULL,       TF_NONE);
  kernel_spawn(nTask,  NULL, NULL,    tasksDraw, NULL,       TF_NONE);
  snPid = kernel_spawn(nSnake, snakeStart, snakeLoop, snakeDraw, snakeEvent, TF_NONE);
  kernel_spawn(nBeat,  beatStart, beatLoop, NULL, NULL,      TF_BACKGROUND);
}

/* ================================================================
   BOOT
   ================================================================ */
/* The logo mark, drawn from the same grid as the SVG.
   u = pixels per cell, so u=2 gives an 18x18 mark. */
static void drawMark(uint8_t ox, uint8_t oy, uint8_t u) {
  oled.drawBox(ox, oy + 3 * u, 4 * u, 3 * u);          /* shaft */
  for (uint8_t c = 0; c < 5; c++)                       /* head: 9-7-5-3-1 */
    oled.drawBox(ox + (4 + c) * u, oy + c * u, u, (uint8_t)((9 - 2 * c) * u));
}

static void splash() {
  display_begin();
  do {
    drawMark(4, 20, 2);

    oled.setFont(u8g2_font_10x20_tf);
    oled.drawStr(30, 36, "arrowOS");
    oled.setFont(u8g2_font_5x7_tf);
    oled.drawStr(30, 48, "v" AOS_VERSION "  booting...");
  } while (display_next());
  delay(1200);
}

void setup() {
  Serial.begin(9600);

  display_init();
  input_init();
  splash();

  kernel_init();
  apps_register();
  kernel_focus(0);

  Serial.println(F("arrowOS " AOS_VERSION " up"));
  Serial.print(F("free SRAM: "));
  Serial.println(freeRam());
}

void loop() {
  kernel_run();
}

/* ================================================================
   ADDING YOUR OWN APP

     const char nMine[] PROGMEM = "MyApp";
     static uint8_t counter = 0;

     static void mineLoop()  { counter++; task_sleep(100); }
     static void mineDraw()  { char b[16];
                               snprintf(b, sizeof(b), "n=%u", counter);
                               oled.setFont(u8g2_font_6x10_tf);
                               oled.drawStr(4, 30, b); }
     static void mineEvent(uint8_t ev) { if (ev == EV_CLICK) counter = 0; }

   Then add one line inside apps_register():

     kernel_spawn(nMine, NULL, mineLoop, mineDraw, mineEvent, TF_NONE);

   It shows up in the Shell menu on its own. Raise AOS_MAX_TASKS
   if you go past 8 tasks.
   ================================================================ */
