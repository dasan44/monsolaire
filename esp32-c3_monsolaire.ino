#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Preferences.h>   // mémoire flash : conserve le point de départ du cumul du jour
#include <Arduino_GFX_Library.h>
#include <PNGdec.h>          // bibliothèque "PNGdec" de Larry Bank (gestionnaire de bibliothèques)

// ======== CONFIGURATION ========
const char* WIFI_SSID   = "";
const char* WIFI_PASS   = "";
const char* SHELLY_IP   = "";
const char* SHELLY_USER = "";
const char* SHELLY_PASS = "";

#define CH_GRID   0     // voie de la pince au Linky
#define CH_SOLAR  1     // voie de la pince solaire
#define GRID_SIGN 1     // mettre -1 si le réseau est négatif quand vous soutirez
#define NEUTRAL_W 10    // zone neutre : de -10 W à +10 W

// Météo : Bouguenais
#define LATITUDE   "47.178"
#define LONGITUDE  "-1.622"

// Heure : fuseau France (heure d'été / hiver automatique) + serveur NTP
const char* TIMEZONE   = "CET-1CEST,M3.5.0,M10.5.0/3";
const char* NTP_SERVER = "pool.ntp.org";

// Écrans, dans l'ordre d'affichage
enum Page { PAGE_ENERGY, PAGE_CUMUL, PAGE_WEATHER, PAGE_CLOCK, PAGE_POKEMON, NUM_PAGES };

// Pokémon tirés au hasard entre 1 et POKEMON_MAX (151 = 1re génération, jusqu'à 1025)
#define POKEMON_MAX 151

const unsigned long REFRESH_MS         = 2000;     // Shelly : 2 s
const unsigned long WEATHER_REFRESH_MS = 600000;   // météo : 10 min
// Durée d'affichage de chaque écran avant de passer au suivant (en ms), dans l'ordre :
// énergie, cumul, météo, horloge, Pokémon. Mettre 0 pour un écran = pas de défilement automatique.
const unsigned long PAGE_DURATION_MS[NUM_PAGES] = {15000, 10000, 10000, 10000, 10000};
// ===============================

#define TFT_BL 3
#define TP_SDA 4
#define TP_SCL 5
#define TP_RST 1
#define TP_ADDR 0x15

Arduino_DataBus *bus = new Arduino_ESP32SPI(2, 10, 6, 7, GFX_NOT_DEFINED);
Arduino_GFX *gfx = new Arduino_GC9A01(bus, GFX_NOT_DEFINED, 0, true);

// ---------- Données ----------
float g_grid = 0, g_solar = 0;

// Compteurs d'énergie lus sur le Shelly (en Wh, depuis sa mise en service)
float g_gridImportWh = 0, g_gridExportWh = 0, g_solarWh = 0;
// Valeurs des compteurs au début de la journée (pour calculer le cumul du jour)
float baseImport = 0, baseExport = 0, baseSolar = 0;
int   baseDay = -1;       // identifiant du jour de ces valeurs (-1 = pas encore connu)
bool  g_shellyOk = false;

struct Weather {
  bool  ok = false;
  float temp, feels, wind, tmin, tmax;
  int   humidity, code, rainProb, isDay;
  char  sunrise[8] = "--:--";
  char  sunset[8]  = "--:--";
} wx;

constexpr uint16_t RGB(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

uint16_t g_wxBg = 0;      // couleur de fond de l'écran météo (aussi utilisée pour le contour des nuages)
#define LABEL_COL 0xCEFF  // bleu très clair pour les petits libellés de l'écran météo

int  page = PAGE_ENERGY;   // écran affiché
bool needFrame = true;    // redessiner le cadre + la page

// ---------- Shelly (EM Gen1) ----------
bool readShelly(float &grid, float &solar) {
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(String("http://") + SHELLY_IP + "/status");
  if (strlen(SHELLY_PASS) > 0) http.setAuthorization(SHELLY_USER, SHELLY_PASS);

  JsonDocument filter, doc;
  for (int i = 0; i < 2; i++) {
    filter["emeters"][i]["power"] = true;
    filter["emeters"][i]["total"] = true;
    filter["emeters"][i]["total_returned"] = true;
  }

  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    ok = !deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  } else {
    Serial.printf("Shelly HTTP %d\n", code);
  }
  http.end();
  if (!ok) return false;

  grid  = (doc["emeters"][CH_GRID]["power"]  | 0.0f) * GRID_SIGN;
  solar = fabsf(doc["emeters"][CH_SOLAR]["power"] | 0.0f);

  // Compteurs d'énergie (Wh) : réseau soutiré / injecté, production solaire
  float gTot = doc["emeters"][CH_GRID]["total"]          | 0.0f;
  float gRet = doc["emeters"][CH_GRID]["total_returned"] | 0.0f;
  g_gridImportWh = (GRID_SIGN > 0) ? gTot : gRet;
  g_gridExportWh = (GRID_SIGN > 0) ? gRet : gTot;
  float sTot = doc["emeters"][CH_SOLAR]["total"]          | 0.0f;
  float sRet = doc["emeters"][CH_SOLAR]["total_returned"] | 0.0f;
  g_solarWh = fmaxf(sTot, sRet);          // la pince solaire compte la production dans un seul des deux sens
  updateDayBaseline();
  return true;
}

// Mémorise les compteurs au début de chaque journée : cumul du jour = compteur actuel - valeur au minuit.
// La base est enregistrée en mémoire flash pour survivre à un redémarrage dans la même journée.
void updateDayBaseline() {
  struct tm ti;
  if (!getLocalTime(&ti, 0)) return;               // heure pas encore synchronisée
  int day = ti.tm_year * 1000 + ti.tm_yday;
  if (day == baseDay) return;

  Preferences prefs;
  prefs.begin("cumul", false);
  if (baseDay == -1 && prefs.getInt("day", -1) == day) {    // redémarrage le même jour : on reprend la base
    baseImport = prefs.getFloat("imp", g_gridImportWh);
    baseExport = prefs.getFloat("exp", g_gridExportWh);
    baseSolar  = prefs.getFloat("sol", g_solarWh);
  } else {                                                  // nouveau jour : la base = compteurs actuels
    baseImport = g_gridImportWh;
    baseExport = g_gridExportWh;
    baseSolar  = g_solarWh;
    Serial.println("Nouveau jour : cumul remis a zero");
    prefs.putInt("day", day);
    prefs.putFloat("imp", baseImport);
    prefs.putFloat("exp", baseExport);
    prefs.putFloat("sol", baseSolar);
  }
  prefs.end();
  baseDay = day;
}

