/*
 * ============================================================
 *  AQUASENSE - Monitor Inteligente de Umidade do Solo  v1.0
 *  Nicho: Agricultura / Irrigacao automatizada
 *  Grupo 7 - Inovatech 2026
 *  Danilo Martinelli Bueno Veiga, Manoel Florenco da Silva Junior,
 *  Rian Godinho Caixeta Oliveira Martins, Yan Nascimento Sousa
 * ============================================================
 *
 *  Esteira de processamento:
 *
 *    [Sensor / Simulacao] -> struct Leitura -> FILA (FIFO) -> Regra de negocio
 *                                                                |
 *                                         PILHA (LIFO) <- evento da bomba
 *
 *  - FILA (FIFO): lista encadeada com malloc. As leituras sao processadas
 *    na mesma ordem em que chegaram do sensor.
 *  - PILHA (LIFO): vetor de capacidade fixa. Guarda o historico de
 *    acionamentos da bomba; o evento mais recente fica no topo.
 *  - Regra (histerese): umidade < 30% liga a bomba, > 60% desliga,
 *    entre os dois o estado atual e mantido (evita liga/desliga constante).
 *
 *  Compilar:  gcc -Wall -Wextra -o aquasense aquasense.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* ---------- Constantes do sistema (sem numeros magicos) ---------- */
#define LIMITE_SECO        30.0f  /* abaixo disso a bomba liga            */
#define LIMITE_ENCHARCADO  60.0f  /* acima disso a bomba desliga          */
#define CAP_FILA           10     /* limite de leituras aguardando na fila */
#define CAP_PILHA          5      /* limite do historico de eventos       */

/* Calibracao do sensor capacitivo (ESP32, ADC de 12 bits: 0 a 4095) */
#define ADC_SECO           3200   /* leitura bruta com o sensor no ar     */
#define ADC_MOLHADO        1300   /* leitura bruta com o sensor na agua   */

/* ---------- Estruturas de dados ---------- */

typedef enum { SOLO_SECO, SOLO_IDEAL, SOLO_ENCHARCADO } StatusSolo;
typedef enum { BOMBA_DESLIGOU, BOMBA_LIGOU } AcaoBomba;

/* Uma leitura do sensor: a "gaveta" principal do sistema */
typedef struct {
    int        id;          /* numero sequencial da leitura          */
    int        adc_bruto;   /* valor cru do sensor (0 a 4095)        */
    float      umidade;     /* valor convertido em porcentagem       */
    StatusSolo status;      /* classificacao pela regra de negocio   */
} Leitura;

/* No da fila: carrega uma Leitura e aponta para o proximo */
typedef struct No {
    Leitura    dado;
    struct No *proximo;
} No;

/* Fila FIFO: ponteiros para o inicio e o fim garantem O(1) */
typedef struct {
    No *inicio;   /* de onde sai (desenfileirar) */
    No *fim;      /* onde entra (enfileirar)     */
    int tamanho;
} Fila;

/* Um acionamento da bomba registrado no historico */
typedef struct {
    int       id_leitura;   /* qual leitura disparou o evento */
    float     umidade;
    AcaoBomba acao;
} Evento;

/* Pilha LIFO estatica: topo = -1 significa pilha vazia */
typedef struct {
    Evento itens[CAP_PILHA];
    int    topo;
} Pilha;

/* Resumo estatistico de tudo o que ja foi processado */
typedef struct {
    int   total;
    float soma;
    float minimo, maximo;
    int   id_minimo, id_maximo;
} Estatisticas;

/* Contadores de memoria: provam que todo malloc teve seu free */
static int total_malloc = 0;
static int total_free   = 0;

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

/* Enfileirar: insere no fim. O(1), pois ja temos o ponteiro do fim. */
int enfileirar(Fila *f, Leitura dado) {
    if (fila_cheia(f)) {
        printf("  [OVERFLOW BLOQUEADO] Fila cheia (%d/%d). Leitura #%d descartada.\n",
               f->tamanho, CAP_FILA, dado.id);
        return 0;
    }

    No *novo = (No *) malloc(sizeof(No));
    if (novo == NULL) {   /* a placa pode ficar sem RAM: nunca usar ponteiro nulo */
        printf("  [ERRO] Memoria insuficiente para alocar a leitura.\n");
        return 0;
    }
    total_malloc++;

    novo->dado    = dado;
    novo->proximo = NULL;

    if (fila_vazia(f)) {
        f->inicio = novo;       /* primeiro elemento: inicio e fim sao o mesmo */
    } else {
        f->fim->proximo = novo; /* o antigo ultimo passa a apontar para o novo */
    }
    f->fim = novo;
    f->tamanho++;
    return 1;
}

