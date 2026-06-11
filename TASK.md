# Roadmap per un'applicazione di clipping/recording leggera per Windows 11

## Introduzione e obiettivi

L'obiettivo è creare un'app per Windows 11 che consenta di registrare lo schermo e salvare **clip** degli ultimi secondi di gioco o di qualsiasi altra applicazione. A differenza di soluzioni come **Insights.gg** o **Medal**, che utilizzano tecnologie webview/electron pesanti, l'app deve essere leggera, **scritta in linguaggi nativi (C/C++ o Rust)** e con un'architettura modulare che permetta di gestire **hotkey globali**, buffering in memoria e codifica efficiente.

### Caratteristiche principali

- **Replay buffer configurabile**: memorizza in un buffer circolare gli ultimi _X_ secondi di video/audio e salva il file quando viene premuto uno shortcut. OBS usa `GraphicsCaptureSession` per acquisire fotogrammi da uno schermo/finestra【889008118973582†L318-L327】 e un buffer circolare simile dovrà essere implementato.
- **Registrazione continua**: start/stop recording con opzione di pause/resume via hotkey.
- **Impostazioni avanzate**: bitrate, risoluzione, codec video (H.264/H.265), hardware encoder NVENC/AMF/Intel QSV, numero di canali audio, destinazione file e formati (mp4/mkv), ecc.
- **Audio multi‑canale**: acquisizione dell’audio tramite **Windows Audio Session API (WASAPI)**, che supporta enumerazione dei dispositivi, cattura PCM e bassa latenza【544553286408125†L191-L200】.
- **Hotkey globali**: registrate tramite la funzione Win32 `RegisterHotKey` che permette di definire hotkey di sistema e ricevere eventi `WM_HOTKEY`【136198929834163†L67-L151】; devono essere configurabili dall’utente.
- **Esecuzione in background**: l’app deve risiedere nell’area di notifica (System Tray) usando `Shell_NotifyIcon` per aggiungere/modificare l’icona, con menu contestuale per avviare/fermare registrazioni e accedere alle impostazioni.
- **Plugin per Stream Deck**: fornisce azioni dedicate per clip/start/stop recording tramite il **Stream Deck SDK** di Elgato. Il plugin viene scritto in Node.js/TypeScript e comunica con l’app principale via IPC o CLI.

## Stack tecnologico

### Linguaggi e runtime

- **C++ o Rust** per il nucleo nativo; C++ è supportato da molti esempi Microsoft e permette l’integrazione diretta con Win32/COM.
- **C/C++ con CMake** come sistema di build. Utilizzare **vcpkg** o **conan** per gestire librerie esterne (FFmpeg, spdlog, etc.).
- **Node.js/TypeScript** per il plugin Stream Deck; il SDK ufficiale richiede Node 24 o superiore e il pacchetto `@elgato/cli` per generare scaffolding【229780622368871†L78-L149】.

### API di acquisizione video

Le moderne API di Windows per la cattura sono:

- **Windows.Graphics.Capture (WinRT)** – consente di acquisire fotogrammi da uno schermo o finestra tramite un’interfaccia sicura. Il metodo `GraphicsCaptureSession.IsSupported()` verifica se la cattura è disponibile【889008118973582†L318-L341】; `GraphicsCapturePicker` presenta all’utente la UI per scegliere la finestra【889008118973582†L361-L380】. I frame vengono poi acquisiti tramite `Direct3D11CaptureFramePool` e processati per la codifica【889008118973582†L385-L414】.
- **Desktop Duplication API (DXGI)** – alternativa Win32 per la cattura ad alte prestazioni (usata da OBS). Richiede l’inizializzazione di un dispositivo Direct3D 11 e la gestione di `IDXGIOutputDuplication` per recuperare i frame.

### API di acquisizione audio

- **WASAPI** (Windows Audio Session API) per catturare audio multicanale. Il sample ufficiale dimostra enumerazione dei dispositivi, cattura PCM e bassa latenza【544553286408125†L191-L200】. Vanno creati `IAudioClient` e `IAudioCaptureClient` per leggere i buffer. Si dovrà implementare la possibilità di selezionare il dispositivo di input/output e configurare la frequenza di campionamento.

### Codifica e compressione

