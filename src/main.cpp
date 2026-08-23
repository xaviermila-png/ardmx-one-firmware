/*
  ARDMX One — firmware ESP32
  Controlador DMX512 d'una única escena estàtica (sense àudio, sense cicles,
  sense múltiples escenes), controlat per BLE (GATT) des de la mateixa app
  Flutter que ja controla l'ARDMX4.

  Migrat de Bluetooth Classic (SPP) a BLE (2026-08): Classic no és accessible
  des d'apps de tercers a iOS sense certificació MFi, BLE sí (CoreBluetooth).
  El Mega original (ardmx4-firmware) NO es toca — es manté amb Classic/HC-06.

  Disseny GATT (NimBLE-Arduino — pila BLE-only, no Bluedroid: molt més
  lleugera en flash/RAM i més fiable en notificacions llargues/reconnexions
  que la llibreria BLE del core; rellevant perquè el firmware germà EVO ja va
  arribar al 87% de flash amb la pila Bluedroth de BluetoothSerial):
    - Un sol servei custom (UUID generat aleatòriament, no reutilitzat de cap
      exemple): BLE_SERVICE_UUID
    - Característica d'ESCRIPTURA (Write + Write Without Response): l'app hi
      escriu bytes crus del protocol `!Vxx=valor$`, igual que abans per SPP.
      BLE_WRITE_CHAR_UUID
    - Característica de NOTIFICACIÓ: el firmware hi envia les respostes
      `!Vxx=valor$`. BLE (a diferència de SPP) no és un flux continu — l'app
      s'hi ha de subscriure explícitament (activar notificacions al CCCD)
      abans de rebre res. BLE_NOTIFY_CHAR_UUID

  Fragmentació per MTU: l'ATT MTU per defecte de BLE és de només 23 bytes (20
  de payload útil); es demana un MTU més gran a la connexió (vegeu
  `NimBLEDevice::setMTU()`), però el MTU negociat depèn del central i no es
  pot donar per fet que serà prou gran per als camps de text més llargs
  (V69=descripció, fins a 384 bytes + capçalera del protocol). Per això
  `sendFrame()` fragmenta explícitament qualsevol trama de sortida en trossos
  de com a màxim `mtu-3` bytes i en fa una notificació per tros — la
  reassemblen els mateixos delimitadors '!'/'$' que ja existien al protocol,
  exactament igual que `feedByte()` (abans `pollBluetooth()`) ja reassembla
  l'entrada byte a byte sense assumir que arribi tota en un sol tros. El
  costat app (fase 2, encara no fet) haurà de fer la mateixa reassemblada en
  rebre notificacions.

  Concurrència: els callbacks de NimBLE (`onWrite`, connexió/desconnexió)
  s'executen en la tasca pròpia de l'stack BLE, NO a la tasca de loop() — a
  diferència de `SerialBT.available()/read()`, que es llegien síncronament
  dins loop(). Per no introduir una condició de carrera sobre `dmxData` i la
  resta d'estat global (que `processFrame()`/`handleWrite()` toquen sense cap
  protecció, pensats per córrer sempre dins loop()), `onWrite()` només copia
  els bytes rebuts a una cua FreeRTOS (`bleRxQueue`); tot el processament real
  (`feedByte()` -> `processFrame()` -> `handleWrite()`/NVS/DMX) segueix
  passant exclusivament dins loop(), igual que abans.

  Reutilitza el mateix protocol de trames `!Vxx=valor$` / `!Vxx=?$` que ja fa
  servir l'app per l'ARDMX4 (índexs sempre a 2 dígits), però només implementa
  el subconjunt d'índexs que té sentit sense escenes/cicle/música:
    V01-V03  valor actual (0-255) dels 3 canals "visibles"
    V04-V06  número de canal DMX (1-510) seleccionat per a cada un d'ells
    V07      avança/retrocedeix de grup de 3 canals (-1/0/+1)
    V08      nombre de canals actius (1-512) que realment s'envien per DMX
    V41      arma/desarma el reset de fàbrica (mateix mecanisme que l'ARDMX4)
    V42      confirma el reset de fàbrica (només té efecte si V41 ja és 1)
    V62      versió de firmware (text), demanada per l'app en connectar
    V64      identificació del dispositiu (JSON, només lectura)
    V65-V67  nom (text) del canal DMX actualment assignat a cada slot 1-3
             (mateixa indirecció que V01-03/V04-06 — vegeu selectedChannel)
    V68      nom del pessebe (text, fins a 32 caràcters)
    V69      descripció (text, fins a 128 caràcters)
    V70      consulta/assignació directa d'UN canal explícit (no toca la
             selecció dels sliders) — pensat per exportar/importar tots els
             canals actius sense passar pels 3 slots visibles:
               V70=N            -> consulta, respon "valor|nom" del canal N
               V70=N|valor|nom  -> assigna valor i nom al canal N

  La resta d'índexs del mapa V[] de l'ARDMX4 (música, cicle, escenes,
  transicions, reset, etc.) no s'implementen — no tenen sentit en aquest
  maquinari i simplement s'ignoren si arriben.

  Nom Bluetooth: lliurement editable des de la pantalla de Configuració de
  l'app (V63=nomNou) — fins a 12 caràcters, només lletres, dígits i '_'
  (sense espais, accents ni altres símbols; vegeu sanitizeName()). El nou nom es desa a
  NVS i l'ESP32 es reinicia tot seguit perquè el Bluetooth arrenqui amb el
  nom actualitzat.

  Identificació del dispositiu: l'app fa un handshake explícit (V64=?,
  resposta JSON) en lloc de mirar el nom Bluetooth — així el nom es pot
  canviar completament lliure (V63) sense que això afecti la identificació.
*/

// Arduino.h: funcions bàsiques (millis, digitalWrite, Serial, String, etc.)
#include <Arduino.h>
// esp_dmx: llibreria que genera i envia el senyal DMX512 per un UART de l'ESP32
#include <esp_dmx.h>

// La pila (stack) per defecte de la tasca loop() en aquest core Arduino-ESP32
// és de 8 KB — confirmat insuficient en maquinari real: apareixien crashes
// intermitents (Guru Meditation Error / LoadStoreError dins el controlador
// DMX, amb el backtrace marcat "CORRUPTED", el símptoma clàssic d'un
// desbordament de pila) després d'escriure a la NVS (String temporals per
// als noms de canal, concatenacions de claus, etc. de saveNames()/
// loadNames()). Es reprodueix igual amb versions anteriors del firmware
// (no és un bug d'una funció concreta) — cal ampliar la pila abans que
// aquesta funció es cridi.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
// NimBLE-Arduino: pila BLE (GATT) independent de Bluedroid — vegeu el
// comentari de capçalera per què (flash/RAM, fiabilitat de notificacions).
#include <NimBLEDevice.h>
// Cua FreeRTOS per passar bytes rebuts per BLE des de la tasca de l'stack BLE
// cap a loop() sense tocar estat global des de dos fils alhora (vegeu el
// comentari de capçalera "Concurrència").
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
// Preferences: llegeix/escriu dades a la memòria no volàtil (NVS) de l'ESP32
#include <Preferences.h>

// ---------------------------------------------------------------------------
// Configuració de maquinari
// ---------------------------------------------------------------------------

