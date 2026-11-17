
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <vector>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <TJpg_Decoder.h>

// ---------- ПИНЫ ----------

// TFT ST7789
#define TFT_CS    10
#define TFT_RST   5
#define TFT_DC    7
#define TFT_MOSI  11
#define TFT_SCK   12
#define TFT_BL    38

// SD-карта
#define SD_CS     14
#define SD_MISO   13

// Кнопки: каждый контакт кнопки соединяется с GND
#define BTN_UP     1
#define BTN_DOWN   2
#define BTN_LEFT   4
#define BTN_RIGHT  6
#define BTN_A      8
#define BTN_B      9

SPIClass displaySPI(FSPI);
Adafruit_ST7789 tft(&displaySPI, TFT_CS, TFT_DC, TFT_RST);

const int SCREEN_W = 170;
const int SCREEN_H = 320;

const uint16_t BLACK  = 0x0000;
const uint16_t WHITE  = 0xFFFF;
const uint16_t BLUE   = 0x001F;
const uint16_t GREEN  = 0x07E0;
const uint16_t RED    = 0xF800;
const uint16_t YELLOW = 0xFFE0;
const uint16_t GREY   = 0x7BEF;

// ---------- КНОПКИ ----------

enum Key {
  NONE, UP, DOWN, LEFT, RIGHT, A, B
};

Key readKey() {
  if (!digitalRead(BTN_UP)) return UP;
  if (!digitalRead(BTN_DOWN)) return DOWN;
  if (!digitalRead(BTN_LEFT)) return LEFT;
  if (!digitalRead(BTN_RIGHT)) return RIGHT;
  if (!digitalRead(BTN_A)) return A;
  if (!digitalRead(BTN_B)) return B;
  return NONE;
}

Key waitKey() {
  while (readKey() == NONE) delay(10);
  Key k = readKey();
  delay(150);
  while (readKey() != NONE) delay(10);
  return k;
}

// ---------- SD-КАРТА ----------

bool sdReady = false;
std::vector<String> fileNames;

String lowerCase(String s) {
  s.toLowerCase();
  return s;
}

bool isTxt(String name) {
  return lowerCase(name).endsWith(".txt");
}

bool isAvi(String name) {
  return lowerCase(name).endsWith(".avi");
}

void scanFiles() {
  fileNames.clear();

  File root = SD.open("/");
  if (!root) return;

  File entry = root.openNextFile();

  while (entry) {
    if (!entry.isDirectory()) {
      String name = String(entry.name());

      if (isTxt(name) || isAvi(name)) {
        fileNames.push_back(name);
      }
    }

    entry.close();
    entry = root.openNextFile();
  }

  root.close();
}

// ---------- ЭКРАН ----------

void drawHeader(String title) {
  tft.fillScreen(BLACK);
  tft.setTextColor(WHITE);
  tft.setTextSize(2);
  tft.setCursor(5, 5);
  tft.println(title);
  tft.drawFastHLine(0, 30, SCREEN_W, BLUE);
}

// ---------- ПРОСМОТР TXT ----------

void readTextFile(String name) {
  File file = SD.open("/" + name, FILE_READ);

  if (!file) {
    drawHeader("ERROR");
    tft.setCursor(5, 50);
    tft.println("Cannot open");
    waitKey();
    return;
  }

  int page = 0;
  const int linesPerPage = 13;

  while (true) {
    file.seek(0);

    // Переходим к началу нужной страницы
    for (int i = 0; i < page * linesPerPage; i++) {
      if (!file.available()) break;
      file.readStringUntil('\n');
    }

    drawHeader("TEXT");
    tft.setTextSize(1);
    tft.setTextColor(WHITE);

    int y = 40;
    int count = 0;

    while (file.available() && count < linesPerPage) {
      String line = file.readStringUntil('\n');
      line.trim();

      tft.setCursor(3, y);
      tft.println(line);

      y += 20;
      count++;
    }

    tft.setTextColor(YELLOW);
    tft.setCursor(5, 300);
    tft.print("UP/DOWN page B exit");

    Key k = waitKey();

    if (k == B) break;
    if (k == DOWN && file.available()) page++;
    if (k == UP && page > 0) page--;
  }

  file.close();
}

// ---------- JPEG ДЛЯ ВИДЕО ----------

bool jpegOutput(int16_t x, int16_t y,
                uint16_t w, uint16_t h,
                uint16_t *bitmap) {
  tft.drawRGBBitmap(x, y, bitmap, w, h);
  return true;
}