// ---------- Météo (Open-Meteo) ----------
bool fetchWeather() {
  WiFiClientSecure client;
  client.setInsecure();   // pas de vérification de certificat (données non sensibles)

  HTTPClient http;
  http.setTimeout(6000);
  String url = String("https://api.open-meteo.com/v1/forecast?latitude=") + LATITUDE +
               "&longitude=" + LONGITUDE +
               "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day"
               "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset"
               "&timezone=Europe%2FParis&forecast_days=1";
  if (!http.begin(client, url)) return false;

  int code = http.GET();
  if (code != 200) {
    Serial.printf("Meteo HTTP %d\n", code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;

  wx.temp     = doc["current"]["temperature_2m"]       | 0.0f;
  wx.feels    = doc["current"]["apparent_temperature"] | 0.0f;
  wx.humidity = doc["current"]["relative_humidity_2m"] | 0;
  wx.code     = doc["current"]["weather_code"]         | 0;
  wx.isDay    = doc["current"]["is_day"]               | 1;
  wx.wind     = doc["current"]["wind_speed_10m"]       | 0.0f;
  wx.tmax     = doc["daily"]["temperature_2m_max"][0]  | 0.0f;
  wx.tmin     = doc["daily"]["temperature_2m_min"][0]  | 0.0f;
  wx.rainProb = doc["daily"]["precipitation_probability_max"][0] | 0;

  // "2026-10-12T08:05" -> "08:05"
  const char *sr = doc["daily"]["sunrise"][0] | "";
  const char *ss = doc["daily"]["sunset"][0]  | "";
  if (strlen(sr) >= 16) { strncpy(wx.sunrise, sr + 11, 5); wx.sunrise[5] = 0; }
  if (strlen(ss) >= 16) { strncpy(wx.sunset,  ss + 11, 5); wx.sunset[5]  = 0; }

  // Pour tester une icône : décommentez et changez le code (95 orage, 61 pluie, 71 neige, 45 brouillard, 2 peu nuageux)
  // wx.code = 95;

  wx.ok = true;
  return true;
}

const char* weatherText(int c) {
  if (c == 0) return "Ciel degage";
  if (c == 1) return "Plutot degage";
  if (c == 2) return "Peu nuageux";
  if (c == 3) return "Couvert";
  if (c == 45 || c == 48) return "Brouillard";
  if (c >= 51 && c <= 57) return "Bruine";
  if (c >= 61 && c <= 67) return "Pluie";
  if (c >= 71 && c <= 77) return "Neige";
  if (c >= 80 && c <= 82) return "Averses";
  if (c == 85 || c == 86) return "Averses de neige";
  if (c >= 95) return "Orage";
  return "---";
}

uint16_t weatherColor(int c) {
  if (c <= 1) return RGB565_YELLOW;
  if (c <= 3 || c == 45 || c == 48) return RGB565_LIGHTGREY;
  if (c >= 95) return RGB565_MAGENTA;
  if (c >= 71 && c <= 77) return RGB565_WHITE;
  return RGB565_CYAN;   // pluie, bruine, averses
}

// ---------- Outils d'affichage ----------
void centerText(int y, const String &txt, uint8_t size, uint16_t color) {
  int16_t x1, y1; uint16_t w, h;
  gfx->setTextSize(size);
  gfx->getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  gfx->setTextColor(color);
  gfx->setCursor((240 - w) / 2, y);
  gfx->print(txt);
}

// Texte centré horizontalement autour de cx
void centerTextAt(int cx, int y, const String &txt, uint8_t size, uint16_t color) {
  int16_t x1, y1; uint16_t w, h;
  gfx->setTextSize(size);
  gfx->getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  gfx->setTextColor(color);
  gfx->setCursor(cx - (int)w / 2, y);
  gfx->print(txt);
}

// Puissance en valeur absolue : "307" + unité "W", ou "1.65" + unité "kW"
String fmtNum(float w, const char **unit) {
  w = fabsf(w);
  if (w >= 1000) { *unit = "kW"; return String(w / 1000.0, 2); }
  *unit = "W";
  return String((int)w);
}

// Assombrit une couleur RGB565 (pour les petites cases de l'écran météo)
uint16_t darken(uint16_t c) {
  int r = ((c >> 11) & 31) * 3 / 5, g = ((c >> 5) & 63) * 3 / 5, b = (c & 31) * 3 / 5;
  return (r << 11) | (g << 5) | b;
}

// Couleur de fond de l'écran météo selon le temps et jour/nuit
uint16_t weatherBg(int c, int isDay) {
  if (c <= 2 && !isDay)               return RGB(12, 24, 70);     // nuit
  if (c <= 1)                         return RGB(30, 110, 220);   // grand soleil
  if (c == 2)                         return RGB(50, 110, 200);   // peu nuageux
  if (c == 3)                         return RGB(90, 110, 140);   // couvert
  if (c == 45 || c == 48)             return RGB(110, 120, 130);  // brouillard
  if (c >= 95)                        return RGB(50, 40, 90);     // orage
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return RGB(110, 140, 180);   // neige
  return RGB(40, 70, 120);                                        // pluie
}

// Petites icônes 24x22 px : 0 = maison, 1 = compteur Linky
void drawIcon(int type, int x, int y, uint16_t c) {
  if (type == 0) {                                   // maison
    gfx->fillTriangle(x + 12, y, x, y + 10, x + 23, y + 10, c);
    gfx->fillRect(x + 3, y + 10, 18, 12, c);
    gfx->fillRect(x + 10, y + 14, 5, 8, RGB565_BLACK);   // porte
  } else {                                           // compteur Linky
    gfx->drawRoundRect(x + 2, y, 20, 22, 3, c);
    gfx->fillRect(x + 5, y + 4, 14, 6, c);               // écran
    gfx->fillCircle(x + 8, y + 16, 2, c);                // boutons
    gfx->fillCircle(x + 16, y + 16, 2, c);
  }
}

void drawFrame(uint16_t color) {
  gfx->fillScreen(RGB565_BLACK);
  gfx->drawCircle(120, 120, 118, color);
}

void drawMessage(const String &msg, uint16_t color) {
  drawFrame(color);
  centerText(110, msg, 2, color);
}

// Petits points en bas pour indiquer la page courante
void drawPageDots() {
  for (int i = 0; i < NUM_PAGES; i++) {
    int x = 120 - (NUM_PAGES - 1) * 8 + i * 16;
    gfx->fillCircle(x, 222, 3, page == i ? RGB565_WHITE : RGB565_DARKGREY);
  }
}

// ---------- Écran énergie (jauges en arc) ----------
#define MAX_SOLAR_W 3000    // production pour laquelle l'arc solaire est plein
#define MAX_HOUSE_W 5000    // consommation pour laquelle l'arc maison est plein
#define MAX_GRID_W  5000    // échange avec le réseau pour lequel l'arc du bas est plein
#define ARC_R_OUT   117
#define ARC_R_IN    108
#define TRACK_COLOR 0x3186  // gris foncé du fond des arcs

bool forceRedraw = true;    // true = tout redessiner (après un changement de page)

// Arc plein entre deux angles (en degrés : 0 = haut, sens horaire) et deux rayons, dessiné en petits quadrilatères
void fillArcBand(float a0, float a1, int rIn, int rOut, uint16_t color) {
  if (a1 <= a0) return;
  for (float a = a0; a < a1; a += 3.0f) {
    float b = a + 3.6f;                       // léger recouvrement pour éviter les jointures
    if (b > a1) b = a1;
    float ra = a * DEG_TO_RAD, rb = b * DEG_TO_RAD;
    int xo1 = 120 + lroundf(rOut * sinf(ra)), yo1 = 120 - lroundf(rOut * cosf(ra));
    int xi1 = 120 + lroundf(rIn  * sinf(ra)), yi1 = 120 - lroundf(rIn  * cosf(ra));
    int xo2 = 120 + lroundf(rOut * sinf(rb)), yo2 = 120 - lroundf(rOut * cosf(rb));
    int xi2 = 120 + lroundf(rIn  * sinf(rb)), yi2 = 120 - lroundf(rIn  * cosf(rb));
    gfx->fillTriangle(xo1, yo1, xi1, yi1, xo2, yo2, color);
    gfx->fillTriangle(xi1, yi1, xi2, yi2, xo2, yo2, color);
  }
}

// Arc des jauges de l'écran énergie
void fillArcSeg(float a0, float a1, uint16_t color) {
  fillArcBand(a0, a1, ARC_R_IN, ARC_R_OUT, color);
}

// Jauge : arc de a0 à a1, partie [lo, hi] colorée, le reste en gris. Ne redessine que si ça a changé.
void drawGauge(int id, float a0, float a1, float lo, float hi, uint16_t color) {
  static int key[3] = {-1, -1, -1};
  static uint16_t col[3] = {0, 0, 0};
  int k = (int)lo * 1000 + (int)hi;
  if (!forceRedraw && key[id] == k && col[id] == color) return;
  key[id] = k;
  col[id] = color;
  fillArcSeg(a0, lo, TRACK_COLOR);
  fillArcSeg(lo, hi, color);
  fillArcSeg(hi, a1, TRACK_COLOR);
}

// Remplissage 0..1 : racine carrée pour que les petites puissances restent visibles
float gaugeFrac(float v, float maxv) {
  float f = sqrtf(fabsf(v) / maxv);
  return f > 1.0f ? 1.0f : f;
}

// Bloc icône + valeur + libellé, centré en cx
void drawEnergyBlock(int cx, int iconType, const String &value, const String &label, uint16_t color) {
  gfx->fillRect(cx - 34, 108, 68, 62, RGB565_BLACK);
  drawIcon(iconType, cx - 12, 110, color);
  centerTextAt(cx, 138, value, 2, color);
  centerTextAt(cx, 158, label, 1, RGB565_LIGHTGREY);
}

// dir > 0 : flèche vers le haut (je soutire) ; dir < 0 : vers le bas (j'injecte) ; 0 : tiret
void drawArrow(int dir, uint16_t c) {
  gfx->fillRect(100, 176, 40, 28, RGB565_BLACK);
  if (dir > 0) {
    gfx->fillTriangle(120, 178, 108, 192, 132, 192, c);
    gfx->fillRect(115, 192, 10, 10, c);
  } else if (dir < 0) {
    gfx->fillTriangle(120, 202, 108, 188, 132, 188, c);
    gfx->fillRect(115, 178, 10, 10, c);
  } else {
    gfx->fillRect(108, 188, 24, 5, c);
  }
}

void drawEnergy() {
  float house = g_solar + g_grid;
  if (house < 0) house = 0;

  uint16_t stateColor;
  const char *statusLabel;
  int dir;                                   // 1 = je soutire, -1 = j'injecte, 0 = équilibre
  if (g_grid > NEUTRAL_W)       { stateColor = RGB565_RED;    statusLabel = "CONSOMMATION EDF"; dir = 1; }
  else if (g_grid < -NEUTRAL_W) { stateColor = RGB565_GREEN;  statusLabel = "INJECTION EDF";    dir = -1; }
  else                          { stateColor = RGB565_YELLOW; statusLabel = "EQUILIBRE EDF";    dir = 0; }

  // --- Arcs ---
  float fHouse = gaugeFrac(house,  MAX_HOUSE_W);
  float fSolar = gaugeFrac(g_solar, MAX_SOLAR_W);
  float fGrid  = gaugeFrac(g_grid,  MAX_GRID_W);
  drawGauge(0, 215, 355, 215, 215 + fHouse * 140, stateColor);      // maison : à gauche, du bas vers le haut
  drawGauge(1, 5, 145, 5, 5 + fSolar * 140, RGB565_GREEN);          // solaire : à droite, du haut vers le bas
  float e = (dir > 0) ? 180 + fGrid * 25 : (dir < 0) ? 180 - fGrid * 25 : 180;
  drawGauge(2, 155, 205, fminf(180, e), fmaxf(180, e), stateColor); // réseau : en bas, soutirage à gauche, injection à droite

  // --- Soleil + production ---
  if (forceRedraw) drawSun(120, 36, 8);
  const char *u1;
  String solarVal = fmtNum(g_solar, &u1);
  gfx->fillRect(56, 56, 128, 34, RGB565_BLACK);
  centerText(58, solarVal, 4, RGB565_GREEN);
  gfx->fillRect(60, 94, 120, 10, RGB565_BLACK);
  centerText(95, String(u1) + " SOLAIRE", 1, RGB565_LIGHTGREY);

  // --- Maison (gauche) et compteur EDF (droite) ---
  const char *u2, *u3;
  String houseVal = fmtNum(house, &u2);
  String gridVal  = fmtNum(g_grid, &u3);
  drawEnergyBlock(62,  0, houseVal, String(u2) + " MAISON", stateColor);
  drawEnergyBlock(178, 1, gridVal,  String(u3) + " EDF",    stateColor);

  // --- Flèche + état du réseau ---
  drawArrow(dir, stateColor);
  gfx->fillRect(72, 206, 96, 10, RGB565_BLACK);
  centerText(207, statusLabel, 1, stateColor);

  // Petit cercle extérieur : rouge si le Linky est positif, vert s'il est négatif (jaune en équilibre)
  static uint16_t lastRing = 0;
  if (forceRedraw || lastRing != stateColor) {
    gfx->drawCircle(120, 120, 118, stateColor);
    lastRing = stateColor;
  }

  forceRedraw = false;
}

// ---------- Écran cumul de la journée (kWh) : bilan production / consommation ----------
// Deux colonnes empilées à la même échelle :
//   production = part consommée sur place (vert) + part injectée (cyan)
//   consommation = part couverte par le solaire (vert) + part soutirée au réseau (rouge)
#define COL_SOLAR     RGB565_GREEN
#define COL_HOUSE     RGB(255, 150, 0)    // orange
#define COL_IMPORT    RGB565_RED
#define COL_EXPORT    RGB(0, 200, 255)    // cyan

#define COL_W    48                 // largeur d'une colonne
#define COL_TOP  76                 // haut des colonnes
#define COL_H    94                 // hauteur des colonnes (échelle maximale)

// kWh avec la précision adaptée : 4.37 / 12.4 / 123
String fmtKwh(float kwh) {
  if (kwh < 10)  return String(kwh, 2);
  if (kwh < 100) return String(kwh, 1);
  return String((int)kwh);
}

// Colonne : piste sombre, segment du bas (v1) puis segment du haut (v2), à l'échelle maxv
void drawStackColumn(int cx, float v1, uint16_t c1, float v2, uint16_t c2, float maxv) {
  const int x = cx - COL_W / 2, base = COL_TOP + COL_H;
  gfx->fillRect(x - 1, COL_TOP - 1, COL_W + 2, COL_H + 2, RGB565_BLACK);
  gfx->fillRoundRect(x, COL_TOP, COL_W, COL_H, 7, TRACK_COLOR);

  int h1 = lroundf(COL_H * v1 / maxv);
  int h2 = lroundf(COL_H * v2 / maxv);
  if (h1 > COL_H) h1 = COL_H;
  int y = base;
  if (h1 > 1) {
    y -= h1;
    gfx->fillRoundRect(x, y, COL_W, h1, h1 / 2 < 5 ? h1 / 2 : 5, c1);
    if (h1 >= 12) centerTextAt(cx, y + h1 / 2 - 4, fmtKwh(v1), 1, RGB565_BLACK);
  }
  if (h2 > 1) {
    int gap = (h1 > 1) ? 2 : 0;                       // petit espace entre les deux segments
    int top = y - gap - h2;
    if (top < COL_TOP) { h2 -= COL_TOP - top; top = COL_TOP; }
    if (h2 > 1) {
      gfx->fillRoundRect(x, top, COL_W, h2, h2 / 2 < 5 ? h2 / 2 : 5, c2);
      if (h2 >= 12) centerTextAt(cx, top + h2 / 2 - 4, fmtKwh(v2), 1, RGB565_BLACK);
    }
  }
}

// "AUTONOMIE 70%" : libellé gris + valeur colorée, centré en cx (police 6 px de large)
void drawTagValue(int cx, int y, const String &tag, const String &val, uint16_t vcol) {
  gfx->setTextSize(1);
  int x0 = cx - ((int)(tag.length() + 1 + val.length()) * 6) / 2;
  gfx->setTextColor(RGB565_LIGHTGREY);
  gfx->setCursor(x0, y);
  gfx->print(tag);
  gfx->setTextColor(vcol);
  gfx->setCursor(x0 + ((int)tag.length() + 1) * 6, y);
  gfx->print(val);
}

void drawCumul() {
  float imp   = fmaxf(0, g_gridImportWh - baseImport) / 1000.0f;   // soutiré au réseau aujourd'hui
  float expo  = fmaxf(0, g_gridExportWh - baseExport) / 1000.0f;   // injecté au réseau aujourd'hui
  float sol   = fmaxf(0, g_solarWh - baseSolar) / 1000.0f;         // produit par les panneaux aujourd'hui
  float self  = fmaxf(0, sol - expo);                              // production consommée sur place
  float house = self + imp;                                        // consommation totale de la maison
  float maxv  = fmaxf(fmaxf(sol, house), 1.0f);                    // échelle commune des deux colonnes
  int autoconso = (sol   > 0.05f) ? lroundf(100 * self / sol)   : 0;   // % de la production utilisée sur place
  int autonomie = (house > 0.05f) ? lroundf(100 * self / house) : 0;   // % de la consommation couverte par le solaire

  // Ne redessine que si une valeur affichée a changé
  String sig = String(sol, 2) + "|" + String(house, 2) + "|" + String(imp, 2) + "|" + String(expo, 2);
  static String lastSig = "";
  if (!forceRedraw && sig == lastSig) return;
  lastSig = sig;

  // Éléments fixes : cercle, titre, libellés, ligne de base et légende
  if (forceRedraw) {
    gfx->drawCircle(120, 120, 118, RGB565_CYAN);
    centerText(14, "kWh AUJOURD'HUI", 1, LABEL_COL);
    centerTextAt(80,  62, "PRODUCTION",   1, RGB565_LIGHTGREY);
    centerTextAt(160, 62, "CONSOMMATION", 1, RGB565_LIGHTGREY);
    gfx->drawFastHLine(50, 172, 140, TRACK_COLOR);
    gfx->setTextSize(1);                                   // légende : carré de couleur + libellé
    gfx->setTextColor(RGB565_LIGHTGREY);
    gfx->fillRect(37, 181, 6, 6, COL_SOLAR);   gfx->setCursor(47, 180);   gfx->print("SOLAIRE");
    gfx->fillRect(97, 181, 6, 6, COL_IMPORT);  gfx->setCursor(107, 180);  gfx->print("RESEAU");
    gfx->fillRect(151, 181, 6, 6, COL_EXPORT); gfx->setCursor(161, 180);  gfx->print("INJECTE");
  }

  // Chiffres du haut
  gfx->fillRect(44, 34, 72, 26, RGB565_BLACK);
  gfx->fillRect(124, 34, 72, 26, RGB565_BLACK);
  centerTextAt(80,  34, fmtKwh(sol),   3, COL_SOLAR);
  centerTextAt(160, 34, fmtKwh(house), 3, COL_HOUSE);

  // Colonnes
  drawStackColumn(80,  self, COL_SOLAR, expo, COL_EXPORT, maxv);   // production : consommée sur place + injectée
  drawStackColumn(160, self, COL_SOLAR, imp,  COL_IMPORT, maxv);   // consommation : solaire + réseau

  // Pourcentages du bas
  gfx->fillRect(30, 192, 180, 10, RGB565_BLACK);
  drawTagValue(76,  192, "AUTOCONSO", String(autoconso) + "%", COL_SOLAR);
  drawTagValue(164, 192, "AUTONOMIE", String(autonomie) + "%", COL_SOLAR);

  forceRedraw = false;
}

// ---------- Icônes météo ----------
void drawSun(int cx, int cy, int r) {
  gfx->fillCircle(cx, cy, r, RGB565_YELLOW);
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4;
    int x1 = cx + cosf(a) * (r + 4),  y1 = cy + sinf(a) * (r + 4);
    int x2 = cx + cosf(a) * (r + 10), y2 = cy + sinf(a) * (r + 10);
    gfx->drawLine(x1, y1, x2, y2, RGB565_YELLOW);
    gfx->drawLine(x1 + 1, y1, x2 + 1, y2, RGB565_YELLOW);   // trait plus épais
  }
}

void drawMoon(int cx, int cy, int r) {
  gfx->fillCircle(cx, cy, r, RGB565_WHITE);
  gfx->fillCircle(cx + r / 2, cy - r / 3, r, g_wxBg);   // croissant (masque de la couleur du fond)
}

void cloudShape(int cx, int cy, int g, uint16_t c) {
  gfx->fillCircle(cx - 14, cy + 4, 10 + g, c);
  gfx->fillCircle(cx + 2,  cy - 4, 14 + g, c);
  gfx->fillCircle(cx + 16, cy + 4, 10 + g, c);
  gfx->fillRect(cx - 14, cy + 4 - g, 30, 10 + 2 * g, c);
}

// Nuage avec un petit contour de la couleur du fond pour le détacher de ce qui est derrière
void drawCloud(int cx, int cy, uint16_t color) {
  if (color == RGB565_LIGHTGREY) color = RGB565_WHITE;   // nuages blancs sur le fond bleu
  cloudShape(cx, cy, 2, g_wxBg);
  cloudShape(cx, cy, 0, color);
}

void drawWeatherIcon(int cx, int cy, int code, bool isDay) {
  if (code == 0) {                                   // ciel dégagé
    if (isDay) drawSun(cx, cy, 16); else drawMoon(cx, cy, 18);

  } else if (code <= 2) {                            // peu nuageux
    if (isDay) drawSun(cx - 12, cy - 10, 10); else drawMoon(cx - 12, cy - 10, 12);
    drawCloud(cx + 6, cy + 6, RGB565_LIGHTGREY);

  } else if (code == 3) {                            // couvert
    drawCloud(cx, cy, RGB565_LIGHTGREY);

  } else if (code == 45 || code == 48) {             // brouillard
    for (int i = 0; i < 4; i++)
      gfx->fillRoundRect(cx - 24 + (i % 2) * 6, cy - 14 + i * 10, 44, 4, 2, RGB565_LIGHTGREY);

  } else if (code >= 95) {                           // orage
    drawCloud(cx, cy - 8, RGB565_DARKGREY);
    gfx->fillTriangle(cx + 5, cy + 4,  cx - 5, cy + 17, cx + 1, cy + 17, RGB565_YELLOW);
    gfx->fillTriangle(cx - 1, cy + 13, cx + 7, cy + 13, cx - 3, cy + 28, RGB565_YELLOW);

  } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {   // neige
    drawCloud(cx, cy - 8, RGB565_LIGHTGREY);
    gfx->fillCircle(cx - 14, cy + 10, 2, RGB565_WHITE);
    gfx->fillCircle(cx,      cy + 16, 2, RGB565_WHITE);
    gfx->fillCircle(cx + 14, cy + 10, 2, RGB565_WHITE);
    gfx->fillCircle(cx - 7,  cy + 22, 2, RGB565_WHITE);
    gfx->fillCircle(cx + 7,  cy + 22, 2, RGB565_WHITE);

  } else {                                           // bruine, pluie, averses
    drawCloud(cx, cy - 8, RGB565_LIGHTGREY);
    for (int i = -1; i <= 1; i++) {
      int x = cx + i * 14;
      gfx->drawLine(x, cy + 8,  x - 4, cy + 20, RGB565_CYAN);
      gfx->drawLine(x + 1, cy + 8, x - 3, cy + 20, RGB565_CYAN);
    }
  }
}

// ---------- Écran météo ----------
// Heure en haut de l'écran météo (mise à jour chaque minute sans tout redessiner)
void drawWeatherTime() {
  struct tm ti;
  char hm[8];
  if (getLocalTime(&ti, 0)) snprintf(hm, sizeof(hm), "%02d:%02d", ti.tm_hour, ti.tm_min);
  else strcpy(hm, "--:--");
  gfx->fillRect(88, 12, 64, 18, g_wxBg);
  centerText(14, hm, 2, RGB565_WHITE);
}

void drawWeather() {
  if (!wx.ok) {
    centerText(105, "Meteo...", 2, RGB565_WHITE);
    return;
  }
  uint16_t col = weatherColor(wx.code);
  g_wxBg = weatherBg(wx.code, wx.isDay);
  gfx->fillCircle(120, 120, 119, g_wxBg);        // disque coloré selon le temps
  gfx->drawCircle(120, 120, 118, col);

  // Haut : heure, puis lever / coucher du soleil
  drawWeatherTime();
  centerText(33, String((char)24) + wx.sunrise + "   " + (char)25 + wx.sunset, 1, RGB565_WHITE);

  // Milieu : température extérieure | icône | humidité
  drawWeatherIcon(120, 72, wx.code, wx.isDay);
  centerTextAt(52, 60, String((int)roundf(wx.temp)) + (char)248, 4, RGB565_WHITE);
  centerTextAt(52, 98, "EXTERIEUR", 1, LABEL_COL);
  centerTextAt(188, 66, String(wx.humidity) + "%", 3, RGB565_WHITE);
  centerTextAt(188, 98, "HUMIDITE", 1, LABEL_COL);

  // Ciel
  centerText(112, weatherText(wx.code), 2, RGB565_WHITE);

  // Ressenti | vent
  centerTextAt(70, 136, String((int)roundf(wx.feels)) + (char)248, 2, RGB565_WHITE);
  centerTextAt(70, 156, "RESSENTI", 1, LABEL_COL);
  centerTextAt(170, 136, String((int)roundf(wx.wind)), 2, RGB565_WHITE);
  centerTextAt(170, 156, "VENT km/h", 1, LABEL_COL);

  // Bas : trois petites cases min / max / pluie
  uint16_t box = darken(g_wxBg);
  const char *labels[3] = {"MIN", "MAX", "PLUIE"};
  String vals[3] = {String((int)roundf(wx.tmin)) + (char)248, String((int)roundf(wx.tmax)) + (char)248,
                    String(wx.rainProb) + "%"};
  for (int i = 0; i < 3; i++) {
    int x = 41 + i * 54;
    gfx->fillRoundRect(x, 172, 50, 34, 7, box);
    centerTextAt(x + 25, 175, labels[i], 1, LABEL_COL);
    centerTextAt(x + 25, 186, vals[i], 2, RGB565_WHITE);
  }
}

// ---------- Écran horloge ----------
const char* JOURS[] = {"Dimanche", "Lundi", "Mardi", "Mercredi", "Jeudi", "Vendredi", "Samedi"};
const char* MOIS[]  = {"janvier", "fevrier", "mars", "avril", "mai", "juin", "juillet",
                       "aout", "septembre", "octobre", "novembre", "decembre"};

void drawClock() {
  struct tm ti;
  char hm[8], ss[4];
  String day, date;

  if (getLocalTime(&ti, 0)) {
    snprintf(hm, sizeof(hm), "%02d:%02d", ti.tm_hour, ti.tm_min);
    snprintf(ss, sizeof(ss), "%02d", ti.tm_sec);
    day  = JOURS[ti.tm_wday];
    date = String(ti.tm_mday) + " " + MOIS[ti.tm_mon] + " " + String(ti.tm_year + 1900);
  } else {                                   // heure pas encore synchronisée
    strcpy(hm, "--:--");
    strcpy(ss, "--");
    day = "Heure...";
  }

  gfx->fillRect(40, 44, 160, 22, RGB565_BLACK);
  centerText(46, day, 2, RGB565_CYAN);

  gfx->fillRect(20, 72, 200, 22, RGB565_BLACK);       // date complète, sous le jour
  centerText(74, date, 2, RGB565_YELLOW);

  gfx->fillRect(30, 106, 180, 52, RGB565_BLACK);       // heure en gros
  centerText(108, hm, 6, RGB565_WHITE);

  gfx->fillRect(90, 162, 60, 20, RGB565_BLACK);        // secondes
  centerText(164, ss, 2, RGB565_LIGHTGREY);

  gfx->drawCircle(120, 120, 118, RGB565_CYAN);   // même cercle fin que l'écran météo
}

// ---------- Écran Pokémon aléatoire (PokeAPI) ----------
struct TypeInfo { const char *en; const char *fr; uint16_t color; };
const TypeInfo TYPES[] = {
  {"normal", "Normal", RGB(168,168,120)}, {"fire", "Feu", RGB(240,128,48)},
  {"water", "Eau", RGB(104,144,240)},     {"electric", "Electrik", RGB(248,208,48)},
  {"grass", "Plante", RGB(120,200,80)},   {"ice", "Glace", RGB(152,216,216)},
  {"fighting", "Combat", RGB(192,48,40)}, {"poison", "Poison", RGB(160,64,160)},
  {"ground", "Sol", RGB(224,192,104)},    {"flying", "Vol", RGB(168,144,240)},
  {"psychic", "Psy", RGB(248,88,136)},    {"bug", "Insecte", RGB(168,184,32)},
  {"rock", "Roche", RGB(184,160,56)},     {"ghost", "Spectre", RGB(112,88,152)},
  {"dragon", "Dragon", RGB(112,56,248)},  {"dark", "Tenebres", RGB(112,88,72)},
  {"steel", "Acier", RGB(184,184,208)},   {"fairy", "Fee", RGB(238,153,172)}
};

struct Poke {
  bool valid = false;    // des données sont disponibles
  bool fresh = false;    // nouveau Pokémon pas encore affiché
  int  id = 0;
  String name, type1, type2;
  uint16_t color = RGB565_WHITE;
  uint8_t *png = nullptr;
  size_t pngLen = 0;
} pk;

PNG *pngDec = nullptr;

// Retire les accents (la police par défaut ne les gère pas)
String asciiName(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (c < 0x80) { o += (char)c; continue; }
    if (c == 0xC3 && i + 1 < s.length()) {
      uint8_t d = s[++i];
      bool up = d < 0xA0;
      if (up) d += 0x20;
      char r = 0;
      if (d >= 0xA0 && d <= 0xA5) r = 'a';
      else if (d == 0xA7) r = 'c';
      else if (d >= 0xA8 && d <= 0xAB) r = 'e';
      else if (d >= 0xAC && d <= 0xAF) r = 'i';
      else if (d >= 0xB2 && d <= 0xB6) r = 'o';
      else if (d >= 0xB9 && d <= 0xBC) r = 'u';
      if (r) o += up ? (char)toupper(r) : r;
    } else if (c == 0xE2 && i + 2 < s.length() && (uint8_t)s[i + 1] == 0x80 && (uint8_t)s[i + 2] == 0x99) {
      o += '\'';
      i += 2;
    } else if (c >= 0xE0) {
      i += 2;                      // autres symboles sur 3 octets (ex. signe femelle) : ignorés
    } else if (c >= 0xC0) {
      i += 1;
    }
  }
  return o;
}

