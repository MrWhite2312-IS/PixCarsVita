# PixCars Vita — visor de personajes (hito 1)

Primer paso del port: carga un personaje del juego convertido desde **tu propia copia
de Cars (2006, PC)**, reproduce sus animaciones con *skinning* en CPU y lo dibuja con
[vitaGL](https://github.com/Rinnegatamante/vitaGL). El VPK no contiene datos del juego.

## 1. Convertir los datos (en el PC)

Con los datos del juego en `PC data/` (ver la raíz del repo):

```bash
python tools/vitaconv.py char McQ extracted/vita/mcq bef_kch bef_won bef_breath bef_ow bef_lost beft_1_6a.
```

Sin filtros convierte todas las animaciones del personaje (McQueen tiene 337).

## 2. Copiar a la Vita

Copia el contenido de `extracted/vita/mcq/` a `ux0:data/pixcars/mcq/`
(VitaShell por FTP o USB): `model.vcm`, los `.vca`, `anims.txt` y `mcq.phy`.

### Audio

```bash
python tools/vitaaudio.py extracted/vita/audio
```

Copia `extracted/vita/audio/` a `ux0:data/pixcars/audio/`: música del juego (música de
Radiator Springs y canciones con licencia, `music/playlist.txt` fija el orden) y el motor de
McQueen (los 6 bucles y el perfil `[Mcq]` de `carengine.aud`: volumen y tono por RPM; las
RPM salen de la velocidad por los tramos de `mcqengineaudio.ini`). Los `.wav` del juego son
IMA ADPCM sin cabeceras de bloque; la Vita los decodifica al vuelo.

### Vídeos (logos y cinemáticas)

Necesita ffmpeg con los decodificadores Bink (p. ej. la compilación de BtbN):

```bash
FFMPEG=/ruta/a/ffmpeg python tools/vitamovies.py extracted/vita/movies
```

Copia `extracted/vita/movies/` a `ux0:data/pixcars/movies/`. Los `.bik` de 512×512 se
mostraban estirados a 4:3; se restaura 4:3 y se recorta el centro a 16:9 (960×544), H.264
para el decodificador por hardware. Al arrancar suenan los logos del juego en su orden
original (BVG, THQ, Rainbow, Pixar); ✕ o START los saltan.

Menú del juego (fuentes, textos en español, capas y cámaras del autocine) y su escena:

```bash
python tools/vitaui.py extracted/vita/ui
python tools/vitascene.py 'data\scenes\fe\fe.scn' extracted/vita/scene/fe.vcs
```

Copia `extracted/vita/ui/` a `ux0:data/pixcars/ui/` y `fe.vcs` a `ux0:data/pixcars/scene/`.
Tras los logos sale la pantalla de título («Pulsa el botón START»); START o ✕ abre el menú
principal (←/→ elige, ✕ acepta, ○ vuelve al título). «Modo historia» entra a Radiator
Springs; «Salir del juego» cierra. En juego, START vuelve al menú.
Animaciones (entrada del fondo, franja de cuadros, carrusel de carteles) y sonidos del menú
(`sfx/ui_fe.aud`, grillos del autocine) salen del juego: tras actualizar, vuelve a ejecutar
`vitaaudio.py` y `vitaui.py` y copia `audio/` y `ui/` de nuevo.

## 3. Compilar

Requiere [VitaSDK](https://vitasdk.org) con vitaGL instalado (`vdpm vitaGL`).

```bash
cd vita
cmake -S . -B build
cmake --build build
```

Genera `build/PixCarsVita.vpk`. Instálalo con VitaShell.

## Controles

| Botón | Acción |
|---|---|
| Stick izquierdo | Girar la cámara |
| Cruceta ↑ / ↓ | Acercar / alejar |
| L / R | Animación anterior / siguiente |
| ✕ | Pausa |
| ○ | Pose base |
| □ | Activar/desactivar iluminación |
| START | Volver al menú principal (sin menú exportado: salir) |

### Escenario (SELECT) — conducir a McQueen

Controles del **preset 1 de PS2** (`UI\controller_presets_ps2.txt` del disco). La Vita no tiene
L2/R2: R1→R, L1→L y R2 (Turbo) pasa al panel táctil trasero.

| Botón | Acción del juego |
|---|---|
| Stick izquierdo / cruceta ←→ | Girar (la cruceta usa sus propias tablas `DPadSteer*`) |
| ✕ | Acelerar |
| □ | Frenar. Parado: suelta y vuelve a pulsar para marcha atrás |
| ○ | Freno de mano (EBrake) |
| R (mantener) | Powerslide, con el stick a fondo y a más de 23 mph |
| L (mantener) + stick | Inclinar: conducir sobre dos ruedas (a más de 20 mph) |
| Panel trasero (mantener) | Turbo (NOX: 1,65 s, se recarga solo) |
| Stick (o cruceta) atrás y luego adelante | Saltar (a más de 20 mph, sin freno) |
| ✕ + □ parado, soltar □ | Burnout (salida con empuje) |
| ✕ + stick a fondo despacio | Donut (derrape a baja velocidad, sin botón) |
| SELECT | Recolocar el coche |
| △ | Cámara libre (stick izq. mover, stick der. mirar, L siguiente escenario, → siguiente canción) |

La física (`src/car.c`) usa los parámetros del propio juego, `mcq.phy` (lo copia
`vitaconv.py char`): peso, centro de gravedad, inercia, suspensión y radio por rueda,
curvas de fuerza de neumático (fórmula del ELF de PS2), curva HP/RPM, marchas y cambios
automáticos, frenos, aerodinámica, curva de dirección por velocidad y cámara. Parado
5 s, McQueen reproduce sus animaciones `beft_idle` originales.
Al conducir, la carrocería se inclina como en el juego: mezcla las animaciones
`bef_lean_*` (lateral según el volante; adelante/atrás según acelerador y freno; juego
lento o rápido entre 30 y 100 mph), con la lógica descifrada del ELF de PS2.

Las acciones de conducción siguen las funciones del vehículo del ELF de PS2 (tabla de
métodos en 0x4da2a0): controles 0x37fc90 (filtros de dirección, rampa de freno, retención
parado, marcha atrás, rampa de gas, carga de burnout), salto 0x381fc0 (`[Preload]`),
powerslide 0x382320 (tracción delantera durante el derrape, donut a baja velocidad),
freno de mano 0x382a00, inclinar 0x382b40 (`[Wheelstand]`), burnout 0x383030, control en el
aire y empuje del powerslide 0x3817c0 (`[Aerodynamics]`) y el NOX del motor 0x225810.
Sonidos: `Whl_Stnd`/`whl_down` al inclinar y `NOS_Boost` del turbo; caras: `MCQ_EF_Boost`,
`MCQ_EF_DonutsL/R` y golpes `MCQ_EF_MinHit*/MaxHit*`. Para tenerlas, convierte con:

```bash
python tools/vitaconv.py char McQ extracted/vita/mcq bef_kch bef_won bef_breath bef_ow bef_lost beft_1_6a. beft_idle bef_lean e_eyes mcq_ef_boost mcq_ef_donuts mcq_ef_minhit mcq_ef_maxhit mcq_ef_cool
python tools/vitaaudio.py extracted/vita/audio
```

Si la pantalla se queda roja, no encuentra `ux0:data/pixcars/mcq/model.vcm`.

## Prueba en el PC (sin VitaSDK)

El núcleo (`src/vcm.c`: carga, esqueleto, animación, *skinning*) es C99 portable:

```bash
gcc -O2 -std=c99 -Ivita/src vita/src/vcm.c vita/tests/host_test.c -lm -o host_test
./host_test extracted/vita/mcq/model.vcm extracted/vita/mcq/mcq_bef_kch1.vca 0.5
```

Imprime los vértices deformados; coinciden con `tools/pose.py` (error < 0,012 unidades,
por los pesos de hueso cuantizados a 8 bits).

Física del coche (lectura del `.phy`, plano, aceleración y marchas, giro, frenada, marcha atrás, pared y, opcional, un escenario real):

```bash
gcc -O2 -std=c99 -Ivita/src vita/src/car.c vita/src/scene.c vita/src/vcm.c vita/tests/car_test.c -lm -o car_test
./car_test extracted/vita/mcq/mcq.phy extracted/vita/scene/rs.vcs
```