uint32_t read32(File &f) {
  uint32_t a = f.read();
  uint32_t b = f.read();
  uint32_t c = f.read();
  uint32_t d = f.read();

  return a | (b << 8) | (c << 16) | (d << 24);
}

// Ищем начало области видеокадров AVI
bool findMovi(File &f) {
  f.seek(0);

  while (f.available() >= 4) {
    if (f.read() == 'm' &&
        f.read() == 'o' &&
        f.read() == 'v' &&
        f.read() == 'i') {
      return true;
    }
  }

  return false;
}

// ---------- AVI MJPEG PLAYER ----------

void playAvi(String name) {
  File video = SD.open("/" + name, FILE_READ);

  if (!video) {
    drawHeader("VIDEO ERROR");
    waitKey();
    return;
  }

  drawHeader("VIDEO");

  if (!findMovi(video)) {
    tft.setCursor(5, 60);
    tft.println("Invalid AVI");
    video.close();
    waitKey();
    return;
  }

  // Максимальный размер одного JPEG-кадра
  const size_t MAX_FRAME = 150000;

  uint8_t *buffer = (uint8_t *)ps_malloc(MAX_FRAME);

  if (!buffer) {
    buffer = (uint8_t *)malloc(MAX_FRAME);
  }

  if (!buffer) {
    tft.setCursor(5, 60);
    tft.println("No memory");
    video.close();
    waitKey();
    return;
  }

  TJpgDec.setJpgScale(1);
  TJpgDec.setCallback(jpegOutput);

  while (video.available()) {
    if (readKey() == B) break;

    char id[5] = {0};

    if (video.readBytes(id, 4) != 4) break;

    uint32_t size = read32(video);

    if (size > video.size() || size > MAX_FRAME) {
      break;
    }

    // В AVI MJPEG кадры обычно имеют ID 00dc или 00db
    bool isFrame =
      (id[0] == '0' && id[1] == '0' &&
       id[2] == 'd' &&
       (id[3] == 'c' || id[3] == 'b'));

    if (isFrame && size > 0) {
      size_t got = video.read(buffer, size);

      if (got == size) {
        TJpgDec.drawJpg(0, 0, buffer, size);
      }
    } else {
      video.seek(video.position() + size);
    }

    // AVI chunks выравниваются по 2 байтам
    if (size & 1) {
      video.seek(video.position() + 1);
    }

    delay(40);
  }

  free(buffer);
  video.close();
}

// ---------- ФАЙЛОВЫЙ БРАУЗЕР ----------

void fileBrowser() {
  scanFiles();

  if (fileNames.empty()) {
    drawHeader("SD CARD");
    tft.setCursor(5, 60);
    tft.println("No TXT or AVI");
    waitKey();
    return;
  }

  int selected = 0;

  while (true) {
    drawHeader("SD FILES");
    tft.setTextSize(1);

    for (int i = 0; i < (int)fileNames.size() && i < 12; i++) {
      tft.setCursor(5, 45 + i * 19);

      if (i == selected) {
        tft.setTextColor(YELLOW);
        tft.print("> ");
      } else {
        tft.setTextColor(WHITE);
        tft.print("  ");
      }

      tft.println(fileNames[i]);
    }

    tft.setTextColor(GREEN);
    tft.setCursor(5, 295);
    tft.println("A OPEN   B BACK");

    Key k = waitKey();

    if (k == B) return;

    if (k == UP && selected > 0) selected--;
    if (k == DOWN && selected < (int)fileNames.size() - 1) selected++;

    if (k == A) {
      String name = fileNames[selected];

      if (isTxt(name)) readTextFile(name);
      if (isAvi(name)) playAvi(name);
    }
  }
}

// ---------- ЗМЕЙКА ----------