// Renvoie l'indice du type dans TYPES, ou -1 (pas de struct dans la signature : l'IDE Arduino
// génère les prototypes avant la définition du struct, ce qui provoquait l'erreur)
int findTypeIndex(const String &en) {
  for (int i = 0; i < (int)(sizeof(TYPES) / sizeof(TYPES[0])); i++)
    if (en == TYPES[i].en) return i;
  return -1;
}

bool httpsGetJson(const String &url, JsonDocument &filter, JsonDocument &doc) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);            // pas de transfert "chunked" : lecture en flux possible
  http.setTimeout(8000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  bool ok = false;
  if (code == 200) ok = !deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  else Serial.printf("HTTP %d pour %s\n", code, url.c_str());
  http.end();
  return ok;
}

uint8_t *httpsGetBinary(const String &url, size_t &len) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(8000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) return nullptr;
  int code = http.GET();
  int sz = http.getSize();
  if (code != 200 || sz <= 0 || sz > 30000) { http.end(); return nullptr; }
  uint8_t *buf = (uint8_t *)malloc(sz);
  if (!buf) { http.end(); return nullptr; }
  WiFiClient *s = http.getStreamPtr();
  int got = 0;
  unsigned long t0 = millis();
  while (got < sz && millis() - t0 < 8000) {
    int n = s->readBytes(buf + got, sz - got);
    if (n > 0) got += n; else delay(10);
  }
  http.end();
  if (got != sz) { free(buf); return nullptr; }
  len = sz;
  return buf;
}

