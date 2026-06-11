# Ricerca approfondita e bozza di TASK.md per un recorder leggero su Windows 11

## Sintesi operativa

Se l’obiettivo è costruire un clipper/recorder “alla Medal o Insights” ma con un footprint molto più basso, io non partirei da Electron, Tauri o da una UI basata su WebView. RePlays, per esempio, dichiara apertamente uno stack C# + TypeScript/React con interfaccia resa tramite Microsoft Edge WebView2, mentre Tauri definisce la propria architettura come una toolkit per applicazioni desktop basate su WebView. Anche Medal non pubblica una documentazione tecnica completa della shell desktop, ma il suo supporto ufficiale parla di Developer Tools e di log/installazione Squirrel, elementi che suggeriscono una shell Chromium-like o comunque web-tech-heavy; in pratica, il tipo di architettura che vuoi evitare se la priorità è ridurre RAM, processi secondari e overhead di runtime. citeturn17view3turn22search1turn22search10turn22search13turn6search6turn6search13

La strada più solida per il tuo caso, quindi, è una **architettura nativa Windows-first**, con **core di capture/encode in C++** e UI separata ma leggera. OBS è il riferimento più credibile qui: il suo backend `libobs` separa sorgenti, output, encoder e servizi, e il progetto mantiene moduli dedicati per Windows capture, WASAPI, FFmpeg, NVENC, Quick Sync e altri componenti di recording. Questo modello è la prova pratica che una pipeline nativa, modulare e ad alte prestazioni è realistica e già validata in produzione. Per la UI, le opzioni più credibili senza browser stack sono **Qt Widgets in C++**, **WinUI 3 come shell Windows-only**, oppure — se il team è già molto forte in Rust — **Rust + windows-rs + Slint** come alternativa moderna ma meno “battle-tested” nel dominio capture/encoder rispetto a C++. citeturn2search2turn3view0turn4view6turn5view0turn5view1turn5view2turn5view3turn5view4turn8search3turn8search6turn26search1turn22search0turn22search3

Se però vuoi comprimere i tempi di time-to-market, esiste anche una seconda strada: **costruire una shell molto leggera sopra `libobs`**, invece di riscrivere tutto. È una via reale: progetti come RePlays e Segra mostrano che un clipper costruito “sopra OBS/libobs” è tecnicamente fattibile. Il problema è che il repository OBS è distribuito sotto GPL-2.0 e quindi il riuso diretto va trattato come scelta sensibile dal punto di vista licenza; inoltre, diversi progetti che riusano libobs finiscono per affiancargli stack più pesanti lato UI, vanificando in parte il vantaggio di un recorder super-leggero. Per un prodotto che metta al primo posto leggerezza, prevedibilità e basso overhead, la mia raccomandazione resta quindi: **custom native engine first**, con una piccola fase iniziale di benchmark contro una baseline `libobs` per evitare decisioni ideologiche. citeturn3view0turn17view3turn17view4turn2search2

## Cosa insegnano OBS, Medal, Insights e i repo open source

Dal lato architetturale, OBS insegna tre cose importanti. La prima è la modularità: `libobs` definisce plugin object separati per **sources**, **outputs**, **encoders** e **services**, e il frontend può caricare moduli, gestire display nativi e controllare record/stream in modo molto pulito. La seconda è il design delle impostazioni: ogni oggetto può esporre proprietà che il frontend traduce in widget, cioè un pattern molto utile se vuoi offrire impostazioni “in stile OBS” senza accoppiare troppo UI e pipeline. La terza è la separazione durissima tra core e frontend, che è esattamente quello che serve a un recorder/clipping app pensato per rimanere leggero mentre sta registrando in background. citeturn2search2turn2search4turn2search7turn2search8turn2search1

Guardando il repository OBS, si vede anche qualcosa di molto utile per il tuo design: il progetto non è “monolitico”. Dentro `plugins/` esistono moduli distinti come `win-capture`, `win-wasapi`, `obs-ffmpeg`, `obs-nvenc`, `obs-qsv11`, `obs-libfdk`, `obs-x264` e perfino `obs-browser`; mentre nel root ci sono directory separate come `libobs`, `libobs-d3d11`, `libobs-winrt` e `frontend`. In altre parole, perfino OBS tratta browser integration come **plugin opzionale**, non come fondazione del prodotto. Questo è un segnale forte: se il tuo obiettivo è un software di clipping leggero, tutte le parti “social”, “editor web”, “cloud feed”, “embedded browser” dovrebbero vivere fuori dal core recorder, non dentro. citeturn3view0turn4view6turn4view7turn5view0

