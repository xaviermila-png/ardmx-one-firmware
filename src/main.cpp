/*
  ARDMX One — firmware ESP32
  Controlador DMX512 d'una única escena estàtica (sense àudio, sense cicles,
  sense múltiples escenes), controlat per Bluetooth Classic (SPP) des de la
  mateixa app Flutter que ja controla l'ARDMX4.

  Reutilitza el mateix protocol de trames `!Vxx=valor$` / `!Vxx=?$` que ja fa
  servir l'app per l'ARDMX4 (índexs sempre a 2 dígits), però només implementa
  el subconjunt d'índexs que té sentit sense escenes/cicle/música:
    V01-V03  valor actual (0-255) dels 3 canals "visibles"
    V04-V06  número de canal DMX (1-510) seleccionat per a cada un d'ells
    V07      avança/retrocedeix de grup de 3 canals (-1/0/+1)
    V62      versió de firmware (text), demanada per l'app en connectar

  La resta d'índexs del mapa V[] de l'ARDMX4 (música, cicle, escenes,
  transicions, reset, etc.) no s'implementen — no tenen sentit en aquest
  maquinari i simplement s'ignoren si arriben.

  Identificació del dispositiu: NO es fa servir cap índex nou ni T62 amb un
  format especial — l'app distingeix l'ARDMX One de l'ARDMX4 pel NOM del
  dispositiu Bluetooth aparellat (veure BLUETOOTH_DEVICE_NAME més avall).
*/

// Arduino.h: funcions bàsiques (millis, digitalWrite, Serial, String, etc.)
#include <Arduino.h>
// esp_dmx: llibreria que genera i envia el senyal DMX512 per un UART de l'ESP32
#include <esp_dmx.h>
// BluetoothSerial: exposa el Bluetooth Classic (SPP) de l'ESP32 com si fos un Serial més
#include "BluetoothSerial.h"
// Preferences: llegeix/escriu dades a la memòria no volàtil (NVS) de l'ESP32
#include <Preferences.h>

// ---------------------------------------------------------------------------
// Configuració de maquinari
// ---------------------------------------------------------------------------

// DMX cap al MAX485 pels pins GPIO17 (TX)/GPIO16 (RX, no usat activament).
// El mòdul MAX485 té commutació de direcció automàtica per maquinari, per
// això no hi ha pin DE/RE: es passa -1 com a pin RTS (esp_dmx accepta -1 per
// dir "no gestionis aquest pin").
//
// S'utilitza el port lògic DMX_NUM_1 (no DMX_NUM_2) encaminat per la matriu
// de GPIO a aquests mateixos pins 17/16 — a l'ESP32 el número de port UART no
// està lligat a cap pin fix, es pot enrutar per la matriu de GPIO a qualsevol
// pin. DMX_NUM_2 provoca un crash en temps real (Guru Meditation Error /
// LoadProhibited dins `dmx_uart_init`/`uart_ll_set_sclk`, confirmat en
// maquinari) amb la combinació de versions esp_dmx 4.1.0 + aquest framework
// Arduino-ESP32 — DMX_NUM_1 és el port que fa servir el propi exemple oficial
// de la llibreria i funciona correctament.
constexpr dmx_port_t DMX_PORT = DMX_NUM_1;  // "port" lògic UART que farem servir per al DMX
constexpr int DMX_TX_PIN = 17;              // pin físic per on surten les dades DMX cap al MAX485
constexpr int DMX_RX_PIN = 16;              // pin físic d'entrada (no s'usa, DMX només surt)
constexpr int DMX_RTS_PIN = -1;             // -1 = no gestionar cap pin de direcció (auto)

// LED d'estat: encès fix amb client Bluetooth connectat, parpellejant si no.
constexpr int STATUS_LED_PIN = 2;       // pin on va connectat el LED indicador
constexpr uint32_t LED_BLINK_MS = 500;  // cada quants ms canvia d'estat el parpelleig

// Univers DMX complet (1-512). Les 3 "finestres" de canal que gestiona la
// pantalla de l'app només poden apuntar a un grup de 3 dins de 1-510 (512 no
// és múltiple de 3); els canals 511/512 queden fora d'aquesta UI però igualment
// es transmeten (amb el seu últim valor desat, per defecte 0).
constexpr int MAX_DMX_CHANNEL = 512;      // nombre total de canals DMX que s'envien
constexpr int MAX_MANAGED_CHANNEL = 510;  // fins a quin canal es pot arribar amb els sliders

// Guardat a NVS: no es desa a cada canvi de canal (desgastaria la flash), es
// desa com a màxim un cop transcorregut aquest temps des de l'últim canvi.
constexpr uint32_t SAVE_DEBOUNCE_MS = 3000;  // temps d'inactivitat abans de desar

