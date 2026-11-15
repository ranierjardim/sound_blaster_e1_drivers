# E1 Studio — software novo para a Sound Blaster E1 no Windows 11

E1 Studio substitui o painel de controle e os efeitos de áudio que a Creative deixou de manter
para a **Sound Blaster E1** (USB `VID_041E&PID_323B`). Ele traz de volta os recursos do painel
original (surround para fones, clareza, graves, volume inteligente, diálogo, equalizador de 10
bandas, Modo Scout, efeitos de voz, redução de ruído do microfone e perfis) com **qualidade máxima
como padrão**: com tudo desligado o caminho do som é **bit-exato** — o E1 Studio não altera um
único bit.

> Projeto independente, sem ligação com a Creative Technology. "Sound Blaster" e "E1" são marcas
> dos seus donos e aparecem aqui só para identificar o aparelho. Nenhum código, imagem, som ou
> tabela de coeficientes da Creative é distribuído; os efeitos foram reescritos do zero.

---

## 1. Por que o som piorou no Windows 11

A investigação (veja `docs/RECONSTRUCTION_SPEC.md`) mostrou que **o driver não é o culpado**: o
driver USB assinado da Creative já envia áudio de 24 bits / 44,1 kHz sem nenhuma alteração. A perda
de qualidade vem de **processamento em software empilhado**:

1. **Dolby Atmos for Headphones** está ativo no dispositivo "Alto-falantes (Sound Blaster E1)".
   Ele "virtualiza" todo o som para fones — algo que o Windows 7 nem tinha.
2. Por cima dele, os efeitos da Creative rodam com os padrões de fábrica (surround virtual em 40 %
   e realce de agudos em 65 %). Resultado: o som é virtualizado **duas vezes**.
3. Em 26/08/2026 a Limpeza de Armazenamento (Storage Sense) apagou o dispositivo antigo; em
   14/11/2026 ele foi recriado com os padrões de fábrica, e seus ajustes se perderam.

O E1 Studio resolve os itens 2 e 3 sozinho. O item 1 é uma opção do Windows que você precisa
desligar (passo 3.4).

## 2. O que o aparelho realmente suporta

A E1 é um dispositivo USB Full-Speed com taxa **fixa de 44,1 kHz**: reprodução estéreo em 16 ou
24 bits e microfone mono de 16 bits. Nenhum driver ou programa consegue mudar isso. Conteúdo em
48 kHz (jogos, navegadores, vídeos) é convertido para 44,1 kHz pelo próprio Windows.

---

## 3. Instalação

### 3.1 Requisitos

* Windows 11 x64, com o driver da Creative instalado (é ele que conversa com o USB; o E1 Studio não
  instala nenhum driver de kernel).
* Uma conta de administrador.
* Para compilar: Visual Studio 2022 Build Tools (carga de trabalho C++) e o Windows SDK 10.0.26100.

### 3.2 Compilar

Abra um Prompt de Comando na pasta do projeto e rode:

```
build.cmd            :: compila tudo em build\x64\Release\
build.cmd test       :: compila e roda todos os testes automáticos
```

No PowerShell, use `.\build.cmd` e `.\build.cmd test`. Outras opções: `debug`, `clean`,
`full` (testes longos) — veja o cabeçalho de `build.cmd`.

Saídas: `e1fx.dll` (efeitos), `e1panel.exe` (painel), `e1ctl.exe` (linha de comando) e os testes.

### 3.3 Instalar

1. Feche o painel da Creative, se estiver aberto.
2. Abra o **PowerShell como administrador** (menu Iniciar > digite *PowerShell* > *Executar como
   administrador*) e vá até a pasta do projeto.
3. **Veja antes o que vai mudar** (simulação — não altera nada e não precisa de administrador):

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\tools\install.ps1 -WhatIf
   ```

4. Instale:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\tools\install.ps1 -DisableCreativePanel -RegisterReapplyTask
   ```

O instalador:

