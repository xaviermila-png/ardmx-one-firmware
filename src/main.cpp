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
    V08      nombre de canals actius (1-512) que realment s'envien per DMX
    V41      arma/desarma el reset de fàbrica (mateix mecanisme que l'ARDMX4)
    V42      confirma el reset de fàbrica (només té efecte si V41 ja és 1)
    V62      versió de firmware (text), demanada per l'app en connectar
    V64      identificació del dispositiu (JSON, només lectura)
    V65-V67  nom (text) del canal DMX actualment assignat a cada slot 1-3
             (mateixa indirecció que V01-03/V04-06 — vegeu selectedChannel)
    V68      nom del pessebe (text, fins a 32 caràcters)
    V69      descripció (text, fins a 128 caràcters)

  La resta d'índexs del mapa V[] de l'ARDMX4 (música, cicle, escenes,
  transicions, reset, etc.) no s'implementen — no tenen sentit en aquest
  maquinari i simplement s'ignoren si arriben.

  Nom Bluetooth: lliurement editable des de la pantalla de Configuració de
  l'app (V63=nomNou) — fins a 12 caràcters, només lletres i dígits (sense
  espais, accents ni símbols; vegeu sanitizeName()). El nou nom es desa a
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
// BluetoothSerial: exposa el Bluetooth Classic (SPP) de l'ESP32 com si fos un Serial més
#include "BluetoothSerial.h"
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

// LED d'estat: encès fix amb client Bluetooth connectat, parpellejant si no.
constexpr int STATUS_LED_PIN = 2;       // pin on va connectat el LED indicador
constexpr uint32_t LED_BLINK_MS = 500;  // cada quants ms canvia d'estat el parpelleig

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
constexpr int MAX_BLUETOOTH_NAME_LENGTH = 12;  // igual que el límit de l'app

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

// Resposta a V64 (identificació). "firmware" és una versió pròpia d'aquest
// esquema JSON (semver), independent del text humà de V62.
const char *IDENTIFY_JSON =
    "{\"tipus\":\"ARDMX_ONE\",\"firmware\":\"1.0.0\",\"num_canals_max\":512}";

// ---------------------------------------------------------------------------
// Estat global
// ---------------------------------------------------------------------------

BluetoothSerial SerialBT;  // objecte que gestiona la connexió Bluetooth Classic (SPP)
Preferences prefs;         // objecte que gestiona la lectura/escriptura a la NVS

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
uint32_t lastChangeMillis = 0;  // instant (millis()) de l'últim canvi de valor

// V41: armat/desarmat del reset de fàbrica (vegeu performFactoryReset()). El
// nom Bluetooth (V63) NO es toca aquí — un reset de fàbrica només afecta
// l'escena (valors de canal) i el nombre de canals actius.
bool resetArmed = false;

String btFrameBuffer;  // acumula els caràcters d'una trama Bluetooth mentre arriba

String btDeviceName;  // nom Bluetooth actual (fins a MAX_BLUETOOTH_NAME_LENGTH caràcters)

// Nom editable de cada canal DMX (1..512), indexat 0-based pel número de
// canal (channelNames[0] = nom del canal DMX 1). Es desa com UN sol blob a
// la NVS (vegeu saveNames()) — 512 x 16 bytes = 8 KB, no una clau per canal.
char channelNames[MAX_DMX_CHANNEL][MAX_CHANNEL_NAME_LENGTH + 1];

String pessebeName;  // nom del pessebe (V68), lliurement editable
String descripcio;   // descripció (V69), lliurement editable

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

// Marca que hi ha un canvi pendent i reinicia el comptador de temps del debounce.
void markDirty() {
  sceneDirty = true;
  lastChangeMillis = millis();  // guarda "ara" com a últim instant de canvi
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

// Filtra qualsevol caràcter que no sigui una lletra ASCII o un dígit (sense
// espais, accents ni símbols) i talla a MAX_BLUETOOTH_NAME_LENGTH.
String sanitizeName(const String &rawInput) {
  String clean = "";
  for (unsigned int i = 0; i < rawInput.length(); i++) {
    const char c = rawInput.charAt(i);
    if (isAlphaNumeric(c)) clean += c;
    if ((int)clean.length() >= MAX_BLUETOOTH_NAME_LENGTH) break;
  }
  return clean;
}

// Elimina '!' i '$' (delimitadors de trama del protocol) i retalla a com a
// màxim maxBytes BYTES — a diferència de sanitizeName(), aquí es permet
// qualsevol altre caràcter (accents, espais...) perquè aquests camps són
// text lliure (noms de canal, pessebe, descripció).
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
    if (c != '!' && c != '$') clean += c;
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

  if (prefs.isKey("chnames")) {
    const size_t bytesRead =
        prefs.getBytes("chnames", channelNames, sizeof(channelNames));
    if (bytesRead != sizeof(channelNames)) {
      memset(channelNames, 0, sizeof(channelNames));
    }
  } else {
    memset(channelNames, 0, sizeof(channelNames));
  }

  pessebeName = prefs.isKey("pessebe") ? prefs.getString("pessebe", "") : "";
  descripcio = prefs.isKey("descripcio") ? prefs.getString("descripcio", "") : "";

  prefs.end();
}