// DMX cap al MAX485 pels pins GPIO22 (TX)/GPIO21 (RX, no usat activament).
// El mòdul MAX485 té commutació de direcció automàtica per maquinari, per
// això no hi ha pin DE/RE: es passa -1 com a pin RTS (esp_dmx accepta -1 per
// dir "no gestionis aquest pin").
//
// S'utilitza el port lògic DMX_NUM_1 (no DMX_NUM_2) encaminat per la matriu
// de GPIO a aquests pins 22/21 — a l'ESP32 el número de port UART no està
// lligat a cap pin fix, es pot enrutar per la matriu de GPIO a qualsevol pin
// d'ús general (21/22 no tenen cap restricció especial, com tampoc en tenien
// els 17/16 usats abans). DMX_NUM_2 provoca un crash en temps real (Guru
// Meditation Error / LoadProhibited dins `dmx_uart_init`/`uart_ll_set_sclk`,
// confirmat en maquinari) amb la combinació de versions esp_dmx 4.1.0 +
// aquest framework Arduino-ESP32 — DMX_NUM_1 és el port que fa servir el
// propi exemple oficial de la llibreria i funciona correctament.
constexpr dmx_port_t DMX_PORT = DMX_NUM_1;  // "port" lògic UART que farem servir per al DMX
constexpr int DMX_TX_PIN = 22;              // pin físic per on surten les dades DMX cap al MAX485
constexpr int DMX_RX_PIN = 21;              // pin físic d'entrada (no s'usa, DMX només surt)
constexpr int DMX_RTS_PIN = -1;             // -1 = no gestionar cap pin de direcció (auto)

// LED d'estat: fix encès i quiet sense client Bluetooth connectat, parpellejant si n'hi ha un.
constexpr int STATUS_LED_PIN = 2;       // pin on va connectat el LED indicador
constexpr uint32_t LED_BLINK_MS = 500;  // cada quants ms canvia d'estat el parpelleig

// Freqüència d'enviament DMX (40 Hz, l'habitual del protocol). Abans es
// cridava dmxSendFrame() a cada volta del bucle sense cap límit, tan ràpid
// com el maquinari ho permetés — això maximitzava la finestra en què una
// trama DMX activa podia coincidir amb una escriptura a la NVS (confirmat
// en maquinari: crashes intermitents del controlador DMX just després de
// desar canvis, sobretot en escriure molt seguit al camp de descripció).
// Limitar la freqüència deixa marge real entre trames perquè les
// escriptures a la flash mai coincideixin amb una transmissió activa,
// mantenint alhora el refresc continu que esperen els receptors DMX (a
// diferència de només enviar a l'arrencada i en cada canvi de valor, que
// podria fer que alguns receptors interpretessin l'absència de trames com
// a pèrdua de senyal).
constexpr uint32_t DMX_SEND_INTERVAL_MS = 25;

// Univers DMX complet (1-512). Les 3 "finestres" de canal que gestiona la
// pantalla de l'app només poden apuntar a un grup de 3 dins de 1-numeroCanals
// (per això numeroCanals sempre s'arrodoneix a un múltiple de 3 — vegeu
// roundDownToMultipleOf3()); els canals per sobre de numeroCanals queden
// fora d'aquesta UI però igualment es transmeten (amb el seu últim valor
// desat, per defecte 0).
constexpr int MAX_DMX_CHANNEL = 512;  // nombre total de canals DMX que s'envien

// Guardat a NVS: no es desa a cada canvi de canal (desgastaria la flash), es
// desa com a màxim un cop transcorregut aquest temps des de l'últim canvi.
constexpr uint32_t SAVE_DEBOUNCE_MS = 3000;  // temps d'inactivitat abans de desar

// Nom Bluetooth lliurement editable (vegeu handleNameChange()) — es desa a
// NVS i es pot canviar en calent (amb reinici) des de la pantalla de
// Configuració de l'app.
const char *DEFAULT_BLUETOOTH_NAME = "ARDMXOne";
constexpr int MAX_BLUETOOTH_NAME_LENGTH = 15;  // igual que el límit de l'app

// Noms editables (canals, pessebe, descripció) — vegeu sanitizeText(),
// loadNames() i saveNames(). Els límits són en BYTES, no en caràcters: amb
// accents (é, ç...) un caràcter pot ocupar 2 bytes en UTF-8. Al nom de canal
// això pot fer perdre 1-2 caràcters en un cas molt accentuat (vegeu
// sanitizeText per què no es corromp mai el text); a pessebeName/descripcio
// no hi ha aquesta pressió (no formen part del blob ×512 canals) així que
// duem un marge folgat perquè els 32/128 caràcters nominals hi càpiguen
// sempre encara que siguin tots accentuats.
constexpr int MAX_CHANNEL_NAME_LENGTH = 15;    // bytes — buffer fix, veure capacitat NVS
constexpr int MAX_PESSEBE_NAME_LENGTH = 96;    // bytes — marge folgat per a 32 caràcters accentuats
constexpr int MAX_DESCRIPTION_LENGTH = 384;    // bytes — marge folgat per a 128 caràcters accentuats

const char *FIRMWARE_VERSION_TEXT = "ARDMX One v1.0";  // text que es respon a la petició V62

// PIN de connexió — opcional (String buida = desactivat, comportament de
// sempre). Quan n'hi ha un, cap V/T es contesta ni s'aplica (V64, V73, V75
// són les úniques excepcions) fins que l'app l'enviï correcte per V73. Es
// desa en clar a NVS (com el nom Bluetooth): no protegeix contra algú amb
// accés físic a la placa, només contra connectar-se sense voler al dispositiu
// del veí en una trobada de pessebristes — no cal xifrar-lo per a això.
constexpr int PIN_LENGTH = 4;
String storedPin = "";
bool pinAuthenticated = false;

// Resposta a V64 (identificació). "firmware" és una versió pròpia d'aquest
// esquema JSON (semver), independent del text humà de V62. Ara inclou si
// cal PIN, així que ja no és una constant fixa.
String buildIdentifyJson() {
  String json = "{\"tipus\":\"ARDMX_ONE\",\"firmware\":\"1.0.0\",";
  json += "\"num_canals_max\":512,\"pin\":";
  json += (storedPin.length() > 0) ? "true" : "false";
  json += "}";
  return json;
}

// ---------------------------------------------------------------------------
// Configuració BLE — UUIDs generats aleatòriament (uuid4), no reutilitzats de
// cap exemple. Vegeu el comentari de capçalera del fitxer pel disseny GATT
// complet.
// ---------------------------------------------------------------------------

constexpr const char *BLE_SERVICE_UUID = "74fdf89b-a063-48f4-837d-03462d2b3687";
constexpr const char *BLE_WRITE_CHAR_UUID = "c7e05764-94cb-4a2f-8cd4-4751163c58ad";
constexpr const char *BLE_NOTIFY_CHAR_UUID = "dd2a9ece-4964-4f42-b986-36719d38b2a3";

// MTU local preferit — el negociat de veritat amb el central pot quedar per
// sota (vegeu getPeerMTU() a sendFrame()), però demanar-ne un de gran des
// d'un inici li dona al central l'oportunitat de negociar-lo amunt.
constexpr uint16_t BLE_PREFERRED_MTU = 247;