/* Desenfileirar: remove do inicio. O(1). Libera o no com free. */
int desenfileirar(Fila *f, Leitura *saida) {
    if (fila_vazia(f)) {
        printf("  [UNDERFLOW BLOQUEADO] Fila vazia, nada para remover.\n");
        return 0;
    }

    No *removido = f->inicio;
    *saida    = removido->dado;      /* copia o dado antes de liberar o no */
    f->inicio = removido->proximo;
    if (f->inicio == NULL) {
        f->fim = NULL;               /* a fila esvaziou: fim nao pode apontar para lixo */
    }

    free(removido);
    total_free++;
    f->tamanho--;
    return 1;
}

/* Libera todos os nos restantes. O(n). Evita memory leak ao encerrar. */
void fila_liberar(Fila *f) {
    while (!fila_vazia(f)) {
        No *removido = f->inicio;
        f->inicio = removido->proximo;
        free(removido);
        total_free++;
        f->tamanho--;
    }
    f->fim = NULL;
}

/* ============================================================
 *  PILHA (LIFO)
 * ============================================================ */

void pilha_iniciar(Pilha *p) { p->topo = -1; }

int pilha_vazia(const Pilha *p) { return p->topo == -1; }
int pilha_cheia(const Pilha *p) { return p->topo == CAP_PILHA - 1; }

/* Empilhar: coloca no topo. O(1). Recusa se estiver cheia (Stack Overflow). */
int empilhar(Pilha *p, Evento e) {
    if (pilha_cheia(p)) {
        printf("  [OVERFLOW BLOQUEADO] Historico cheio (%d/%d). Desempilhe antes.\n",
               CAP_PILHA, CAP_PILHA);
        return 0;
    }
    p->topo++;
    p->itens[p->topo] = e;
    return 1;
}

/* Desempilhar: retira do topo. O(1). Recusa se estiver vazia (Stack Underflow). */
int desempilhar(Pilha *p, Evento *saida) {
    if (pilha_vazia(p)) {
        printf("  [UNDERFLOW BLOQUEADO] Historico vazio, nada para remover.\n");
        return 0;
    }
    *saida = p->itens[p->topo];
    p->topo--;
    return 1;
}

/* ============================================================
 *  SENSOR E REGRA DE NEGOCIO
 * ============================================================ */

/* Converte a leitura bruta do sensor em porcentagem (0 a 100%). */
float adc_para_umidade(int adc) {
    float pct = (float) (ADC_SECO - adc) * 100.0f / (ADC_SECO - ADC_MOLHADO);
    if (pct < 0.0f)   pct = 0.0f;     /* sensor acima do limite seco    */
    if (pct > 100.0f) pct = 100.0f;   /* sensor abaixo do limite molhado */
    return pct;
}

