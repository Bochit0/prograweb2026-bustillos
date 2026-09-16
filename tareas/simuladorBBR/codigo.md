# Explicación paso a paso: `simBBR.c`

Este documento recorre el código del simulador de fases BBR función por función, explicando qué hace cada parte y mostrando ejemplos reales de lo que se ve en terminal al ejecutarlo.

---

## 1. Cómo compilar y ejecutar

El programa está escrito en ANSI C (C89) puro, sin librerías externas, así que compila con cualquier `gcc` disponible:

```bash
$ gcc -ansi -pedantic -o bbr_simulador simBBR.c
$ ./bbr_simulador
```

Si todo compiló bien, `gcc` **no imprime nada** (silencio = éxito). Al ejecutar el binario, lo primero que aparece es el encabezado y el menú principal:

```bash
==================================================
 SIMULADOR DE FASES BBR (Bottleneck Bandwidth and RTT)
 Protocolo de control de congestion de Google para TCP
==================================================

--------------------- MENU PRINCIPAL ---------------------
1. MOVER (configurar) valores iniciales de la red
2. CAMBIAR valores de la red (simular cambio de enlace)
3. Ver valores actuales de la red y de BBR
4. Ejecutar simulacion PASO A PASO
5. Ejecutar simulacion AUTOMATICA (N rondas)
6. Reiniciar estado de BBR (conservando la red)
0. Salir
Seleccione una opcion:
```

---

## 2. Visión general del archivo

El código se organiza en cinco bloques, en este orden:

1. **Constantes y tipos** — parámetros fijos del modelo y las estructuras de datos.
2. **Configuración de la red** (`RedParams`) — lo que el usuario puede mover/cambiar.
3. **Estado de BBR** (`BBRState`) — todo lo que el algoritmo va calculando ronda a ronda.
4. **El motor de simulación** (`simular_round`) — el corazón del programa.
5. **Visualización y menú** — cómo se imprime todo y cómo se controla la ejecución.

---

## 3. Constantes y tipos

```c
#define WINDOW               10
#define BAR_WIDTH            50
#define PROBE_RTT_INTERVAL   10
#define PROBE_RTT_DURATION   2
#define MIN_CWND_PACKETS     4.0
#define PACKET_SIZE_KB       1.5

typedef enum { STARTUP, DRAIN, PROBE_BW, PROBE_RTT_PHASE } Phase;
```

- `WINDOW`: tamaño de la ventana deslizante (en rondas) que usa BBR para recordar el máximo ancho de banda y el mínimo RTT observados recientemente.
- `BAR_WIDTH`: cuántos caracteres de ancho tienen las barras ASCII en pantalla.
- `PROBE_RTT_INTERVAL` / `PROBE_RTT_DURATION`: cada cuántas rondas se dispara la fase `PROBE_RTT` y cuánto dura.
- `MIN_CWND_PACKETS` / `PACKET_SIZE_KB`: el tamaño mínimo de ventana que BBR jamás baja, expresado en "paquetes" simulados de 1.5 KB.
- `Phase`: un `enum` con las 4 fases del algoritmo — así el código puede comparar `b->phase == STARTUP` en vez de usar números sueltos.

---

## 4. `RedParams` — los valores que tú controlas

```c
typedef struct {
    double real_bandwidth; /* Mbps  - ancho de banda del cuello de botella */
    double real_rtt;       /* ms    - RTT base de propagacion              */
    double buffer_size;    /* KB    - tamano del buffer del cuello botella */
} RedParams;
```

Esta estructura representa **la red física**, no el algoritmo. Son los tres números que defines desde las opciones 1 y 2 del menú: cuánto aguanta el cuello de botella, cuánta latencia pura tiene el camino, y cuánto buffer hay antes de que se empiecen a perder paquetes.

---

## 5. `BBRState` — lo que el algoritmo va aprendiendo

```c
typedef struct {
    double btlbw_hist[WINDOW];
    double rtprop_hist[WINDOW];
    int    hist_count;
    int    hist_index;

    double btlbw_est;
    double rtprop_est;

    double pacing_gain;
    double cwnd_gain;
    double pacing_rate;
    double cwnd;
    double inflight;

    Phase  phase;
    int    round;
    int    rounds_in_phase;
    int    rounds_since_probertt;

    double startup_prev_btlbw;
    int    startup_plateau_count;

    double probebw_cycle[8];
    int    probebw_cycle_index;

    double ultimo_queue_delay;
    double ultimo_dropped;
    double ultimo_observed_rtt;
    double ultimo_bdp;
    double ultimo_capacity;
} BBRState;
```

