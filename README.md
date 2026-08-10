# arrow
Arrow

A cooperative multitasking operating system for the Arduino UNO. A scheduler, a task table, an event system, a shell and twelve apps — in 2 KB of SRAM.

Built by Nextmap.

What this is

Arrow is a real operating system, not a sketch with a menu. It has a round-robin scheduler that runs every ready task once per pass, a task table with states you can watch change live, an event system that routes joystick input only to the app that owns the screen, and cooperative sleeping so no task can block another.

It runs on an ATmega328P: 32 KB of flash, 2 KB of SRAM, no memory management unit. The whole thing is small enough to read in one sitting, which is the point.

Hardware
Part	Notes
Arduino UNO R3	ATmega328P — 32 KB flash, 2 KB SRAM
SSD1306 OLED, 0.96"	Four-pin I²C version. Seven pins means SPI — wrong one
KY-023 joystick	Two analog axes plus a push switch
Breadboard	Half-size is plenty
Jumper wires	9 male-to-male
Wiring
From	To	Carries
OLED GND	GND	ground
OLED VCC	5V	power
OLED SDA	A4	I²C data
OLED SCL	A5	I²C clock
Joystick GND	GND	ground
Joystick +5V	5V	power
Joystick VRx	A0	left / right
Joystick VRy	A1	up / down
Joystick SW	D2	button, active low

A4 and A5 are the hardware I²C pins on the 328P — the display cannot go anywhere else.

Leave A3 empty. Snake reads the floating pin as a noise source to seed its random number generator. Connect something to it and every game starts identically.

Full diagram: docs/wiring.svg

Install

Install U8g2 by oliver (Arduino IDE → Tools → Manage Libraries). That's the only dependency.

Do not use Adafruit_SSD1306. It allocates a full 1024-byte framebuffer — half the chip's memory. U8g2 in page mode uses 128 bytes instead. That single decision is what makes this project fit.

Then:

Open the sketch, select the board (Arduino Uno) and your port
Upload
Open the Serial Monitor at 9600 baud — you should see the version line and a free-memory number

If Serial prints but the screen stays dark, the problem is the display link, not the code.

Builds
Build	Tasks	Use it when
arrow_core	6	You want the smallest working system, or you're short on SRAM
arrow_full	12	You want everything

Both are single files. Nothing to organise, no headers to keep in sync.

Controls
Input	Does
Stick	Navigate, or play
Click	Select / confirm
Hold 0.6 s	Home — back to the Shell from anywhere

The kernel intercepts the long press before any app sees it, so no app can trap you.

Apps
App	What it does
Shell	pid 0. Scrolling menu. Home always returns here
Clock	Up/down sets hours, left/right sets minutes, click resets
SysMon	Live free SRAM with a bar graph, scheduler passes per second, uptime
Tasks	The actual task table — pid, name, state
Snake	16×8 grid. Proof the scheduler handles a real-time loop
Simon	Repeat the arrow sequence. Grows by one each round
Draw	32×16 sketchpad. The whole canvas is a 64-byte bitmap
Stopwatch	Click starts and stops, left resets
Scope	Rolling plot of the voltage on A2. Left/right change the sample rate
Pins	Drives D3–D9 high or low by hand. A hardware tester with no reflashing
Settings	Contrast, joystick deadzone, Y-axis flip, 180° rotation — all live
Heartbeat	Hidden background task. Blinks pin 13 forever

Open Tasks and watch Heartbeat flip between R and S twice a second while you're in a different app. That's the whole point of the OS, visible in one place.

How it works
loop()
 └── kernel_run()            one scheduler pass
      ├── focus handover     EV_FOCUS to a task that just gained the screen
      ├── input_poll()       joystick -> event -> focused task
      ├── round robin        every READY task's onLoop(), once each
      └── every 50 ms        focused task's onDraw(), once per page

Each task is 16 bytes: five function pointers, a wake time, a state and flags. Task names live in flash via PROGMEM, so they cost no SRAM at all.

The one rule

onDraw() is called once per 8-pixel page — roughly eight times per frame, because the display renders in horizontal bands to save memory. Never change state inside it. Logic goes in onLoop(), pixels go in onDraw(). Breaking this produces bugs that look exactly like hardware faults.

Cooperative, not preemptive

A task that blocks blocks everything. Never call delay() inside a task — call task_sleep(ms), which marks the task sleeping and hands the processor straight back to the scheduler.

Writing your own app

Three functions and one line:

cpp
const char nMine[] PROGMEM = "MyApp";
static uint8_t counter = 0;

static void mineLoop()  { counter++; task_sleep(100); }
static void mineDraw()  { char b[16];
                          snprintf(b, sizeof(b), "n=%u", counter);
                          oled.setFont(u8g2_font_6x10_tf);
                          oled.drawStr(4, 30, b); }
static void mineEvent(uint8_t ev) { if (ev == EV_CLICK) counter = 0; }

Then inside apps_register():

cpp
kernel_spawn(nMine, NULL, mineLoop, mineDraw, mineEvent, TF_NONE);

It appears in the Shell menu automatically.

Troubleshooting
Symptom	Cause
Blank screen	Wrong I²C address. Most panels are 0x3C; run an I²C scanner. Or SDA/SCL swapped
Garbled or offset display	You have an SH1106 (1.3"), not an SSD1306. Swap the constructor
Up and down reversed	Flip Y in Settings, or set JOY_INVERT_Y to 0
Menu scrolls by itself	Joystick isn't centred. Raise the deadzone in Settings
Button does nothing	SW on the wrong pin. It's active low with an internal pull-up
Random resets or garbage text	SRAM exhaustion. Check SysMon — under ~200 B free is the danger zone
Roadmap
Preemptive switching via a Timer1 ISR and per-task stacks
A filesystem in the 328P's 1 KB of EEPROM, for saved settings and scores
Message passing between tasks instead of shared globals
SLEEP_MODE_IDLE when every task is asleep
License

MIT — see LICENSE. Do what you like with it; keep the copyright notice.

Not affiliated with Arduino or Elegoo.