/* Caminho inverso, usado quando a umidade e digitada manualmente. */
int umidade_para_adc(float pct) {
    return ADC_SECO - (int) (pct * (ADC_SECO - ADC_MOLHADO) / 100.0f);
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

/* Sensor simulado: sorteia um valor bruto um pouco alem da faixa calibrada. */
int ler_sensor_simulado(void) {
    int faixa = (ADC_SECO + 100) - (ADC_MOLHADO - 100);
    return (ADC_MOLHADO - 100) + rand() % (faixa + 1);
}

void atualizar_estatisticas(Estatisticas *est, Leitura l) {
    if (est->total == 0 || l.umidade < est->minimo) {
        est->minimo = l.umidade;
        est->id_minimo = l.id;
    }
    if (est->total == 0 || l.umidade > est->maximo) {
        est->maximo = l.umidade;
        est->id_maximo = l.id;
    }
    est->soma += l.umidade;
    est->total++;
}

/*
 * Esvazia a fila na ordem de chegada (FIFO), aplica a regra com histerese
 * e empilha um evento so quando a bomba muda de estado. O(n) para n leituras.
 */
void processar_fila(Fila *f, Pilha *historico, int *bomba_ligada, Estatisticas *est) {
    Leitura l;

    if (fila_vazia(f)) {
        printf("  Nenhuma leitura aguardando processamento.\n");
        return;
    }

    printf("\n  %-8s %-6s %-9s %-11s %s\n", "LEITURA", "ADC", "UMIDADE", "STATUS", "BOMBA");
    while (!fila_vazia(f)) {
        desenfileirar(f, &l);
        atualizar_estatisticas(est, l);

        if (!*bomba_ligada && l.umidade < LIMITE_SECO) {
            *bomba_ligada = 1;
            Evento e = { l.id, l.umidade, BOMBA_LIGOU };
            empilhar(historico, e);
        } else if (*bomba_ligada && l.umidade > LIMITE_ENCHARCADO) {
            *bomba_ligada = 0;
            Evento e = { l.id, l.umidade, BOMBA_DESLIGOU };
            empilhar(historico, e);
        }

        printf("  #%-7d %-6d %6.1f%%   %-11s %s\n", l.id, l.adc_bruto, l.umidade,
               nome_status(l.status), *bomba_ligada ? "LIGADA" : "desligada");
    }
}

/* ============================================================
 *  EXIBICAO
 * ============================================================ */

void exibir_fila(const Fila *f) {
    printf("\n  FILA (%d/%d)  inicio -> ", f->tamanho, CAP_FILA);
    if (fila_vazia(f)) {
        printf("(vazia)\n");
        return;
    }
    for (const No *atual = f->inicio; atual != NULL; atual = atual->proximo) {
        printf("[#%d %.1f%%] ", atual->dado.id, atual->dado.umidade);
    }
    printf("<- fim\n");
}

void exibir_pilha(const Pilha *p) {
    printf("\n  PILHA DE EVENTOS (%d/%d)\n", p->topo + 1, CAP_PILHA);
    if (pilha_vazia(p)) {
        printf("  (vazia)\n");
        return;
    }
    for (int i = p->topo; i >= 0; i--) {   /* do topo para a base */
        printf("  %s [leitura #%d | %.1f%% | bomba %s]\n",
               i == p->topo ? "TOPO ->" : "       ",
               p->itens[i].id_leitura, p->itens[i].umidade,
               p->itens[i].acao == BOMBA_LIGOU ? "LIGOU" : "DESLIGOU");
    }
}

void exibir_estatisticas(const Estatisticas *est, int bomba_ligada) {
    printf("\n  --- PAINEL DO SOLO ---\n");
    if (est->total == 0) {
        printf("  Nenhuma leitura processada ainda.\n");
    } else {
        printf("  Leituras processadas: %d\n", est->total);
        printf("  Umidade media:        %.1f%%\n", est->soma / est->total);
        printf("  Mais seca:            %.1f%% (leitura #%d)\n", est->minimo, est->id_minimo);
        printf("  Mais umida:           %.1f%% (leitura #%d)\n", est->maximo, est->id_maximo);
    }
    printf("  Bomba agora:          %s\n", bomba_ligada ? "LIGADA" : "desligada");
}

/* ============================================================
 *  ENTRADA DO USUARIO (validada: o sistema nunca aceita lixo)
 * ============================================================ */

static void limpar_entrada(void) {
    int c;
    while ((c = getchar()) != '\n' && c != EOF) { }
}

int ler_inteiro(const char *msg, int min, int max) {
    int valor;
    for (;;) {
        printf("%s", msg);
        if (scanf("%d", &valor) == 1 && valor >= min && valor <= max) {
            limpar_entrada();
            return valor;
        }
        if (feof(stdin)) return min;   /* fim da entrada: sai com seguranca */
        limpar_entrada();
        printf("  Valor invalido. Digite entre %d e %d.\n", min, max);
    }
}

float ler_float(const char *msg, float min, float max) {
    float valor;
    for (;;) {
        printf("%s", msg);
        if (scanf("%f", &valor) == 1 && valor >= min && valor <= max) {
            limpar_entrada();
            return valor;
        }
        if (feof(stdin)) return min;
        limpar_entrada();
        printf("  Valor invalido. Digite entre %.0f e %.0f.\n", min, max);
    }
}

/* ============================================================
 *  TESTE DE ESTRESSE: prova as protecoes de overflow e underflow
 *  usando estruturas temporarias (nao mexe no estado do sistema)
 * ============================================================ */

void teste_de_estresse(void) {
    Fila    f;
    Pilha   p;
    Leitura l;
    Evento  e = { 0, 0.0f, BOMBA_LIGOU };

    fila_iniciar(&f);
    pilha_iniciar(&p);

    printf("\n  >> 1. Underflow na fila: removendo de uma fila vazia\n");
    desenfileirar(&f, &l);

    printf("\n  >> 2. Overflow na fila: tentando inserir %d leituras (limite %d)\n",
           CAP_FILA + 1, CAP_FILA);
    for (int i = 1; i <= CAP_FILA + 1; i++) {
        enfileirar(&f, criar_leitura(i, ler_sensor_simulado()));
    }
    exibir_fila(&f);

    printf("\n  >> 3. Underflow na pilha: desempilhando de uma pilha vazia\n");
    desempilhar(&p, &e);

    printf("\n  >> 4. Overflow na pilha: tentando empilhar %d eventos (limite %d)\n",
           CAP_PILHA + 1, CAP_PILHA);
    for (int i = 1; i <= CAP_PILHA + 1; i++) {
        e.id_leitura = i;
        e.umidade    = 10.0f * i;
        empilhar(&p, e);
    }
    exibir_pilha(&p);

    fila_liberar(&f);
    printf("\n  Teste concluido. Fila temporaria liberada da memoria.\n");
}

/* ============================================================
 *  PROGRAMA PRINCIPAL
 * ============================================================ */

int main(void) {
    Fila         fila;
    Pilha        historico;
    Estatisticas est = { 0, 0.0f, 0.0f, 0.0f, 0, 0 };
    int          bomba_ligada = 0;
    int          proximo_id   = 1;
    int          opcao;

    srand((unsigned) time(NULL));
    fila_iniciar(&fila);
    pilha_iniciar(&historico);

    printf("==================================================\n");
    printf("   AQUASENSE v1.0 - Monitor de Umidade do Solo\n");
    printf("   Liga a bomba abaixo de %.0f%% | desliga acima de %.0f%%\n",
           LIMITE_SECO, LIMITE_ENCHARCADO);
    printf("==================================================\n");

    do {
        printf("\n--------------- MENU ---------------\n");
        printf(" 1. Inserir leitura manual (%%)\n");
        printf(" 2. Gerar leituras do sensor simulado\n");
        printf(" 3. Processar fila (aplicar regra)\n");
        printf(" 4. Ver fila de leituras\n");
        printf(" 5. Ver historico de eventos (pilha)\n");
        printf(" 6. Desempilhar ultimo evento\n");
        printf(" 7. Painel de estatisticas\n");
        printf(" 8. Teste de estresse (overflow/underflow)\n");
        printf(" 0. Sair\n");
        opcao = ler_inteiro("Opcao: ", 0, 8);

        switch (opcao) {
            case 1: {
                float pct = ler_float("  Umidade do solo (0 a 100): ", 0.0f, 100.0f);
                if (enfileirar(&fila, criar_leitura(proximo_id, umidade_para_adc(pct)))) {
                    printf("  Leitura #%d enfileirada.\n", proximo_id++);
                }
                break;
            }
            case 2: {
                int n = ler_inteiro("  Quantas leituras? (1 a 10): ", 1, CAP_FILA);
                for (int i = 0; i < n; i++) {
                    if (!enfileirar(&fila, criar_leitura(proximo_id, ler_sensor_simulado()))) {
                        break;   /* fila cheia: para de gerar */
                    }
                    proximo_id++;
                }
                exibir_fila(&fila);
                break;
            }
            case 3:
                processar_fila(&fila, &historico, &bomba_ligada, &est);
                break;
            case 4:
                exibir_fila(&fila);
                break;
            case 5:
                exibir_pilha(&historico);
                break;
            case 6: {
                Evento e;
                if (desempilhar(&historico, &e)) {
                    printf("  Removido do topo: leitura #%d (%.1f%%), bomba %s.\n",
                           e.id_leitura, e.umidade,
                           e.acao == BOMBA_LIGOU ? "LIGOU" : "DESLIGOU");
                }
                break;
            }
            case 7:
                exibir_estatisticas(&est, bomba_ligada);
                break;
            case 8:
                teste_de_estresse();
                break;
        }
    } while (opcao != 0);

    fila_liberar(&fila);
    printf("\nEncerrando AquaSense.\n");
    printf("Memoria: %d malloc / %d free -> %s\n", total_malloc, total_free,
           total_malloc == total_free ? "sem vazamento" : "VAZAMENTO DETECTADO");
    return 0;
}