Dal lato prodotto, Medal e Insights mostrano chiaramente cosa gli utenti si aspettano nel 2026. Medal espone impostazioni di **resolution, FPS, bitrate, encoder CPU/GPU, codec, GPU selector**, supporta clip buffering, full-session recording, storage cap e multi-track recording; la sua documentazione parla anche di clip mode per gli ultimi secondi/minuti, recording buffer in RAM o su disco e fino a 4K/144 FPS in alcuni preset. Insights, dal canto suo, si posiziona come game recorder orientato a **lower FPS impact**, supporto a molti giochi, automatic highlights, separazione audio e hardware-accelerated encoding con **NVENC, AMD AMF e Intel encoders**. Queste feature sono utili non perché devi copiarle tutte, ma perché definiscono il **minimum expected feature set** per competere nel segmento. citeturn7search0turn7search1turn7search2turn7search6turn7search9turn7search15turn7search17turn6search1turn7search11turn7search14turn7search19

Un dettaglio particolarmente importante è il design del buffer di clipping. Medal documenta esplicitamente i pro e i contro di buffer in **memory** versus **disk**, e osserva che le impostazioni più spinte possono richiedere parecchia memoria libera; inoltre, supporta storage cap e cancellazione automatica delle clip più vecchie. Questo è esattamente il tipo di controllo che deve entrare nella tua app, ma implementato in modo più prevedibile: il buffer non è solo una feature utente, è una **decisione di architettura** che influenza memoria resident, burst I/O, latenza del salvataggio e rischio di stutter quando l’utente preme la hotkey del clip. citeturn7search17turn7search9turn7search1

I repository open source recenti aiutano anche a leggere i compromessi. RePlays usa `libobs` per il recording, ma combina backend C# con frontend TypeScript/React e WebView2. Segra si presenta come recorder built on OBS, quindi conferma che prendere OBS come “recording backbone” è una strada reale. Capturinha, invece, è interessante perché va nella direzione opposta: C++ quasi puro, NVENC, focus esplicito su performance, frame-rate stability e configurabilità. Questi esempi suggeriscono che la vera biforcazione non è “OBS sì / OBS no”, ma **native engine vs wrapper app pesante**. citeturn17view3turn17view4turn17view0

## Stack e architettura consigliati

Per la **cattura video**, la soluzione più pragmatica è una pipeline ibrida. La **primary path** dovrebbe essere `Windows.Graphics.Capture` per cattura di finestre e display, perché è l’API moderna di Windows per acquisire frame da una finestra o da un display, inclusi scenari di screen capture e video recording. Come **fallback / alternate method** per il full-desktop, conviene supportare anche la **Desktop Duplication API**, che fornisce l’immagine del desktop tramite `IDXGIOutputDuplication`; Microsoft documenta anche che se vuoi duplicare l’intero desktop multi-monitor devi creare un duplicator per ogni output. Questo doppio binario è importante perché `Windows.Graphics.Capture` è comoda e moderna, ma Microsoft nota che l’exclusive fullscreen può non essere garantito con WGC, mentre il tree di OBS suggerisce che il “vero game capture” è un sottosistema separato, con `graphics-hook`, `inject-helper` e `game-capture.c`, quindi più complesso e decisamente da pianificare come fase successiva. citeturn1search2turn10search3turn1search1turn1search10turn1search13turn12view0turn12view1turn12view2

Per l’**audio**, la base corretta è **WASAPI loopback** per il system mix, più capture separata di microfono, con resampling interno e mux su tracce diverse. Microsoft documenta chiaramente che il loopback mode consente di catturare il flusso riprodotto da un rendering endpoint; inoltre, il sample ufficiale di **Application Loopback Audio Capture** mostra come catturare l’audio di un **process tree specifico**, oppure tutto il system audio **escludendo** un process tree. Questo è strategico per una roadmap moderna: in v1 puoi avere game/system/mic su tracce separate; in v1.5 o v2 puoi aggiungere include/exclude di Discord, browser, Spotify o di processi rumore, senza dover cambiare l’intera architettura audio. citeturn8search0turn14search1turn14search0turn14search4turn14search9

