/*
 * ============================================================
 *  AQUASENSE v1.0 - versao ESP32 (Wokwi / placa real)
 *  Grupo 7 - Inovatech 2026
 * ============================================================
 *  Mesma esteira do aquasense.c, mas a leitura vem do pino analogico:
 *
 *    GPIO34 (sensor / potenciometro) -> struct Leitura -> FILA (FIFO)
 *      -> regra de negocio -> GPIO26 (rele da bomba) + PILHA (LIFO)
 *
 *  Monitor serial: 115200 baud. Comandos pelo serial:
 *    f = ver fila | p = ver pilha | d = desempilhar | e = estatisticas
 */

#include <stdlib.h>

/* ---------- Pinagem ---------- */
#define PINO_SENSOR        34   /* entrada analogica (ADC 12 bits: 0 a 4095) */
#define PINO_RELE          26   /* saida que aciona o rele da bomba          */
#define PINO_LED_STATUS    2    /* LED aceso enquanto a bomba esta ligada    */
#define BAUD_RATE          115200

/* ---------- Constantes do sistema ---------- */
#define LIMITE_SECO        30.0f
#define LIMITE_ENCHARCADO  60.0f
#define CAP_FILA           10
#define CAP_PILHA          5
#define ADC_SECO           3200
#define ADC_MOLHADO        1300
#define INTERVALO_LEITURA  1000  /* ms entre leituras do sensor        */
#define PROCESSAR_A_CADA   3     /* processa a fila a cada 3 leituras  */

/* ---------- Estruturas de dados (iguais ao aquasense.c) ---------- */

typedef enum { SOLO_SECO, SOLO_IDEAL, SOLO_ENCHARCADO } StatusSolo;
typedef enum { BOMBA_DESLIGOU, BOMBA_LIGOU } AcaoBomba;

typedef struct {
    int        id;
    int        adc_bruto;
    float      umidade;
    StatusSolo status;
} Leitura;

typedef struct No {
    Leitura    dado;
    struct No *proximo;
} No;

typedef struct {
    No *inicio;
    No *fim;
    int tamanho;
} Fila;

typedef struct {
    int       id_leitura;
    float     umidade;
    AcaoBomba acao;
} Evento;

typedef struct {
    Evento itens[CAP_PILHA];
    int    topo;
} Pilha;

typedef struct {
    int   total;
    float soma;
    float minimo, maximo;
} Estatisticas;

/* ---------- Estado global do firmware ---------- */
Fila          fila;
Pilha         historico;
Estatisticas  est = { 0, 0.0f, 0.0f, 0.0f };
int           bomba_ligada  = 0;
int           proximo_id    = 1;
int           total_malloc  = 0;
int           total_free    = 0;
unsigned long ultima_leitura = 0;

/* ============================================================
 *  FILA (FIFO)
 * ============================================================ */

void fila_iniciar(Fila *f) {
    f->inicio  = NULL;
    f->fim     = NULL;
    f->tamanho = 0;
}

int fila_vazia(const Fila *f) { return f->inicio == NULL; }
int fila_cheia(const Fila *f) { return f->tamanho >= CAP_FILA; }

/* O(1): insere no fim */
int enfileirar(Fila *f, Leitura dado) {
    if (fila_cheia(f)) {
        Serial.printf("[OVERFLOW BLOQUEADO] Fila cheia. Leitura #%d descartada.\n", dado.id);
        return 0;
    }
    No *novo = (No *) malloc(sizeof(No));
    if (novo == NULL) {
        Serial.println("[ERRO] Memoria insuficiente.");
        return 0;
    }
    total_malloc++;
    novo->dado    = dado;
    novo->proximo = NULL;
    if (fila_vazia(f)) {
        f->inicio = novo;
    } else {
        f->fim->proximo = novo;
    }
    f->fim = novo;
    f->tamanho++;
    return 1;
}

/* O(1): remove do inicio e libera o no */
int desenfileirar(Fila *f, Leitura *saida) {
    if (fila_vazia(f)) {
        Serial.println("[UNDERFLOW BLOQUEADO] Fila vazia.");
        return 0;
    }
    No *removido = f->inicio;
    *saida    = removido->dado;
    f->inicio = removido->proximo;
    if (f->inicio == NULL) {
        f->fim = NULL;
    }
    free(removido);
    total_free++;
    f->tamanho--;
    return 1;
}

/* ============================================================
 *  PILHA (LIFO)
 * ============================================================ */

void pilha_iniciar(Pilha *p) { p->topo = -1; }

int pilha_vazia(const Pilha *p) { return p->topo == -1; }
int pilha_cheia(const Pilha *p) { return p->topo == CAP_PILHA - 1; }

/* O(1): recusa se cheia (stack overflow) */
int empilhar(Pilha *p, Evento e) {
    if (pilha_cheia(p)) {
        Serial.println("[OVERFLOW BLOQUEADO] Historico cheio. Envie 'd' para desempilhar.");
        return 0;
    }
    p->topo++;
    p->itens[p->topo] = e;
    return 1;
}

/* O(1): recusa se vazia (stack underflow) */
int desempilhar(Pilha *p, Evento *saida) {
    if (pilha_vazia(p)) {
        Serial.println("[UNDERFLOW BLOQUEADO] Historico vazio.");
        return 0;
    }
    *saida = p->itens[p->topo];
    p->topo--;
    return 1;
}

/* ============================================================
 *  SENSOR E REGRA DE NEGOCIO
 * ============================================================ */

float adc_para_umidade(int adc) {
    float pct = (float) (ADC_SECO - adc) * 100.0f / (ADC_SECO - ADC_MOLHADO);
    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return pct;
}

