/* ============================================================
   SIMULADOR DE FASES BBR (Bottleneck Bandwidth and RTT)
   ------------------------------------------------------------
   Simula de forma simplificada pero fiel a la logica real las
   fases del algoritmo de control de congestion BBR de Google:
       STARTUP, DRAIN, PROBE_BW, PROBE_RTT

   Permite:
     - MOVER (configurar) los valores iniciales de la red.
     - CAMBIAR los valores de la red durante la simulacion.
     - Ver graficamente (barras ASCII) el estado de:
         BtlBw (ancho de banda del cuello de botella estimado)
         RTT (real y observado)
         BDP (Bandwidth-Delay Product)
         Delay (retardo de cola)
         Fase actual de BBR y sus ganancias

   Escrito en ANSI C (C89) - sin dependencias externas.
   ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define WINDOW               10     /* ventana para btlbw max y rtprop min   */
#define BAR_WIDTH            50     /* ancho de las barras graficas          */
#define PROBE_RTT_INTERVAL   10     /* rondas entre sondeos de PROBE_RTT     */
#define PROBE_RTT_DURATION   2      /* duracion en rondas de PROBE_RTT       */
#define MIN_CWND_PACKETS     4.0    /* cwnd minima (en paquetes)             */
#define PACKET_SIZE_KB       1.5    /* tamano de paquete simulado (KB)       */

typedef enum { STARTUP, DRAIN, PROBE_BW, PROBE_RTT_PHASE } Phase;

/* ---------------- Parametros de la red (configurables) ---------------- */
typedef struct {
    double real_bandwidth; /* Mbps  - ancho de banda del cuello de botella */
    double real_rtt;       /* ms    - RTT base de propagacion              */
    double buffer_size;    /* KB    - tamano del buffer del cuello botella */
} RedParams;

/* ---------------------- Estado interno de BBR -------------------------- */
typedef struct {
    double btlbw_hist[WINDOW];
    double rtprop_hist[WINDOW];
    int    hist_count;
    int    hist_index;

    double btlbw_est;      /* KBps - estimacion de ancho de banda (ventana) */
    double rtprop_est;     /* ms   - estimacion de RTT minimo (ventana)     */

    double pacing_gain;
    double cwnd_gain;
    double pacing_rate;    /* KBps */
    double cwnd;           /* KB   */
    double inflight;       /* KB   */

    Phase  phase;
    int    round;
    int    rounds_in_phase;
    int    rounds_since_probertt;

    double startup_prev_btlbw;
    int    startup_plateau_count;

    double probebw_cycle[8];
    int    probebw_cycle_index;

    /* datos de la ultima ronda, para graficar */
    double ultimo_queue_delay;
    double ultimo_dropped;
    double ultimo_observed_rtt;
    double ultimo_bdp;
    double ultimo_capacity;
} BBRState;

/* ------------------------- Prototipos ---------------------------------- */
double leer_double(double actual);
int    leer_entero(int actual);
void   configurar_red(RedParams *r, const char *titulo);
void   mostrar_valores_red(RedParams *r);
const char *phase_name(Phase p);
void   draw_bar(const char *label, double value, double max_value, const char *unit);
void   actualizar_historial(BBRState *b, double delivery_rate, double observed_rtt);
void   init_red_params(RedParams *r);
void   init_bbr_state(BBRState *b, RedParams *r);
void   simular_round(RedParams *r, BBRState *b);
void   mostrar_estado(RedParams *r, BBRState *b);
void   ejecutar_paso_a_paso(RedParams *r, BBRState *b);
void   ejecutar_automatica(RedParams *r, BBRState *b);

/* ================= Utilidades de lectura de entrada ==================== */

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

int leer_entero(int actual)
{
    char linea[64];
    int valor;

    if (fgets(linea, sizeof(linea), stdin) == NULL) {
        return actual;
    }
    if (sscanf(linea, "%d", &valor) == 1) {
        return valor;
    }
    return actual;
}

/* ===================== Configuracion de la red ========================= */

void init_red_params(RedParams *r)
{
    r->real_bandwidth = 10.0;  /* Mbps */
    r->real_rtt       = 50.0;  /* ms   */
    r->buffer_size    = 100.0; /* KB   */
}

