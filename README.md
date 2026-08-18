# ARDMX One — firmware ESP32

Controlador DMX512 d'una única escena estàtica (fins a 512 canals), sense
àudio ni cicles, controlat per Bluetooth Classic (SPP) des de la mateixa app
Flutter que ja controla l'ARDMX4. Detalls del disseny i del subconjunt de
protocol implementat: veure els comentaris a `src/main.cpp`.

## Maquinari
- ESP32 DevKit V1 (ESP32-WROOM-32)
- Mòdul MAX485 amb direcció automàtica: GND, VCC, RXD←GPIO22, TXD→GPIO21 (no usat activament)
- LED d'estat a GPIO2 (encès fix = client Bluetooth connectat, parpelleig = esperant connexió)
- Alimentació 12V per jack DC (regulador ja integrat a la placa)

## Compilar i pujar (VS Code + PlatformIO)
1. Obre aquesta carpeta com a projecte PlatformIO (icona de la formiga a la barra lateral, o `File > Open Folder`).
2. Connecta l'ESP32 per USB (fase de prototipat).
3. **PlatformIO: Build** (✓ a la barra inferior) o `pio run`.
4. **PlatformIO: Upload** (→ a la barra inferior) o `pio run --target upload --upload-port COMx`.
5. **PlatformIO: Monitor** (🔌) o `pio device monitor --port COMx --baud 115200` per veure els logs de debug per USB — completament independent del DMX i del Bluetooth.

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
| `boot_app0.bin` | `0xe000` | Selector d'partició OTA (de quin `app` arrenca) |
| `firmware.bin` | `0x10000` | El programa (l'aplicació ARDMX One en si) |

**La memòria NVS (`0x9000`-`0xdfff`, entre la taula de particions i
`boot_app0`) mai es toca** — reflashejar amb aquest instal·lador **no esborra**
la configuració desada (noms de canal, nom del pessebre, descripció, nombre de
canals actius, nom Bluetooth...). Només un `esptool erase_flash` complet (que
l'instal·lador no fa) l'esborraria.

**Per actualitzar els binaris de l'instal·lador** quan canvia el codi font
(`src/main.cpp`): `pio run` per compilar, i després copiar
`.pio/build/esp32dev/bootloader.bin`, `.pio/build/esp32dev/partitions.bin` i
`.pio/build/esp32dev/firmware.bin` a la carpeta `bin/` de totes dues
instal·ladors (`boot_app0.bin` no canvia mai, no cal tornar-lo a copiar).

## Nom del dispositiu Bluetooth
Lliurement editable (fins a 15 caràcters, només lletres i xifres) des de la
pantalla "Configuració del sistema" de l'app, un cop connectat (escriu V63).
El nou nom es desa a NVS i l'ESP32 es reinicia perquè el Bluetooth arrenqui
amb el nom actualitzat — cal oblidar i reaparellar el dispositiu des dels
ajustos de Bluetooth d'Android per veure el nom nou. L'app identifica que es
tracta d'un ARDMX One mitjançant un handshake explícit (V64), no pel nom
Bluetooth, així que el nom es pot canviar sense afectar la identificació.

## App de control
Controlat des de la mateixa app Flutter que l'ARDMX4: [ardmx-app](https://github.com/xaviermila-png/ARDMX4_APP).

## Llicència
Creative Commons Atribució-NoComercial-CompartirIgual 4.0 (CC BY-NC-SA 4.0).
Vegeu [LICENSE](LICENSE).