StatusSolo classificar(float umidade) {
    if (umidade < LIMITE_SECO)       return SOLO_SECO;
    if (umidade > LIMITE_ENCHARCADO) return SOLO_ENCHARCADO;
    return SOLO_IDEAL;
}

const char *nome_status(StatusSolo s) {
    switch (s) {
        case SOLO_SECO:       return "SECO";
        case SOLO_IDEAL:      return "IDEAL";
        case SOLO_ENCHARCADO: return "ENCHARCADO";
    }
    return "?";
}

Leitura criar_leitura(int id, int adc) {
    Leitura l;
    l.id        = id;
    l.adc_bruto = adc;
    l.umidade   = adc_para_umidade(adc);
    l.status    = classificar(l.umidade);
    return l;
}

/* Liga ou desliga o atuador fisico */
void acionar_bomba(int ligar) {
    digitalWrite(PINO_RELE, ligar ? HIGH : LOW);
    digitalWrite(PINO_LED_STATUS, ligar ? HIGH : LOW);
}

/* O(n): esvazia a fila na ordem de chegada e aplica a regra com histerese */
void processar_fila(void) {
    Leitura l;
    while (!fila_vazia(&fila)) {
        desenfileirar(&fila, &l);

        if (est.total == 0 || l.umidade < est.minimo) est.minimo = l.umidade;
        if (est.total == 0 || l.umidade > est.maximo) est.maximo = l.umidade;
        est.soma += l.umidade;
        est.total++;

        if (!bomba_ligada && l.umidade < LIMITE_SECO) {
            bomba_ligada = 1;
            Evento e = { l.id, l.umidade, BOMBA_LIGOU };
            empilhar(&historico, e);
            Serial.printf(">> Leitura #%d abaixo de %.0f%%: BOMBA LIGOU\n", l.id, LIMITE_SECO);
        } else if (bomba_ligada && l.umidade > LIMITE_ENCHARCADO) {
            bomba_ligada = 0;
            Evento e = { l.id, l.umidade, BOMBA_DESLIGOU };
            empilhar(&historico, e);
            Serial.printf(">> Leitura #%d acima de %.0f%%: BOMBA DESLIGOU\n", l.id, LIMITE_ENCHARCADO);
        }
        acionar_bomba(bomba_ligada);

        Serial.printf("#%-4d ADC %-5d %6.1f%%  %-10s bomba %s\n", l.id, l.adc_bruto,
                      l.umidade, nome_status(l.status), bomba_ligada ? "LIGADA" : "desligada");
    }
}

/* ============================================================
 *  COMANDOS PELO MONITOR SERIAL
 * ============================================================ */

void exibir_fila(void) {
    Serial.printf("FILA (%d/%d) inicio -> ", fila.tamanho, CAP_FILA);
    for (No *atual = fila.inicio; atual != NULL; atual = atual->proximo) {
        Serial.printf("[#%d %.1f%%] ", atual->dado.id, atual->dado.umidade);
    }
    Serial.println("<- fim");
}

void exibir_pilha(void) {
    Serial.printf("PILHA (%d/%d)\n", historico.topo + 1, CAP_PILHA);
    for (int i = historico.topo; i >= 0; i--) {
        Serial.printf("%s leitura #%d | %.1f%% | bomba %s\n",
                      i == historico.topo ? "TOPO ->" : "       ",
                      historico.itens[i].id_leitura, historico.itens[i].umidade,
                      historico.itens[i].acao == BOMBA_LIGOU ? "LIGOU" : "DESLIGOU");
    }
}

void tratar_comando(char c) {
    Evento e;
    switch (c) {
        case 'f': exibir_fila(); break;
        case 'p': exibir_pilha(); break;
        case 'd':
            if (desempilhar(&historico, &e)) {
                Serial.printf("Removido do topo: leitura #%d (%.1f%%)\n", e.id_leitura, e.umidade);
            }
            break;
        case 'e':
            if (est.total > 0) {
                Serial.printf("Leituras: %d | media %.1f%% | min %.1f%% | max %.1f%%\n",
                              est.total, est.soma / est.total, est.minimo, est.maximo);
            }
            Serial.printf("Memoria: %d malloc / %d free | nos na RAM: %d\n",
                          total_malloc, total_free, total_malloc - total_free);
            break;
    }
}

/* ============================================================
 *  SETUP E LOOP
 * ============================================================ */

void setup() {
    Serial.begin(BAUD_RATE);
    pinMode(PINO_RELE, OUTPUT);
    pinMode(PINO_LED_STATUS, OUTPUT);
    acionar_bomba(0);
    analogReadResolution(12);

    fila_iniciar(&fila);
    pilha_iniciar(&historico);

    Serial.println("==================================================");
    Serial.println("   AQUASENSE v1.0 - ESP32");
    Serial.printf("   Liga abaixo de %.0f%% | desliga acima de %.0f%%\n", LIMITE_SECO, LIMITE_ENCHARCADO);
    Serial.println("   Comandos: f=fila p=pilha d=desempilhar e=estatisticas");
    Serial.println("==================================================");
}

void loop() {
    if (Serial.available()) {
        tratar_comando((char) Serial.read());
    }

    /* millis() em vez de delay(): o loop continua respondendo aos comandos */
    if (millis() - ultima_leitura >= INTERVALO_LEITURA) {
        ultima_leitura = millis();
        Leitura l = criar_leitura(proximo_id, analogRead(PINO_SENSOR));
        if (enfileirar(&fila, l)) {
            proximo_id++;
        }
        if (fila.tamanho >= PROCESSAR_A_CADA) {
            processar_fila();
        }
    }
}