Per l’**encoding**, io esporrei un’astrazione con backend **NVENC**, **AMF** e **Intel oneVPL / QSV**, più fallback CPU via encoder software. NVIDIA documenta le NVENCODE APIs e le capacità NVENC, Intel presenta oneVPL come set unico di API per encoding/decoding/video processing sui GPU Intel, e AMD definisce AMF come framework leggero per multimedia processing; inoltre, l’AMF repo e wiki mostrano chiaramente l’integrazione con FFmpeg. Una nota importante sul tuo testo: assumo che “AFM” fosse un refuso per **AMF**; AMF è un framework **video** accelerato su GPU AMD, non un audio encoder. Per l’audio, le opzioni realistiche da esporre sono **AAC nativo di FFmpeg** come default, più eventualmente `libfdk_aac` dove le policy di distribuzione lo consentono. FFmpeg però avverte che `libfdk_aac` richiede `--enable-nonfree` ed è incompatibile con build GPL standard, quindi andrebbe trattato come componente opzionale e sensibile dal punto di vista licensing/distribution. citeturn13search14turn23view1turn13search5turn13search21turn13search0turn24search1turn24search7turn8search5turn8search17

Sui **container**, io non registrerei in MP4 “classico” come default. OBS ha introdotto Hybrid MP4 / Hybrid MOV proprio per mitigare la fragilità del MP4 tradizionale, e la conoscenza storica del progetto resta la stessa: se la registrazione viene interrotta, MP4/MOV possono corrompersi; MKV/FLV sono più sicuri e il remux finale è la strada standard. FFmpeg, inoltre, documenta chiaramente la gestione di stream multipli e del `-map`, quindi una pipeline a più tracce audio è perfettamente sensata dal lato muxing. Per la tua app io farei così: **default = MKV**, opzione **auto-remux a MP4**, e come modalità sperimentale opzionale **Hybrid MP4** o equivalente solo dopo test seri. citeturn21search0turn21search5turn21search9turn21search13turn21search2turn21search4turn21search6turn21search16

Per la **UI**, la scelta dipende più dal team che dalla pura performance. Se vuoi il percorso meno rischioso per un’app impostazioni-pesanti, io sceglierei **C++ + Qt Widgets**: Qt Widgets è un toolkit desktop classico, maturo e non browser-based, e OBS stesso usa Qt nel frontend. Se invece vuoi una shell più “Windows 11-native” e il team è più veloce in C#, allora una **WinUI 3 shell** che chiama un **engine nativo C++** può funzionare molto bene; in quel caso, Native AOT resta interessante per ridurre startup time e footprint del livello managed. Come terza opzione, **Rust + windows-rs + Slint** è credibile, perché `windows`/`windows-sys` permettono accesso all’API Windows e Slint offre UI native in Rust e C++, ma la consiglierei solo a un team già forte in Rust. Tauri, invece, lo escluderei proprio per coerenza con il requisito “no webviews”. citeturn8search3turn27search1turn27search3turn22search5turn25search1turn25search2turn26search1turn22search0turn22search3turn22search1turn22search10

Per la **distribuzione**, non legherei il progetto al Microsoft Store come primo passo. La documentazione Microsoft conferma che app desktop possono usare Windows App SDK anche in modalità unpackaged, e che MSIX offre install/uninstall puliti, package identity e automatic updates. Per questo farei: **installer EXE/MSI o packaged-with-external-location come canale primario**, **winget come canale secondario**, e **MSIX/Store** come opzione successiva se ti serviranno package identity, notifiche o un canale di distribuzione più “Windows-native”. citeturn20search2turn20search3turn20search11turn20search1

## Bozza di TASK.md

La bozza sotto assume la direzione che ritengo più sensata dopo la ricerca: **engine nativo Windows-first**, **no WebView/Electron**, **pipeline WGC + Desktop Duplication + WASAPI + FFmpeg/hardware encoders**, **container crash-safe**, **UI separata e leggera**, con una piccola fase iniziale per confrontare oggettivamente questa via con una baseline `libobs` prima di congelare l’architettura. Questa impostazione è coerente con il modello OBS, con le API Windows moderne di capture/audio e con le feature set che Medal e Insights espongono lato utente. citeturn2search2turn4view6turn5view0turn8search0turn14search0turn21search0turn7search6turn6search1

