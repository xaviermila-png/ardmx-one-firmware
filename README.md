# ARDMX One v2 — firmware ESP32

Controlador DMX512 (fins a 512 canals) amb **4 escenes** i **4 transicions
per canal DMX** (una per parell consecutiu d'escenes, cíclica 4→1; cada
canal pot tenir el seu propi tipus — Lineal, Salt, Ease In, Ease Out — i
percentatge de salt, independent de la resta de canals), controlat per
Bluetooth Low Energy (BLE/GATT) des de la mateixa app Flutter que controla
també l'ARDMX EVO. Detalls del disseny i del subconjunt de protocol
implementat: veure els comentaris a `src/main.cpp`.

> Aquest firmware és la **v2**. Les unitats ARDMX One **v1** ja desplegades
> (una única escena estàtica, sense transicions, Bluetooth Classic) es
> mantenen amb el seu firmware original i mai es reflashegen amb aquest —
> l'app distingeix v1 de v2 pel número de versió del handshake
> d'identificació (V64), no pel tipus de dispositiu.

## Maquinari
- ESP32 DevKit V1 (ESP32-WROOM-32)
- Mòdul MAX485 amb direcció automàtica: GND, VCC, RXD←GPIO22, TXD→GPIO21 (no usat activament)
- LED d'estat a GPIO2 (encès fix = client BLE connectat, parpelleig = esperant connexió)
- Alimentació 12V per jack DC (regulador ja integrat a la placa)

## Compilar i pujar (VS Code + PlatformIO)
1. Obre aquesta carpeta com a projecte PlatformIO (icona de la formiga a la barra lateral, o `File > Open Folder`).
2. Connecta l'ESP32 per USB (fase de prototipat).
3. **PlatformIO: Build** (✓ a la barra inferior) o `pio run`.
4. **PlatformIO: Upload** (→ a la barra inferior) o `pio run --target upload --upload-port COMx`.
5. **PlatformIO: Monitor** (🔌) o `pio device monitor --port COMx --baud 115200` per veure els logs de debug per USB — completament independent del DMX i del BLE.

## Instal·lar el firmware sense compilar

Dues carpetes llestes per flashejar un ESP32 amb el firmware ja compilat, sense
haver d'obrir el projecte ni compilar res:

- **`installer_standalone/`** (recomanada) — completament autònoma. Porta el
  seu propi `esptool.exe` (binari oficial, ~14 MB, sense dependències). Es pot
  copiar la carpeta sencera a qualsevol PC amb Windows i executar-la tal qual,
  encara que no hi hagi Python ni PlatformIO instal·lats.
- **`installer/`** — versió lleugera (només els 4 fitxers `.bin`, uns 670 KB)
  que reutilitza el Python i l'`esptool.py` que ja porta PlatformIO instal·lat
  en aquest ordinador. Útil només en una màquina que ja té PlatformIO.

Totes dues funcionen igual: dona doble clic a `install_ardmx_one.bat`, escriu
el port COM on hi ha connectat l'ESP32 (es mostren els ports disponibles) i
prem Enter. Al final mostra "Firmware instal·lat correctament" o l'error
concret (port incorrecte, cable mal endollat...).

**Què fa exactament**: escriu 4 blocs al flash de l'ESP32 amb `esptool`:

| Fitxer | Adreça | Contingut |
|---|---|---|
| `bootloader.bin` | `0x1000` | Bootloader de l'ESP32 |
| `partitions.bin` | `0x8000` | Taula de particions |
| `boot_app0.bin` | `0xe000` | Selector de partició OTA (de quin `app` arrenca) |
| `firmware.bin` | `0x20000` | El programa (l'aplicació ARDMX One v2 en si) |

**La NVS (`0x10000`-`0x1ffff`, 64 KB) mai es toca** — reflashejar amb aquest
instal·lador **no esborra** la configuració desada (valors/transicions de
canal, noms, nom del pessebre, descripció, nombre de canals/escenes actius,
nom Bluetooth, PIN...). Només un `esptool erase_flash` complet (que
l'instal·lador no fa) l'esborraria.

> **Nota històrica**: `firmware.bin` es va moure de `0x10000` a `0x20000` (i
> la NVS, de 20 KB a 64 KB) perquè el format de canal nou (valors + 4
> transicions pròpies) ja no cabia a la NVS per defecte de l'ESP32 —
> `prefs.putBytes()` fallava en silenci amb `NOT_ENOUGH_SPACE`. Vegeu
> `partitions.csv` i el comentari de capçalera per al detall complet,
> incloent per què la NVS ha de començar just DESPRÉS de `0xe000`-`0xffff`
> (PlatformIO hi escriu sempre `boot_app0.bin`, independentment del
> contingut de la taula de particions).

**Per actualitzar els binaris de l'instal·lador** quan canvia el codi font
(`src/main.cpp`): `pio run` per compilar, i després copiar
`.pio/build/esp32dev/bootloader.bin`, `.pio/build/esp32dev/partitions.bin` i
`.pio/build/esp32dev/firmware.bin` a la carpeta `bin/` de totes dues
instal·ladors (`boot_app0.bin` no canvia mai, no cal tornar-lo a copiar).

## Nom del dispositiu Bluetooth
Lliurement editable (fins a 15 caràcters, només lletres, xifres i `_`) des de
la pantalla "Configuració" de l'app, un cop connectat (escriu V63). El nou
nom es desa a NVS i l'ESP32 es reinicia perquè el BLE arrenqui amb el nom
actualitzat. L'app identifica que es tracta d'un ARDMX One (i de quina
versió, v1 o v2) mitjançant un handshake explícit (V64), no pel nom
Bluetooth, així que el nom es pot canviar sense afectar la identificació.

## Escenes i transicions
4 escenes fixes; cada canal DMX desa un valor (0-255) per a cadascuna i les
seves pròpies 4 transicions (una per sortida d'escena, cíclica 4→1: escena
1→2, 2→3, 3→4, 4→1), amb tipus (Lineal/Salt/Ease In/Ease Out) i percentatge
de salt independents de la resta de canals. El nombre d'escenes actives
(1-4, configurable a Paràmetres) limita la navegació — amb menys de 4, el
mode Automàtic i la pantalla de Cicle queden desactivats a l'app si només
n'hi ha 1.

## App de control
Controlat des de la mateixa app Flutter que l'ARDMX EVO:
[ardmx_app](https://github.com/xaviermila-png/ardmx_app).

Aquest firmware no genera ni llegeix cap fitxer JSON — l'exportació/
importació de la configuració des de la pantalla "Configuració" de l'app es
munta i s'aplica sencera a base de peticions d'aquest mateix protocol
`!Vxx=valor$` (V71 per canal, V18/V08/V21-28/V68/V69...). L'esquema JSON
unificat (vàlid tant per a l'ARDMX One v2 com per a l'ARDMX EVO) es
documenta a `ardmx_app`.

## Llicència
Creative Commons Atribució-NoComercial-CompartirIgual 4.0 (CC BY-NC-SA 4.0).
Vegeu [LICENSE](LICENSE).