void snakeGame() {
  const int CELL = 10;
  const int MAX = 150;

  int sx[MAX], sy[MAX];
  int length = 4;

  sx[0] = 8; sy[0] = 8;
  sx[1] = 7; sy[1] = 8;
  sx[2] = 6; sy[2] = 8;
  sx[3] = 5; sy[3] = 8;

  int dx = 1, dy = 0;
  int foodX = 12, foodY = 12;

  while (true) {
    Key k = readKey();

    if (k == B) break;
    if (k == UP && dy == 0)    { dx = 0; dy = -1; }
    if (k == DOWN && dy == 0)  { dx = 0; dy = 1; }
    if (k == LEFT && dx == 0)  { dx = -1; dy = 0; }
    if (k == RIGHT && dx == 0) { dx = 1; dy = 0; }

    int nx = sx[0] + dx;
    int ny = sy[0] + dy;

    if (nx < 0 || nx >= 17 || ny < 0 || ny >= 30) break;

    for (int i = 0; i < length; i++) {
      if (sx[i] == nx && sy[i] == ny) {
        nx = -1;
        break;
      }
    }

    if (nx < 0) break;

    bool ate = (nx == foodX && ny == foodY);

    if (ate && length < MAX) length++;

    for (int i = length - 1; i > 0; i--) {
      sx[i] = sx[i - 1];
      sy[i] = sy[i - 1];
    }

    sx[0] = nx;
    sy[0] = ny;

    if (ate) {
      foodX = random(1, 16);
      foodY = random(1, 29);
    }

    tft.fillScreen(BLACK);
    tft.fillRect(foodX * CELL, foodY * CELL, CELL, CELL, RED);

    for (int i = 0; i < length; i++) {
      tft.fillRect(sx[i] * CELL, sy[i] * CELL,
                   CELL - 1, CELL - 1, GREEN);
    }

    delay(140);
  }

  drawHeader("GAME OVER");
  tft.setCursor(5, 60);
  tft.println("Press any key");
  waitKey();
}

// ---------- PONG ----------

void pongGame() {
  int paddleY = 130;
  int ballX = 85, ballY = 150;
  int vx = 2, vy = 2;

  while (true) {
    Key k = readKey();

    if (k == B) break;
    if (k == UP) paddleY -= 6;
    if (k == DOWN) paddleY += 6;

    paddleY = constrain(paddleY, 0, 260);

    ballX += vx;
    ballY += vy;

    if (ballY <= 3 || ballY >= 316) vy = -vy;
    if (ballX >= 165) vx = -vx;

    if (ballX <= 12 &&
        ballY >= paddleY &&
        ballY <= paddleY + 60) {
      vx = abs(vx);
    }

    if (ballX < 0) {
      drawHeader("GAME OVER");
      waitKey();
      break;
    }

    tft.fillScreen(BLACK);
    tft.fillRect(3, paddleY, 6, 60, GREEN);
    tft.fillCircle(ballX, ballY, 4, WHITE);

    delay(20);
  }
}

// ---------- ГЛАВНОЕ МЕНЮ ----------

const char *menuItems[] = {
  "SD CARD",
  "SNAKE",
  "PONG"
};

const int MENU_COUNT = 3;

void mainMenu() {
  int selected = 0;

  while (true) {
    drawHeader("MINI CONSOLE");

    tft.setTextSize(2);

    for (int i = 0; i < MENU_COUNT; i++) {
      tft.setCursor(15, 65 + i * 45);

      if (i == selected) {
        tft.setTextColor(YELLOW);
        tft.print("> ");
      } else {
        tft.setTextColor(WHITE);
        tft.print("  ");
      }

      tft.println(menuItems[i]);
    }

    tft.setTextSize(1);
    tft.setTextColor(GREY);
    tft.setCursor(5, 295);

    if (sdReady) tft.println("SD: READY");
    else tft.println("SD: NOT FOUND");

    Key k = waitKey();

    if (k == UP && selected > 0) selected--;
    if (k == DOWN && selected < MENU_COUNT - 1) selected++;

    if (k == A) {
      if (selected == 0 && sdReady) fileBrowser();
      if (selected == 1) snakeGame();
      if (selected == 2) pongGame();
    }
  }
}

// ---------- SETUP ----------

void setup() {
  Serial.begin(115200);

  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_LEFT, INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT_PULLUP);
  pinMode(BTN_A, INPUT_PULLUP);
  pinMode(BTN_B, INPUT_PULLUP);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  displaySPI.begin(TFT_SCK, SD_MISO, TFT_MOSI, TFT_CS);

  tft.init(170, 320);
  tft.setRotation(0);
  tft.fillScreen(BLACK);

  tft.setTextColor(WHITE);
  tft.setTextSize(2);
  tft.setCursor(15, 100);
  tft.println("MINI CONSOLE");

  sdReady = SD.begin(SD_CS, displaySPI, 4000000);

  delay(1000);

  mainMenu();
}

void loop() {
}