// Noms français des 151 premiers Pokémon (sans accents), intégrés : pas de requête réseau en plus
const char *FR_GEN1[151] = {
  "Bulbizarre", "Herbizarre", "Florizarre", "Salameche", "Reptincel", "Dracaufeu", "Carapuce", "Carabaffe",
  "Tortank", "Chenipan", "Chrysacier", "Papilusion", "Aspicot", "Coconfort", "Dardargnan", "Roucool",
  "Roucoups", "Roucarnage", "Rattata", "Rattatac", "Piafabec", "Rapasdepic", "Abo", "Arbok",
  "Pikachu", "Raichu", "Sabelette", "Sablaireau", "Nidoran F", "Nidorina", "Nidoqueen", "Nidoran M",
  "Nidorino", "Nidoking", "Melofee", "Melodelfe", "Goupix", "Feunard", "Rondoudou", "Grodoudou",
  "Nosferapti", "Nosferalto", "Mystherbe", "Ortide", "Rafflesia", "Paras", "Parasect", "Mimitoss",
  "Aeromite", "Taupiqueur", "Triopikeur", "Miaouss", "Persian", "Psykokwak", "Akwakwak", "Ferosinge",
  "Colossinge", "Caninos", "Arcanin", "Ptitard", "Tetarte", "Tartard", "Abra", "Kadabra",
  "Alakazam", "Machoc", "Machopeur", "Mackogneur", "Chetiflor", "Boustiflor", "Empiflor", "Tentacool",
  "Tentacruel", "Racaillou", "Gravalanch", "Grolem", "Ponyta", "Galopa", "Ramoloss", "Flagadoss",
  "Magneti", "Magneton", "Canarticho", "Doduo", "Dodrio", "Otaria", "Lamantine", "Tadmorv",
  "Grotadmorv", "Kokiyas", "Crustabri", "Fantominus", "Spectrum", "Ectoplasma", "Onix", "Soporifik",
  "Hypnomade", "Krabby", "Krabboss", "Voltorbe", "Electrode", "Noeunoeuf", "Noadkoko", "Osselait",
  "Ossatueur", "Kicklee", "Tygnon", "Excelangue", "Smogo", "Smogogo", "Rhinocorne", "Rhinoferos",
  "Leveinard", "Saquedeneu", "Kangourex", "Hypotrempe", "Hypocean", "Poissirene", "Poissoroy", "Stari",
  "Staross", "M. Mime", "Insecateur", "Lippoutou", "Elektek", "Magmar", "Scarabrute", "Tauros",
  "Magicarpe", "Leviator", "Lokhlass", "Metamorph", "Evoli", "Aquali", "Voltali", "Pyroli",
  "Porygon", "Amonita", "Amonistar", "Kabuto", "Kabutops", "Ptera", "Ronflex", "Artikodin",
  "Electhor", "Sulfura", "Minidraco", "Draco", "Dracolosse", "Mewtwo", "Mew"
};

