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

## Nom del dispositiu Bluetooth
Lliurement editable (fins a 12 caràcters, només lletres i xifres) des de la
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