Todo lo que BBR "sabe" en un instante dado vive aquí. Vale la pena notar tres grupos:

- **Historial** (`btlbw_hist`, `rtprop_hist`, `hist_count`, `hist_index`): un arreglo circular de tamaño `WINDOW` que guarda las últimas 10 mediciones, usado para calcular el máximo y el mínimo.
- **Estimaciones y controles activos** (`btlbw_est`, `rtprop_est`, `pacing_gain`, `cwnd_gain`, `pacing_rate`, `cwnd`, `inflight`): los valores que se recalculan cada ronda y determinan cuánto se envía.
- **Control de fases** (`phase`, `rounds_in_phase`, `startup_plateau_count`, `probebw_cycle_index`, etc.): contadores que el código usa para decidir cuándo saltar de una fase a otra.

---

## 6. Funciones de lectura de entrada

```c
double leer_double(double actual)
{
    char linea[128];
    double valor;

    if (fgets(linea, sizeof(linea), stdin) == NULL) {
        return actual;
    }
    if (sscanf(linea, "%lf", &valor) == 1) {
        return valor;
    }
    return actual;
}
```

`leer_double` y su equivalente `leer_entero` leen una línea completa con `fgets` y luego intentan interpretarla como número con `sscanf`. Si el usuario solo presiona ENTER (o escribe algo no numérico), la función devuelve el valor `actual` sin modificarlo — por eso en el menú puedes "dejar pasar" un campo sin cambiarlo.

---

## 7. Configuración de la red

```c
void configurar_red(RedParams *r, const char *titulo)
{
    printf("\n--- %s ---\n", titulo);

    printf("Ancho de banda del cuello de botella en Mbps [actual %.2f]: ", r->real_bandwidth);
    r->real_bandwidth = leer_double(r->real_bandwidth);
    if (r->real_bandwidth <= 0.0) r->real_bandwidth = 0.1;
    /* ... RTT y buffer siguen el mismo patron ... */
}
```

Esta única función atiende **tanto la opción 1 como la opción 2** del menú — la diferencia entre "mover valores iniciales" y "cambiar valores" no está en el código de `configurar_red` (es idéntico), sino en qué hace `main` después de llamarla:

- Opción 1 (`MOVER`) llama a `configurar_red(...)` y luego a `init_bbr_state(...)`, reiniciando el algoritmo desde cero.
- Opción 2 (`CAMBIAR`) llama solo a `configurar_red(...)`, dejando que BBR siga corriendo con su estado actual sobre la red nueva.

Ejemplo real de la opción 1 en terminal (cambiando a 15 Mbps, 30 ms y 50 KB de buffer):

```bash
--- MOVER VALORES INICIALES DE LA RED ---
Ancho de banda del cuello de botella en Mbps [actual 10.00]: 15
RTT base de propagacion en ms [actual 50.00]: 30
Tamano del buffer del cuello de botella en KB [actual 100.00]: 50
Valores de red actualizados correctamente.
```

---

## 8. `init_bbr_state` — poner a BBR en su punto de partida

```c
void init_bbr_state(BBRState *b, RedParams *r)
{
    static const double ciclo[8] = { 1.25, 0.75, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 };
    /* ... limpia el historial ... */
    b->btlbw_est   = 0.0;   /* aun no sabe cuanto ancho de banda hay */
    b->rtprop_est  = 1e9;   /* "infinito", para que el primer RTT medido siempre sea menor */
    b->pacing_gain = 2.89;
    b->cwnd_gain   = 2.89;
    b->phase       = STARTUP;
    /* ... */
}
```

Dos detalles de diseño importantes:

- `rtprop_est` arranca en `1e9` (mil millones) a propósito: como el código busca el **mínimo** RTT observado, necesita un valor inicial absurdamente alto para que la primera medición real siempre "gane" y quede guardada.
- `ciclo[8]` es la secuencia de ganancias que usará `PROBE_BW`: sondeo alcista (1.25), corrección (0.75) y seis rondas neutras (1.0).

