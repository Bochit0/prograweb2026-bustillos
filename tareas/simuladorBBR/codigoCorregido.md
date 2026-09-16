# Cambios hechos a `simBBR.c`

---

## 1. La cola ahora se acumula entre rondas

**Qué había antes:** cada ronda calculaba la cola desde cero, mirando solo lo que se envió en esa ronda. Si en la ronda 3 quedaban 50 KB atorados, en la ronda 4 el programa los "olvidaba" y empezaba de nuevo.

**Qué se hizo:** se agregó un campo nuevo al estado de BBR:
, sin ningún efecto en el comportamiento.
```
double queue_kb;   /* KB acumulados en el buffer del cuello de botella */
```

Ahora cada ronda suma lo que quedó encolado antes con lo que llega nuevo, drena solo lo que el enlace aguanta, y lo que sobra se guarda para la siguiente ronda:

```
total_pendiente_kb = b->queue_kb + sent_kb;

if (total_pendiente_kb <= drain_capacity_kb) {
    delivered_kb = total_pendiente_kb;
    b->queue_kb  = 0.0;
} else {
    delivered_kb = drain_capacity_kb;
    sobrante_kb  = total_pendiente_kb - drain_capacity_kb;
    /* si no cabe en el buffer, se descarta lo que sobra */
}
```

 una cola real no se borra sola cada RTT. Con el modelo viejo, la fase DRAIN duraba siempre 1 ronda porque la cola aparecía mágicamente vacía apenas bajaba el envío. Eso hacía que DRAIN no sirviera para nada.

**Cómo se nota ahora** (red de 10 Mbps, RTT 50 ms, buffer 100 KB):

```
Ronda 1  STARTUP   cola   0.00 KB
Ronda 2  STARTUP   cola  28.52 KB
Ronda 3  STARTUP   cola  86.60 KB
Ronda 4  STARTUP   cola 100.00 KB   >>> PERDIDA 47.14 KB
Ronda 5  DRAIN     cola 100.00 KB   >>> PERDIDA 55.74 KB
Ronda 6  DRAIN     cola  55.36 KB
Ronda 7  DRAIN     cola  10.09 KB
Ronda 8  PROBE_BW  cola   0.00 KB
```

La cola sube poco a poco, topa en los 100 KB del buffer (ahí empieza a descartar), y DRAIN tarda 3 rondas en vaciarla. Eso ya se parece a la realidad.

---

## 2. DRAIN ahora sale cuando la cola se vacía de verdad

**Antes:**

```
if (b->inflight <= bdp_est_kb || b->rounds_in_phase >= 5)
```

Esa condición se cumplía siempre en la primera ronda, porque con `pacing_gain` de 0.35 el `inflight` queda chiquito automáticamente. O sea, salía de DRAIN por la razón equivocada.

**Ahora:**

```
if (b->queue_kb <= QUEUE_EMPTY_KB)
```

Sale cuando la cola realmente llegó a cero (bueno, a menos de 0.5 KB, para no pelear con decimales). Es el cambio que le da sentido a la fase.

---

## 3. Se arregló el `probebw_cycle`

Eran dos cosas:

**a) Estaba copiado dentro de cada estado.** El arreglo `{1.25, 0.75, 1, 1, 1, 1, 1, 1}` nunca cambia, pero vivía dentro de `BBRState` y se copiaba entero cada vez que se reiniciaba BBR. Ahora es una constante global:

```
static const double PROBEBW_CYCLE[CYCLE_LEN] = {
    1.25, 0.75, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0
};
```

**b) Reiniciaba en el pulso agresivo.** Al salir de PROBE_RTT (donde el envío está al mínimo), el índice volvía a 0 — o sea, entraba directo al 1.25. Justo después de haber vaciado la ventana, eso es contraproducente. Ahora arranca en el índice 2, que es una ganancia neutra de 1.0:

```
#define CYCLE_RESTART_INDEX  2
```

Lo mismo al entrar a PROBE_BW desde DRAIN.

---

## 4. Se eliminó `ultimo_capacity`

Era una variable muerta: se declaraba, se inicializaba, se le asignaba valor cada ronda... y nunca se leía en ningún lado `mostrar_estado` ni la miraba. Se borró completa.

---

## 5. Se quitó `mostrar_valores_red` de la opción 3

La opción 3 llamaba a dos funciones `mostrar_valores_red` + `mostrar_estado` y la primera repetía información que la segunda ya mostraba en barras (BW real, RTT real). Se eliminó la función entera.

Para no perder el dato del buffer (que sí era exclusivo de ahí), se metió una línea compacta dentro de `mostrar_estado`, así aparece en todas las rondas, no solo en la opción 3:

```
Red: 10.00 Mbps | RTT base 50.00 ms | Buffer 100.00 KB
```

---