// Télécharge un Pokémon au hasard : types, nom français et sprite
bool fetchPokemon() {
  int id = (int)(esp_random() % POKEMON_MAX) + 1;
  String t1, t2, name;

  // 1) types (petite réponse)
  {
    JsonDocument filter, doc;
    filter["name"] = true;
    filter["types"][0]["type"]["name"] = true;
    if (!httpsGetJson("https://pokeapi.co/api/v2/pokemon-form/" + String(id) + "/", filter, doc)) return false;
    name = doc["name"] | "";
    t1 = doc["types"][0]["type"]["name"] | "";
    t2 = doc["types"][1]["type"]["name"] | "";
    if (name.length()) name[0] = toupper(name[0]);
  }
  // 2) nom français : table intégrée pour les 151 premiers, sinon via l'API (nom anglais en cas d'échec)
  if (id <= 151) {
    name = FR_GEN1[id - 1];
  } else {
    JsonDocument filter, doc;
    filter["names"][0]["name"] = true;
    filter["names"][0]["language"]["name"] = true;
    if (httpsGetJson("https://pokeapi.co/api/v2/pokemon-species/" + String(id) + "/", filter, doc)) {
      for (JsonObject n : doc["names"].as<JsonArray>()) {
        if (strcmp(n["language"]["name"] | "", "fr") == 0) { name = String(n["name"] | ""); break; }
      }
    }
  }
  // 3) sprite fixe 96x96
  size_t len = 0;
  uint8_t *buf = httpsGetBinary(
      "https://raw.githubusercontent.com/PokeAPI/sprites/master/sprites/pokemon/" + String(id) + ".png", len);
  if (!buf) return false;

  if (pk.png) free(pk.png);
  pk.png = buf;
  pk.pngLen = len;
  pk.id = id;
  pk.name = asciiName(name);
  int a = findTypeIndex(t1), b = findTypeIndex(t2);
  pk.type1 = a >= 0 ? TYPES[a].fr : t1;
  pk.type2 = b >= 0 ? TYPES[b].fr : t2;
  pk.color = a >= 0 ? TYPES[a].color : RGB565_WHITE;
  pk.valid = true;
  pk.fresh = true;
  return true;
}