// Mida de cada tros de la cua de recepció (vegeu bleRxQueue) — prou gran per
// no fragmentar en excés una escriptura normal, però petita comparada amb la
// pila de la tasca BLE.
constexpr size_t BLE_RX_CHUNK_MAX = 256;

struct BleRxChunk {
  uint8_t data[BLE_RX_CHUNK_MAX];
  size_t length;
};

// ---------------------------------------------------------------------------
// Estat global
// ---------------------------------------------------------------------------

Preferences prefs;  // objecte que gestiona la lectura/escriptura a la NVS

NimBLEServer *bleServer = nullptr;
NimBLECharacteristic *bleNotifyCharacteristic = nullptr;
QueueHandle_t bleRxQueue = nullptr;

// Actualitzat només des dels callbacks de connexió/desconnexió (tasca BLE) i
// llegit des de loop() (updateStatusLed(), sendFrame()) — un bool de lectura/
// escriptura atòmica en aquesta plataforma, no cal cap mutex addicional per
// aquest ús concret (mai es fa un read-modify-write compartit entre tasques).
volatile bool bleClientConnected = false;
volatile uint16_t bleConnHandle = BLE_HS_CONN_HANDLE_NONE;

// Índex 0 = start code DMX (sempre 0). Índexs 1..512 = valors dels canals.
uint8_t dmxData[DMX_PACKET_SIZE];  // buffer amb TOT l'univers DMX que s'envia cada cicle

// Nombre de canals actius que realment s'envien per DMX. Els canals per
// sobre d'aquest valor es queden al buffer (amb el seu últim valor) però no
// s'inclouen a la trama enviada. Sempre és un múltiple de 3 (vegeu
// roundDownToMultipleOf3()) perquè els 3 sliders de l'app puguin avançar per
// grups sencers sense mai sobrepassar aquest límit. Per defecte, tot
// l'univers arrodonit avall (512 no és múltiple de 3).
int numeroCanals = 510;

// Arrodoneix cap avall al múltiple de 3 més proper (mínim 3).
int roundDownToMultipleOf3(int value) {
  if (value < 3) return 3;
  return (value / 3) * 3;
}

// Números de canal DMX (1-based) actualment seleccionats per als 3 sliders
// visibles a la pantalla de l'app (V01-V03 / V04-V06).
int selectedChannel[3] = {1, 2, 3};  // per defecte, els sliders apunten als canals 1, 2 i 3

bool sceneDirty = false;        // true = hi ha canvis pendents de desar a la NVS
bool namesDirty = false;        // true = hi ha canvis de noms/pessebe/descripció pendents de desar
uint32_t lastChangeMillis = 0;  // instant (millis()) de l'últim canvi (escena o noms)

// V41: armat/desarmat del reset de fàbrica (vegeu performFactoryReset()). El
// nom Bluetooth (V63) NO es toca aquí — un reset de fàbrica només afecta
// l'escena (valors de canal) i el nombre de canals actius.
bool resetArmed = false;

String btFrameBuffer;  // acumula els caràcters d'una trama Bluetooth mentre arriba

String btDeviceName;  // nom Bluetooth actual (fins a MAX_BLUETOOTH_NAME_LENGTH caràcters)

// Nom editable de cada canal DMX (1..512), indexat 0-based pel número de
// canal (channelNames[0] = nom del canal DMX 1). Es desa a la NVS partit en
// trossos (vegeu CHANNEL_NAME_CHUNK_SIZE / saveNames()) — no en una única
// entrada, ni tampoc en una clau per canal.
char channelNames[MAX_DMX_CHANNEL][MAX_CHANNEL_NAME_LENGTH + 1];

// El blob complet de tots els 512 noms (8 KB) no cap en una sola entrada
// NVS: nvs_set_blob limita cada valor individual a uns pocs KB (confirmat
// en maquinari real — l'escriptura dels 8 KB fallava sempre, en silenci,
// perquè el codi no comprovava el valor de retorn de putBytes; els noms
// semblaven correctes durant la sessió (viuen en RAM) però mai s'arribaven
// a desar de veritat, i es perdien a cada reinici de l'ESP32). Es parteix
// en trossos petits, cadascun molt per sota d'aquest límit, cada un amb la
// seva pròpia clau NVS ("chn0".."chn15").
constexpr int CHANNEL_NAME_CHUNK_SIZE = 32;  // canals per tros (32 x 16 bytes = 512 bytes/tros)
constexpr int CHANNEL_NAME_CHUNK_COUNT = MAX_DMX_CHANNEL / CHANNEL_NAME_CHUNK_SIZE;  // 16 trossos

String pessebeName;  // nom del pessebe (V68), lliurement editable
String descripcio;   // descripció (V69), lliurement editable

// ---------------------------------------------------------------------------
// Escenes / Transicions (firmware v2) — 4 escenes estàtiques + 4 transicions
// globals entre parells consecutius (cíclic: escena 4 -> escena 1). Substitueix
// el buffer únic `dmxData` editat en directe (comportament v1) per un model amb
// escena activa + interpolació — mateix disseny que ardmx4-evo-firmware, aquí
// portat sense res d'àudio/cicle-per-pulsador (maquinari sense DFPlayer/trigger).
// ---------------------------------------------------------------------------

// Tipus de transició entre dues escenes consecutives. SALT és l'únic que fa
// servir saltPercent (percentatge del temps de la transició en què es
// produeix el salt instantani); la resta l'ignoren.
enum TipusTransicio : uint8_t { LINEAL = 0, SALT = 1, EASE_IN = 2, EASE_OUT = 3 };

struct Transicio {
  TipusTransicio tipus;
  uint8_t saltPercent;  // 0-100, només rellevant si tipus==SALT
};

// Interpola el valor (0-255) d'un canal entre l'escena origen (v0) i destí
// (v1) segons el progrés de la transició (t_pct, 0-1000 mil·lèsimes). Tot en
// enters (sense floats) per eficiència a l'ESP32.
uint8_t interpolar(uint8_t v0, uint8_t v1, uint16_t t_pct, TipusTransicio tipus, uint8_t salt_pct) {
  switch (tipus) {
    case SALT:
      return (t_pct < (uint16_t)salt_pct * 10) ? v0 : v1;
    case EASE_OUT: {
      // Ràpid al principi, s'alenteix al final: f(t) = 1-(1-t)^2
      int32_t inv = 1000 - (int32_t)t_pct;
      int32_t f = 1000 - (inv * inv) / 1000;
      return (uint8_t)(v0 + ((int32_t)(v1 - v0) * f) / 1000);
    }
    case EASE_IN: {
      // Lent al principi, accelera al final: f(t) = t^2
      int32_t f = ((int32_t)t_pct * t_pct) / 1000;
      return (uint8_t)(v0 + ((int32_t)(v1 - v0) * f) / 1000);
    }
    case LINEAL:
    default:
      return (uint8_t)(v0 + ((int32_t)(v1 - v0) * t_pct) / 1000);
  }
}

// Valor (0-255) de cada canal a cadascuna de les 4 escenes.
struct CanalData {
  uint8_t valors[4];
};

CanalData canalsData[MAX_DMX_CHANNEL];       // per canal DMX (0-based), 4 valors (una per escena)
float valorActual[MAX_DMX_CHANNEL];          // valor interpolat que s'envia ara mateix per DMX
Transicio transicions[4];                    // transicions[i]: escena i+1 -> escena ((i+1)%4)+1

