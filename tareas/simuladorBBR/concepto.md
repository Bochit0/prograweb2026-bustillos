# El algoritmo BBR: conceptos, fases y ejemplo numérico

## 1. Qué problema resuelve BBR

Un algoritmo de control de congestión decide, en cada instante, **cuántos datos puede tener un emisor "en vuelo"** (enviados pero aún no confirmados) sin saturar la red.

Los algoritmos clásicos (Reno, CUBIC) funcionan por **pérdida**: van aumentando el envío hasta que se pierde un paquete, interpretan la pérdida como señal de congestión y retroceden. El problema es que, para que se pierda un paquete, primero tuvo que llenarse por completo el buffer de algún router del camino — es decir, actúan **después** de que ya se formó una cola innecesaria (lo que se conoce como *bufferbloat*).

BBR (Bottleneck Bandwidth and RTT) cambia el enfoque: en lugar de esperar la pérdida, **mide directamente** dos propiedades físicas de la ruta de red y calcula matemáticamente el punto óptimo de envío, sin necesidad de llenar ninguna cola.

Esas dos propiedades — y dos valores derivados de ellas — son el núcleo de todo el algoritmo.

---

## 2. Los cuatro conceptos clave

### 2.1 Bottleneck Bandwidth (BtlBw)

Es la capacidad del **tramo más lento** de toda la ruta entre emisor y receptor. No importa qué tan rápidas sean las demás partes del camino: la velocidad máxima sostenible de la conexión siempre queda limitada por el enlace más angosto.

BBR no puede preguntarle a la red cuál es este valor; lo **estima observando la tasa de entrega real de los datos** (cuántos bytes llegan confirmados por unidad de tiempo) y se queda con el valor máximo observado dentro de una ventana de tiempo reciente (en la práctica, los últimos RTT). Usar el máximo (y no un promedio) es intencional: en cuanto la red demuestra que puede entregar a cierta velocidad, BBR asume que esa capacidad sigue disponible, incluso si una ronda puntual entrega menos por congestión temporal.

### 2.2 RTprop (RTT de propagación)

Es el tiempo mínimo que tarda un paquete en ir y volver **sin ninguna espera en colas** — solo la latencia física del medio (distancia, velocidad de la luz en la fibra, procesamiento de los routers). El RTT que realmente se mide en cada ronda (`RTT observado`) es casi siempre mayor a este valor, porque incluye el tiempo que el paquete pasó esperando en algún buffer.

BBR estima RTprop tomando el **valor mínimo** de RTT observado dentro de una ventana de tiempo reciente. Usa el mínimo (y no un promedio) porque cualquier RTT medido incluye, en el peor caso, retardo de cola — pero nunca puede ser *menor* al RTT físico real. El mínimo observado es, entonces, la mejor aproximación posible a la latencia pura.

### 2.3 BDP (Bandwidth-Delay Product)

Es el valor derivado más importante de todos:

```
BDP = BtlBw × RTprop
```

Representa **cuántos datos caben "en tránsito" dentro de la ruta en un instante dado** — es decir, el tamaño de la "tubería" de red. Pensándolo físicamente: si el enlace más lento entrega a `BtlBw` bytes por segundo y un dato tarda `RTprop` segundos en completar el viaje de ida y vuelta, entonces `BDP` bytes es exactamente lo que se necesita tener enviado y sin confirmar para mantener el enlace **siempre ocupado, sin que sobre ni falte nada**.

- Enviar **menos** que el BDP desperdicia capacidad disponible (el enlace queda ocioso parte del tiempo).
- Enviar **más** que el BDP no aumenta el rendimiento (el enlace ya está saturado) y solo agrega paquetes a una cola — es decir, produce **delay** sin ningún beneficio.

Por eso BDP es el "objetivo" que BBR persigue constantemente para su ventana de congestión (`cwnd`).

### 2.4 Delay (retardo de cola)

Es el tiempo adicional que se suma al RTprop cuando se envían más datos de los que el cuello de botella puede drenar de inmediato. Ese excedente se acumula en el buffer del router del cuello de botella, y cada byte adicional en esa cola le agrega tiempo de espera a todos los paquetes detrás de él.

```
RTT observado = RTprop + Delay de cola
```

Si el excedente enviado supera la capacidad del buffer, ya no se trata solo de delay: los paquetes que no caben se **descartan** (pérdida). BBR tolera generar algo de delay de forma controlada y temporal (sobre todo en STARTUP y al inicio de PROBE_BW), pero el objetivo de largo plazo es mantenerlo en cero.