// Agrandit le sprite x2 (96 -> 192 px) et le dessine ligne par ligne
#define SPRITE_X 24
#define SPRITE_Y 6
static uint16_t srcLine[128], dstLine[192];

int pngDraw(PNGDRAW *p) {            // cette version de PNGdec attend un int (1 = continuer)
  if (p->iWidth > 128) return 0;
  pngDec->getLineAsRGB565(p, srcLine, PNG_RGB565_LITTLE_ENDIAN, 0x000000);
  int dw = p->iWidth * 2;
  for (int x = 0; x < dw; x++) dstLine[x] = srcLine[x / 2];
  for (int y = 2 * p->y; y < 2 * p->y + 2; y++)
    gfx->draw16bitRGBBitmap(SPRITE_X, SPRITE_Y + y, dstLine, dw, 1);
  return 1;
}

void drawPokemon() {
  if (!pk.valid) {
    centerText(100, "Pokemon...", 2, RGB565_WHITE);
    centerText(125, "chargement", 1, RGB565_LIGHTGREY);
    return;
  }

  pngDec = new PNG();
  if (pngDec->openRAM(pk.png, pk.pngLen, pngDraw) == PNG_SUCCESS) {
    pngDec->decode(NULL, 0);
    pngDec->close();
  }
  delete pngDec;
  pngDec = nullptr;

  centerText(186, pk.name, pk.name.length() <= 12 ? 2 : 1, RGB565_WHITE);
  char num[8];
  snprintf(num, sizeof(num), "#%03d  ", pk.id);
  String types = String(num) + (pk.type2.length() ? pk.type1 + " / " + pk.type2 : pk.type1);
  centerText(206, types, 1, pk.color);

  gfx->drawCircle(120, 120, 118, pk.color);   // même cercle fin que météo et horloge
  pk.fresh = false;                      // affiché : on pourra en préparer un autre
}