constexpr int CHANNEL_CHUNK_SIZE = 32;  // canals per tros de NVS, mateix patró que channelNames
constexpr int CHANNEL_CHUNK_COUNT = MAX_DMX_CHANNEL / CHANNEL_CHUNK_SIZE;  // 16 trossos

bool canalsDirty = false;  // true = hi ha canvis d'escenes pendents de desar a la NVS

// Blob NVS amb tot el que no són els valors de canal (que van a part, per
// trossos): escena/nombre d'escenes actives, selector principal, durades
// dels 8 períodes del cicle (escena/transició alternades) i les 4 transicions.
struct ParametresEscenes {
  int EscenaActiva;
  int NumeroEscenes;
  int EstatSelector;
  uint32_t tempsPeriodes[8];  // microsegons
  Transicio transicions[4];
};
ParametresEscenes ParamEscenes;
bool paramEscenesDirty = false;

// ---- Estat de la màquina d'estats de cicle/escenes (mateix disseny que EVO) ----
int EscenaActiva = 1;       // V09, 1-4
int NumeroEscenes = 4;      // V18
int EstatSelector = 0;      // V11: 1=Automàtic, 3-6=Escena fixa N, 7=Config (sense Trigger, l'One no en té)
int num_periodes = NumeroEscenes * 2;

uint32_t Temps[8];              // durada de cada període (escena/transició alternades), microsegons
uint32_t TempsAcumulat[8];      // temps acumulat (per validar seqüència i mostrar a l'app), segons
uint32_t tiempoTotalCiclo = 0;  // microsegons

uint32_t tempsActualEstat = 0, referenciaTempsEstat = 0;
uint32_t tempsActualCicle = 0, referenciaTempsCicle = 0;
uint32_t tempsActualTransicio = 0, referenciaTempsTransicio = 0;
uint32_t contadorPuntTransicio = 0, numeroPuntsTransicio = 0;
constexpr uint32_t tempsCiclesTransicio = 10000;  // microsegons (>= 10000), pas d'interpolació
int EstatActual = 0, EstatAntic = 0;
bool cicloEnCurso = false;

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

// Envia UNA trama DMX (numeroCanals canals de dmxData, no sempre els 512
// sencers — vegeu numeroCanals) i espera que acabi de sortir.
void dmxSendFrame() {
  const int packetSize = numeroCanals + 1;  // +1 pel start code (índex 0)
  dmx_write(DMX_PORT, dmxData, packetSize);  // copia dmxData al buffer intern del driver
  dmx_send_num(DMX_PORT, packetSize);        // comença a transmetre'l pel cable
  dmx_wait_sent(DMX_PORT, DMX_TIMEOUT_TICK);  // bloqueja fins que la trama ha sortit del tot
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

  // Nombre de canals actius — per defecte tot l'univers (arrodonit) si mai
  // s'ha desat. Es torna a arrodonir en carregar per si mai queda un valor
  // desat d'una versió anterior que no apliqués encara aquesta regla.
  if (prefs.isKey("numch")) {
    const int stored = constrain((int)prefs.getUInt("numch", MAX_DMX_CHANNEL), 1, MAX_DMX_CHANNEL);
    numeroCanals = roundDownToMultipleOf3(stored);
  } else {
    numeroCanals = roundDownToMultipleOf3(MAX_DMX_CHANNEL);
  }

  prefs.end();          // tanca l'accés a la NVS
  dmxData[0] = 0;        // el primer byte del paquet DMX és sempre el "start code" (0)
}

// Es crida quan cal desar l'escena actual a la NVS (des del debounce del loop()).
void sceneSave() {
  prefs.begin("ardmxone", false);                       // obre l'espai de NVS en escriptura
  prefs.putBytes("scene", &dmxData[1], MAX_DMX_CHANNEL);  // escriu els 512 valors de canal
  prefs.putUInt("numch", (uint32_t)numeroCanals);         // escriu el nombre de canals actius
  prefs.end();                                          // tanca l'accés a la NVS
  sceneDirty = false;                                   // ja no queden canvis pendents de desar
}

// Marca que hi ha un canvi pendent (escena) i reinicia el comptador de temps del debounce.
void markDirty() {
  sceneDirty = true;
  lastChangeMillis = millis();  // guarda "ara" com a últim instant de canvi
}

// Marca que hi ha canvis pendents als valors de canal de les 4 escenes
// (canalsData) — mateix debounce compartit que la resta (lastChangeMillis).
void markCanalsDirty() {
  canalsDirty = true;
  lastChangeMillis = millis();
}

// Marca que hi ha canvis pendents als paràmetres d'escenes/cicle/transicions.
void markParamEscenesDirty() {
  paramEscenesDirty = true;
  lastChangeMillis = millis();
}

// Es crida un cop, a l'arrencada: recupera els valors de canal de les 4
// escenes (o els deixa a 0 si mai s'han desat). Mateix patró de trossos que
// loadNames()/saveNames() — un blob de canalsData sencer (512 canals x 4
// bytes = 2 KB) ja hi cabria en teoria en una sola entrada NVS, però es
// parteix igualment pel mateix límit pràctic de nvs_set_blob documentat a
// CHANNEL_NAME_CHUNK_SIZE, i per mantenir el mateix patró arreu del fitxer.
void loadCanals() {
  prefs.begin("ardmxone", false);

  const size_t chunkBytes = CHANNEL_CHUNK_SIZE * sizeof(CanalData);
  bool allChunksOk = true;
  for (int chunk = 0; chunk < CHANNEL_CHUNK_COUNT; chunk++) {
    const String key = "chv" + String(chunk);
    CanalData *dest = &canalsData[chunk * CHANNEL_CHUNK_SIZE];
    if (!prefs.isKey(key.c_str())) {
      allChunksOk = false;
      break;
    }
    const size_t bytesRead = prefs.getBytes(key.c_str(), dest, chunkBytes);
    if (bytesRead != chunkBytes) {
      allChunksOk = false;
      break;
    }
  }
  if (!allChunksOk) {
    memset(canalsData, 0, sizeof(canalsData));
  }

  prefs.end();
}

// Es crida quan cal desar els valors de canal de les 4 escenes (des del
// debounce del loop()).
void saveCanals() {
  prefs.begin("ardmxone", false);

  const size_t chunkBytes = CHANNEL_CHUNK_SIZE * sizeof(CanalData);
  for (int chunk = 0; chunk < CHANNEL_CHUNK_COUNT; chunk++) {
    const String key = "chv" + String(chunk);
    const CanalData *src = &canalsData[chunk * CHANNEL_CHUNK_SIZE];
    if (prefs.putBytes(key.c_str(), src, chunkBytes) != chunkBytes) {
      Serial.printf("ERROR desant valors de canal (tros %d)\n", chunk);
    }
  }

  prefs.end();
  canalsDirty = false;
}