const char *BLUETOOTH_DEVICE_NAME = "ARDMXOne_001";  // nom amb què es veu al mòbil en aparellar
const char *FIRMWARE_VERSION_TEXT = "ARDMX One v1.0";  // text que es respon a la petició V62

// ---------------------------------------------------------------------------
// Estat global
// ---------------------------------------------------------------------------

BluetoothSerial SerialBT;  // objecte que gestiona la connexió Bluetooth Classic (SPP)
Preferences prefs;         // objecte que gestiona la lectura/escriptura a la NVS

// Índex 0 = start code DMX (sempre 0). Índexs 1..512 = valors dels canals.
uint8_t dmxData[DMX_PACKET_SIZE];  // buffer amb TOT l'univers DMX que s'envia cada cicle

// Números de canal DMX (1-based) actualment seleccionats per als 3 sliders
// visibles a la pantalla de l'app (V01-V03 / V04-V06).
int selectedChannel[3] = {1, 2, 3};  // per defecte, els sliders apunten als canals 1, 2 i 3

bool sceneDirty = false;        // true = hi ha canvis pendents de desar a la NVS
uint32_t lastChangeMillis = 0;  // instant (millis()) de l'últim canvi de valor

String btFrameBuffer;  // acumula els caràcters d'una trama Bluetooth mentre arriba

// ---------------------------------------------------------------------------
// DMX
// ---------------------------------------------------------------------------

// Configura i arrenca el controlador DMX de la llibreria esp_dmx.
void dmxInit() {
  dmx_config_t config = DMX_CONFIG_DEFAULT;   // configuració estàndard de DMX (framerate, etc.)
  dmx_personality_t personalities[] = {};     // no fem servir "personalitats" RDM, llista buida
  dmx_driver_install(DMX_PORT, &config, personalities, 0);  // crea el driver DMX intern
  dmx_set_pin(DMX_PORT, DMX_TX_PIN, DMX_RX_PIN, DMX_RTS_PIN);  // assigna els pins físics al driver
}

// Envia UNA trama DMX completa (els 512 canals de dmxData) i espera que acabi de sortir.
void dmxSendFrame() {
  dmx_write(DMX_PORT, dmxData, DMX_PACKET_SIZE);       // copia dmxData al buffer intern del driver
  dmx_send_num(DMX_PORT, DMX_PACKET_SIZE);             // comença a transmetre'l pel cable
  dmx_wait_sent(DMX_PORT, DMX_TIMEOUT_TICK);           // bloqueja fins que la trama ha sortit del tot
}

// ---------------------------------------------------------------------------
// Persistència (NVS) — desa/recupera l'escena completa (canals 1..512)
// ---------------------------------------------------------------------------

// Es crida un cop, a l'arrencada: recupera l'última escena desada (o la deixa a 0 si no n'hi ha cap).
void sceneLoad() {
  // Obert en mode lectura/escriptura (no només lectura): la primera vegada
  // que arrenca el dispositiu el namespace "ardmxone" encara no existeix a la
  // NVS, i obrir-lo en mode només-lectura fallaria (nvs_open NOT_FOUND),
  // deixant l'objecte Preferences en un estat invàlid.
  prefs.begin("ardmxone", false);  // obre (o crea) l'espai de NVS anomenat "ardmxone"

  // A més, cridar getBytes() directament quan la clau "scene" encara no
  // existeix (primer arrencada, mai desada) provoca un crash dins la pròpia
  // llibreria Preferences d'aquest core d'Arduino-ESP32 (Guru Meditation
  // Error / LoadProhibited, confirmat en maquinari real) — cal comprovar
  // isKey() abans de llegir-la.
  if (prefs.isKey("scene")) {  // ¿ja hi ha una escena desada d'un cop anterior?
    // Llegeix els 512 bytes desats directament dins dmxData (a partir de l'índex 1)
    size_t bytesRead = prefs.getBytes("scene", &dmxData[1], MAX_DMX_CHANNEL);
    if (bytesRead != MAX_DMX_CHANNEL) {       // si la lectura no ha portat les dades esperades...
      memset(&dmxData[1], 0, MAX_DMX_CHANNEL);  // ...es descarta i es comença de zero (tot apagat)
    }
  } else {
    memset(&dmxData[1], 0, MAX_DMX_CHANNEL);  // primera vegada: tots els canals a 0 (apagat)
  }

  prefs.end();          // tanca l'accés a la NVS
  dmxData[0] = 0;        // el primer byte del paquet DMX és sempre el "start code" (0)
}