- **FFmpeg/libav** – libreria di riferimento per la codifica di video/audio; supporta vari codec e hardware encoder (NVENC, AMF, QSV). Integrare ffmpeg come libreria dinamica (link statico o dinamico) e fornire opzioni avanzate di bitrate (CBR, VBR), qualità e profili.
- Possibile alternativa: **Media Foundation** con `SinkWriter` per utilizzare encoder hardware senza dipendenze esterne; tuttavia la flessibilità di FFmpeg giustifica l’integrazione.

### UI, hotkey e system tray

- **Win32 API** per creare la finestra principale (eventualmente nascosta) e gestire il ciclo dei messaggi.
- **Hotkey**: registrare combinazioni di tasti con `RegisterHotKey(HWND hWnd,int id,UINT fsModifiers,UINT vk)`【136198929834163†L67-L121】. Le costanti `MOD_ALT`, `MOD_CONTROL`, `MOD_SHIFT`, `MOD_WIN` definiscono i modificatori; le chiavi di funzione come F12 sono riservate【136198929834163†L119-L160】.
- **System Tray**: usare `Shell_NotifyIcon` con struttura `NOTIFYICONDATA` per aggiungere/modificare l’icona e il menu contestuale nell’area di notifica.
- **WinUI 3** o **Qt (widgets)** come front‑end per le impostazioni; entrambi supportano l’aggiunta di controlli senza pesanti runtime. WinUI 3 richiede Windows App SDK.

### Plugin Stream Deck

- Utilizzare il **Stream Deck SDK** (versione 2.x). Il wizard `streamdeck create` del CLI genera una struttura con `*.sdPlugin`, `src` e `manifest.json`【229780622368871†L126-L204】.
- Il plugin è un’app Node.js che comunica con il software Elgato tramite WebSocket e definisce azioni personalizzate nel `manifest.json`. Verranno create tre azioni: _Clip Replay Buffer_, _Start/Stop Recording_ e _Pause/Resume Recording_. Ciascuna azione invierà un comando all’app principale (via socket locale, named pipe o file di lock).
- Il plugin può essere distribuito sul marketplace o localmente copiando la cartella `.sdPlugin` nella directory di plugin di Stream Deck.

## Architettura proposta

L’app sarà suddivisa in moduli autonomi per facilitare sviluppo e manutenzione:

1. **Core di acquisizione video** – inizializza il dispositivo Direct3D 11, avvia la sessione di cattura con Windows.Graphics.Capture o Desktop Duplication, gestisce buffer dei frame e notifica quando nuovi frame sono disponibili.
2. **Core di acquisizione audio** – istanzia WASAPI (o alternativi come ASIO per hardware specifici) per registrare audio multicanale; fornisce buffer in sincronia con il video.
3. **Replay buffer** – implementa un buffer circolare in memoria che memorizza i pacchetti compressi (o i frame grezzi con timestamp) degli ultimi _N_ secondi. Quando si preme la hotkey di clip, il modulo estrae i dati dal buffer e li scrive su disco.
4. **Recording manager** – avvia e ferma registrazioni continue, gestisce pause/resume, crea file nella cartella di destinazione con nomi univoci. Condivide codice con il replay buffer per evitare duplicazione.
5. **Encoder** – wrapper su FFmpeg; configura flusso di codifica video/audio in base alle impostazioni (bitrate, preset, profilo, frame rate). Permette di scegliere encoder hardware (NVENC/AMF/QSV) se disponibile.
6. **Configurazione e UI** – gestisce un file di configurazione (JSON/YAML) e un’interfaccia grafica per modificarlo. Presenta campi per cartella output, durata replay buffer, hotkey (combinazione di tasti), parametri di codifica e dispositivi audio/video.
7. **Event dispatcher** – riceve messaggi `WM_HOTKEY` e menu del system tray, mappa le azioni agli handler (salva clip, inizia/termina registrazione, apre impostazioni, esci).
8. **Comunicazione plugin** – server IPC che riceve comandi dal plugin Stream Deck (via WebSocket locale, NamedPipe o gRPC). Implementa un protocollo semplice (JSON) per esporre `clip()`, `startRecording()`, `stopRecording()`, `pauseResumeRecording()`.
9. **Plugin Stream Deck** – implementa azioni che, alla pressione dei tasti sul dispositivo, inviano messaggi al server IPC dell’app. Implementa property inspector per configurare durata clip e altre opzioni a livello di azione.