// Es crida un cop, a l'arrencada: recupera l'escena activa, el nombre
// d'escenes, el selector principal, les durades dels 8 períodes i les 4
// transicions — o valors de fàbrica si mai s'han desat.
void loadParamEscenes() {
  prefs.begin("ardmxone", false);

  bool ok = prefs.isKey("paramesc");
  if (ok) {
    const size_t bytesRead = prefs.getBytes("paramesc", &ParamEscenes, sizeof(ParamEscenes));
    ok = (bytesRead == sizeof(ParamEscenes));
  }

  if (!ok) {
    ParamEscenes.EscenaActiva = 1;
    ParamEscenes.NumeroEscenes = 4;
    ParamEscenes.EstatSelector = 0;
    for (int i = 0; i < 8; i++) ParamEscenes.tempsPeriodes[i] = 5000000UL;  // 5s per defecte
    for (int i = 0; i < 4; i++) ParamEscenes.transicions[i] = {LINEAL, 0};
  }

  EscenaActiva = ParamEscenes.EscenaActiva;
  NumeroEscenes = ParamEscenes.NumeroEscenes;
  EstatSelector = ParamEscenes.EstatSelector;
  num_periodes = NumeroEscenes * 2;
  for (int i = 0; i < 8; i++) Temps[i] = ParamEscenes.tempsPeriodes[i];
  for (int i = 0; i < 4; i++) transicions[i] = ParamEscenes.transicions[i];

  prefs.end();
}

// Es crida quan cal desar l'escena activa/nombre d'escenes/selector/durades/
// transicions (des del debounce del loop()).
void saveParamEscenes() {
  ParamEscenes.EscenaActiva = EscenaActiva;
  ParamEscenes.NumeroEscenes = NumeroEscenes;
  ParamEscenes.EstatSelector = EstatSelector;
  for (int i = 0; i < 8; i++) ParamEscenes.tempsPeriodes[i] = Temps[i];
  for (int i = 0; i < 4; i++) ParamEscenes.transicions[i] = transicions[i];

  prefs.begin("ardmxone", false);
  if (prefs.putBytes("paramesc", &ParamEscenes, sizeof(ParamEscenes)) != sizeof(ParamEscenes)) {
    Serial.println("ERROR desant els parametres d'escenes");
  }
  prefs.end();
  paramEscenesDirty = false;
}

// Igual que markDirty() però pels noms/pessebe/descripció — comparteix el
// mateix debounce que l'escena (vegeu SAVE_DEBOUNCE_MS), important sobretot
// durant una importació massiva (V70) que pot tocar centenars de canals
// seguits: sense això, cada canal escriuria els 8 KB del blob de noms a la
// NVS individualment.
void markNamesDirty() {
  namesDirty = true;
  lastChangeMillis = millis();
}

// Es crida un cop, a l'arrencada: recupera el nom desat (o el de per defecte).
void loadBtName() {
  prefs.begin("ardmxone", false);
  if (prefs.isKey("btname")) {
    btDeviceName = prefs.getString("btname", DEFAULT_BLUETOOTH_NAME);
  } else {
    btDeviceName = DEFAULT_BLUETOOTH_NAME;
  }
  prefs.end();
}

void loadPin() {
  prefs.begin("ardmxone", false);
  storedPin = prefs.getString("pin", "");
  prefs.end();
}

// Només xifres, exactament PIN_LENGTH — qualsevol altra cosa (buit, massa
// curt, no numèric) es considera invàlida (String buida de retorn).
String sanitizePin(const String &rawInput) {
  String clean = "";
  for (unsigned int i = 0; i < rawInput.length(); i++) {
    const char c = rawInput.charAt(i);
    if (isDigit(c)) clean += c;
  }
  if (clean.length() != PIN_LENGTH) return "";
  return clean;
}

// Filtra qualsevol caràcter que no sigui una lletra ASCII, un dígit o '_'
// (sense espais, accents ni altres símbols) i talla a
// MAX_BLUETOOTH_NAME_LENGTH.
String sanitizeName(const String &rawInput) {
  String clean = "";
  for (unsigned int i = 0; i < rawInput.length(); i++) {
    const char c = rawInput.charAt(i);
    if (isAlphaNumeric(c) || c == '_') clean += c;
    if ((int)clean.length() >= MAX_BLUETOOTH_NAME_LENGTH) break;
  }
  return clean;
}

// Elimina '!', '$' i '|' (delimitadors reservats del protocol — '|' separa
// els camps de V70) i retalla a com a màxim maxBytes BYTES — a diferència
// de sanitizeName(), aquí es permet qualsevol altre caràcter (accents,
// espais...) perquè aquests camps són text lliure (noms de canal, pessebe,
// descripció).
//
// El retall és conscient d'UTF-8: si el byte número maxBytes cauria enmig
// d'un caràcter multibyte (é, ç, l·l...), es retrocedeix fins al final del
// caràcter complet anterior, en lloc de partir-lo pel mig i corrompre el
// text mostrat després. Als bytes de continuació UTF-8 sempre hi ha el
// patró de bits 10xxxxxx (màscara 0xC0 == 0x80).
String sanitizeText(const String &rawInput, int maxBytes) {
  String clean = "";
  for (unsigned int i = 0; i < rawInput.length(); i++) {
    const char c = rawInput.charAt(i);
    if (c != '!' && c != '$' && c != '|') clean += c;
  }
  if ((int)clean.length() <= maxBytes) return clean;

  int cut = maxBytes;
  while (cut > 0 && (clean.charAt(cut) & 0xC0) == 0x80) cut--;
  return clean.substring(0, cut);
}

// Es crida un cop, a l'arrencada: recupera els noms de canal, el nom del
// pessebe i la descripció (o els deixa buits si mai s'han desat).
void loadNames() {
  prefs.begin("ardmxone", false);

  const size_t chunkBytes = CHANNEL_NAME_CHUNK_SIZE * (MAX_CHANNEL_NAME_LENGTH + 1);
  bool allChunksOk = true;
  for (int chunk = 0; chunk < CHANNEL_NAME_CHUNK_COUNT; chunk++) {
    const String key = "chn" + String(chunk);
    char *dest = &channelNames[chunk * CHANNEL_NAME_CHUNK_SIZE][0];
    if (!prefs.isKey(key.c_str())) {
      allChunksOk = false;
      break;
    }
    const size_t bytesRead = prefs.getBytes(key.c_str(), dest, chunkBytes);
    if (bytesRead != chunkBytes) {
      allChunksOk = false;
      break;
    }
  }
  if (!allChunksOk) {
    memset(channelNames, 0, sizeof(channelNames));
  }

  pessebeName = prefs.isKey("pessebe") ? prefs.getString("pessebe", "") : "";
  descripcio = prefs.isKey("descripcio") ? prefs.getString("descripcio", "") : "";

  prefs.end();
}