```md
# TASK.md

## Visione prodotto

Creare un'app nativa per Windows 11 focalizzata su clipping e recording leggero, con UX semplice in stile Medal / Insights ma con impostazioni avanzate in stile OBS.

Obiettivi chiave:
- Registrazione continua in background con salvataggio clip su hotkey.
- Recording manuale full-session.
- Impostazioni dettagliate ma comprensibili.
- Impatto minimo su FPS, input latency, CPU e RAM.
- Architettura senza Electron, senza WebView, senza browser embedded nel core dell'app.
- Motore di recording separato dalla UI.

Metriche prodotto iniziali:
- Startup UI a freddo percepito < 1 secondo su macchina mid-range.
- Idle RAM della UI/tray molto contenuta.
- Recorder stabile per sessioni lunghe.
- Salvataggio clip rapido e prevedibile.
- Nessuna corruzione dei file in scenari normali di utilizzo.
- Nessun account obbligatorio per usare il recorder.

## Principi non negoziabili

- No Electron.
- No WebView/WebView2/Tauri nel core prodotto.
- Nessun browser plugin o feed social nella build v1.
- Architettura local-first.
- Privacy by default: niente telemetry invasiva, solo opt-in.
- Settings avanzate esposte in modo chiaro e persistente.
- Ogni feature va misurata con benchmark reali, non accettata “a sensazione”.
- Design orientato a resilienza: se la UI va in crash, il recorder non deve perdere la sessione.

## Scope v1

Include:
- Clip buffer con hotkey.
- Recording manuale.
- Window capture.
- Display capture.
- Audio system loopback + microphone.
- Multi-track audio base.
- Encoder video hardware + CPU fallback.
- Encoder audio configurabile.
- Bitrate, FPS, resolution, preset, keyframe, codec, directory, naming pattern.
- Cartella libreria locale clip/recording.
- Storage cap e auto-cleanup.
- Per-game / per-app profiles base.
- Tray app + hotkeys globali.
- Logging, crash dumps, benchmark mode.

Non include in v1:
- Streaming live.
- Scene compositing in stile OBS completo.
- Browser sources.
- Overlay social.
- Cloud upload obbligatorio.
- Built-in video editor avanzato.
- AI auto-highlights basati su CV/LLM.
- Hooking game capture aggressivo per anti-cheat-sensitive titles.

## Gate decisionale iniziale

Prima di congelare l’architettura, completare 2 spike paralleli:

### Spike A
Build minimal custom engine con:
- Window/display capture.
- System audio + mic.
- Encode hardware H.264.
- Clip ring buffer.
- Scrittura MKV.

### Spike B
Build minimal wrapper su libobs con:
- Nessuna scena complessa.
- Nessun browser source.
- UI ridotta al minimo.
- Clip + recording only.

Decisione finale:
- Se custom engine vince su footprint, controllo, licenza e prevedibilità -> procedere con custom engine.
- Se libobs riduce drasticamente rischio/time-to-market senza sforare i budget di peso -> considerare variante libobs.
- Documentare la scelta in un ADR firmato.

## Architettura target

Processi:
- RecorderService.exe
- TrayUI.exe
- CrashHandler.exe

Responsabilità:
- RecorderService:
  - pipeline capture/audio/encode/mux
  - hot buffer clipping
  - gestione profili
  - scrittura file
  - logging tecnico
- TrayUI:
  - settings
  - stato recorder
  - libreria locale
  - hotkey bindings
  - first-run wizard
- CrashHandler:
  - minidump
  - log packaging
  - restart flow controllato

Moduli interni del recorder:
- CaptureManager
- AudioCaptureManager
- BufferManager
- EncoderManager
- MuxerManager
- SettingsManager
- ProfileManager
- StorageManager
- HotkeyManager
- DiagnosticsManager

## Scelte tecniche raccomandate

Engine:
- C++20
- CMake
- Direct3D 11
- Windows.Graphics.Capture per primary window/display capture
- Desktop Duplication come fallback / alternate display capture path
- WASAPI loopback + mic capture
- FFmpeg libs per muxing / codec abstraction / remux
- Backend encoder:
  - NVENC
  - AMF
  - Intel oneVPL / QSV
  - x264 CPU fallback
- Container default:
  - MKV
- Opzione:
  - auto-remux a MP4 dopo finalize

UI:
- Opzione preferita: C++ + Qt Widgets
- Opzione secondaria: C# WinUI 3 shell con engine C++ nativo
- Vietato introdurre browser UI nel percorso critico

## Backlog di lavoro

### Fase discovery e benchmark

- [ ] Definire 3 macchine target per benchmark:
  - low-end laptop
  - mid-range gaming PC
  - high-end gaming PC
- [ ] Misurare startup time, idle RAM, process count e CPU idle di:
  - OBS
  - Medal
  - Insights Capture
  - Spike custom
  - Spike libobs
- [ ] Creare benchmark script ripetibili per:
  - idle
  - background clip buffer
  - full recording 1080p60
  - recording 1440p60
  - clip save burst
- [ ] Scrivere ADR iniziale:
  - custom engine vs libobs
  - Qt vs WinUI 3
  - single process vs service + tray split
- [ ] Fissare i budget prestazionali ufficiali del progetto.

### Fase core recorder

- [ ] Creare structure solution / repo.
- [ ] Implementare logging strutturato con livelli:
  - trace
  - info
  - warn
  - error
- [ ] Implementare settings schema versionato.
- [ ] Implementare migration system per settings futuri.
- [ ] Implementare crash dump generation.
- [ ] Implementare health status del recorder.
- [ ] Implementare IPC tra TrayUI e RecorderService.
- [ ] Implementare watchdog leggero.
- [ ] Implementare safe shutdown e restart.

### Fase capture video

- [ ] Implementare Window Capture primary path.
- [ ] Implementare Display Capture primary path.
- [ ] Implementare capture method selection:
  - Auto
  - WGC
  - Desktop Duplication
- [ ] Implementare cursor capture on/off.
- [ ] Gestire DPI awareness e coordinate corrette.
- [ ] Gestire resize dinamico della finestra catturata.
- [ ] Gestire minimizzazione / alt-tab / device-lost.
- [ ] Gestire multi-monitor.
- [ ] Gestire color pipeline SDR in modo stabile.
- [ ] Aggiungere groundwork per HDR, ma solo se non destabilizza v1.
- [ ] Aprire epic separata per “real game capture hook” da post-v1.

### Fase audio

- [ ] Implementare system audio loopback.
- [ ] Implementare microphone capture separata.
- [ ] Implementare mettere su tracce distinte:
  - Track 1 mixdown
  - Track 2 game/system
  - Track 3 mic
- [ ] Implementare gain per source.
- [ ] Implementare mute / push-to-talk roadmap hook.
- [ ] Implementare sample-rate normalization.
- [ ] Implementare resampler stabile.
- [ ] Investigare supporto process-loopback include/exclude per:
  - Discord
  - browser
  - music app
- [ ] Implementare meters e test audio devices nel pannello settings.

### Fase encoding e muxing

- [ ] Definire encoder abstraction comune.
- [ ] Implementare discovery encoder capabilities runtime.
- [ ] Implementare backend video:
  - NVENC H.264
  - NVENC HEVC
  - AV1 dove disponibile
  - AMF H.264 / HEVC / AV1 se disponibile
  - Intel QSV / oneVPL
  - x264 fallback
- [ ] Implementare backend audio:
  - AAC default
  - eventuali codec opzionali solo se licensing OK
- [ ] Implementare parametri esposti:
  - resolution
  - FPS
  - bitrate
  - CBR / VBR / CQ / CQP dove supportato
  - preset
  - profile
  - keyframe interval
  - B-frames se supportate
- [ ] Implementare “Advanced FFmpeg Options” validato:
  - input sanitization
  - allowlist / advanced mode
  - error surfacing leggibile
- [ ] Implementare mux MKV robusto.
- [ ] Implementare auto-remux a MP4.
- [ ] Gestire finalize lento, stop timeout, force finalize e recovery flow.
- [ ] Salvare metadata recording:
  - codec
  - fps
  - bitrate
  - duration
  - tracks
  - capture source
  - profile usato

### Fase clipping buffer e storage

- [ ] Implementare ring buffer in RAM.
- [ ] Implementare ring buffer su disco.
- [ ] Implementare modalità:
  - clip-only
  - clip + manual recording
  - record-only
- [ ] Implementare save last:
  - 15s
  - 30s
  - 60s
  - 2m
  - 5m
  - 10m
  - 20m
  - custom
- [ ] Implementare pre-roll / post-roll model se necessario.
- [ ] Implementare storage cap globale.
- [ ] Implementare cleanup automatico FIFO.
- [ ] Implementare naming pattern configurabile.
- [ ] Implementare directory separate:
  - clips
  - recordings
  - temp/remux
  - logs/crash
- [ ] Implementare low disk space warnings.
- [ ] Implementare recovery pulita di temp files dopo crash.

### Fase UI e UX

- [ ] Implementare first-run wizard.
- [ ] Proporre preset rapidi:
  - Competitive Low-Impact
  - Balanced
  - High Quality
  - Creator
  - Custom
- [ ] Implementare settings page “General”.
- [ ] Implementare settings page “Recording”.
- [ ] Implementare settings page “Clipping”.
- [ ] Implementare settings page “Audio”.
- [ ] Implementare settings page “Storage”.
- [ ] Implementare settings page “Advanced”.
- [ ] Implementare settings page “Per-Game Profiles”.
- [ ] Implementare tray menu rapido:
  - start/stop recording
  - save clip
  - mute mic
  - pause
  - open folder
  - open settings
  - exit
- [ ] Implementare status HUD minimale opzionale:
  - buffering
  - recording
  - mic state
- [ ] Implementare libreria locale con:
  - preview thumbnail
  - open in folder
  - copy path
  - delete
  - search / filter base
- [ ] Implementare UX per errori comuni:
  - encoder unavailable
  - disk full
  - audio device lost
  - protected content / unsupported window
  - driver mismatch

### Fase performance engineering

- [ ] Automatizzare profili ETW / WPA / VS Profiler.
- [ ] Misurare:
  - CPU idle
  - CPU record
  - GPU encode usage
  - RAM working set
  - disk throughput
  - file finalize latency
- [ ] Definire regressions test per:
  - 1080p60
  - 1440p60
  - 4K60
  - 120/144 fps buffers
- [ ] Implementare frame drop counters.
- [ ] Implementare audio drift counters.
- [ ] Implementare backpressure strategy.
- [ ] Implementare telemetry locale diagnostica per benchmark mode.
- [ ] Bloccare merge di feature che rompono i budget stabiliti.

### Fase qualità, test e release

- [ ] Scrivere test matrix hardware:
  - NVIDIA desktop
  - AMD desktop
  - Intel iGPU desktop
  - laptop hybrid GPU
- [ ] Scrivere test matrix software:
  - borderless game
  - exclusive fullscreen game
  - browser window
  - video player
  - Discord call + mic
- [ ] Scrivere checklist QA per clip saving hotkey spam.
- [ ] Scrivere checklist QA per crash-safe recording.
- [ ] Testare upgrade path settings schema.
- [ ] Testare install / uninstall / reinstall.
- [ ] Preparare symbols server / PDB retention.
- [ ] Preparare changelog format.
- [ ] Preparare beta ring.
- [ ] Preparare signed release build.

### Fase packaging e distribuzione

- [ ] Preparare installer signed.
- [ ] Preparare auto-update strategy.
- [ ] Preparare manifest winget.
- [ ] Valutare MSIX come canale secondario.
- [ ] Definire policy update:
  - silent
  - notify-only
  - beta / stable channels
- [ ] Verificare redistribuzione dipendenze:
  - FFmpeg
  - encoder SDK/runtime
  - eventuale runtime UI
- [ ] Verificare implicazioni licenze prima di release pubblica.

## Definition of Done v1

La v1 è “done” solo se:
- l’utente può usare clip buffer e recording manuale senza account
- esistono impostazioni avanzate credibili
- le clip vengono salvate in modo affidabile
- multi-track audio base funziona
- software resta leggero in idle e stabile in sessioni lunghe
- il path di default non dipende da browser stack
- il prodotto ha benchmark reali, non solo impressioni
- licenze e redistribuzione sono state revisionate

## Open questions

- L’app deve avere editor integrato o solo “open in external editor” in v1?
- Conviene shipping con Qt Widgets o con WinUI 3 shell?
- Il game capture hook deve stare in v1.5 o v2?
- Vale la pena esporre AV1 in UI solo dove testato bene?
- Ha senso offrire una build “proprietary-friendly” senza componenti licensing-sensitive?
```