void configurar_red(RedParams *r, const char *titulo)
{
    printf("\n--- %s ---\n", titulo);

    printf("Ancho de banda del cuello de botella en Mbps [actual %.2f]: ", r->real_bandwidth);
    r->real_bandwidth = leer_double(r->real_bandwidth);
    if (r->real_bandwidth <= 0.0) r->real_bandwidth = 0.1;

    printf("RTT base de propagacion en ms [actual %.2f]: ", r->real_rtt);
    r->real_rtt = leer_double(r->real_rtt);
    if (r->real_rtt <= 0.0) r->real_rtt = 1.0;

    printf("Tamano del buffer del cuello de botella en KB [actual %.2f]: ", r->buffer_size);
    r->buffer_size = leer_double(r->buffer_size);
    if (r->buffer_size < 0.0) r->buffer_size = 0.0;

    printf("Valores de red actualizados correctamente.\n");
}

void mostrar_valores_red(RedParams *r)
{
    printf("\n--- VALORES ACTUALES DE LA RED ---\n");
    printf("Ancho de banda del cuello de botella : %.2f Mbps\n", r->real_bandwidth);
    printf("RTT base de propagacion              : %.2f ms\n", r->real_rtt);
    printf("Tamano del buffer                    : %.2f KB\n", r->buffer_size);
}

/* ========================= Estado de BBR ================================ */

void init_bbr_state(BBRState *b, RedParams *r)
{
    int i;
    static const double ciclo[8] = { 1.25, 0.75, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 };

    for (i = 0; i < WINDOW; i++) {
        b->btlbw_hist[i]  = 0.0;
        b->rtprop_hist[i] = 0.0;
    }
    b->hist_count  = 0;
    b->hist_index  = 0;

    b->btlbw_est   = 0.0;
    b->rtprop_est  = 1e9;

    b->pacing_gain = 2.89;
    b->cwnd_gain   = 2.89;
    b->pacing_rate = 0.0;
    b->cwnd        = 0.0;
    b->inflight    = 0.0;

    b->phase                 = STARTUP;
    b->round                 = 0;
    b->rounds_in_phase       = 0;
    b->rounds_since_probertt = 0;

    b->startup_prev_btlbw     = 0.0;
    b->startup_plateau_count  = 0;

    for (i = 0; i < 8; i++) b->probebw_cycle[i] = ciclo[i];
    b->probebw_cycle_index = 0;

    b->ultimo_queue_delay   = 0.0;
    b->ultimo_dropped       = 0.0;
    b->ultimo_observed_rtt  = r->real_rtt;
    b->ultimo_bdp           = 0.0;
    b->ultimo_capacity      = 0.0;
}

const char *phase_name(Phase p)
{
    switch (p) {
        case STARTUP:        return "STARTUP  (arranque exponencial)";
        case DRAIN:           return "DRAIN    (vaciado de la cola)";
        case PROBE_BW:        return "PROBE_BW (exploracion de ancho de banda)";
        case PROBE_RTT_PHASE: return "PROBE_RTT(medicion de RTT minimo)";
        default:              return "DESCONOCIDA";
    }
}

void actualizar_historial(BBRState *b, double delivery_rate, double observed_rtt)
{
    int idx = b->hist_index;
    int i;
    double maxv, minv;

    b->btlbw_hist[idx]  = delivery_rate;
    b->rtprop_hist[idx] = observed_rtt;

    b->hist_index = (idx + 1) % WINDOW;
    if (b->hist_count < WINDOW) b->hist_count++;

    maxv = b->btlbw_hist[0];
    minv = b->rtprop_hist[0];
    for (i = 1; i < b->hist_count; i++) {
        if (b->btlbw_hist[i]  > maxv) maxv = b->btlbw_hist[i];
        if (b->rtprop_hist[i] < minv) minv = b->rtprop_hist[i];
    }
    b->btlbw_est  = maxv;
    b->rtprop_est = minv;
}