---

## 3. Las cuatro fases de BBR

BBR no conoce `BtlBw` ni `RTprop` de antemano: los va descubriendo y refinando con el tiempo, transitando por cuatro fases con comportamientos claramente distintos.

### 3.1 STARTUP — arranque exponencial

**Objetivo:** encontrar `BtlBw` lo más rápido posible.

- Ganancia de envío (`pacing_gain`) y de ventana (`cwnd_gain`) fijas en **2.89** (≈ 2/ln 2).
- En cada ronda, la tasa de envío se calcula como `pacing_gain × BtlBw_estimado`. Como la estimación crece con cada ronda, el envío crece de forma aproximadamente exponencial (se duplica cada RTT).
- **Condición de salida:** cuando la estimación de `BtlBw` deja de crecer al menos un 25% durante 3 rondas seguidas, BBR concluye que ya encontró el límite real del enlace — casi siempre a costa de generar una cola y alguna pérdida de paquetes al final de esta fase, porque el envío exponencial inevitablemente "se pasa" del límite real antes de poder detectarlo.

### 3.2 DRAIN — vaciado de la cola

**Objetivo:** eliminar la cola que STARTUP generó, sin perder la estimación de `BtlBw` ya conseguida.

- `pacing_gain` se invierte a **1 / 2.89 ≈ 0.346** (envía deliberadamente por debajo del `BtlBw` estimado).
- `cwnd_gain` se mantiene en 2.89 (la ventana no se reduce; solo se envía más despacio, dejando que la cola se drene).
- **Condición de salida:** cuando los datos en tránsito (`inflight`) caen por debajo del `BDP` estimado, la cola ya se vació y no tiene sentido seguir frenando.

### 3.3 PROBE_BW — exploración del ancho de banda

**Objetivo:** usar el enlace a su capacidad óptima de forma sostenida, mientras sondea periódicamente si hay más ancho de banda disponible (por ejemplo, si otro flujo que compartía el enlace terminó).

- `cwnd_gain` fijo en **2.0** (la ventana permite tener el doble del BDP en tránsito, dando margen sin ser agresivo).
- `pacing_gain` cicla por 8 valores, uno por ronda: **1.25, 0.75, 1, 1, 1, 1, 1, 1**.
  - El **1.25** es el sondeo: envía un 25% más de lo estimado para ver si la red puede entregarlo (si puede, `BtlBw_estimado` sube).
  - El **0.75** inmediatamente después compensa el posible exceso de cola generado por el sondeo anterior.
  - Los seis **1.0** restantes mantienen el envío exactamente en el punto óptimo (ni cola ni desperdicio).
- Esta es la fase en la que BBR pasa la mayor parte del tiempo durante una conexión estable y de larga duración.

### 3.4 PROBE_RTT — remedición del RTT mínimo

**Objetivo:** evitar que `RTprop_estimado` quede "envejecido" y demasiado alto. Como BBR casi siempre mantiene algo de datos en tránsito, es posible que durante mucho tiempo nunca se observe un RTT verdaderamente libre de cola, y la estimación del mínimo iría perdiendo precisión.

- Cada cierto intervalo (en el BBR real, cada 10 segundos sin haber visto un RTT mínimo nuevo), BBR reduce la ventana a un valor mínimo fijo (típicamente el equivalente a 4 paquetes) durante un par de rondas.
- Con tan poco enviado, es prácticamente imposible que se forme cola, así que el RTT observado en esas rondas es una medición limpia del RTT de propagación real.
- Al terminar, BBR vuelve a PROBE_BW con una estimación de `RTprop` actualizada.

---

## 4. Ejemplo numérico paso a paso

### Escenario

Se simula una conexión con estos valores de red:

| Parámetro | Valor |
|---|---|
| Ancho de banda del cuello de botella (`BtlBw` real) | 10 Mbps |
| RTT base de propagación (`RTprop` real) | 50 ms |
| Tamaño del buffer del cuello de botella | 100 KB |

Convertido a unidades de trabajo: 10 Mbps equivale a **1250 KB/s**. La "capacidad ideal por ronda" (cuánto puede drenar el enlace en un RTT) es:

```
1250 KB/s × 0.05 s = 62.5 KB
```

Ese 62.5 KB es, en este escenario, el valor exacto que el BDP real debería alcanzar una vez que BBR conozca bien la red (`BtlBw × RTprop` = 1250 KB/s × 0.05 s = 62.5 KB).

### Tabla ronda a ronda

