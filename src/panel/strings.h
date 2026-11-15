/*
 * strings.h - UI strings. English and Brazilian Portuguese; the table is chosen from
 * GetUserDefaultUILanguage() (any Portuguese UI language selects pt-BR) or /lang:en|pt.
 * Neutral feature names only (no third-party trademarks in UI text).
 */
#ifndef E1P_STRINGS_H
#define E1P_STRINGS_H

#include <wchar.h>

#define STRING_TABLE(X) \
    X(S_APP_NAME, L"E1 Studio", L"E1 Studio") \
    X(S_LOGO, L"E1 STUDIO", L"E1 STUDIO") \
    X(S_NOT_CONNECTED, L"Device not connected", L"Dispositivo não conectado") \
    X(S_CONNECTED, L"Connected", L"Conectado") \
    X(S_TIP_MENU, L"Menu", L"Menu") \
    X(S_TIP_MIN, L"Minimize", L"Minimizar") \
    X(S_TIP_CLOSE, L"Close to the notification area", L"Fechar para a área de notificação") \
    X(S_MENU_STARTUP, L"Start with Windows", L"Iniciar com o Windows") \
    X(S_MENU_RESTORE, L"Restore defaults…", L"Restaurar padrões…") \
    X(S_MENU_ABOUT, L"About E1 Studio…", L"Sobre o E1 Studio…") \
    X(S_MENU_EXIT, L"Exit", L"Sair") \
    X(S_TRAY_SHOW, L"Open E1 Studio", L"Abrir o E1 Studio") \
    X(S_TRAY_STILL_RUNNING, L"E1 Studio keeps running in the notification area so hot keys stay active.", L"O E1 Studio continua em execução na área de notificação para manter as teclas de atalho ativas.") \
    X(S_TRAY_SCOUT_ON, L"Scout Mode on", L"Modo Scout ligado") \
    X(S_TRAY_SCOUT_OFF, L"Scout Mode off", L"Modo Scout desligado") \
    X(S_NAV_STUDIO, L"Studio", L"Estúdio") \
    X(S_NAV_VOICE, L"Voice", L"Voz") \
    X(S_NAV_SCOUT, L"Scout Mode", L"Modo Scout") \
    X(S_NAV_SPEAKERS, L"Speakers / Headphones", L"Alto-falantes / Fones") \
    X(S_NAV_MIXER, L"Mixer", L"Mixer") \
    X(S_NAV_EQ, L"Equalizer", L"Equalizador") \
    X(S_NAV_ADVANCED, L"Advanced", L"Avançado") \
    X(S_NAV_PROFILE, L"Profile", L"Perfil") \
    X(S_PROFILE_NONE, L"No profile loaded", L"Nenhum perfil carregado") \
    X(S_PROFILE_MODIFIED, L"%s (modified)", L"%s (modificado)") \
    X(S_TIP_MUTE, L"Mute / unmute", L"Silenciar / reativar") \
    X(S_TIP_VOLUME, L"Speaker volume", L"Volume do alto-falante") \
    X(S_ENGINE_LINE, L"Effects engine: %s", L"Mecanismo de efeitos: %s") \
    X(S_ENG_RUNNING, L"running", L"em execução") \
    X(S_ENG_IDLE, L"idle (no audio)", L"ocioso (sem áudio)") \
    X(S_ENG_NOT_LOADED, L"not loaded", L"não carregado") \
    X(S_STUDIO_TITLE, L"Studio", L"Estúdio") \
    X(S_STUDIO_ON, L"Effects on", L"Efeitos ligados") \
    X(S_STUDIO_OFF, L"Effects off — bit-exact passthrough", L"Efeitos desligados — passagem bit-exata") \
    X(S_SCOUT_FORCED, L"  ·  Scout Mode active", L"  ·  Modo Scout ativo") \
    X(S_STUDIO_OFF_EQ, L"Studio effects off  ·  Equalizer active", L"Efeitos do Estúdio desligados  ·  Equalizador ativo") \
    X(S_STUDIO_OFF_SCOUT, L"Studio effects off  ·  Scout Mode active (surround + Scout EQ)", L"Efeitos do Estúdio desligados  ·  Modo Scout ativo (surround + EQ Scout)") \
    X(S_TIP_STUDIO_POWER, L"Turn all Studio effects on or off. The signal reaches the device untouched only while these effects, the Equalizer and Scout Mode are all off (and the output trim is 0 dB).", L"Liga ou desliga todos os efeitos do Estúdio. O sinal só chega intacto ao dispositivo quando estes efeitos, o Equalizador e o Modo Scout estão todos desligados (e o ajuste de saída é 0 dB).") \
    X(S_CH_UNIT, L"ch", L"canais") \
    X(S_METER_L, L"L", L"E") \
    X(S_METER_R, L"R", L"D") \
    X(S_IMPORTED, L"Imported", L"Importado") \
    X(S_MENU_LANGUAGE, L"Language…", L"Idioma…") \
    X(S_LANG_AUTO, L"Automatic (Windows language)", L"Automático (idioma do Windows)") \
    X(S_LANG_EN, L"English", L"English") \
    X(S_LANG_PT, L"Português (Brasil)", L"Português (Brasil)") \
    X(S_LANG_TITLE, L"Language", L"Idioma") \
    X(S_LANG_RESTART, L"The new language is used the next time E1 Studio starts.", L"O novo idioma será usado na próxima vez que o E1 Studio for iniciado.") \
    X(S_VO_WINSET, L"Windows settings…", L"Ajustes do Windows…") \
    X(S_TIP_VO_WINSET, L"Opens the Windows recording devices: set the microphone as the default device, or use \"Listen to this device\" (Listen tab) to monitor the mic in the headphones.", L"Abre os dispositivos de gravação do Windows: defina o microfone como dispositivo padrão ou use \"Escutar este dispositivo\" (guia Escutar) para monitorar o microfone nos fones.") \
    X(S_TIP_ENGINE_RAW, L"Apps that open the device in RAW mode (some communication and pro-audio apps) bypass E1 Studio by design; their audio is never processed, whatever the settings.", L"Aplicativos que abrem o dispositivo em modo RAW (alguns de comunicação e de áudio profissional) ignoram o E1 Studio por definição; o áudio deles nunca é processado, quaisquer que sejam as configurações.") \
    X(S_SURROUND, L"Surround", L"Surround") \
    X(S_TIP_SURROUND, L"Control the level of audio immersion in music, movies and games.", L"Controla o nível de imersão em músicas, filmes e jogos.") \
    X(S_CLARITY, L"Clarity", L"Clareza") \
    X(S_TIP_CLARITY, L"Restore detail and punch lost in compressed music and movies.", L"Recupera detalhes e impacto perdidos em músicas e filmes comprimidos.") \
    X(S_BASS, L"Bass", L"Graves") \
    X(S_TIP_BASS, L"Add low-frequency weight to headphones and small speakers.", L"Acrescenta peso às baixas frequências em fones e alto-falantes pequenos.") \
    X(S_CROSSOVER, L"Crossover", L"Corte") \
    X(S_TIP_CROSSOVER, L"Frequency below which the bass enhancement works.", L"Frequência abaixo da qual o realce de graves atua.") \
    X(S_SMARTVOL, L"Smart Volume", L"Volume Inteligente") \
    X(S_TIP_SMARTVOL, L"Keep loudness even between quiet and loud passages.", L"Mantém a intensidade uniforme entre trechos baixos e altos.") \
    X(S_SVM_NORMAL, L"Normal", L"Normal") \
    X(S_SVM_LOUD, L"Loud", L"Alto") \
    X(S_SVM_NIGHT, L"Night", L"Noturno") \
    X(S_TIP_SVM_NORMAL, L"Balanced automatic loudness.", L"Intensidade automática equilibrada.") \
    X(S_TIP_SVM_LOUD, L"Maintain loudness at the maximum level at all times.", L"Mantém a intensidade sempre no nível máximo.") \
    X(S_TIP_SVM_NIGHT, L"Preserve audio details at quieter volume levels.", L"Preserva os detalhes do áudio em volumes mais baixos.") \
    X(S_DIALOG, L"Dialog+", L"Diálogo+") \
    X(S_TIP_DIALOG, L"Make voices in movies clearer.", L"Deixa as vozes dos filmes mais claras.") \
    X(S_VOICE_TITLE, L"Voice", L"Voz") \
    X(S_VOICE_ON, L"Voice effects on", L"Efeitos de voz ligados") \
    X(S_VOICE_OFF, L"Voice effects off — bit-exact", L"Efeitos de voz desligados — bit-exato") \
    X(S_TIP_VOICE_POWER, L"Turn all microphone effects on or off.", L"Liga ou desliga todos os efeitos do microfone.") \
    X(S_MIC_VOLUME, L"Recording volume", L"Volume de gravação") \
    X(S_MIC_NOT_CONNECTED, L"Microphone not connected", L"Microfone não conectado") \
    X(S_TIP_MIC_MUTE, L"Mute / unmute the microphone", L"Silenciar / reativar o microfone") \
    X(S_TIP_MIC_VOL, L"Microphone recording level", L"Nível de gravação do microfone") \
    X(S_VFX, L"Voice FX", L"Efeitos de voz") \
    X(S_TIP_VFX, L"Morph your voice into different characters and accents.", L"Transforma sua voz em diferentes personagens e sotaques.") \
    X(S_TIP_VSVM, L"Be heard clearly without having to shout or whisper.", L"Seja ouvido com clareza sem precisar gritar ou sussurrar.") \
    X(S_NR, L"Noise Reduction", L"Redução de ruído") \
    X(S_TIP_NR, L"Remove steady background noise (fans, hum) from your voice.", L"Remove ruídos de fundo constantes (ventoinhas, zumbido) da sua voz.") \
    X(S_NR_STRENGTH, L"Strength", L"Intensidade") \
    X(S_TIP_NR_STRENGTH, L"Higher values remove more noise but can colour the voice.", L"Valores maiores removem mais ruído, mas podem alterar a voz.") \
    X(S_SCOUT_TITLE, L"Scout Mode", L"Modo Scout") \
    X(S_SCOUT_DESC, L"Hear footsteps and distant sounds earlier in games. While on, a tuned curve replaces the equalizer and surround is enabled.", L"Ouça passos e sons distantes mais cedo nos jogos. Enquanto ligado, uma curva ajustada substitui o equalizador e o surround é ativado.") \
    X(S_HOTKEYS, L"Hot keys", L"Teclas de atalho") \
    X(S_TASK, L"Task", L"Tarefa") \
    X(S_HOTKEY, L"Hot key", L"Atalho") \
    X(S_HK_ON, L"Turn on Scout Mode", L"Ligar o Modo Scout") \
    X(S_HK_OFF, L"Turn off Scout Mode", L"Desligar o Modo Scout") \
    X(S_HK_NONE, L"None", L"Nenhum") \
    X(S_HK_PRESS, L"Press a key combination…", L"Pressione uma combinação…") \
    X(S_HK_ACTIVE, L"Active", L"Ativo") \
    X(S_HK_FAILED, L"Unavailable — used by another program", L"Indisponível — em uso por outro programa") \
    X(S_HK_HINT, L"Click a field and press the keys (with Ctrl, Alt or Win — Shift only together with one of them — or a function key). Backspace clears, Esc cancels. Hot keys work while E1 Studio runs, also from the notification area — enable \"Start with Windows\" in the menu.", L"Clique em um campo e pressione as teclas (com Ctrl, Alt ou Win — Shift só junto com uma delas — ou uma tecla de função). Backspace limpa, Esc cancela. Os atalhos funcionam enquanto o E1 Studio estiver em execução, inclusive na área de notificação — ative \"Iniciar com o Windows\" no menu.") \
    X(S_SPK_TITLE, L"Speakers / Headphones", L"Alto-falantes / Fones") \
    X(S_SPK_DESC, L"Tell E1 Studio what is connected so Surround renders correctly.", L"Informe ao E1 Studio o que está conectado para o Surround funcionar corretamente.") \
    X(S_SPK_HEADPHONES, L"Headphones", L"Fones de ouvido") \
    X(S_SPK_HEADPHONES_DESC, L"Binaural 3D rendering for headphones and headsets", L"Renderização 3D binaural para fones e headsets") \
    X(S_SPK_STEREO, L"Stereo speakers", L"Alto-falantes estéreo") \
    X(S_SPK_STEREO_DESC, L"Crosstalk-cancelled virtual surround for 2.0 / 2.1 speakers", L"Surround virtual com cancelamento de diafonia para caixas 2.0 / 2.1") \
    X(S_SPK_NOTE, L"Surround and Bass are tuned for what you pick. Clarity, Smart Volume, Dialog+ and the equalizer work the same in both modes.", L"O Surround e os Graves são ajustados conforme a escolha. Clareza, Volume Inteligente, Diálogo+ e o equalizador funcionam igual nos dois modos.") \
    X(S_MIXER_TITLE, L"Mixer", L"Mixer") \
    X(S_PLAYBACK, L"Playback", L"Reprodução") \
    X(S_RECORDING, L"Recording", L"Gravação") \
    X(S_BALANCE, L"Balance", L"Balanço") \
    X(S_TIP_BALANCE, L"Left / right balance (double-click to centre)", L"Balanço esquerda / direita (clique duplo centraliza)") \
    X(S_EQ_TITLE, L"Equalizer", L"Equalizador") \
    X(S_TIP_EQ_ENABLE, L"Turn the equalizer on or off (independent of the Studio switch).", L"Liga ou desliga o equalizador (independente do botão do Estúdio).") \
    X(S_SAVE, L"Save", L"Salvar") \
    X(S_DELETE, L"Delete", L"Excluir") \
    X(S_EQ_CUSTOM, L"Custom", L"Personalizado") \
    X(S_LEVEL, L"LEVEL", L"NÍVEL") \
    X(S_EQ_SCOUT_OVERRIDE, L"Scout Mode is on — its curve replaces this equalizer", L"Modo Scout ligado — a curva dele substitui este equalizador") \
    X(S_EQ_SAVE_TITLE, L"Save equalizer preset", L"Salvar predefinição do equalizador") \
    X(S_NAME_PROMPT, L"Name (up to 32 characters):", L"Nome (até 32 caracteres):") \
    X(S_NAME_INVALID, L"Use 1 to 32 characters, without \\ / : * ? \" < > | [ ]", L"Use de 1 a 32 caracteres, sem \\ / : * ? \" < > | [ ]") \
    X(S_NAME_RESERVED, L"This name belongs to a built-in preset. Choose another name.", L"Este nome pertence a uma predefinição de fábrica. Escolha outro nome.") \
    X(S_CONFIRM_OVERWRITE, L"\"%s\" already exists. Overwrite it?", L"\"%s\" já existe. Substituir?") \
    X(S_CONFIRM_DELETE, L"Delete \"%s\"?", L"Excluir \"%s\"?") \
    X(S_SAVE_FAILED, L"The file could not be written:\n%s", L"Não foi possível gravar o arquivo:\n%s") \
    X(S_ADV_TITLE, L"Advanced", L"Avançado") \
    X(S_OUTPUT, L"Output", L"Saída") \
    X(S_OUTPUT_TRIM, L"Output trim", L"Ajuste de saída") \
    X(S_TIP_TRIM, L"Final gain before the limiter. Applies only while effects are active.", L"Ganho final antes do limitador. Só atua com efeitos ativos.") \
    X(S_LIMITER, L"Safety limiter", L"Limitador de segurança") \
    X(S_TIP_LIMITER, L"Prevents clipping when effects add gain.", L"Evita saturação quando os efeitos aumentam o ganho.") \
    X(S_ENGINE, L"Effects engine", L"Mecanismo de efeitos") \
    X(S_STATUS, L"Status", L"Estado") \
    X(S_PATH, L"Signal path", L"Caminho do sinal") \
    X(S_PATH_BITEXACT, L"Bit-exact (bypass)", L"Bit-exato (bypass)") \
    X(S_PATH_PROCESSING, L"Processing", L"Processando") \
    X(S_PATH_UNKNOWN, L"—", L"—") \
    X(S_STREAM, L"Stream", L"Fluxo") \
    X(S_XRUNS, L"Skipped buffers", L"Buffers ignorados") \
    X(S_GR, L"Limiter", L"Limitador") \
    X(S_DEVICE, L"Device", L"Dispositivo") \
    X(S_OUTPUT_FMT, L"Output", L"Saída") \
    X(S_MIC_FMT, L"Microphone", L"Microfone") \
    X(S_ENDPOINT, L"Endpoint", L"Ponto final") \
    X(S_SETTINGS_FILE, L"Settings", L"Configurações") \
    X(S_SETTINGS_OK, L"saved", L"salvas") \
    X(S_SETTINGS_ERR, L"cannot write (error %lu)", L"não foi possível gravar (erro %lu)") \
    X(S_QUALITY, L"Quality check", L"Verificação de qualidade") \
    X(S_Q_SPATIAL_OFF, L"Windows Spatial Sound is off on this output.", L"O Som Espacial do Windows está desligado nesta saída.") \
    X(S_Q_SPATIAL_ON, L"%s is active on this output and virtualizes everything before E1 Studio (double processing). Turn it off in Sound settings for maximum quality.", L"%s está ativo nesta saída e virtualiza tudo antes do E1 Studio (processamento duplo). Desligue-o nas Configurações de som para máxima qualidade.") \
    X(S_Q_SPATIAL_GENERIC, L"Windows Spatial Sound", L"O Som Espacial do Windows") \
    X(S_Q_NO_DEVICE, L"Connect the device to run the checks.", L"Conecte o dispositivo para executar as verificações.") \
    X(S_Q_24_OK, L"Output runs at %s.", L"A saída opera em %s.") \
    X(S_Q_24_BAD, L"Output runs at %s. Select 24-bit, 44100 Hz in Sound settings > device > Output settings > Format.", L"A saída opera em %s. Selecione 24 bits, 44100 Hz em Configurações de som > dispositivo > Formato.") \
    X(S_Q_FMT_UNKNOWN, L"The output format could not be read.", L"Não foi possível ler o formato de saída.") \
    X(S_Q_ENGINE_OK, L"Effects engine is loaded on the output.", L"O mecanismo de efeitos está carregado na saída.") \
    X(S_Q_ENGINE_BAD, L"Effects engine is not loaded — effects will not apply. Install E1 Studio, then play some audio.", L"O mecanismo de efeitos não está carregado — os efeitos não serão aplicados. Instale o E1 Studio e reproduza algum áudio.") \
    X(S_OPEN_SOUND_SETTINGS, L"Sound settings", L"Configurações de som") \
    X(S_PROFILE_TITLE, L"Profiles", L"Perfis") \
    X(S_PROFILE_DESC, L"A profile stores the Studio effects, the equalizer and the voice settings.", L"Um perfil guarda os efeitos do Estúdio, o equalizador e as configurações de voz.") \
    X(S_PROFILE_SELECT, L"Select a profile…", L"Selecione um perfil…") \
    X(S_IMPORT, L"Import", L"Importar") \
    X(S_EXPORT, L"Export", L"Exportar") \
    X(S_PROFILE_SAVE_TITLE, L"Save profile", L"Salvar perfil") \
    X(S_PROFILE_BAD_FILE, L"The file is not a valid profile.", L"O arquivo não é um perfil válido.") \
    X(S_XML_FILTER, L"Profile (*.xml)", L"Perfil (*.xml)") \
    X(S_CURRENT_SETTINGS, L"Current settings", L"Configurações atuais") \
    X(S_ON, L"On", L"Ligado") \
    X(S_OFF, L"Off", L"Desligado") \
    X(S_FACTORY_1, L"Adventure and Action", L"Aventura e Ação") \
    X(S_FACTORY_2, L"Driving Simulation", L"Simulação de Direção") \
    X(S_FACTORY_3, L"First Person Shooter", L"Tiro em Primeira Pessoa") \
    X(S_FACTORY_4, L"Real Time Strategy", L"Estratégia em Tempo Real") \
    X(S_BTN_OK, L"OK", L"OK") \
    X(S_CANCEL, L"Cancel", L"Cancelar") \
    X(S_YES, L"Yes", L"Sim") \
    X(S_NO, L"No", L"Não") \
    X(S_RESTORE_TITLE, L"Restore defaults", L"Restaurar padrões") \
    X(S_RESTORE_CONFIRM, L"Restore every effect setting to its default? All effects will be switched off (bit-exact passthrough).", L"Restaurar todas as configurações de efeitos? Todos os efeitos serão desligados (passagem bit-exata).") \
    X(S_ABOUT_TITLE, L"About E1 Studio", L"Sobre o E1 Studio") \
    X(S_ABOUT_TEXT, L"Version %s\n\nControl panel for the E1 USB headphone amplifier (USB VID 041E, PID 323B): surround, clarity, bass, smart volume, dialog, 10-band equalizer and voice effects, with a bit-exact path when effects are off.\n\nIndependent software. Not affiliated with or endorsed by the device manufacturer.", L"Versão %s\n\nPainel de controle do amplificador de fones USB E1 (USB VID 041E, PID 323B): surround, clareza, graves, volume inteligente, diálogo, equalizador de 10 bandas e efeitos de voz, com caminho bit-exato quando os efeitos estão desligados.\n\nSoftware independente. Sem vínculo com o fabricante do dispositivo.") \
    X(S_EQP_FLAT, L"Flat", L"Plano") \
    X(S_EQP_ACOUSTIC, L"Acoustic", L"Acústico") \
    X(S_EQP_CLASSICAL, L"Classical", L"Clássico") \
    X(S_EQP_COUNTRY, L"Country", L"Country") \
    X(S_EQP_DANCE, L"Dance", L"Dance") \
    X(S_EQP_JAZZ, L"Jazz", L"Jazz") \
    X(S_EQP_NEWAGE, L"New Age", L"New Age") \
    X(S_EQP_POP, L"Pop", L"Pop") \
    X(S_EQP_ROCK, L"Rock", L"Rock") \
    X(S_EQP_VOCAL, L"Vocal", L"Vocal") \
    X(S_VFX_NEUTRAL, L"Neutral", L"Neutro") \
    X(S_VFX_F2M, L"Female to Male", L"Feminina para masculina") \
    X(S_VFX_M2F, L"Male to Female", L"Masculina para feminina") \
    X(S_VFX_KID, L"Scrappy Kid", L"Criança arteira") \
    X(S_VFX_ELDERLY, L"Elderly", L"Idoso") \
    X(S_VFX_ORC, L"Orc", L"Orc") \
    X(S_VFX_ELF, L"Elf", L"Elfo") \
    X(S_VFX_DWARF, L"Dwarf", L"Anão") \
    X(S_VFX_ALIEN_BRUTE, L"Alien Brute", L"Alienígena brutamontes") \
    X(S_VFX_ALIEN_INF, L"Alien Infiltrator", L"Alienígena infiltrado") \
    X(S_VFX_ROBOT, L"Robot", L"Robô") \
    X(S_VFX_MARINE, L"Marine", L"Fuzileiro") \
    X(S_VFX_EMO, L"Emo", L"Emo") \
    X(S_VFX_UNSTABLE, L"Unstable", L"Instável") \
    X(S_VFX_NORTH, L"From Up North", L"Lá do Norte") \
    X(S_VFX_DEEP, L"Deep Voice", L"Voz grave") \
    X(S_VFX_MUNCHKIN, L"Munchkin", L"Baixinho") \
    X(S_VFX_DEMON, L"Demon", L"Demônio")

typedef enum StrId {
#define X_ENUM(id, en, pt) id,
    STRING_TABLE(X_ENUM)
#undef X_ENUM
    S__COUNT
} StrId;

enum { LANG_EN_ = 0, LANG_PT_ = 1 };

void           str_set_lang(int lang);
int            str_lang(void);
int            str_detect_lang(void);           /* from GetUserDefaultUILanguage */
const wchar_t *tr(StrId id);
/* Translates a name coming from a DSP table (EQ / VoiceFX presets) via its English text. */
const wchar_t *tr_preset_name(const char *english);
/* Factory profile i (0..3) display name. */
const wchar_t *tr_factory(int i);
/* In place: a '.' between two digits becomes the decimal separator of the UI language (',' in pt-BR). */
void           str_decimal(wchar_t *s);

#endif /* E1P_STRINGS_H */