## Piano di sviluppo

### Fase 1 – Ricerca e configurazione

1. **Studiare le API di cattura**: preparare prototipi con `GraphicsCaptureSession` verificando `IsSupported()`【889008118973582†L318-L341】 e testare la latenza nella cattura di frame e gestione del frame pool.
2. **Studiare WASAPI**: analizzare il sample Microsoft per imparare l’enumerazione di dispositivi e la cattura PCM【544553286408125†L191-L200】. Verificare come trattare più canali e come eseguire il loopback per l’audio di sistema.
3. **Valutare il Desktop Duplication API**: testare prestazioni su varie GPU. Confrontare con Windows.Graphics.Capture.
4. **Selezionare linguaggio**: analizzare pro e contro di C++ rispetto a Rust (ecosistema, FFI con FFmpeg, mancanza di GC, sicurezza). Scegliere C++ per compatibilità e perché OBS utilizza C/C++.
5. **Preparare l’ambiente di build**: configurare CMake, vcpkg/conan per FFmpeg, spdlog e eventuali GUI library (Qt/WinUI). Configurare cross-compilazione a 64 bit.

### Fase 2 – Progetto base e moduli di cattura

1. **Creare repository e struttura**: sorgenti in `src/`, librerie in `external/`, build in `build/`. Includere script CMake e definire opzioni di configurazione.
2. **Modulo video**: implementare un wrapper su Windows.Graphics.Capture. Esporre funzioni per avviare/fermare cattura e restituire `ID3D11Texture2D` o buffer. Considerare fallback su Desktop Duplication.
3. **Modulo audio**: implementare wrapper su WASAPI per cattura multicanale. Consentire la selezione del dispositivo di input/output.
4. **Buffer circolare**: implementare struttura dati che mantiene una coda dei pacchetti video/audio compressi per la durata configurata. Per ogni pacchetto memorizzare timestamp e pointer al blocco di dati. Gestire la sincronizzazione tra thread produttore (cattura) e consumatore (salvataggio clip).
5. **Encoder FFmpeg**: inizializzare contesto AVFormat/AVCodec, creare flussi, impostare parametri (bitrate, fps, gop, preset). Implementare funzioni `encodeFrame()` che accettano texture o buffer PCM e restituiscono pacchetti compressi.

### Fase 3 – Registrazione e replay buffer

1. **Replay buffer**: integrare cattura ed encoder. In modalità replay, comprimere e memorizzare i pacchetti nel buffer. Alla ricezione della hotkey di clip, leggere i pacchetti dalle code e scriverli in un file di output (mp4/mkv) con indice aggiornato.
2. **Registrazione continua**: avviare un file writer all’inizio della registrazione e scrivere pacchetti in tempo reale. Implementare pausa che sospende la scrittura senza rimuovere i pacchetti; l’utilizzatore deve poter riprendere. Supportare file segmentati (auto‐split) se la dimensione supera un limite.
3. **Gestione errori**: monitorare saturazione del buffer e segnalare all’utente se il replay buffer è pieno o la scrittura fallisce.

### Fase 4 – Interfaccia utente e configurazioni

1. **System tray**: creare finestra invisibile e aggiungere icona con `Shell_NotifyIcon`. Implementare menu contestuale con voci _Clip_, _Start Recording_, _Stop_, _Pause/Resume_, _Impostazioni_, _Esci_.
2. **Finestra impostazioni**: usare WinUI 3 (o Qt) per costruire un dialogo con campi per:
   - Durata replay buffer (in secondi/minuti).
   - Cartella di salvataggio delle clip/registrazioni.
   - Hotkey: definire combinazione di tasti; registrarla con `RegisterHotKey`【136198929834163†L67-L121】. Prevedere la possibilità di rilevare conflitti e avvisare l’utente.
   - Codec video/audio, bitrate, risoluzione, framerate.
   - Selezione dispositivo audio/video.
3. **Persistenza**: salvare le impostazioni in un file JSON o nel registro. Caricarle all’avvio.

### Fase 5 – Integrazione plugin Stream Deck

