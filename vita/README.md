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

### Cinemáticas del modo historia

Como en el juego de PS2 (0x211cb0), una partida nueva empieza por la actividad
`RR_1_1\CS_1_1A` y sigue cada `NextActivity`: CS_1_1A → CS_1_1A2 → CS_1_1C → la carrera
RR_1_1 (el sueño de McQueen) → CS_1_1Z, y luego se conduce libre por Radiator Springs. Son cinemáticas en el motor: los personajes animados en sus posiciones del `.toy`,
cámaras animadas, la voz en español, subtítulos con las capas `Letterbox` y `Subtitles` del
juego y fundidos. ✕ o START saltan cada una.

```bash
for n in cs_1_1a cs_1_1a2 cs_1_1c cs_1_1z; do python tools/vitacutscene.py rr_1_1/$n extracted/vita/act/$n; done
python tools/vitarace.py rr_1_1/rr_1_1 extracted/vita/act/rr_1_1
for c in fle ram hud sal gui lui mat; do python tools/vitaconv.py char $c extracted/vita/$c zzz_no_anims; done
python tools/vitaui.py extracted/vita/ui
```

Copia `extracted/vita/act/` a `ux0:data/pixcars/act/`, la carpeta de cada personaje
(`model.vcm`) a `ux0:data/pixcars/<código>/` y `ui/` de nuevo. Aún no se dibujan las
partículas (humo de los escapes) ni las sombras de los personajes.

La carrera sigue el código del juego de PS2 (`race.c`): los cinco en una fila de salida
(`StartLineOrder` 1–5), cuenta atrás de 3 s con los controles bloqueados, el conductor IA del
juego (dirección, acelerador/freno, punto 60 ft por delante en la línea, desvío aleatorio, tiempos
de vuelta del `.rpt` por corredor con el ajuste al jugador) y el gestor de vueltas del juego
(vueltas, posición, distancia a meta). Dificultad Normal hasta portar su menú.
Aún falta: recolocar el coche al salirse o ir en dirección contraria, que la IA esquive a otros
coches, sus derrapes/saltos, los 91 objetos de `rr_1_1.toy`, choques entre coches, ruedas de los
rivales y el resto del marcador.

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
| ○, soltar, ○ (stick recto, > 50 mph) | Giro de 180° y conducir hacia atrás; bajo 50 mph vuelve a girar |
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
`MCQ_EF_DonutsL/R` y golpes `MCQ_EF_MinHit*/MaxHit*`. También: control de aterrizaje 0x386060 (`[LandingControl]`), conducir hacia atrás 0x386790, frases de McQueen en español (`Audio\Dialogue\lm.aud`: Cool tras el 180, el donut o el turbo, Hit en golpes, Idle con las animaciones de espera; esperas y probabilidades de `dialogue.txt`) y el HUD: el medidor de turbo `BoostMeterOverlay` (0x23dde0). Vuelve a ejecutar también `vitaui.py`. Para tenerlas, convierte con:

```bash
python tools/vitaconv.py char McQ extracted/vita/mcq bef_kch bef_won bef_breath bef_ow bef_lost beft_1_6a. beft_idle bef_lean e_eyes mcq_ef_boost mcq_ef_donuts mcq_ef_minhit mcq_ef_maxhit mcq_ef_cool
python tools/vitaaudio.py extracted/vita/audio
```

Cámara y HUD del juego: la cámara de persecución es la `CarsCameraFollow` del ELF de PS2
(actualización 0x21f4f0) con los presets de `data\rscamera.ini` (`[Preset1]` + `[McQ1]`:
26° por encima, 19 ft detrás, FOV 75°; filtros por eje `LookFilters`/`PosFilters`, ángulo
reducido a baja velocidad, giro con `CameraSwing`). `vitaconv.py char` copia `rscamera.ini`
en la carpeta del personaje. Triángulo (Camera Preset) pasa Preset 1 → Preset 2 → cámara
libre. El HUD añade el velocímetro digital `SpeedometerOverlay` (0x3b01b0, en km/h; «R» en
marcha atrás) y la brújula `ExploreMap` (0x3ac950, gira con la vista, flecha naranja al
norte). Vuelve a ejecutar `vitaui.py` (nueva fuente `48pt_bullet_stroke`). Sin hacer aún:
colisión de la cámara, efecto de FOV del turbo, temblor, y los puntos del radar (esperan a las
actividades del modo explorar).

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

Carrera (cinco coches con la IA en su casilla de salida en el escenario real; todos deben terminar
las vueltas con tiempos parecidos a los del `.rpt`; el último número opcional traza a un corredor):

```bash
gcc -O2 -std=c99 -Ivita/src vita/src/race.c vita/src/car.c vita/src/scene.c vita/src/vcm.c vita/tests/race_test.c -lm -o race_test
./race_test extracted/vita/act/rr_1_1/race.txt extracted/vita/scene/rs.vcs extracted/vita
```

Iluminación (lectura del `.lit`, las tres luces por objeto, sombreado):

```bash
gcc -O2 -std=c99 -Ivita/src vita/src/light.c vita/tests/light_test.c -lm -o light_test
./light_test extracted/vita/scene/rs.lit
```

## Iluminación de la escena

`vitascene.py` escribe junto a cada `<escena>.vcs` un `<escena>.lit` con las luces del propio
`.scn` (zona 1): ambiente, sol direccional, focos (con color negativo, que oscurecen) y niebla.
La fórmula sale del shader de vértices del juego `DXVAnimSkin4Diffuse.xvu` (microcódigo NV2A
desensamblado): ambiente + 3 luces × max(0, N·L) × color, saturado, y niebla lineal.

- **Escenario**: las mallas sin luz horneada se iluminan al exportar con esas luces (por
  rotación de cada instancia); las prelit conservan sus colores.
- **McQueen**: cada fotograma recibe el sol y los focos que más le afectan en su posición.
- **Niebla y cielo**: color y distancias de `[Fog]` (el final se recorta a la distancia de
  dibujo de la Vita).
- **Sombra**: la textura `McQ_shadow` del juego bajo el coche (el original usa sombras
  proyectadas en tiempo real).

Re-exporta las escenas y el personaje y copia `scene/` y `mcq/` (con `shadow.vcs`):

```bash
python tools/vitascene.py data/scenes/rs/rs.scn extracted/vita/scene/rs.vcs start=9760,440,19560 dir=0.58,-0.08,0.81
```