---

## 9. `actualizar_historial` — la ventana deslizante

```c
void actualizar_historial(BBRState *b, double delivery_rate, double observed_rtt)
{
    int idx = b->hist_index;
    b->btlbw_hist[idx]  = delivery_rate;
    b->rtprop_hist[idx] = observed_rtt;
    b->hist_index = (idx + 1) % WINDOW;
    if (b->hist_count < WINDOW) b->hist_count++;

    /* recorre el historial y guarda el maximo y el minimo */
    for (i = 1; i < b->hist_count; i++) {
        if (b->btlbw_hist[i]  > maxv) maxv = b->btlbw_hist[i];
        if (b->rtprop_hist[i] < minv) minv = b->rtprop_hist[i];
    }
    b->btlbw_est  = maxv;
    b->rtprop_est = minv;
}
```

Esta función se llama **una vez por ronda**, dentro de `simular_round`. Guarda la medición más reciente en un arreglo circular (`idx` avanza y da la vuelta con `% WINDOW`) y recalcula `btlbw_est` como el máximo de las últimas 10 mediciones, y `rtprop_est` como el mínimo. Así se implementa, en pocas líneas, la regla de oro de BBR: *"el ancho de banda real es el máximo que alguna vez viste entregar; el RTT real es el mínimo que alguna vez viste tardar."*

---

## 10. `simular_round` — el corazón del programa

Esta función se ejecuta una vez por cada "ronda" (equivalente a un RTT). Sigue seis pasos internos, marcados con comentarios en el propio código:

### Paso 1 — Elegir las ganancias según la fase

```c
switch (b->phase) {
    case STARTUP:
        b->pacing_gain = 2.89;
        b->cwnd_gain   = 2.89;
        break;
    case DRAIN:
        b->pacing_gain = 1.0 / 2.89;
        b->cwnd_gain   = 2.89;
        break;
    case PROBE_BW:
        b->pacing_gain = b->probebw_cycle[b->probebw_cycle_index];
        b->cwnd_gain   = 2.0;
        break;
    case PROBE_RTT_PHASE:
        b->pacing_gain = 1.0;
        b->cwnd_gain   = 1.0;
        break;
}
```

Cada fase tiene su propia "personalidad" numérica, tal como se explicó en el documento conceptual anterior.

### Paso 2 — Calcular la tasa de envío (`pacing_rate`)

```c
if (b->btlbw_est <= 0.0) {
    pacing_rate_kbps = real_bw_kbps * 0.5; /* aun no sabe nada: arranca a la mitad */
} else {
    pacing_rate_kbps = b->pacing_gain * b->btlbw_est;
}
```

Si todavía no hay ninguna estimación de ancho de banda (primera ronda del programa), se usa un valor conservador (mitad de la capacidad real, que el algoritmo no conoce pero el simulador sí, para poder "generar" tráfico). En cualquier otra ronda, se aplica la ganancia sobre la última estimación.

### Paso 3 — Calcular BDP y `cwnd`

```c
if (b->rtprop_est <= 0.0 || b->rtprop_est >= 1e8) {
    bdp_est_kb = pacing_rate_kbps * (r->real_rtt / 1000.0);
} else {
    bdp_est_kb = b->btlbw_est * (b->rtprop_est / 1000.0);
}
cwnd_kb = b->cwnd_gain * bdp_est_kb;
if (cwnd_kb < MIN_CWND_PACKETS * PACKET_SIZE_KB) {
    cwnd_kb = MIN_CWND_PACKETS * PACKET_SIZE_KB;
}
```

Aquí se aplica literalmente la fórmula `BDP = BtlBw × RTprop`. La condición `rtprop_est >= 1e8` detecta si todavía estamos en el valor "infinito" inicial (ver sección 8) y, en ese caso, usa el RTT real como aproximación de arranque.

### Paso 4 — Decidir cuánto se envía realmente

```c
sent_kb = pacing_rate_kbps * rtt_for_send_sec;
if (sent_kb > cwnd_kb) sent_kb = cwnd_kb;
b->inflight = sent_kb;
```