// Es crida quan cal desar l'escena actual a la NVS (des del debounce del loop()).
void sceneSave() {
  prefs.begin("ardmxone", false);                       // obre l'espai de NVS en escriptura
  prefs.putBytes("scene", &dmxData[1], MAX_DMX_CHANNEL);  // escriu els 512 valors de canal
  prefs.end();                                          // tanca l'accés a la NVS
  sceneDirty = false;                                   // ja no queden canvis pendents de desar
}

// Marca que hi ha un canvi pendent i reinicia el comptador de temps del debounce.
void markDirty() {
  sceneDirty = true;
  lastChangeMillis = millis();  // guarda "ara" com a últim instant de canvi
}

// ---------------------------------------------------------------------------
// Selecció de grup de 3 canals (V04-V06 / V07)
// ---------------------------------------------------------------------------

// Alinea un número de canal (1-based) a l'inici del seu grup de 3 (1,4,7,...).
int groupStart(int channel) {
  return ((channel - 1) / 3) * 3 + 1;  // p.ex. canal 5 -> grup que comença al canal 4
}

// Assigna els 3 canals seleccionats a partir d'un canal inicial (startChannel, startChannel+1, +2).
void selectGroup(int startChannel) {
  for (int i = 0; i < 3; i++) selectedChannel[i] = startChannel + i;
}

// Mou la selecció actual un grup de 3 canals endavant (+1) o enrere (-1), amb "volta" als límits.
void advanceGroup(int direction) {
  const int totalGroups = MAX_MANAGED_CHANNEL / 3;               // quants grups de 3 hi ha en total
  const int currentGroup = (groupStart(selectedChannel[0]) - 1) / 3;  // grup actual (0-based)
  // suma la direcció i fa mòdul perquè, si es passa pels dos costats, torni a l'altre extrem
  const int nextGroup = ((currentGroup + direction) % totalGroups + totalGroups) % totalGroups;
  selectGroup(nextGroup * 3 + 1);  // aplica el nou grup (torna a 1-based)
}

// ---------------------------------------------------------------------------
// Protocol `!Vxx=valor$` / `!Vxx=?$`
// ---------------------------------------------------------------------------

// Envia per Bluetooth la resposta a una petició numèrica, p.ex. "!V04=7$".
void replyNumber(int index, long value) {
  SerialBT.print('!');
  SerialBT.print('V');
  if (index < 10) SerialBT.print('0');  // els índexs d'1 xifra s'envien sempre amb 2 dígits
  SerialBT.print(index);
  SerialBT.print('=');
  SerialBT.print(value);
  SerialBT.print('$');
}

// Igual que replyNumber() però pel valor de text (només s'usa per V62, la versió de firmware).
void replyText(int index, const char *text) {
  SerialBT.print('!');
  SerialBT.print('V');
  if (index < 10) SerialBT.print('0');
  SerialBT.print(index);
  SerialBT.print('=');
  SerialBT.print(text);
  SerialBT.print('$');
}

// S'executa quan arriba una escriptura "!Vxx=valor$" (valor diferent de "?").
void handleWrite(int index, long value) {
  switch (index) {
    case 1:
    case 2:
    case 3: {
      // V01/V02/V03: canvia el valor (0-255) d'un dels 3 canals actualment seleccionats
      const int slot = index - 1;                  // 0, 1 o 2 (posició dins selectedChannel)
      const int channel = selectedChannel[slot];    // a quin canal DMX real correspon
      const uint8_t newValue = (uint8_t)constrain(value, 0, 255);  // limita sempre a 0-255
      if (dmxData[channel] != newValue) {           // només actua si el valor REALMENT canvia
        dmxData[channel] = newValue;                // aplica el nou valor al buffer DMX
        markDirty();                                // marca que cal desar-ho més endavant
      }
      break;
    }
    case 4:
    case 5:
    case 6: {
      // V04/V05/V06: l'app tria un nou canal DMX per a un dels 3 sliders
      const int channel = constrain((int)value, 1, MAX_MANAGED_CHANNEL);  // dins de rang vàlid
      selectGroup(groupStart(channel));  // alinea tot el grup de 3 a partir d'aquest canal
      break;
    }
    case 7:
      // V07: l'app demana avançar (+1) o retrocedir (-1) al grup de 3 canals següent/anterior
      if (value > 0) advanceGroup(1);
      else if (value < 0) advanceGroup(-1);
      break;
    default:
      // Índexs de música/cicle/escenes/reset de l'ARDMX4 no s'implementen aquí.
      break;
  }
}