* encontra os dispositivos da E1 ("Alto-falantes" e "Microfone"; o "What U Hear" não é tocado);
* **faz backup** de tudo o que vai mudar em `C:\ProgramData\E1Studio\backup\<data-hora>\`
  (arquivos `.reg` + `manifest.json`);
* copia os programas para `C:\Program Files\E1Studio\` e registra a `e1fx.dll`;
* troca os efeitos da Creative pelos do E1 Studio nesses dois dispositivos;
* cria `C:\ProgramData\E1Studio\settings.bin` com tudo **desligado** (bit-exato), se ainda não existir;
* reinicia o serviço de áudio do Windows (o som some por alguns segundos) e confere o resultado.

Rodar o instalador de novo é seguro: se nada mudou, ele não faz nada.

Opções:

| Opção | O que faz |
|---|---|
| `-WhatIf` | Só mostra cada alteração que seria feita. |
| `-DisableCreativePanel` | Remove a inicialização automática do painel da Creative (`SBE.exe`) e o fecha (com backup). |
| `-RegisterReapplyTask` | Cria uma tarefa agendada que, na inicialização e no logon, reaplica o E1 Studio se o Windows recriar o dispositivo (como fez a Limpeza de Armazenamento). |
| `-SetFormat24` | Garante o formato do dispositivo em 44,1 kHz / 24 bits (no seu PC ele já está assim). |
| `-SkipCapture` | Não mexe no microfone. |
| `-IncludeNotPresent` | Configura também dispositivos antigos que estão "não presentes". |
| `-NoRestart` | Não reinicia o áudio agora (vale após reiniciar o PC). |

### 3.4 Desligue o áudio espacial (importante)

Configurações > Sistema > Som > **Alto-falantes (Sound Blaster E1)** > **Áudio espacial: Desativado**.

O painel do E1 Studio e o comando `e1ctl find` avisam quando ele está ligado.

### 3.5 Para ouvir de forma bit-perfeita

* Deixe os efeitos desligados (padrão) — o painel mostra **"Bit-exato (bypass)"**.
* Em players com modo exclusivo (WASAPI exclusivo), escolha **44,1 kHz / 24 bits**. A E1 só aceita
  16 bits ou 24 bits "compactado"; formatos de 32 bits são recusados pelo driver da Creative.
* Músicas em 44,1 kHz (CD, a maioria dos serviços de streaming) não passam por conversão.

---

## 4. Uso

### 4.1 Painel (`e1panel.exe`)

Abra pelo menu Iniciar ou em `C:\Program Files\E1Studio\e1panel.exe`. Páginas:

| Página | Conteúdo |
|---|---|
| **Estúdio** | Botão geral dos efeitos + Surround, Clareza, Graves (com frequência de corte), Volume Inteligente (Normal / Alto / Noturno) e Diálogo+. |
| **Voz** | Efeitos do microfone: Efeitos de voz (18 personagens), Volume Inteligente do microfone e Redução de ruído. |
| **Modo Scout** | Curva para ouvir passos em jogos, com teclas de atalho globais. |
| **Alto-falantes / Fones** | Diz ao Surround se você usa fones (3D binaural) ou caixas estéreo. |
| **Mixer** | Volumes de reprodução e gravação. |
| **Equalizador** | 10 bandas (31 Hz a 16 kHz), pré-amplificação e predefinições; funciona mesmo com o botão do Estúdio desligado. |
| **Avançado** | Ganho final, limitador de segurança e informações do caminho de áudio. |
| **Perfil** | Salvar, carregar, importar e exportar perfis. |

Opções de linha de comando: `/tray` (inicia na área de notificação), `/lang:pt` ou `/lang:en`,
`/page:N`, `/exit`.

### 4.2 Linha de comando (`e1ctl.exe`)

Não precisa de administrador. Exemplos (no Prompt de Comando, em `C:\Program Files\E1Studio`):

```
e1ctl find                                  :: dispositivos da E1, formato, efeitos, áudio espacial
e1ctl status                                :: o efeito está rodando? taxa, canais, medidores, bypass
e1ctl status --watch                        :: medidores ao vivo (Ctrl+C para sair)
e1ctl get                                   :: todos os parâmetros
e1ctl set render.master_on 1 render.surround_on 1 render.surround_level 0.3
e1ctl set render.eq_on 1 render.eq.1k -3    :: banda de 1 kHz em -3 dB
e1ctl preset eq list                        :: predefinições do equalizador
e1ctl preset eq rock
e1ctl preset vfx list                       :: predefinições de voz
e1ctl defaults                              :: volta tudo ao padrão (desligado = bit-exato)
e1ctl keys                                  :: lista de chaves, faixas e significados
e1ctl --dry-run set ...                     :: mostra o resultado sem gravar
```

Valores lógicos aceitam `1/0`, `on/off`; `render.svm_mode` aceita `normal|loud|night`;
`render.speaker_mode` aceita `headphones|stereo`. As mudanças valem na hora (se o efeito estiver
rodando) e são salvas em `C:\ProgramData\E1Studio\settings.bin`.

---

## 5. Desinstalação

No PowerShell como administrador:

```powershell
powershell -ExecutionPolicy Bypass -File "C:\Program Files\E1Studio\uninstall.ps1" -WhatIf   # simulação
powershell -ExecutionPolicy Bypass -File "C:\Program Files\E1Studio\uninstall.ps1"
```

O desinstalador devolve cada dispositivo **exatamente** ao estado do backup (os efeitos da Creative
voltam), restaura a inicialização do painel da Creative, remove a tarefa agendada, reinicia o áudio,
remove o registro da DLL e apaga `C:\Program Files\E1Studio`. Os backups ficam guardados.

| Opção | O que faz |
|---|---|
| `-List` | Lista os backups disponíveis. |
| `-Backup <nome>` | Restaura um backup específico em vez da escolha automática. |
| `-RemoveSettings` | Apaga também `settings.bin` e as preferências do painel. |
| `-KeepFiles` | Restaura os dispositivos mas mantém os programas instalados. |

---

## 6. Solução de problemas

* **`e1ctl status` diz "not available" ou "idle".** O efeito só é carregado quando algum programa
  toca som na E1. Toque algo e repita. Se continuar, rode `e1ctl find` e confira se a linha
  `MFX (mode)` mostra `(E1 Studio render)`.
* **`e1ctl find` mostra "enhancements: DISABLED".** Em Configurações > Som > dispositivo, ative
  "Melhorias de áudio" (*Efeitos padrão do dispositivo*). Sem isso o Windows não carrega nenhum efeito.
* **Depois de uma atualização do Windows os efeitos da Creative voltaram.** O Windows recriou o
  dispositivo. Rode `install.ps1` de novo (ou instale com `-RegisterReapplyTask`, que faz isso sozinho).
* **Algo deu errado durante a instalação.** O instalador desfaz sozinho as alterações do registro
  se uma gravação falhar. Em último caso, `uninstall.ps1` restaura o backup; os arquivos `.reg` em
  `C:\ProgramData\E1Studio\backup\` podem ser importados manualmente.
* **Registros de execução**: `C:\ProgramData\E1Studio\logs\`.

## 7. Limitações conhecidas

* Taxa fixa de 44,1 kHz e microfone de 16 bits (limite do hardware).
* O "What U Hear" continua sendo o da Creative (16 bits) e não recebe efeitos.
* A troca da entrada de fone/microfone ("Advanced Features" do painel original) e o nível de bateria
  ainda não estão disponíveis: o protocolo do aparelho para isso não foi descoberto.
* O efeito não é assinado digitalmente; ele carrega porque o Windows deste PC permite efeitos não
  protegidos (`DisableProtectedAudioDG = 1`). O instalador avisa se isso mudar.

## 8. Para desenvolvedores

Arquitetura, contratos entre os componentes, regras de tempo real, o que o instalador grava no
registro e as permissões encontradas: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).
