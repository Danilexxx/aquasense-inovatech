# AquaSense no Wokwi (simulador de ESP32)

1. Acesse https://wokwi.com, faça login e clique em **New Project → ESP32 → Arduino**.
2. Na aba `sketch.ino`, apague tudo e cole o conteúdo de `sketch.ino` desta pasta.
3. Na aba `diagram.json`, apague tudo e cole o conteúdo de `diagram.json` desta pasta.
4. Clique em **Play** (▶). O monitor serial abre embaixo, a 115200 baud.
5. Gire o **potenciômetro**: ele faz o papel do sensor de umidade.
   - Girado para a **direita** (ADC alto, até 4095) → solo seco → abaixo de 30% o relé liga e os LEDs azul (bomba) e verde (status) acendem.
   - Girado para a **esquerda** (ADC baixo) → solo molhado → acima de 60% o relé desliga. Por volta da metade do giro (ADC ~1900) dá ~67%.
   - A fila é processada a cada 3 leituras (1 por segundo), então a bomba reage em até 3 s.
6. No monitor serial, digite `f` (fila), `p` (pilha), `d` (desempilhar) ou `e` (estatísticas e memória).
7. Clique em **Save** e guarde o link do projeto para abrir no dia da feira.

## Ligações

| Componente | Pino | ESP32 |
|---|---|---|
| Potenciômetro (no lugar do sensor) | VCC / GND / SIG | 3V3 / GND / GPIO34 |
| Módulo relé | VCC / GND / IN | 5V / GND / GPIO26 |
| LED azul "BOMBA" (via resistor 220Ω) | ânodo | relé NO (COM no 5V) |
| LED verde "STATUS" (via resistor 220Ω) | ânodo | GPIO2 |