// Es crida cada cop que canvia un nom de canal, el nom del pessebe o la
// descripció — es desa a l'instant (no cal el debounce de l'escena: aquí
// cada canvi ja és una acció puntual de l'usuari en confirmar un camp de
// text, no un arrossegament continu com els sliders).
void saveNames() {
  prefs.begin("ardmxone", false);
  prefs.putBytes("chnames", channelNames, sizeof(channelNames));  // un sol blob pels 512 noms
  prefs.putString("pessebe", pessebeName);
  prefs.putString("descripcio", descripcio);
  prefs.end();
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
// (tots els canals a 0) i el nombre de canals actius al seu valor de
// fàbrica, i ho desa immediatament (no cal esperar el debounce, ja que és
// una operació explícita i puntual). El nom Bluetooth es manté intacte.
void performFactoryReset() {
  memset(&dmxData[1], 0, MAX_DMX_CHANNEL);          // tots els canals a 0 (apagat)
  numeroCanals = roundDownToMultipleOf3(MAX_DMX_CHANNEL);  // torna al valor de fàbrica
  selectGroup(1);                                    // selecció dels sliders de tornada a 1,2,3
  sceneSave();                                        // desa immediatament (també neteja sceneDirty)
  Serial.println("Reset de fàbrica: escena i nombre de canals reinicialitzats");
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

// Igual que replyNumber() però pel valor de text.
void replyText(int index, const char *text) {
  SerialBT.print('!');
  SerialBT.print('V');
  if (index < 10) SerialBT.print('0');
  SerialBT.print(index);
  SerialBT.print('=');
  SerialBT.print(text);
  SerialBT.print('$');
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

// Es crida en rebre V65/V66/V67=<nom nou> — assigna el nom al canal DMX que
// actualment ocupa aquest slot (mateixa indirecció que V01-03/V04-06). A
// diferència del nom Bluetooth, no cal reiniciar: és només dada, no afecta
// la pila BT.
void handleChannelNameChange(int index, const String &rawInput) {
  const int slot = index - 65;                // 0, 1 o 2
  const int channel = selectedChannel[slot];   // canal DMX real d'aquest slot
  const String clean = sanitizeText(rawInput, MAX_CHANNEL_NAME_LENGTH);
  clean.toCharArray(channelNames[channel - 1], MAX_CHANNEL_NAME_LENGTH + 1);
  saveNames();
  replyText(index, channelNames[channel - 1]);
}

// Es crida en rebre V68=<nom nou> (nom del pessebe).
void handlePessebeNameChange(const String &rawInput) {
  pessebeName = sanitizeText(rawInput, MAX_PESSEBE_NAME_LENGTH);
  saveNames();
  replyText(68, pessebeName.c_str());
}

// Es crida en rebre V69=<text nou> (descripció).
void handleDescriptionChange(const String &rawInput) {
  descripcio = sanitizeText(rawInput, MAX_DESCRIPTION_LENGTH);
  saveNames();
  replyText(69, descripcio.c_str());
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
      // Retorna la identificació del dispositiu (JSON, vegeu IDENTIFY_JSON)
      replyText(64, IDENTIFY_JSON);
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
  } else if (index == 63) {
    handleNameChange(rhs);  // nom Bluetooth
  } else if (index >= 65 && index <= 67) {
    handleChannelNameChange(index, rhs);  // nom d'un dels 3 canals seleccionats
  } else if (index == 68) {
    handlePessebeNameChange(rhs);  // nom del pessebe
  } else if (index == 69) {
    handleDescriptionChange(rhs);  // descripció
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
      // Salvaguarda si mai arriba soroll. Cal marge per sobre del camp de
      // text més llarg (V69=descripció, fins a MAX_DESCRIPTION_LENGTH bytes
      // + "V69=").
      if (btFrameBuffer.length() > 512) btFrameBuffer = "";
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
  loadBtName();                     // recupera el nom Bluetooth (o el de per defecte)
  loadNames();                      // recupera els noms de canal, pessebe i descripció
  dmxInit();                        // engega el driver DMX
  SerialBT.begin(btDeviceName.c_str());  // engega el Bluetooth amb el nom actual

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