Se calcula cuánto se enviaría a la tasa `pacing_rate` durante un RTT, pero nunca se permite superar la ventana `cwnd` — son dos límites independientes que actúan como "el que sea más chico gana", igual que en el BBR real.

### Paso 5 — Simular qué le pasa a ese envío en el cuello de botella

```c
capacity_kb = real_bw_kbps * (r->real_rtt * jitter / 1000.0);

if (sent_kb <= capacity_kb) {
    queue_delay_ms = 0.0;
    delivered_kb   = sent_kb;
} else {
    excess = sent_kb - capacity_kb;
    if (excess <= r->buffer_size) {
        queue_delay_ms = (excess / real_bw_kbps) * 1000.0;
        delivered_kb   = sent_kb;
    } else {
        dropped_kb     = excess - r->buffer_size;
        delivered_kb   = sent_kb - dropped_kb;
        queue_delay_ms = (r->buffer_size / real_bw_kbps) * 1000.0;
    }
}
observed_rtt_ms = r->real_rtt * jitter + queue_delay_ms;
```

Este es el bloque que simula la física del cuello de botella:

- Si lo enviado cabe en la capacidad del enlace → sin retardo, todo llega.
- Si sobra pero cabe en el buffer → se forma cola, el retardo (`queue_delay_ms`) crece, pero todo llega igual.
- Si ni siquiera cabe en el buffer → parte se descarta (`dropped_kb`), y el retardo queda "topado" al tamaño máximo del buffer.

### Paso 6 — Decidir si toca cambiar de fase

```c
case STARTUP:
    if (b->startup_prev_btlbw > 0.0) {
        double crecimiento = (b->btlbw_est - b->startup_prev_btlbw) / b->startup_prev_btlbw;
        if (crecimiento < 0.25) {
            b->startup_plateau_count++;
        } else {
            b->startup_plateau_count = 0;
        }
    }
    if (b->startup_plateau_count >= 3) {
        b->phase = DRAIN;
    }
    break;
```

Cada fase tiene su propio bloque `case` con su condición de salida particular (ya descritas en el documento conceptual): STARTUP mide el crecimiento porcentual, DRAIN compara `inflight` contra el BDP, PROBE_BW cuenta rondas para disparar PROBE_RTT, y PROBE_RTT cuenta su propia duración fija.

### Ejemplo real de tres rondas de `simular_round` en acción

Con una red de 15 Mbps / 30 ms / 50 KB de buffer, ejecutando la opción 5 (3 rondas automáticas):

```bash
============================================================
Ronda: 1      Fase actual: STARTUP  (arranque exponencial)
------------------------------------------------------------
BW real          [########################--------------------------]      15.00 Mbps
BtlBw est.       [############--------------------------------------]       7.45 Mbps
Pacing rate      [####----------------------------------------------]       7.50 Mbps
RTT real         [########------------------------------------------]      30.00 ms
RTprop est.      [########------------------------------------------]      30.21 ms
RTT observado    [########------------------------------------------]      30.21 ms
Delay de cola    [--------------------------------------------------]       0.00 ms
BDP estimado     [########------------------------------------------]      28.12 KB
Cwnd             [#######-------------------------------------------]      81.28 KB
Inflight         [##------------------------------------------------]      28.12 KB
------------------------------------------------------------
Ganancias  -> pacing_gain: 2.89   cwnd_gain: 2.89
============================================================

============================================================
Ronda: 2      Fase actual: STARTUP  (arranque exponencial)
------------------------------------------------------------
BW real          [########################--------------------------]      15.00 Mbps
BtlBw est.       [########################--------------------------]      15.00 Mbps
Pacing rate      [###########---------------------------------------]      21.53 Mbps
RTT observado    [############--------------------------------------]      43.05 ms
Delay de cola    [####----------------------------------------------]      13.96 ms
Inflight         [#######-------------------------------------------]      80.72 KB
------------------------------------------------------------
Ganancias  -> pacing_gain: 2.89   cwnd_gain: 2.89
============================================================

============================================================
Ronda: 3      Fase actual: STARTUP  (arranque exponencial)
------------------------------------------------------------
BW real          [########################--------------------------]      15.00 Mbps
BtlBw est.       [########################--------------------------]      15.00 Mbps
Pacing rate      [#######################---------------------------]      43.35 Mbps
RTT observado    [################----------------------------------]      55.68 ms
Delay de cola    [#######-------------------------------------------]      26.67 ms
Inflight         [###############-----------------------------------]     162.56 KB
------------------------------------------------------------
Ganancias  -> pacing_gain: 2.89   cwnd_gain: 2.89
>>> PERDIDA DE PAQUETES: 58.17 KB descartados (buffer saturado)
============================================================
```