1. **Installare SDK**: installare Node.js ≥ 24 e `@elgato/cli`【229780622368871†L78-L149】. Creare il plugin con `streamdeck create` specificando un `UUID` univoco (es. `com.company.clipapp`).
2. **Definire manifest**: aggiungere tre azioni (Clip, StartStopRecording, PauseResume). Ogni azione include nome, descrizione, icona e un `uuid` interno. Configurare la sezione `PropertyInspector` per parametri specifici (es. durata clip predefinita).
3. **Implementare codice del plugin**: in `plugin.ts`, aprire un WebSocket o TCP verso il server locale dell’app. Alla ricezione di `willAppear` o `keyUp` inviare il comando appropriato (`clip`, `start`, `stop`, `pause/resume`). Gestire errori e mostrare feedback luminoso sul tasto.
4. **Distribuzione**: compilare il plugin (`npm run build`) e firmare il pacchetto se necessario. Fornire istruzioni per l’installazione manuale o la pubblicazione sul marketplace.

### Fase 6 – Testing e ottimizzazione

1. **Profiling**: misurare utilizzo CPU/GPU e latenza del replay buffer. Ottimizzare pipeline riducendo copie di memoria e sfruttando surfaces GPU.
2. **Gestione memoria**: assicurarsi che il buffer circolare utilizzi al massimo 75 % della RAM fisica (OBS impone tale limite per il replay buffer). Implementare parametri di dimensione in base alla RAM disponibile.
3. **Test hotkey**: verificare che le hotkey si registrino correttamente e non confliggano con altre app. Ricordare che F12 è riservato e non deve essere usato【136198929834163†L119-L160】.
4. **Test plugin**: verificare la comunicazione dal plugin Stream Deck all’app; simulare latenza e disconnessioni; fornire messaggi d’errore chiari.

### Fase 7 – Distribuzione e manutenzione

1. **Packaging**: generare un installer MSI o ZIP. Includere tutti i binari necessari (ffmpeg dlls, librerie) e un file README.
2. **Aggiornamenti automatici**: considerare l’integrazione con un servizio di update (es. Squirrel.Windows) per distribuire nuove versioni.
3. **Documentazione**: creare manuale utente e guide per la configurazione, includendo screenshot. Documentare l’API interna per il plugin.
4. **Supporto community**: predisporre repository GitHub con bug tracker e wiki. Spiegare come compilare l’app e contribuire.

## Riferimenti utili

- Microsoft documentation on `Windows.Graphics.Capture` – Descrive la procedura per controllare se la cattura dello schermo è supportata (`IsSupported()`), lanciare la UI di selezione (`GraphicsCapturePicker`) e creare un pool di frame con `Direct3D11CaptureFramePool`【889008118973582†L318-L414】.
- Windows Audio Session API sample – Spiega come enumerare i dispositivi e catturare audio PCM a bassa latenza【544553286408125†L191-L200】.
- Win32 `RegisterHotKey` – Specifica la funzione per registrare hotkey globali; definisce i parametri `fsModifiers` (MOD_ALT, MOD_CONTROL, etc.) e la necessità di evitare tasti riservati come F12【136198929834163†L67-L160】.
- Stream Deck SDK Getting Started – Introduce i prerequisiti (Node.js 24+, Stream Deck 7.1+, device), l’uso del CLI `streamdeck create` e la struttura del plugin `.sdPlugin`【229780622368871†L78-L204】.

Questa roadmap fornisce una guida dettagliata per lo sviluppo dell’app, ponendo enfasi sulla leggerezza, sulle API native di Windows e su una modularità che consente di espandere funzioni e integrare facilmente un plugin Stream Deck.
---

## Reconciled decisions (v1)

The following foundational choices have been locked after planning. See `docs/DECISIONS.md`
for the full ADR records.

- **Repo layout**: `/app` + `/libs` + `/plugins` (see §7 of PROJECT_PLAN.md)
- **Config schema**: snake_case rich JSON (see §8 of PROJECT_PLAN.md)
- **IPC transport**: localhost WebSocket/TCP JSON-RPC for v1; named pipes deferred (ADR-0007)
- **Process model**: single-process MVP with isolated /libs modules; two-process deferred (ADR-0008)
- **Command vocab**: `save_replay`, `recording_start`, `recording_stop`, `pause_resume`