Se preferisci una variante più rapida da costruire, puoi mantenere quasi identico il file sopra ma sostituire la parte “custom engine” con una baseline `libobs` e far diventare lo Spike A/B il vero gate di progetto. RePlays e Segra dimostrano che la strada OBS-backed è reale, mentre la documentazione OBS conferma che il backend ha già l’astrazione `sources / outputs / encoders / services` che serve a un recorder serio; il contro rimane la sensibilità della licenza GPL e una superficie di dipendenze più ampia. citeturn17view3turn17view4turn2search2turn3view0

## skills.sh per sviluppo assistito da AI

Fuori dal `TASK.md`, queste sono le **skills** che userei per guidare lo sviluppo con AI in modo disciplinato. Le ho scelte perché corrispondono ai punti davvero critici emersi dalla ricerca: API di capture Windows, audio loopback/process capture, FFmpeg e hardware encoders, UI nativa, profiling, packaging e licensing. citeturn1search2turn1search1turn14search0turn8search0turn13search14turn13search0turn13search21turn8search5turn20search3turn20search1

```bash
#!/usr/bin/env bash
# skills.sh
# AI skills manifest for a lightweight native Windows 11 clipping/recording app

declare -A SKILL_HINTS=(
  ["native-product-architecture"]="Design module boundaries, RecorderService/TrayUI split, IPC, config schema, failure modes, startup/shutdown flow."
  ["windows-capture-apis"]="Choose between Windows.Graphics.Capture, Desktop Duplication and later game-hook paths; handle DPI, cursor, monitor selection, device-lost, fullscreen edge cases."
  ["wasapi-and-process-loopback"]="Implement system loopback, mic capture, resampling, drift handling, and per-process include/exclude audio capture strategies."
  ["ffmpeg-libav-pipeline"]="Design muxing, remuxing, codec abstraction, stream mapping, temp files, finalize lifecycle, metadata writing."
  ["hardware-encoders"]="Implement and validate NVENC, AMD AMF, Intel oneVPL/QSV backends, capability probing, fallback rules, preset exposure."
  ["audio-codec-policy"]="Define audio codec defaults, optional advanced codecs, quality presets, and licensing-safe distribution rules."
  ["ring-buffer-clipping"]="Implement RAM/disk ring buffers, clip save semantics, storage caps, cleanup policies, low-disk behavior."
  ["settings-schema-and-migration"]="Create versioned settings, migrations, per-game profiles, defaults, validation, import/export."
  ["native-ui-shell"]="Build light native settings UI, tray menu, first-run wizard, status panel, error UX, accessibility, keyboard flows."
  ["performance-profiling"]="Use ETW/WPA/Visual Studio profiling data to diagnose CPU, GPU, memory, I/O, encode stalls, frame drops, audio drift."
  ["reliability-and-crash-recovery"]="Design crash-safe recording, temp recovery, watchdogs, minidumps, restart behavior, corruption prevention."
  ["installer-update-packaging"]="Prepare signed installers, optional MSIX, winget manifests, update channels, runtime/dll redistribution."
  ["license-and-third-party-review"]="Check implications of FFmpeg options, optional FDK AAC, OBS/libobs reuse, SDK redistribution and notices."
  ["qa-hardware-matrix"]="Plan tests across NVIDIA/AMD/Intel, desktop/laptop, multiple monitors, fullscreen modes, Discord/browser/game combinations."
  ["privacy-local-first"]="Keep the product usable offline, telemetry opt-in, minimal data collection, transparent logs, user-owned media folders."
  ["webview-bloat-avoidance"]="Reject browser-based shortcuts unless explicitly approved; bias toward native UI, native previews and no embedded web runtime."
  ["release-engineering"]="Define beta/stable rings, crash symbols retention, changelog quality, rollback strategy and reproducible builds."
)

SKILLS=(
  native-product-architecture
  windows-capture-apis
  wasapi-and-process-loopback
  ffmpeg-libav-pipeline
  hardware-encoders
  audio-codec-policy
  ring-buffer-clipping
  settings-schema-and-migration
  native-ui-shell
  performance-profiling
  reliability-and-crash-recovery
  installer-update-packaging
  license-and-third-party-review
  qa-hardware-matrix
  privacy-local-first
  webview-bloat-avoidance
  release-engineering
)

# Suggested usage pattern for AI-assisted development:
# 1. Before each epic, load the most relevant 2-4 skills.
# 2. Ask the AI to produce:
#    - architecture notes
#    - edge-case checklist
#    - test plan
#    - code review rubric
# 3. Never let the AI implement capture/encode paths without a benchmarking and failure-mode checklist.
```