// Es crida quan cal desar els noms/pessebe/descripció a la NVS (des del
// debounce del loop(), vegeu markNamesDirty()). Cada escriptura es verifica
// (vegeu el comentari a CHANNEL_NAME_CHUNK_SIZE per l'incident que ho va
// motivar) — si mai torna a fallar, com a mínim quedarà constància per USB.
void saveNames() {
  prefs.begin("ardmxone", false);

  const size_t chunkBytes = CHANNEL_NAME_CHUNK_SIZE * (MAX_CHANNEL_NAME_LENGTH + 1);
  for (int chunk = 0; chunk < CHANNEL_NAME_CHUNK_COUNT; chunk++) {
    const String key = "chn" + String(chunk);
    const char *src = &channelNames[chunk * CHANNEL_NAME_CHUNK_SIZE][0];
    if (prefs.putBytes(key.c_str(), src, chunkBytes) != chunkBytes) {
      Serial.printf("ERROR desant noms de canal (tros %d)\n", chunk);
    }
  }

  if (prefs.putString("pessebe", pessebeName) == 0 && pessebeName.length() > 0) {
    Serial.println("ERROR desant el nom del pessebre");
  }
  if (prefs.putString("descripcio", descripcio) == 0 && descripcio.length() > 0) {
    Serial.println("ERROR desant la descripció");
  }

  prefs.end();
  namesDirty = false;
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

// Mou la selecció actual un grup de 3 canals endavant (+1) o enrere (-1), sense volta:
// si ja s'és al primer grup i es demana enrere (o a l'últim i es demana endavant), no fa res.
void advanceGroup(int direction) {
  const int totalGroups = numeroCanals / 3;  // exacte: numeroCanals sempre és múltiple de 3
  const int currentGroup = (groupStart(selectedChannel[0]) - 1) / 3;  // grup actual (0-based)
  int nextGroup = constrain(currentGroup + direction, 0, totalGroups - 1);  // ancorat als límits
  selectGroup(nextGroup * 3 + 1);  // aplica el nou grup (torna a 1-based)
}

// Es crida en rebre V42=1 amb el reset ja armat (V41=1). Torna l'escena
// (tots els canals a 0), el nombre de canals actius, els noms de canal, el
// nom del pessebre i la descripció al seu valor de fàbrica, i ho desa
// immediatament (no cal esperar el debounce, ja que és una operació
// explícita i puntual). El nom Bluetooth es manté intacte.
void performFactoryReset() {
  memset(&dmxData[1], 0, MAX_DMX_CHANNEL);          // tots els canals a 0 (apagat)
  numeroCanals = roundDownToMultipleOf3(MAX_DMX_CHANNEL);  // torna al valor de fàbrica
  selectGroup(1);                                    // selecció dels sliders de tornada a 1,2,3
  sceneSave();                                        // desa immediatament (també neteja sceneDirty)

  memset(channelNames, 0, sizeof(channelNames));      // esborra tots els noms de canal
  pessebeName = "";
  descripcio = "";
  saveNames();                                        // desa immediatament (també neteja namesDirty)

  Serial.println("Reset de fàbrica: escena, canals, noms, pessebre i descripció reinicialitzats");
}

// ---------------------------------------------------------------------------
// Protocol `!Vxx=valor$` / `!Vxx=?$`
// ---------------------------------------------------------------------------

// Envia una trama completa per BLE, fragmentada en trossos de com a màxim el
// MTU negociat amb el client actualment connectat (vegeu el comentari de
// capçalera "Fragmentació per MTU"). Sense client connectat/subscrit,
// notify() no fa res perillós per si mateix, però ho evitem directament
// (mateix esperit que SerialBT.print() abans, que simplement no anava enlloc
// sense connexió).
void sendFrame(const String &frame) {
  if (!bleClientConnected || bleNotifyCharacteristic == nullptr) return;

  const uint16_t mtu = bleServer != nullptr
      ? bleServer->getPeerMTU(bleConnHandle)
      : 0;
  // 3 bytes d'overhead ATT; si encara no hi ha MTU negociat (mtu==0) o és
  // massa petit, cau al mínim garantit per l'especificació BLE (23 - 3 = 20).
  const size_t chunkSize = (mtu > 23) ? (size_t)(mtu - 3) : 20;

  const size_t len = frame.length();
  size_t offset = 0;
  while (offset < len) {
    const size_t n = min(len - offset, chunkSize);
    bleNotifyCharacteristic->setValue(
        (const uint8_t *)frame.c_str() + offset, n);
    bleNotifyCharacteristic->notify();
    offset += n;
  }
}

// Envia la resposta a una petició numèrica, p.ex. "!V04=7$".
void replyNumber(int index, long value) {
  String frame = "!V";
  if (index < 10) frame += '0';  // els índexs d'1 xifra s'envien sempre amb 2 dígits
  frame += index;
  frame += '=';
  frame += value;
  frame += '$';
  sendFrame(frame);
}

// Igual que replyNumber() però pel valor de text.
void replyText(int index, const char *text) {
  String frame = "!V";
  if (index < 10) frame += '0';
  frame += index;
  frame += '=';
  frame += text;
  frame += '$';
  sendFrame(frame);
}

// Es crida en rebre V63=<nom nou>. Filtra a lletres/dígits i talla a
// MAX_BLUETOOTH_NAME_LENGTH (vegeu sanitizeName()), desa el nou nom a NVS i
// reinicia l'ESP32 perquè el Bluetooth arrenqui amb el nom actualitzat.
void handleNameChange(const String &rawInput) {
  const String clean = sanitizeName(rawInput);
  if (clean.length() == 0) return;  // entrada invàlida, no es toca res

  prefs.begin("ardmxone", false);
  prefs.putString("btname", clean);
  prefs.end();

  btDeviceName = clean;
  // Confirma el canvi abans de reiniciar, perquè l'app el pugui mostrar al
  // registre encara que la connexió es talli tot seguit.
  replyText(63, btDeviceName.c_str());

  delay(200);  // marge perquè la trama anterior surti abans de tallar el Bluetooth
  ESP.restart();
}

// V73: l'app hi envia el PIN per autenticar-se just després de connectar.
// Sempre es processa (encara que ja calgui PIN i no s'hagi enviat encara),
// és precisament l'única manera d'arribar a autenticar-se.
void handlePinVerify(const String &rawInput) {
  const String attempt = sanitizePin(rawInput);
  pinAuthenticated = storedPin.length() > 0 && attempt == storedPin;
  replyText(73, pinAuthenticated ? "OK" : "ERROR");
}

// V74: posar/canviar el PIN — només té efecte si ja estàs autenticat (o si
// encara no hi havia cap PIN, cas en què "autenticat" ja és cert per
// definició, vegeu la comprovació al capdamunt de processFrame()).
void handlePinSet(const String &rawInput) {
  const String clean = sanitizePin(rawInput);
  if (clean.length() == 0) return;

  prefs.begin("ardmxone", false);
  prefs.putString("pin", clean);
  prefs.end();

  storedPin = clean;
  replyText(74, "OK");
}

// V75: restableix el PIN (torna a "sense PIN"). Sempre s'accepta,
// independentment de pinAuthenticated — vegeu el comentari de storedPin
// sobre per què això és acceptable per l'amenaça real que es vol evitar.
void handlePinReset(const String &rawInput) {
  prefs.begin("ardmxone", false);
  prefs.remove("pin");
  prefs.end();

  storedPin = "";
  pinAuthenticated = false;
  replyText(75, "OK");
}

// Es crida en rebre V65/V66/V67=<nom nou> — assigna el nom al canal DMX que
// actualment ocupa aquest slot (mateixa indirecció que V01-03/V04-06). A
// diferència del nom Bluetooth, no cal reiniciar: és només dada, no afecta
// la pila BT.
void handleChannelNameChange(int index, const String &rawInput) {
  const int slot = index - 65;                // 0, 1 o 2
  const int channel = selectedChannel[slot];   // canal DMX real d'aquest slot
  const String clean = sanitizeText(rawInput, MAX_CHANNEL_NAME_LENGTH);
  clean.toCharArray(channelNames[channel - 1], MAX_CHANNEL_NAME_LENGTH + 1);
  markNamesDirty();
  replyText(index, channelNames[channel - 1]);
}

// Es crida en rebre V68=<nom nou> (nom del pessebe).
void handlePessebeNameChange(const String &rawInput) {
  pessebeName = sanitizeText(rawInput, MAX_PESSEBE_NAME_LENGTH);
  markNamesDirty();
  replyText(68, pessebeName.c_str());
}

// Es crida en rebre V69=<text nou> (descripció).
void handleDescriptionChange(const String &rawInput) {
  descripcio = sanitizeText(rawInput, MAX_DESCRIPTION_LENGTH);
  markNamesDirty();
  replyText(69, descripcio.c_str());
}

// Es crida en rebre V70=<...> — consulta o assignació directa d'UN canal
// explícit (vegeu el bloc de capçalera del fitxer). No toca la selecció
// dels sliders (selectedChannel): a diferència de V01-03/V04-06/V65-67,
// aquest índex s'adreça sempre pel número de canal real, pensat perquè
// l'app pugui exportar/importar tots els canals actius sense passar pels 3
// slots visibles.
void handleChannelBulk(const String &rawInput) {
  const int pipe1 = rawInput.indexOf('|');
  int channel;

  if (pipe1 == -1) {
    // Només un número de canal: consulta (no modifica res).
    channel = constrain(rawInput.toInt(), 1, MAX_DMX_CHANNEL);
  } else {
    // "N|valor|nom": assignació directa del valor DMX i del nom.
    channel = constrain(rawInput.substring(0, pipe1).toInt(), 1, MAX_DMX_CHANNEL);
    const int pipe2 = rawInput.indexOf('|', pipe1 + 1);
    const String valuePart = pipe2 == -1 ? rawInput.substring(pipe1 + 1)
                                          : rawInput.substring(pipe1 + 1, pipe2);
    const String namePart = pipe2 == -1 ? "" : rawInput.substring(pipe2 + 1);

    const uint8_t newValue = (uint8_t)constrain(valuePart.toInt(), 0, 255);
    if (dmxData[channel] != newValue) {
      dmxData[channel] = newValue;
      markDirty();
    }

    const String cleanName = sanitizeText(namePart, MAX_CHANNEL_NAME_LENGTH);
    cleanName.toCharArray(channelNames[channel - 1], MAX_CHANNEL_NAME_LENGTH + 1);
    markNamesDirty();
  }

  const String reply = String(dmxData[channel]) + "|" + String(channelNames[channel - 1]);
  replyText(70, reply.c_str());
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
      // (mai més enllà del nombre de canals actius configurat)
      const int channel = constrain((int)value, 1, numeroCanals);
      selectGroup(groupStart(channel));  // alinea tot el grup de 3 a partir d'aquest canal
      break;
    }
    case 7:
      // V07: l'app demana avançar (+1) o retrocedir (-1) al grup de 3 canals següent/anterior
      if (value > 0) advanceGroup(1);
      else if (value < 0) advanceGroup(-1);
      break;
    case 8: {
      // V08: canvia el nombre de canals actius (1-512) que s'envien per DMX
      // — sempre arrodonit avall a un múltiple de 3 (vegeu numeroCanals)
      const int requested = constrain((int)value, 1, MAX_DMX_CHANNEL);
      const int newValue = roundDownToMultipleOf3(requested);
      if (numeroCanals != newValue) {
        numeroCanals = newValue;
        // Si la selecció actual de canals ha quedat fora del nou rang
        // actiu (numeroCanals s'ha reduït per sota d'on apuntaven els
        // sliders), la duu de tornada al darrer grup vàlid.
        if (groupStart(selectedChannel[0]) + 2 > numeroCanals) {
          selectGroup(numeroCanals - 2);
        }
        markDirty();
      }
      break;
    }
    case 41:
      // V41: arma (valor != 0) o desarma (0) el reset de fàbrica — per si
      // mateix no reinicialitza res, només habilita el botó de confirmar.
      resetArmed = (value != 0);
      break;
    case 42:
      // V42: confirma el reset — només té efecte si ja estava armat, per
      // evitar un reset accidental si arribés sol (p.ex. per un error de
      // trama).
      if (value != 0 && resetArmed) {
        performFactoryReset();
        resetArmed = false;
      }
      break;
    default:
      // Índexs de música/cicle/escenes de l'ARDMX4 no s'implementen aquí.
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
    case 8:
      // Retorna el nombre de canals actius configurat
      replyNumber(8, numeroCanals);
      break;
    case 41:
      // Retorna si el reset de fàbrica està armat (1) o no (0)
      replyNumber(41, resetArmed ? 1 : 0);
      break;
    case 42:
      // Sempre 0: és un disparador puntual, mai queda "armat" en si mateix
      // (l'app el fa servir només per detectar que el reset ja s'ha
      // completat, comprovant que torna a ser 0 alhora que V41).
      replyNumber(42, 0);
      break;
    case 62:
      // Retorna el text de versió de firmware
      replyText(62, FIRMWARE_VERSION_TEXT);
      break;
    case 63:
      // Retorna el nom Bluetooth actual
      replyText(63, btDeviceName.c_str());
      break;
    case 64:
      // Retorna la identificació del dispositiu (JSON, vegeu buildIdentifyJson())
      replyText(64, buildIdentifyJson().c_str());
      break;
    case 65:
    case 66:
    case 67: {
      // Retorna el nom del canal DMX actualment assignat a aquest slot
      const int channel = selectedChannel[index - 65];
      replyText(index, channelNames[channel - 1]);
      break;
    }
    case 68:
      replyText(68, pessebeName.c_str());
      break;
    case 69:
      replyText(69, descripcio.c_str());
      break;
    case 76:
      // Índex separat del 74 (que és l'ACK "OK"/"ERROR" d'activar el PIN) a
      // propòsit: si es reutilitzés el 74, l'app rebria l'ACK de la
      // desada com si fos el valor del PIN. Només es pot arribar aquí ja
      // autenticat (o sense PIN) — vegeu el "gated" a processFrame() —
      // així que ensenyar-lo no exposa res que qui pregunta no hagi pogut
      // demostrar ja que sap.
      replyText(76, storedPin.c_str());
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

  // Mentre calgui PIN i encara no s'hagi enviat el correcte, només es
  // processen V64 (identificació — cal per saber que fa falta PIN), V73
  // (verificar-lo) i V75 (restablir-lo) — tota la resta es descarta en
  // silenci, tant lectures com escriptures.
  const bool gated = storedPin.length() > 0 && !pinAuthenticated;
  if (gated && index != 64 && index != 73 && index != 75) return;

  if (rhs == "?") {
    handleRequest(index);       // és una petició de lectura
  } else if (index == 63) {
    handleNameChange(rhs);  // nom Bluetooth
  } else if (index >= 65 && index <= 67) {
    handleChannelNameChange(index, rhs);  // nom d'un dels 3 canals seleccionats
  } else if (index == 68) {
    handlePessebeNameChange(rhs);  // nom del pessebe
  } else if (index == 69) {
    handleDescriptionChange(rhs);  // descripció
  } else if (index == 70) {
    handleChannelBulk(rhs);  // consulta/assignació directa d'un canal (export/import)
  } else if (index == 73) {
    handlePinVerify(rhs);
  } else if (index == 74) {
    handlePinSet(rhs);
  } else if (index == 75) {
    handlePinReset(rhs);
  } else {
    handleWrite(index, rhs.toInt());  // és una escriptura amb un valor numèric
  }
}