// ---------- Transition entre écrans ----------
// 1) "paupières" : deux rideaux noirs se rejoignent au centre et effacent l'écran
// 2) le nouvel écran est dessiné rétroéclairage éteint, puis apparaît en fondu
bool fadePending = false;    // true = rétroéclairage éteint, le fondu d'entrée reste à faire

void setBacklight(int v) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(TFT_BL, v);
#else
  ledcWrite(0, v);
#endif
}

void transitionOut() {
  for (int y = 0; y < 120; y += 8) {
    gfx->fillRect(0, y, 240, 8, RGB565_BLACK);          // rideau du haut
    gfx->fillRect(0, 232 - y, 240, 8, RGB565_BLACK);    // rideau du bas
    delay(6);
  }
  gfx->fillScreen(RGB565_BLACK);
  setBacklight(0);
  fadePending = true;
}

void fadeIn() {
  for (int b = 0; b <= 255; b += 15) { setBacklight(b); delay(12); }
  setBacklight(255);
}

// À appeler dès que la page est dessinée : lance le fondu si une transition est en cours
void showBacklight() {
  if (fadePending) { fadeIn(); fadePending = false; }
}

// ---------- Rendu de la page courante ----------
void render() {
  if (needFrame) {
    gfx->fillScreen(RGB565_BLACK);
    gfx->drawCircle(120, 120, 118, RGB565_DARKGREY);
    needFrame = false;
    forceRedraw = true;      // les arcs et les éléments fixes des écrans énergie / cumul doivent être redessinés
  }
  if (page == PAGE_ENERGY || page == PAGE_CUMUL) {
    if (!g_shellyOk) { drawMessage("Shelly injoignable", RGB565_RED); needFrame = true; }
    else if (page == PAGE_CUMUL && baseDay < 0) { drawMessage("Cumul en attente", RGB565_YELLOW); needFrame = true; }
    else if (page == PAGE_ENERGY) drawEnergy();
    else drawCumul();
  } else if (page == PAGE_WEATHER) {
    drawWeather();
  } else if (page == PAGE_CLOCK) {
    drawClock();
  } else {
    drawPokemon();
  }
  drawPageDots();
}

// Dessine la page courante, puis fait apparaître l'écran en fondu si on sort d'une transition
void renderPage() {
  render();
  showBacklight();
}

// ---------- Tactile CST816 ----------
bool touchPressed() {
  Wire.beginTransmission(TP_ADDR);
  Wire.write(0x02);                       // registre : nombre de doigts
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(TP_ADDR, 1) != 1) return false;
  return Wire.read() > 0;
}

// ---------- Animation de démarrage (style interface HUD) ----------
// Affichée une seule fois, au démarrage, pendant que le module se connecte au WiFi.
#define BOOT_MIN_MS 4500                       // durée minimale de l'animation
const char *BOOT_TITLE = "ALMADA";
const uint16_t HUD_CYAN  = RGB(0, 200, 255);
const uint16_t HUD_BLUE  = RGB(0, 110, 255);
const uint16_t HUD_LIGHT = RGB(170, 240, 255);
const uint16_t HUD_DIM   = RGB(0, 55, 85);

// Fait avancer un arc de d degrés en ne redessinant que ses extrémités (pas de scintillement)
void moveArc(int rIn, int rOut, float s, float len, float d, uint16_t col) {
  if (fabsf(d) >= len) {
    fillArcBand(s - 1, s + len + 1, rIn - 1, rOut + 1, RGB565_BLACK);
    fillArcBand(s + d, s + d + len, rIn, rOut, col);
  } else if (d > 0) {
    fillArcBand(s - 1, s + d + 1, rIn - 1, rOut + 1, RGB565_BLACK);
    fillArcBand(s + len, s + len + d + 0.5f, rIn, rOut, col);
  } else {
    fillArcBand(s + len + d - 1, s + len + 1, rIn - 1, rOut + 1, RGB565_BLACK);
    fillArcBand(s + d - 0.5f, s, rIn, rOut, col);
  }
}

void bootStatus(const char *msg, uint16_t col) {
  gfx->fillRect(56, 128, 128, 10, RGB565_BLACK);
  centerText(129, msg, 1, col);
}

void bootAnimation() {
  float aA[3] = {0, 120, 240};         // anneau extérieur : 3 arcs, sens horaire
  float aB[2] = {30, 210};             // anneau intermédiaire : 2 arcs, sens inverse
  float aC[4] = {0, 90, 180, 270};     // anneau intérieur : 4 arcs, sens horaire
  const float LEN_A = 60, LEN_B = 90, LEN_C = 30;
  const float D_A = 4, D_B = -6, D_C = 9;          // degrés par image

  gfx->fillScreen(RGB565_BLACK);

  // 1) graduations tracées une à une autour de l'écran
  for (int i = 0; i < 36; i++) {
    float a = i * 10 * DEG_TO_RAD;
    int r0 = (i % 3 == 0) ? 117 : 118;
    gfx->drawLine(120 + lroundf(r0 * sinf(a)),  120 - lroundf(r0 * cosf(a)),
                  120 + lroundf(119 * sinf(a)), 120 - lroundf(119 * cosf(a)),
                  (i % 3 == 0) ? HUD_LIGHT : HUD_CYAN);
    delay(14);
  }

  // 2) éléments fixes : cercle intérieur, piste de progression, réacteur, arcs de départ
  gfx->drawCircle(120, 120, 70, HUD_DIM);
  fillArcBand(0, 360, 74, 77, HUD_DIM);
  gfx->drawCircle(120, 72, 11, HUD_CYAN);
  gfx->drawCircle(120, 72, 7, HUD_BLUE);
  for (int i = 0; i < 3; i++) fillArcBand(aA[i], aA[i] + LEN_A, 110, 115, HUD_CYAN);
  for (int i = 0; i < 2; i++) fillArcBand(aB[i], aB[i] + LEN_B, 100, 104, HUD_BLUE);
  for (int i = 0; i < 4; i++) fillArcBand(aC[i], aC[i] + LEN_C, 88, 92, HUD_LIGHT);

  // 3) animation, jusqu'à la connexion WiFi (et au moins BOOT_MIN_MS)
  const char *lastMsg = "";
  int lastPct = -1, letters = 0;
  float prog = 0, shown = 0;                       // progression 0..1 et partie déjà tracée (en degrés)
  bool pulseOn = false;
  unsigned long t0 = millis(), pulse = 0;

  while (true) {
    unsigned long t = millis() - t0;
    bool wifiOk = (WiFi.status() == WL_CONNECTED);

    // arcs en rotation
    for (int i = 0; i < 3; i++) { moveArc(110, 115, aA[i], LEN_A, D_A, HUD_CYAN);  aA[i] = fmodf(aA[i] + D_A + 360, 360); }
    for (int i = 0; i < 2; i++) { moveArc(100, 104, aB[i], LEN_B, D_B, HUD_BLUE);  aB[i] = fmodf(aB[i] + D_B + 360, 360); }
    for (int i = 0; i < 4; i++) { moveArc(88, 92, aC[i], LEN_C, D_C, HUD_LIGHT);   aC[i] = fmodf(aC[i] + D_C + 360, 360); }

    // progression : suit le temps, plafonnée à 85 % tant que le WiFi n'est pas connecté
    float timeFrac = (t >= BOOT_MIN_MS) ? 1.0f : t / (float)BOOT_MIN_MS;
    float target = timeFrac * (wifiOk ? 1.0f : 0.85f);
    prog += (target - prog) * 0.15f;
    if (wifiOk && timeFrac >= 1.0f && prog > 0.985f) prog = 1.0f;
    float end = prog * 360;
    if (end > shown + 1 || (prog >= 1.0f && shown < 360)) {
      fillArcBand(shown, end, 74, 77, HUD_CYAN);
      shown = end;
    }
    int pct = (int)(prog * 100 + 0.5f);
    if (pct != lastPct) {
      lastPct = pct;
      gfx->fillRect(92, 142, 56, 18, RGB565_BLACK);
      centerText(143, String(pct) + "%", 2, HUD_CYAN);
    }

    // titre : une lettre à la fois
    int want = (t < 500) ? 0 : (int)((t - 500) / 150);
    if (want > 6) want = 6;
    while (letters < want) {
      gfx->setTextSize(3);
      gfx->setTextColor(HUD_LIGHT);
      gfx->setCursor(66 + letters * 18, 92);
      gfx->print(BOOT_TITLE[letters]);
      letters++;
    }

    // message d'état
    const char *msg;
    uint16_t mcol = HUD_CYAN;
    if (!wifiOk && t > 30000)  { msg = "WIFI INDISPONIBLE"; mcol = RGB565_RED; }
    else if (t < 1200)         msg = "INITIALISATION";
    else if (!wifiOk)          msg = "CONNEXION WIFI";
    else if (prog < 0.98f)     msg = "RESEAU CONNECTE";
    else                       { msg = "SYSTEME PRET"; mcol = RGB565_GREEN; }
    if (strcmp(msg, lastMsg) != 0) { lastMsg = msg; bootStatus(msg, mcol); }

    // cœur du réacteur qui pulse
    if (millis() - pulse > 280) {
      pulse = millis();
      pulseOn = !pulseOn;
      gfx->fillCircle(120, 72, 4, pulseOn ? HUD_LIGHT : HUD_BLUE);
    }

    if (wifiOk && prog >= 1.0f) break;
    delay(8);
  }
  delay(600);                                      // laisse lire "SYSTEME PRET"
}