Se ve exactamente el comportamiento esperado: en la ronda 1 `BtlBw est.` empieza en 7.45 Mbps (la mitad conservadora inicial); en la ronda 2 ya alcanzó los 15 Mbps reales; y en la ronda 3, al seguir aplicando la ganancia de 2.89 sobre una estimación que ya no crece, el envío se pasa de la capacidad del buffer (50 KB) y el programa reporta la pérdida.

---

## 11. Visualización: `draw_bar` y `mostrar_estado`

```c
void draw_bar(const char *label, double value, double max_value, const char *unit)
{
    int filled = (int) (ratio * BAR_WIDTH);
    for (i = 0; i < BAR_WIDTH; i++) {
        bar[i] = (i < filled) ? '#' : '-';
    }
    printf("%-16s [%s] %10.2f %s\n", label, bar, value, unit);
}
```

`draw_bar` no sabe nada de BBR: solo recibe un valor, un máximo de referencia, y dibuja una barra de `#` proporcional (con `-` rellenando el resto). `mostrar_estado` es quien decide, para cada métrica, cuál es ese "máximo de referencia" (por ejemplo, el doble del ancho de banda real) y llama a `draw_bar` una vez por cada fila que ves en pantalla.

---

## 12. Ciclos de ejecución: paso a paso vs. automático

```c
void ejecutar_paso_a_paso(RedParams *r, BBRState *b)
{
    while (continuar) {
        simular_round(r, b);
        mostrar_estado(r, b);
        printf("ENTER = continuar | q = salir: ");
        if (fgets(linea, sizeof(linea), stdin) == NULL) break;
        if (linea[0] == 'q' || linea[0] == 'Q') continuar = 0;
    }
}
```

Ambas funciones (`ejecutar_paso_a_paso` y `ejecutar_automatica`) hacen exactamente lo mismo en el fondo — llamar a `simular_round` y luego a `mostrar_estado` — la única diferencia es el control del bucle: una espera tu ENTER en cada vuelta, la otra corre un número fijo de veces (`leer_entero`) sin pausas.

---

## 13. `main` — el menú que conecta todo

```c
switch (opcion) {
    case 1:
        configurar_red(&red, "MOVER VALORES INICIALES DE LA RED");
        init_bbr_state(&bbr, &red);
        break;
    case 2:
        configurar_red(&red, "CAMBIAR VALORES DE LA RED");
        break;
    case 3:
        mostrar_valores_red(&red);
        mostrar_estado(&red, &bbr);
        break;
    case 4:
        ejecutar_paso_a_paso(&red, &bbr);
        break;
    case 5:
        ejecutar_automatica(&red, &bbr);
        break;
    case 6:
        init_bbr_state(&bbr, &red);
        break;
}
```

`main` solo declara las dos estructuras (`RedParams red` y `BBRState bbr`), las inicializa una vez, y luego entra en un bucle `while` que lee la opción del menú y llama a la función correspondiente. Todo el trabajo real vive en las funciones ya explicadas — `main` es únicamente el "controlador de tráfico" entre el usuario y ellas.

### Tabla resumen de opciones del menú

| Opción | Función que llama | Efecto sobre `RedParams` | Efecto sobre `BBRState` |
|---|---|---|---|
| 1 | `configurar_red` + `init_bbr_state` | Se modifica | Se reinicia por completo |
| 2 | `configurar_red` | Se modifica | No se toca (sigue con su fase actual) |
| 3 | `mostrar_valores_red` + `mostrar_estado` | Solo lectura | Solo lectura |
| 4 | `ejecutar_paso_a_paso` | No se toca | Avanza una ronda por ENTER |
| 5 | `ejecutar_automatica` | No se toca | Avanza N rondas seguidas |
| 6 | `init_bbr_state` | No se toca | Se reinicia por completo |
| 0 | — | — | Termina el programa |