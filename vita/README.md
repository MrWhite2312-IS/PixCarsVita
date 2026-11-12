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

Controles del preset 1 del juego (acelerar A, frenar X, freno de mano Y, powerslide RT).

| Botón | Acción |
|---|---|
| ✕ | Acelerar |
| □ / L | Frenar; parado, marcha atrás |
| Stick izquierdo | Girar |
| △ | Freno de mano |
| R (mantener) | Powerslide (derrape controlado, con el volante a fondo) |
| Cruceta ↓ | Recolocar el coche (derecho, en el sitio) |
| ○ | Cámara libre (stick izq. mover, stick der. mirar, L siguiente escenario) |
| Cruceta → | Siguiente canción |

La física (`src/car.c`) usa los parámetros del propio juego, `mcq.phy` (lo copia
`vitaconv.py char`): peso, centro de gravedad, inercia, suspensión y radio por rueda,
curvas de fuerza de neumático (fórmula del ELF de PS2), curva HP/RPM, marchas y cambios
automáticos, frenos, aerodinámica, curva de dirección por velocidad y cámara. Parado
5 s, McQueen reproduce sus animaciones `beft_idle` originales.
Al conducir, la carrocería se inclina como en el juego: mezcla las animaciones
`bef_lean_*` (lateral según el volante; adelante/atrás según acelerador y freno; juego
lento o rápido entre 30 y 100 mph), con la lógica descifrada del ELF de PS2.

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
