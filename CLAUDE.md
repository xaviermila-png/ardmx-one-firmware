# Instruccions per a Claude Code — ardmx-one-firmware

Firmware de l'ARDMX One (ESP32, PlatformIO). Respon sempre en català.

## Cada vegada que es publica una versió nova del firmware

Fes els passos en aquest ordre i, al final, digues a en Xavier què has fet i què li queda per fer a ell.

1. **Número de versió al codi** (`src/main.cpp`). Actualitza:
   - `FIRMWARE_VERSION_TEXT` — `"ARDMX One vX.Y"` (és el que l'app mostra a Crèdits, resposta a V62).
   - El camp `"firmware"` de `buildIdentifyJson()` (V64) — `"X.Y.0"`. El número major ha de continuar sent 2: l'app distingeix v1 i v2 per aquest número.
   - La línia «Versió actual» del `README.md`.
2. **Compila**: `pio run`.
3. **Copia els binaris** de `.pio/build/esp32dev/` a la carpeta `bin/` de **totes dues** carpetes d'instal·lador (`installer/` i `installer_standalone/`): `bootloader.bin`, `partitions.bin` i `firmware.bin`. `boot_app0.bin` no canvia mai.
4. **Actualitza `bin/version.txt`** a totes dues carpetes, amb aquest format exacte (el `.bat` el mostra i el generador del ZIP el llegeix):
   ```
   ARDMX One vX.Y
   Generat: AAAA-MM-DD
   ```
5. **Fes commit i push** a `main` (missatge en català).
6. **Genera el ZIP de l'instal·lador** amb l'script del repositori de la web
   ([xaviermila-png/lallumdelpessebre](https://github.com/xaviermila-png/lallumdelpessebre), `eines/genera_zip_firmware.py`):
   ```
   python3 eines/genera_zip_firmware.py <ruta ardmx-one-firmware> <ruta ardmx-evo-firmware> <carpeta de sortida>
   ```
   Genera els dos ZIP alhora; només cal adjuntar el d'aquest controlador: **`ardmx_one_instal-lador.zip`**.
   No canviïs el nom del fitxer: la web hi enllaça amb aquest nom exacte.
   No posis el ZIP dins del repositori: va adjunt a la Release.
7. **Crea la Release de GitHub**:
   - Etiqueta: **`ardmx-one-vX.Y`** (format de les etiquetes existents, p. ex. `ardmx-one-v1.0`).
   - Títol: `ARDMX One vX.Y`.
   - Adjunta `ardmx_one_instal-lador.zip` i marca-la com a **latest**.
   - Descripció (català i castellà): què canvia en aquesta versió, com instal·lar-la (descomprimir i doble clic a `install_ardmx_one.bat`, només Windows) i l'avís que la instal·lació esborra tota la memòria de la placa (cal exportar abans des de l'APP ARDMX, Eines → Exportar).
   - Amb `gh`: `gh release create ardmx-one-vX.Y <zip> --title "ARDMX One vX.Y" --notes-file <notes.md> --latest`.
     Si `gh` no hi és o no té permís, prepara-ho tot i dona a en Xavier l'enllaç
     `https://github.com/xaviermila-png/ardmx-one-firmware/releases/new?tag=ardmx-one-vX.Y&title=ARDMX%20One%20vX.Y`
     perquè la creï ell des de l'ordinador.
8. **Web lallumdelpessebre.cat**: a les pàgines «Descàrregues» (CA, pàgina 278) i «Descargas» (ES, pàgina 847), targeta «Firmware ARDMX One i ARDMX EVO»:
   - el text del botó (`ARDMX One vX.Y`);
   - el paràgraf «Versions: … compilades el …» / «Versiones: … compiladas el …».
   Els botons apunten a `https://github.com/xaviermila-png/ardmx-one-firmware/releases/latest/download/ardmx_one_instal-lador.zip`, que sempre baixa l'última Release: l'enllaç no s'ha de canviar.
   Abans d'editar les pàgines (ja publicades), ensenya a en Xavier el canvi i espera que digui que sí. Si no tens accés a WordPress, digues-li exactament quin text ha de canviar.
9. **Comprova** que l'enllaç `releases/latest/download/...` baixa el ZIP nou i que dins hi ha el `version.txt` correcte.

## Regles fixes

- L'instal·lador (`install_ardmx_one.bat`) **sempre** esborra tota la memòria de la placa i la deixa de fàbrica. No s'hi afegeix cap camí d'actualització que conservi la configuració.
- Els instal·ladors són **només per a Windows**: les rutes del ZIP amb «\» i els salts de línia CRLF són intencionats.
- Els textos per a l'usuari es refereixen al menú **Eines** de l'app per exportar/importar (Eines → Exportar), com al manual de l'APP ARDMX.
- Aquest firmware és només per a l'ARDMX One **v2**. Les unitats v1 (Bluetooth Classic) no es reflashegen mai; l'avís és al LLEGEIX-ME del ZIP.