// ---------- Setup / Loop ----------
unsigned long lastShelly = 0, lastWeather = 0, lastTouch = 0, lastSwitch = 0;

void setup() {
  Serial.begin(115200);
  // Rétroéclairage en PWM (pour le fondu entre les écrans)
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(TFT_BL, 5000, 8);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TFT_BL, 0);
#endif
  setBacklight(255);

  gfx->begin();
  gfx->fillScreen(RGB565_BLACK);

  // Tactile
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW);  delay(10);
  digitalWrite(TP_RST, HIGH); delay(50);
  Wire.begin(TP_SDA, TP_SCL);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  bootAnimation();                      // animation de démarrage, pendant la connexion au WiFi

  configTzTime(TIMEZONE, NTP_SERVER);   // heure via Internet, fuseau France

  // Fin de l'animation : rideaux, puis le premier écran (énergie) apparaît en fondu
  transitionOut();
  g_shellyOk = readShelly(g_grid, g_solar);
  lastShelly = millis();
  needFrame = true;
  renderPage();
  lastSwitch = millis();
}

bool wasTouched = false;

void loop() {
  unsigned long now = millis();
  bool redraw = false;

  // --- Tactile : un appui change de page ---
  if (now - lastTouch > 40) {
    lastTouch = now;
    bool touched = touchPressed();
    if (touched && !wasTouched) {
      transitionOut();
      page = (page + 1) % NUM_PAGES;
      needFrame = true;
      redraw = true;
      lastSwitch = now;
    }
    wasTouched = touched;
  }

  // --- Changement automatique ---
  if (PAGE_DURATION_MS[page] > 0 && now - lastSwitch > PAGE_DURATION_MS[page]) {
    transitionOut();
    page = (page + 1) % NUM_PAGES;
    needFrame = true;
    redraw = true;
    lastSwitch = now;
  }

  // --- WiFi ---
  if (WiFi.status() != WL_CONNECTED) {
    drawMessage("WiFi perdu", RGB565_RED);
    showBacklight();
    needFrame = true;
    WiFi.reconnect();
    delay(1000);
    return;
  }

  // --- Shelly ---
  if (now - lastShelly >= REFRESH_MS) {
    lastShelly = now;
    g_shellyOk = readShelly(g_grid, g_solar);
    if (g_shellyOk) Serial.printf("solaire=%.0f W  reseau=%.0f W\n", g_solar, g_grid);
    if (page == PAGE_ENERGY || page == PAGE_CUMUL) redraw = true;
  }

  // --- Météo (au démarrage, puis toutes les 10 min ; nouvel essai après 30 s en cas d'échec) ---
  if (lastWeather == 0 || now - lastWeather >= (wx.ok ? WEATHER_REFRESH_MS : 30000UL)) {
    lastWeather = now;
    if (fetchWeather()) Serial.printf("meteo: %.1f C, code %d\n", wx.temp, wx.code);
    if (page == PAGE_WEATHER) { needFrame = true; redraw = true; }
  }

  // --- Arrivée sur la page Pokémon : il faut toujours un Pokémon neuf ---
  static int lastPage = 0;
  if (page == PAGE_POKEMON && lastPage != PAGE_POKEMON && !pk.fresh) {
    // Pas de Pokémon prêt (changement de page rapide ou échec précédent) : on en cherche un tout de suite
    gfx->fillScreen(RGB565_BLACK);
    centerText(100, "Pokemon", 2, RGB565_WHITE);
    centerText(124, "chargement...", 1, RGB565_LIGHTGREY);
    drawPageDots();
    showBacklight();                          // l'écran de chargement apparaît tout de suite en fondu
    if (!fetchPokemon()) fetchPokemon();      // un 2e essai en cas d'échec
    needFrame = true;
    redraw = true;
  }
  lastPage = page;

  // --- Pokémon : on prépare le suivant quand on n'est pas sur sa page ---
  static unsigned long lastPokeTry = 0;
  if (!pk.fresh && (page != PAGE_POKEMON || !pk.valid) &&
      (lastPokeTry == 0 || now - lastPokeTry > 15000UL)) {
    if (redraw) { renderPage(); redraw = false; }   // afficher d'abord la page courante (le téléchargement dure quelques secondes)
    lastPokeTry = now;
    if (fetchPokemon()) {
      Serial.printf("pokemon #%d %s\n", pk.id, pk.name.c_str());
      if (page == PAGE_POKEMON) { needFrame = true; redraw = true; }
    }
  }

  // --- Horloge : redessinée à chaque changement de seconde ---
  static int lastSec = -1;
  struct tm ti;
  if (page == PAGE_CLOCK && getLocalTime(&ti, 0) && ti.tm_sec != lastSec) {
    lastSec = ti.tm_sec;
    redraw = true;
  }

  // --- Écran météo : l'heure en haut est mise à jour à chaque minute ---
  static int lastMin = -1;
  if (page == PAGE_WEATHER && wx.ok && getLocalTime(&ti, 0) && ti.tm_min != lastMin) {
    lastMin = ti.tm_min;
    drawWeatherTime();
  }

  if (redraw) renderPage();
}