// Alimenta UN byte a l'acumulador de trames — mateixa lògica que abans tenia
// pollBluetooth() directament, ara extreta perquè drainBleRxQueue() la pugui
// cridar byte a byte per cada tros rebut de la cua BLE (vegeu bleRxQueue).
void feedByte(char c) {
  if (c == '!') {
    btFrameBuffer = "";  // '!' marca l'inici d'una trama nova: descarta qualsevol residu previ
  } else if (c == '$') {
    processFrame(btFrameBuffer);  // '$' marca el final: processa tot el que s'ha acumulat
    btFrameBuffer = "";
  } else {
    btFrameBuffer += c;  // qualsevol altre caràcter: forma part del contingut de la trama
    // Salvaguarda si mai arriba soroll. Cal marge per sobre del camp de
    // text més llarg (V69=descripció, fins a MAX_DESCRIPTION_LENGTH bytes
    // + "V69=").
    if (btFrameBuffer.length() > 512) btFrameBuffer = "";
  }
}

// Buida la cua de trossos rebuts per BLE (omplerta pel callback onWrite(),
// que corre a la tasca de l'stack BLE) i alimenta cada byte a feedByte() —
// aquí, dins loop(), és on realment es crida processFrame()/handleWrite() i
// es toca tot l'estat global (dmxData, etc.), mai directament des del
// callback (vegeu el comentari de capçalera "Concurrència").
void drainBleRxQueue() {
  BleRxChunk chunk;
  while (xQueueReceive(bleRxQueue, &chunk, 0) == pdTRUE) {
    for (size_t i = 0; i < chunk.length; i++) {
      feedByte((char)chunk.data[i]);
    }
  }
}

