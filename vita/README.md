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
(VitaShell por FTP o USB): `model.vcm`, los `.vca` y `anims.txt`.

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
| START | Salir |

### Escenario (SELECT) — conducir a McQueen

| Botón | Acción |
|---|---|
| R / ✕ | Acelerar |
| L / □ | Frenar; parado, marcha atrás |
| Stick izquierdo | Girar |
| ○ | Freno de mano |
| Cruceta ↓ | Recolocar el coche (derecho, en el sitio) |
| △ | Cámara libre (stick izq. mover, stick der. mirar, L siguiente escenario) |

La física (`src/car.c`) es un sólido rígido con 4 ruedas: suspensión por rayos contra la
geometría sólida del escenario, agarre con círculo de fricción y choques contra paredes.

Si la pantalla se queda roja, no encuentra `ux0:data/pixcars/mcq/model.vcm`.

## Prueba en el PC (sin VitaSDK)

El núcleo (`src/vcm.c`: carga, esqueleto, animación, *skinning*) es C99 portable:

```bash
gcc -O2 -std=c99 -Ivita/src vita/src/vcm.c vita/tests/host_test.c -lm -o host_test
./host_test extracted/vita/mcq/model.vcm extracted/vita/mcq/mcq_bef_kch1.vca 0.5
```

Imprime los vértices deformados; coinciden con `tools/pose.py` (error < 0,012 unidades,
por los pesos de hueso cuantizados a 8 bits).

Física del coche (plano, giro, frenada, pared y, opcional, un escenario real):

```bash
gcc -O2 -std=c99 -Ivita/src vita/src/car.c vita/src/scene.c vita/src/vcm.c vita/tests/car_test.c -lm -o car_test
./car_test extracted/vita/scene/rs.vcs
```