// S'executa quan arriba una petició de lectura "!Vxx=?$".
void handleRequest(int index) {
  switch (index) {
    case 1:
    case 2:
    case 3:
      // Retorna el valor DMX actual del canal seleccionat en aquest slot
      replyNumber(index, dmxData[selectedChannel[index - 1]]);
      break;
    case 4:
    case 5:
    case 6:
      // Retorna quin número de canal DMX té assignat aquest slot ara mateix
      replyNumber(index, selectedChannel[index - 4]);
      break;
    case 62:
      // Retorna el text de versió de firmware
      replyText(62, FIRMWARE_VERSION_TEXT);
      break;
    default:
      // Qualsevol altre índex sol·licitat (V09, V11, V50, etc.) es queda sense resposta a propòsit
      break;
  }
}

// Processa el contingut d'una trama ja aïllada entre '!' i '$' (p.ex. "V04=7").
void processFrame(const String &body) {
  if (body.length() < 2 || body[0] != 'V') return;  // només interessen trames que comencen per "V"
  const int eq = body.indexOf('=');                 // busca la posició del signe "="
  if (eq < 2) return;                               // trama mal formada, es descarta

  const int index = body.substring(1, eq).toInt();  // el número entre "V" i "=" -> índex (p.ex. 4)
  const String rhs = body.substring(eq + 1);         // tot el que hi ha després del "=" -> valor

  if (rhs == "?") {
    handleRequest(index);       // és una petició de lectura
  } else {
    handleWrite(index, rhs.toInt());  // és una escriptura amb un valor numèric
  }
}

// Llegeix tots els bytes disponibles del Bluetooth i en va extraient trames completes.
void pollBluetooth() {
  while (SerialBT.available()) {         // mentre quedin bytes per llegir...
    const char c = (char)SerialBT.read();  // llegeix un caràcter
    if (c == '!') {
      btFrameBuffer = "";  // '!' marca l'inici d'una trama nova: descarta qualsevol residu previ
    } else if (c == '$') {
      processFrame(btFrameBuffer);  // '$' marca el final: processa tot el que s'ha acumulat
      btFrameBuffer = "";
    } else {
      btFrameBuffer += c;  // qualsevol altre caràcter: forma part del contingut de la trama
      if (btFrameBuffer.length() > 32) btFrameBuffer = "";  // salvaguarda si mai arriba soroll
    }
  }
}

// ---------------------------------------------------------------------------
// LED d'estat
// ---------------------------------------------------------------------------

// Actualitza el LED: fix si hi ha un mòbil connectat, parpellejant si no n'hi ha cap.
void updateStatusLed() {
  static uint32_t lastToggle = 0;  // recorda entre crides quan va canviar per última vegada
  static bool ledOn = false;       // recorda entre crides si el LED està encès ara mateix

  if (SerialBT.hasClient()) {          // ¿hi ha algun dispositiu Bluetooth connectat ara?
    digitalWrite(STATUS_LED_PIN, HIGH);  // sí: LED sempre encès
    return;
  }

  // no hi ha ningú connectat: fem parpellejar el LED cada LED_BLINK_MS mil·lisegons
  const uint32_t now = millis();
  if (now - lastToggle >= LED_BLINK_MS) {
    ledOn = !ledOn;                                    // inverteix l'estat (encès <-> apagat)
    digitalWrite(STATUS_LED_PIN, ledOn ? HIGH : LOW);
    lastToggle = now;
  }
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------

// S'executa un únic cop quan arrenca l'ESP32.
void setup() {
  // UART0 (USB) queda lliure per debug — no interfereix amb el DMX (UART2)
  // ni amb el Bluetooth Classic (que fa servir el controlador BT intern).
  Serial.begin(115200);            // engega el port sèrie de debug (per USB)
  pinMode(STATUS_LED_PIN, OUTPUT);  // configura el pin del LED com a sortida

  sceneLoad();                      // recupera l'última escena desada (o zeros)
  dmxInit();                        // engega el driver DMX
  SerialBT.begin(BLUETOOTH_DEVICE_NAME);  // engega el Bluetooth amb el nom del dispositiu

  Serial.println("ARDMX One iniciat");  // confirma per Serial que l'arrencada ha anat bé
}

// S'executa contínuament, un cop rere l'altre, mentre l'ESP32 estigui engegat.
void loop() {
  pollBluetooth();     // llegeix i processa qualsevol trama Bluetooth pendent
  updateStatusLed();   // actualitza l'estat del LED

  // Si hi ha canvis pendents (sceneDirty) i ja ha passat prou temps sense nous canvis, desa a NVS
  if (sceneDirty && millis() - lastChangeMillis > SAVE_DEBOUNCE_MS) {
    sceneSave();
    Serial.println("Escena desada a NVS");
  }

  dmxSendFrame();  // envia sempre una trama DMX completa a cada volta del bucle
}