// ---------------------------------------------------------------------------
// Callbacks de NimBLE — s'executen a la tasca de l'stack BLE, no a loop()
// (vegeu el comentari de capçalera "Concurrència").
// ---------------------------------------------------------------------------

// NimBLE-Arduino 1.4.x (a diferència de la 2.x) encara fa servir
// ble_gap_conn_desc* en comptes de NimBLEConnInfo& en aquests callbacks —
// confirmat compilant contra la versió que resol aquest platformio.ini.
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server, ble_gap_conn_desc *desc) override {
    bleClientConnected = true;
    bleConnHandle = desc->conn_handle;
    pinAuthenticated = false;
  }

  void onDisconnect(NimBLEServer *server, ble_gap_conn_desc *desc) override {
    bleClientConnected = false;
    bleConnHandle = BLE_HS_CONN_HANDLE_NONE;
    // NimBLE atura l'advertising en connectar — cal reiniciar-lo explícitament
    // per poder acceptar una reconnexió sense haver de reiniciar l'ESP32.
    NimBLEDevice::startAdvertising();
  }
};

class WriteCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic,
               ble_gap_conn_desc *desc) override {
    const std::string value = characteristic->getValue();
    size_t offset = 0;
    while (offset < value.size()) {
      BleRxChunk chunk;
      chunk.length = min(value.size() - offset, BLE_RX_CHUNK_MAX);
      memcpy(chunk.data, value.data() + offset, chunk.length);
      // Temps d'espera 0: si la cua estigués plena (no hauria de passar mai
      // amb 8 posicions de 256 bytes cadascuna per a trames de com a màxim
      // ~400 bytes), és millor descartar aquest tros que bloquejar la tasca
      // BLE.
      xQueueSend(bleRxQueue, &chunk, 0);
      offset += chunk.length;
    }
  }
};

// ---------------------------------------------------------------------------
// LED d'estat
// ---------------------------------------------------------------------------

// Actualitza el LED: fix encès i quiet si no hi ha cap mòbil connectat,
// parpellejant si n'hi ha un.
void updateStatusLed() {
  static uint32_t lastToggle = 0;  // recorda entre crides quan va canviar per última vegada
  static bool ledOn = false;       // recorda entre crides si el LED està encès ara mateix

  if (!bleClientConnected) {           // ¿no hi ha cap dispositiu BLE connectat?
    digitalWrite(STATUS_LED_PIN, HIGH);  // no: LED fix encès i quiet
    ledOn = true;                        // per si es connecta tot seguit, el parpelleig comença encès
    return;
  }

  // hi ha un mòbil connectat: fem parpellejar el LED cada LED_BLINK_MS mil·lisegons
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

// Engega la pila BLE: dispositiu, servei, característiques d'escriptura/
// notificació, i comença l'advertising amb el nom actual (btDeviceName).
void bleInit() {
  NimBLEDevice::init(btDeviceName.c_str());
  NimBLEDevice::setMTU(BLE_PREFERRED_MTU);

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  NimBLEService *service = bleServer->createService(BLE_SERVICE_UUID);

  NimBLECharacteristic *writeCharacteristic = service->createCharacteristic(
      BLE_WRITE_CHAR_UUID,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  writeCharacteristic->setCallbacks(new WriteCallbacks());

  bleNotifyCharacteristic = service->createCharacteristic(
      BLE_NOTIFY_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);

  service->start();

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->start();
}

// S'executa un únic cop quan arrenca l'ESP32.
void setup() {
  // UART0 (USB) queda lliure per debug — no interfereix amb el DMX (UART2)
  // ni amb la pila BLE (que fa servir el controlador BT intern).
  Serial.begin(115200);            // engega el port sèrie de debug (per USB)
  pinMode(STATUS_LED_PIN, OUTPUT);  // configura el pin del LED com a sortida

  sceneLoad();                      // recupera l'última escena desada (o zeros)
  loadBtName();                     // recupera el nom Bluetooth (o el de per defecte)
  loadPin();                        // recupera el PIN de connexió (o cap, per defecte)
  loadNames();                      // recupera els noms de canal, pessebe i descripció
  dmxInit();                        // engega el driver DMX

  bleRxQueue = xQueueCreate(8, sizeof(BleRxChunk));
  bleInit();  // engega el BLE amb el nom actual

  Serial.println("ARDMX One iniciat");  // confirma per Serial que l'arrencada ha anat bé
}

// S'executa contínuament, un cop rere l'altre, mentre l'ESP32 estigui engegat.
void loop() {
  drainBleRxQueue();  // processa qualsevol trama BLE rebuda des de l'última volta
  updateStatusLed();   // actualitza l'estat del LED

  // Si hi ha canvis pendents (sceneDirty) i ja ha passat prou temps sense nous canvis, desa a NVS
  if (sceneDirty && millis() - lastChangeMillis > SAVE_DEBOUNCE_MS) {
    sceneSave();
    Serial.println("Escena desada a NVS");
  }
  if (namesDirty && millis() - lastChangeMillis > SAVE_DEBOUNCE_MS) {
    saveNames();
    Serial.println("Noms/pessebe/descripció desats a NVS");
  }

  // Envia com a màxim cada DMX_SEND_INTERVAL_MS (vegeu el comentari a la
  // constant) — no a cada volta del bucle sense límit.
  static uint32_t lastDmxSendMillis = 0;
  const uint32_t now = millis();
  if (now - lastDmxSendMillis >= DMX_SEND_INTERVAL_MS) {
    dmxSendFrame();
    lastDmxSendMillis = now;
  }
}