| Ronda | Fase | Ganancia envío | BtlBw estimado | RTprop estimado | BDP estimado | Enviado | Delay de cola | RTT observado | Nota |
|---|---|---|---|---|---|---|---|---|---|
| 1 | STARTUP | 2.89 | 5.0 Mbps | 50 ms | 31.3 KB | 31.3 KB | 0 ms | 50 ms | Primer envío, conservador |
| 2 | STARTUP | 2.89 | 10.0 Mbps | 50 ms | 31.3 KB | 90.3 KB | 22.2 ms | 72.2 ms | Ya alcanzó el ancho real |
| 3 | STARTUP | 2.89 | 10.0 Mbps | 50 ms | 62.5 KB | 180.7 KB | 80 ms | 130 ms | Buffer lleno: se descartan 18.2 KB |
| 4 | STARTUP | 2.89 | 10.0 Mbps | 50 ms | 62.5 KB | 180.7 KB | 80 ms | 130 ms | Sin crecimiento (2do plateau) |
| 5 | STARTUP | 2.89 | 10.0 Mbps | 50 ms | 62.5 KB | 180.7 KB | 80 ms | 130 ms | 3er plateau → pasa a DRAIN |
| 6 | DRAIN | 0.346 | 10.0 Mbps | 50 ms | 62.5 KB | 21.6 KB | 0 ms | 50 ms | Cola vacía → pasa a PROBE_BW |
| 7 | PROBE_BW | 1.25 | 10.0 Mbps | 50 ms | 62.5 KB | 78.1 KB | 12.5 ms | 62.5 ms | Sondeo alcista |
| 8 | PROBE_BW | 0.75 | 10.0 Mbps | 50 ms | 62.5 KB | 46.9 KB | 0 ms | 50 ms | Compensa el sondeo anterior |

*(Valores redondeados a una cifra decimal para claridad.)*

### Análisis de resultados

- **Rondas 1-2:** con la red completamente desconocida, BBR arranca con una estimación conservadora y en solo dos rondas ya descubrió el ancho de banda real (10 Mbps), gracias al crecimiento exponencial de STARTUP.
- **Rondas 3-5:** una vez alcanzado el límite real, seguir aplicando la ganancia de 2.89 hace que el envío sobrepase la capacidad del enlace. El excedente llena el buffer (100 KB) y empieza a descartar paquetes — esto es esperado y forma parte del diseño de STARTUP, no un error. Al no crecer más la estimación de `BtlBw` durante 3 rondas, BBR detecta el plateau y cambia de fase.
- **Ronda 6:** DRAIN reduce el envío a menos de un tercio (ganancia 0.346) y la cola desaparece de inmediato, dejando el sistema listo para operar en régimen estable.
- **Rondas 7-8:** PROBE_BW retoma el control fino: el 1.25 sondea si hay más capacidad disponible (genera un poco de delay, 12.5 ms, al enviar por encima del punto óptimo), y el 0.75 inmediato siguiente compensa ese excedente, devolviendo el delay a cero. En las siguientes seis rondas del ciclo (no mostradas), la ganancia se mantiene en 1.0 y el sistema opera exactamente en el punto `BDP` (62.5 KB en tránsito), sin cola y sin desperdicio de capacidad.

Este patrón — crecimiento agresivo, corrección, y luego oscilación fina alrededor del punto óptimo — es la firma característica de BBR frente a otros algoritmos que solo reaccionan cuando ya hay pérdida sostenida.

---

## 5. Resumen de fórmulas clave

```
BDP              = BtlBw_estimado × RTprop_estimado
RTT observado    = RTprop_real + Delay de cola
pacing_rate      = pacing_gain × BtlBw_estimado
cwnd             = cwnd_gain × BDP_estimado
BtlBw_estimado   = máximo de la tasa de entrega en la ventana reciente
RTprop_estimado  = mínimo del RTT observado en la ventana reciente
```

| Fase | pacing_gain | cwnd_gain | Condición de salida |
|---|---|---|---|
| STARTUP | 2.89 | 2.89 | BtlBw estimado sin crecer ≥25% durante 3 rondas |
| DRAIN | 1/2.89 ≈ 0.346 | 2.89 | Datos en tránsito ≤ BDP estimado |
| PROBE_BW | cicla 1.25 / 0.75 / 1×6 | 2.0 | (permanente, salvo disparo periódico de PROBE_RTT) |
| PROBE_RTT | 1.0 (cwnd forzada al mínimo) | 1.0 | Transcurridas las rondas de la fase (recupera RTprop limpio) |