/* Simula una ronda (aprox. un RTT) del algoritmo BBR sobre la red dada */
void simular_round(RedParams *r, BBRState *b)
{
    double real_bw_kbps;
    double bdp_est_kb;
    double capacity_kb;
    double pacing_rate_kbps;
    double cwnd_kb;
    double rtt_for_send_sec;
    double sent_kb;
    double delivered_kb;
    double dropped_kb = 0.0;
    double excess;
    double queue_delay_ms;
    double observed_rtt_ms;
    double delivery_rate_kbps;
    double jitter;

    b->round++;
    b->rounds_in_phase++;

    /* --- 1. Seleccion de ganancias segun la fase actual --- */
    switch (b->phase) {
        case STARTUP:
            b->pacing_gain = 2.89; /* 2/ln(2) aprox., igual que BBR real */
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

    real_bw_kbps = r->real_bandwidth * 1000.0 / 8.0; /* Mbps -> KBps */

    /* --- 2. Calculo de pacing rate --- */
    if (b->btlbw_est <= 0.0) {
        pacing_rate_kbps = real_bw_kbps * 0.5; /* estimacion inicial conservadora */
    } else {
        pacing_rate_kbps = b->pacing_gain * b->btlbw_est;
    }
    b->pacing_rate = pacing_rate_kbps;

    /* --- 3. Calculo de BDP estimado y cwnd --- */
    if (b->rtprop_est <= 0.0 || b->rtprop_est >= 1e8) {
        bdp_est_kb = pacing_rate_kbps * (r->real_rtt / 1000.0);
    } else {
        bdp_est_kb = b->btlbw_est * (b->rtprop_est / 1000.0);
    }

    cwnd_kb = b->cwnd_gain * bdp_est_kb;
    if (b->phase == PROBE_RTT_PHASE) {
        cwnd_kb = MIN_CWND_PACKETS * PACKET_SIZE_KB; /* cwnd minima forzada */
    }
    if (cwnd_kb < MIN_CWND_PACKETS * PACKET_SIZE_KB) {
        cwnd_kb = MIN_CWND_PACKETS * PACKET_SIZE_KB;
    }
    b->cwnd = cwnd_kb;

    /* --- 4. Cuanto se envia esta ronda --- */
    rtt_for_send_sec = r->real_rtt / 1000.0;
    sent_kb = pacing_rate_kbps * rtt_for_send_sec;
    if (sent_kb > cwnd_kb) sent_kb = cwnd_kb;
    b->inflight = sent_kb;

    /* --- 5. Capacidad real del enlace (con leve variacion aleatoria) --- */
    jitter = 0.95 + 0.1 * ((double) rand() / (double) RAND_MAX);
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

    observed_rtt_ms    = r->real_rtt * jitter + queue_delay_ms;
    delivery_rate_kbps = delivered_kb / (observed_rtt_ms / 1000.0);

    actualizar_historial(b, delivery_rate_kbps, observed_rtt_ms);

    b->ultimo_queue_delay  = queue_delay_ms;
    b->ultimo_dropped      = dropped_kb;
    b->ultimo_observed_rtt = observed_rtt_ms;
    b->ultimo_bdp          = bdp_est_kb;
    b->ultimo_capacity     = capacity_kb;

    /* --- 6. Transiciones de fase --- */
    switch (b->phase) {
        case STARTUP:
            if (b->startup_prev_btlbw > 0.0) {
                double crecimiento = (b->btlbw_est - b->startup_prev_btlbw) / b->startup_prev_btlbw;
                if (crecimiento < 0.25) {
                    b->startup_plateau_count++;
                } else {
                    b->startup_plateau_count = 0;
                }
            }
            b->startup_prev_btlbw = b->btlbw_est;
            if (b->startup_plateau_count >= 3) {
                b->phase = DRAIN;
                b->rounds_in_phase = 0;
                b->startup_plateau_count = 0;
            }
            break;

        case DRAIN:
            if (b->inflight <= bdp_est_kb || b->rounds_in_phase >= 5) {
                b->phase = PROBE_BW;
                b->rounds_in_phase = 0;
                b->probebw_cycle_index = 0;
            }
            break;

        case PROBE_BW:
            b->probebw_cycle_index = (b->probebw_cycle_index + 1) % 8;
            b->rounds_since_probertt++;
            if (b->rounds_since_probertt >= PROBE_RTT_INTERVAL) {
                b->phase = PROBE_RTT_PHASE;
                b->rounds_in_phase = 0;
            }
            break;

        case PROBE_RTT_PHASE:
            if (b->rounds_in_phase >= PROBE_RTT_DURATION) {
                b->phase = PROBE_BW;
                b->rounds_in_phase = 0;
                b->rounds_since_probertt = 0;
                b->probebw_cycle_index = 0;
            }
            break;
    }
}

/* ========================= Visualizacion grafica ========================= */

void draw_bar(const char *label, double value, double max_value, const char *unit)
{
    int i, filled;
    double ratio;
    char bar[BAR_WIDTH + 1];

    if (max_value <= 0.0) max_value = 1.0;
    ratio = value / max_value;
    if (ratio > 1.0) ratio = 1.0;
    if (ratio < 0.0) ratio = 0.0;
    filled = (int) (ratio * BAR_WIDTH);

    for (i = 0; i < BAR_WIDTH; i++) {
        bar[i] = (i < filled) ? '#' : '-';
    }
    bar[BAR_WIDTH] = '\0';

    printf("%-16s [%s] %10.2f %s\n", label, bar, value, unit);
}

void mostrar_estado(RedParams *r, BBRState *b)
{
    double btlbw_mbps  = b->btlbw_est * 8.0 / 1000.0;
    double pacing_mbps = b->pacing_rate * 8.0 / 1000.0;
    double rtprop_ms   = (b->rtprop_est >= 1e8) ? r->real_rtt : b->rtprop_est;
    double max_bw       = r->real_bandwidth * 2.0 + 1.0;
    double max_rtt       = r->real_rtt * 4.0 + 50.0;
    double max_bdp        = (r->real_bandwidth * 1000.0 / 8.0) * (r->real_rtt / 1000.0) * 3.0 + 1.0;

    printf("\n============================================================\n");
    printf("Ronda: %-5d  Fase actual: %s\n", b->round, phase_name(b->phase));
    printf("------------------------------------------------------------\n");
    draw_bar("BW real",       r->real_bandwidth, max_bw,  "Mbps");
    draw_bar("BtlBw est.",    btlbw_mbps,        max_bw,  "Mbps");
    draw_bar("Pacing rate",   pacing_mbps,       max_bw * 3.0, "Mbps");
    draw_bar("RTT real",      r->real_rtt,       max_rtt, "ms");
    draw_bar("RTprop est.",   rtprop_ms,         max_rtt, "ms");
    draw_bar("RTT observado", b->ultimo_observed_rtt, max_rtt, "ms");
    draw_bar("Delay de cola", b->ultimo_queue_delay,  max_rtt, "ms");
    draw_bar("BDP estimado",  b->ultimo_bdp,     max_bdp, "KB");
    draw_bar("Cwnd",          b->cwnd,           max_bdp * 3.0, "KB");
    draw_bar("Inflight",      b->inflight,       max_bdp * 3.0, "KB");
    printf("------------------------------------------------------------\n");
    printf("Ganancias  -> pacing_gain: %.2f   cwnd_gain: %.2f\n", b->pacing_gain, b->cwnd_gain);
    if (b->ultimo_dropped > 0.0) {
        printf(">>> PERDIDA DE PAQUETES: %.2f KB descartados (buffer saturado)\n", b->ultimo_dropped);
    }
    printf("============================================================\n");
}

/* ========================= Ciclos de ejecucion ============================ */

void ejecutar_paso_a_paso(RedParams *r, BBRState *b)
{
    char linea[16];
    int continuar = 1;

    printf("\nSimulacion PASO A PASO.\n");
    printf("Presione ENTER para avanzar una ronda, o escriba 'q' + ENTER para salir.\n");

    while (continuar) {
        simular_round(r, b);
        mostrar_estado(r, b);
        printf("ENTER = continuar | q = salir: ");
        if (fgets(linea, sizeof(linea), stdin) == NULL) break;
        if (linea[0] == 'q' || linea[0] == 'Q') continuar = 0;
    }
}

void ejecutar_automatica(RedParams *r, BBRState *b)
{
    int n, i;

    printf("\nCuantas rondas desea simular automaticamente: ");
    n = leer_entero(10);
    if (n <= 0) n = 1;

    for (i = 0; i < n; i++) {
        simular_round(r, b);
        mostrar_estado(r, b);
    }
}

/* =============================== main ===================================== */

int main(void)
{
    RedParams red;
    BBRState  bbr;
    int  opcion = -1;
    char linea[16];

    srand((unsigned int) time(NULL));
    init_red_params(&red);
    init_bbr_state(&bbr, &red);

    printf("==================================================\n");
    printf(" SIMULADOR DE FASES BBR (Bottleneck Bandwidth and RTT)\n");
    printf(" Protocolo de control de congestion de Google para TCP\n");
    printf("==================================================\n");

    while (opcion != 0) {
        printf("\n--------------------- MENU PRINCIPAL ---------------------\n");
        printf("1. MOVER (configurar) valores iniciales de la red\n");
        printf("2. CAMBIAR valores de la red (simular cambio de enlace)\n");
        printf("3. Ver valores actuales de la red y de BBR\n");
        printf("4. Ejecutar simulacion PASO A PASO\n");
        printf("5. Ejecutar simulacion AUTOMATICA (N rondas)\n");
        printf("6. Reiniciar estado de BBR (conservando la red)\n");
        printf("0. Salir\n");
        printf("Seleccione una opcion: ");

        if (fgets(linea, sizeof(linea), stdin) == NULL) break;
        if (sscanf(linea, "%d", &opcion) != 1) opcion = -1;

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
                printf("Estado de BBR reiniciado.\n");
                break;
            case 0:
                printf("Saliendo del simulador...\n");
                break;
            default:
                printf("Opcion invalida, intente de nuevo.\n");
        }
    }

    return 0;
}