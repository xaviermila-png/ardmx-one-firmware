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
1. Obre la carpeta `firmware/ardmx_one` com a projecte PlatformIO (icona de la formiga a la barra lateral, o `File > Open Folder`).
2. Connecta l'ESP32 per USB (fase de prototipat).
3. **PlatformIO: Build** (✓ a la barra inferior) o `pio run`.
4. **PlatformIO: Upload** (→ a la barra inferior) o `pio run --target upload`.
5. **PlatformIO: Monitor** (🔌) o `pio device monitor` per veure els logs de debug per USB (115200 baud) — completament independent del DMX i del Bluetooth.

## Nom del dispositiu Bluetooth
Es publica com **`ARDMXOne_` + un número de 3 xifres** (per defecte `001`,
desat a NVS). L'app identifica que es tracta d'un ARDMX One pel nom del
dispositiu aparellat, no per cap camp del protocol — si es fabriquen diverses
unitats, cal donar-los números únics. Això es pot fer sense reflashejar: des
de la pantalla de Debug de l'app, un cop connectat, hi ha un camp per
canviar el número (l'ESP32 el desa i es reinicia tot sol).
