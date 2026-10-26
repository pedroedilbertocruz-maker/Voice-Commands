# Voice Commands (Dusklight mod)

Fale o nome de um item e o Link usa o item, **mesmo que ele não esteja no X ou no Y**. Funciona em português e em inglês, e o reconhecimento é **offline**: o áudio nunca sai do seu computador.

- Diga **"Botas de Ferro"** (ou "Iron Boots") e o Link calça as botas.
- Diga de novo e o Link tira as botas: elas saem do botão e **o item que estava antes volta**.

## Como funciona

1. O mod confere se o Link já tem o item.
2. Coloca o item em um dos botões de item e simula o aperto. O Link usa o item com a animação normal.
3. Falar o mesmo item de novo devolve o item anterior ao botão. No caso das Botas de Ferro, o Link primeiro as tira com a animação normal.

O item precisa ficar num botão enquanto está em uso, porque o próprio jogo tira as botas e guarda itens de mão quando eles não estão no X ou no Y. Se o item já estiver num dos botões, o mod apenas aperta esse botão.

## Comandos

| Item | Português | English |
|---|---|---|
| Botas de Ferro | "botas de ferro", "botas" | "iron boots", "boots" |
| Gancho | "gancho", "garra" | "claw shot", "hook shot" |
| Gancho Duplo | "gancho duplo" | "double claw shot", "double hook" |
| Bumerangue | "bumerangue" | "boomerang" |
| Arco | "arco", "flecha" | "bow", "arrows" |
| Lanterna | "lanterna" | "lantern", "lamp" |
| Pião | "pião", "girador" | "spinner" |
| Bola de Ferro | "bola de ferro" | "ball and chain", "iron ball" |
| Bastão do Domínio | "bastão", "cetro" | "dominion rod" |
| Bombas | "bombas" | "bombs" |
| Bombas Aquáticas | "bombas aquáticas" | "water bombs" |
| Bombinhas | "bombinhas" | "bomblings" |
| Estilingue | "estilingue", "atiradeira" | "slingshot" |
| Vara de Pescar | "vara de pescar" | "fishing rod" |
| Olho de Falcão | "olho de falcão" | "hawk eye" |
| Chamar Cavalo | "chamar cavalo", "cavalo", "epona" | "horse call", "horse", "epona" |
| Garrafa | "garrafa" | "bottle" |

A lista completa de frases aparece no painel do mod, em **Settings and Commands**.

## Opções (menu de mods → Voice Commands)

- **Voice Commands:** liga ou desliga o microfone.
- **Language:** English ou Português. Cada idioma tem o próprio modelo de voz.
- **Item Button:** o botão que recebe os itens chamados por voz. Em **Auto**, o mod usa um botão vazio se houver, senão o X.
- **Show What Was Understood:** mostra uma notificação com o item reconhecido.
- **Test Without Microphone:** botões para testar cada item sem falar.

Na primeira vez que um idioma é usado, o modelo de voz é instalado na pasta de dados do mod, levando alguns segundos sem travar o jogo.

## Plataformas

Windows (x64) e Linux (x64 e ARM). Em outras plataformas, o mod carrega sem a parte de voz.

## Créditos

- [Vosk](https://github.com/alphacep/vosk-api) e os modelos de voz small en-us / small pt (Apache-2.0)
- [miniaudio](https://github.com/mackron/miniaudio) (domínio público / MIT-0)

## Compilar localmente

```
cmake -B build
cmake --build build
```

O build baixa o Vosk, o miniaudio e os modelos de voz. Para compilar sem os modelos, use `-DVOICE_BUNDLE_MODELS=OFF`.
