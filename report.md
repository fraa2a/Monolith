# Regole di esecuzione concordate

- Ambito autorizzato: tutto il report, per fasi. I riscontri statici vanno verificati sul codice prima di modificarlo.
- Ogni subagente che scrive codice deve leggere questo file e `CLAUDE.md` prima di iniziare.
- Ultima richiesta dell’utente: completare U6 con **GPT 5.6 Terra medium**. Il writer U6 usa `openai-codex/gpt-5.6-terra:medium`; le sessioni Astra e Terra low restano evidenza storica. Nessun ulteriore cambio modello per i writer senza consenso. Resta obbligatorio un solo writer attivo e il checkout è stato salvato prima del passaggio.
- Un solo writer attivo nel checkout. Riutilizzare gli agenti; niente fan-out indiscriminato.
- Conservare le modifiche DLL già presenti e i file originali dell’utente. Niente commit/push/release senza richiesta.
- Audit originale sotto: non cancellare i riscontri per segnalarli come risolti. Registrare avanzamento e prove in una sezione separata.
- Non dichiarare verifiche Windows o benefici prestazionali senza esecuzione/misure. Fermarsi per scelte di prodotto, incompatibilità o dipendenze pesanti.

## Piano per fasi

1. Risorse limitate: replay RAM e IPC (R1, I1, I2), test regressione.
2. Integrità media/storage: segmenti, trim, filename, catalogo/bookmark e operazioni filesystem.
3. Lifecycle/realtime/audio: callback, clock, backpressure e writer asincroni.
4. Aggiornamenti/distribuzione: autenticità, rollback, versioni, download e shutdown.
5. UI e background: query, caricamento lazy, polling, artwork e logging.
6. Video GPU/colore e tuning: percorso hardware, fallback, telemetria; scelte HDR/qualità/autenticazione IPC da concordare prima di cambiare il comportamento.

Stato iniziale: nessuna fase di ottimizzazione ancora implementata; hardening DLL della sessione precedente già nel working tree.

---

# Monolith - DLL mancanti e audit tecnico

## 1. Esito e limiti

Revisione del checkout `main`, HEAD `6232dc2e5c0424f731f6d31c2a962fdebdd2765d`.

- **DLL:** la causa individuata nel codice storico è il packaging con filtri sui nomi. `aom.dll`, dipendenza FFmpeg abilitata in `vcpkg.json:8`, non corrispondeva a `av*.dll`, `sw*.dll`, `lib*.dll` o `sqlite3.dll`. Sia installer sia aggiornamento engine potevano ometterla.
- **Precisazione importante:** HEAD conteneva già la correzione che copia tutte le DLL. Questa sessione ha aggiunto un controllo generale delle dipendenze al posto delle sole verifiche su nomi noti. Non è stato prodotto né installato un nuovo eseguibile/installer: l'installazione Windows già guasta non è stata riparata su questa macchina.
- **Audit:** sola lettura su cattura, encoding, audio/mixer, replay RAM/disco, recording, trim, storage, UI/host, IPC, updater, logging e integrazioni. Nessuna ottimizzazione applicata.
- **Ambiente:** Linux, senza esecuzione Windows, MSVC/dumpbin o PowerShell disponibili. Le conclusioni sono da codice; non sono benchmark e non garantiscono assenza di ulteriori difetti.
- Le priorità indicano ordine consigliato, non incidenti riprodotti: **P1** = correttezza, affidabilità, consumo non limitato o forte rischio operativo; **P2** = ottimizzazione/hardening; **P3** = scelta evolutiva da misurare.
- Nessuna percentuale di risparmio è dimostrata. Il beneficio atteso è qualitativo. Le condizioni necessarie a manifestare i problemi sono esplicitate.

Questa sessione non ha modificato né eliminato il `REPORT.md` preesistente o `.claude/`. Erano presenti all'inizio e ancora alla conclusione del worker; nell'ultimo controllo entrambi risultano assenti, senza cambiamento di HEAD. La causa di questa variazione esterna non è stata determinata; non sono stati ricreati o sovrascritti. Questo report è conservato negli artefatti della sessione, fuori dal repository.

## 2. Correzione DLL: cosa è cambiato

### File modificati in questa sessione

1. `.github/workflows/windows-ci.yml`: esegue il controllo delle dipendenze sul payload nativo della build.
2. `.github/workflows/version-tag.yml`: stesso controllo sulla build di release e sul payload engine già predisposto per lo ZIP.
3. `scripts/verify-runtime-dlls.ps1`: nuovo verificatore con `dumpbin /DEPENDENTS`; controlla gli import degli EXE/DLL presenti direttamente nella cartella indicata e le relative dipendenze transitive, senza hardcoding di `aom.dll`.
4. `docs/handover/ACTIVE_HANDOVER.md`: aggiornamento di consegna relativo esclusivamente alle DLL e ai limiti di verifica.

### Regole già corrette, confermate

- `installer/monolith.iss:61-72` include tutte le DLL accanto a `Monolith.exe`.
- `.github/workflows/version-tag.yml:245-257` include tutte le DLL nello ZIP engine.
- Il difetto precedente è visibile nelle versioni `HEAD^` di questi file. Non era necessario rimuovere AV1 o scaricare DLL singole: la dipendenza deve arrivare dalla stessa build vcpkg/FFmpeg dell'app.

### Copertura e limiti del verificatore

Il verificatore fallisce se un import vcpkg non è presente nel payload. Accetta API-set Windows e DLL disponibili in System32 sull'host CI. **Non** ispeziona `ui/` o altre sottocartelle, non scopre caricamenti esclusivamente dinamici tramite `LoadLibrary`, e non prova che una DLL trovata sull'host CI sia presente su una Windows pulita. Non sostituisce quindi i test di installazione.

La revisione indipendente ha rilevato una descrizione inizialmente troppo ampia: è stata corretta nel commento/output dello script e nell'handover, restringendola esplicitamente al payload nativo di primo livello. Non è stato esteso impropriamente il lavoro alla distribuzione delle dipendenze della UI.

### Verifiche effettivamente eseguite

- `git diff --check`: PASS.
- Parsing Python/PyYAML dei due workflow: PASS.
- Asserzioni Python sui sorgenti: wildcard DLL in installer/engine ZIP, una chiamata al verificatore nella CI ordinaria e due nella release, visita degli import transitivi e delimitazione della copertura: PASS.
- Nessun file staged; nessun commit, push o release eseguito.
- **Non eseguiti:** script PowerShell, build C++ Windows, installer Inno Setup, avvio Windows, benchmark CPU/RAM/GPU.

### Come chiudere davvero il problema sull'installazione Windows

1. Generare una nuova build/release Windows contenente questi controlli e verificarne l'esito.
2. Installare il pacchetto completo, non copiare soltanto `Monolith.exe`. Non scaricare `aom.dll` da siti di DLL generiche.
3. Su Windows 11 pulita, senza vcpkg/Visual Studio nel PATH, verificare installazione e aggiornamento da una versione precedente, avvio tray, replay, recording, UI e updater.
4. Testare H.264, HEVC e AV1 sui dispositivi supportati; l'assenza del messaggio DLL non dimostra che ogni encoder funzioni.
5. Per distribuire via component updater, assicurarsi che la versione engine del nuovo pacchetto sia effettivamente superiore a quella installata: una ricostruzione con la stessa versione può non essere proposta come aggiornamento. Nessuna versione è stata incrementata arbitrariamente in questa sessione.

## 3. Sintesi: dove intervenire prima

| Obiettivo | Intervento consigliato | Impatto atteso |
|---|---|---|
| RAM stabile per ore | Limite replay anche senza keyframe; recupero thread IPC terminati | Elimina due sorgenti di crescita senza limite |
| CPU inferiore durante la cattura | Texture D3D11 → conversione GPU → encoder hardware | Riduce readback, memcpy e conversione software |
| Video/audio fluidi sotto carico | Writer asincroni con code limitate; clock e timestamp corretti | Isola il realtime dagli stalli del disco |
| Clip integre | Correggere trim/timebase e segmenti che non iniziano su keyframe | Previene durata, sincronizzazione e decodifica errate |
| UI leggera con molti video | Paginazione/virtualizzazione, probing solo vicino al viewport, query aggregate | Riduce letture, decoder, DOM e serializzazione |
| Avvio affidabile anche dopo update | Firma obbligatoria, rollback e test dei payload | Evita aggiornamenti non autenticati o installazioni parziali |

L'ordine consigliato è: **sicurezza/integrità e limiti delle risorse → eliminazione dell'I/O realtime → UI/idle → percorso GPU → tuning qualitativo**.

## 4. Cattura, encoding e qualità video

### V1 - P1: shutdown della cattura senza attesa dimostrabile dei callback

**Evidenza:** `libs/capture/capture.cpp:323-442,475-510`; `app/recorder/src/main.cpp:2537-2634`.

`stop()` abbassa `active`, attende 30 ms, revoca l'evento e libera risorse. Il callback controlla `active` all'ingresso, ma può essere già in esecuzione, anche impegnato nell'apertura encoder. Un'attesa fissa non dimostra il completamento. Lo stato C++ del callback non ha una protezione generale; qualora le invocazioni si sovrappongano, la protezione multithread D3D non protegge automaticamente quei campi.

**Consiglio:** protocollo di arresto con conteggio callback in volo e attesa esplicita, più serializzazione dello stato o un unico consumer. Non introdurre mutex senza analizzare ordine dei lock e semantica di revoca/Close.

**Beneficio:** meno crash intermittenti su stop, resize, cambio display e riconfigurazione. **Test:** cicli ripetuti start/stop/resize con encoder lento; contatore callback a zero prima del rilascio. La probabilità del problema richiede verifica Windows.

### V2 - P1: fallback encoder può cambiare codec senza consenso

**Evidenza:** `libs/encoding/encoding.cpp:47-53,132-159,246-253`; `app/recorder/src/main.cpp:2556-2570`.

La risoluzione iniziale rispetta il codec richiesto, ma l'apertura reale aggiunge tutti i candidati globali. Se l'encoder preferito fallisce, il fallback può passare a un'altra famiglia codec.

**Consiglio:** limitare i fallback allo stesso codec, mostrare dispositivo/codec effettivi e rendere esplicito un eventuale cambio. **Beneficio:** compatibilità prevedibile, evita fallback software o formati inattesi. **Test:** forzare fallimenti per ogni codec e controllare l'output con `ffprobe`.

### V3 - P1/P2: HDR non preservato e colore SDR non esplicitamente definito

**Evidenza:** `libs/capture/capture.cpp:315-319,403-406`; `libs/encoding/encoding.cpp:273-274,469-487`; `libs/encoding/mux_common.cpp:65-93`.

Il percorso è BGRA8 → YUV420P 8 bit. Non emerge una politica esplicita di matrice/range in swscale né tagging completo primarie/transfer/matrice. Questo non è un percorso HDR.

**Consiglio:** definire SDR BT.709/range in conversione e metadata; rilevare HDR e offrire tonemapping dichiarato oppure un percorso FP16/P010 e codec 10 bit. Non etichettare l'attuale percorso come HDR o tonemapped senza implementarlo.

**Beneficio:** colori riproducibili, meno highlight tagliati o neri alterati. **Costo:** HDR richiede lavoro significativo e test di compatibilità. **Test:** color bars, gradienti SDR/HDR, valori decodificati e metadata MediaInfo/ffprobe.

### V4 - P2, massimo potenziale CPU: eliminare il percorso GPU→CPU→GPU

**Evidenza:** `libs/capture/capture.cpp:416-430`; `app/recorder/src/main.cpp:459-473`; `libs/encoding/encoding.cpp:469-492`.

Per frame accettato: copia su staging, Map sincrona, copia BGRA nel pacer, conversione software RGB→YUV; un encoder hardware deve poi consumare questi dati CPU. Il downscale GPU già disponibile riduce parte del costo ma non elimina questo percorso.

**Consiglio:** mantenere texture sulla GPU, convertire/scalare a NV12/P010 e usare hardware frames FFmpeg. Gestire esplicitamente adapter differenti e mantenere un fallback CPU sicuro.

**Beneficio:** meno CPU, banda memoria e stalli, soprattutto UHD/high-FPS. **Costo:** implementazione complessa; più risorse GPU e vincoli interop. **Misura:** ETW/GPUView, tempi readback/memcpy/sws, frame persi, frametime del gioco. Non basta misurare gli FPS nominali del file.

### V5 - P2: apertura/probing encoder nel callback di cattura

**Evidenza:** `app/recorder/src/main.cpp:2415-2634`; `libs/encoding/encoding.cpp:89-112,132-159,201-409`.

La prima immagine attiva prove/apertura codec direttamente nel callback WGC, con frame pool di due buffer.

**Consiglio:** worker di controllo pipeline; callback limitato a pubblicare dimensioni/frame. Cache delle capacità invalidata su cambio driver/device, evitando doppio probing inutile.

**Beneficio:** avvio/ripristino più prevedibili e meno frame iniziali persi. **Test:** encoder occupato/indisponibile, tempo primo frame→ready e durata p95/p99 callback.

### V6 - P2: pacer con intervalli interi e risvegli relativi

**Evidenza:** `app/recorder/src/main.cpp:509-531,576-598`.

`1000/fps` tronca l'intervallo: 144 FPS produce 6 ms, per esempio. Il QPC corregge i timestamp/slot ma il timer resta relativo e arrotondato.

**Consiglio:** calcolare la prossima deadline dal clock di riferimento con precisione adeguata; evitare spin e catch-up a raffica.

**Beneficio:** meno risvegli superflui e jitter di scheduling. **Test:** wakeup/s, distribuzione lateness, frame duplicati/saltati e PTS; non dedurre FPS reali solo dall'intervallo timer.

### V7 - P2: errori e drop poco osservabili

**Evidenza:** `app/recorder/src/main.cpp:459-473,590-598,2443-2518`; `libs/encoding/encoding.cpp:479-493`; `libs/recording/recording.cpp:245`.

Sostituzioni del latest-frame non contate; il contatore submitted sale anche se `push_bgra` fallisce internamente; errori mux non sempre propagati.

**Consiglio:** distinguere frame catturati, sostituiti, duplicati, scartati dal pacer, accettati encoder, emessi e scritti; contare EAGAIN/errori, code e latenze.

**Beneficio:** permette ottimizzazioni basate su fatti e diagnosi di clip degradate. **Test:** fault injection su encode/mux e riconciliazione dei contatori.

### V8 - P2: mancati controlli sulla creazione thread/event del pacer

**Evidenza:** `app/recorder/src/main.cpp:608-658`.

Event/thread possono fallire ma lo stato può risultare running.

**Consiglio:** verificare handle, ripulire avvii parziali, ripristinare timer e pubblicare errore. **Beneficio:** niente pipeline fantasma o handle persi in condizioni di risorse scarse. **Test:** fallimento controllato di CreateEvent/CreateThread.

## 5. Replay, recording, audio e memoria

### R1 - P1: il tetto replay 512 MB non è garantito

**Evidenza:** `libs/replay-buffer/replay_buffer.cpp:58-95,171-195`; `app/recorder/src/main.cpp:2402-2415`.

L'espulsione richiede `keyframes > 2`. Se audio parte e video non arriva, fallisce o smette di produrre keyframe, il buffer può crescere senza rispettare durata/byte. Inoltre i byte contati sono payload, non memoria complessiva: metadata, deque, allocator, snapshot e riferimenti mantenuti durante save sono extra.

**Consiglio:** cap rigido indipendente dalla disponibilità di GOP; buffer pre-video limitato o niente audio prima del primo keyframe. Esporre durata realmente conservata e memoria logica separatamente dai private bytes. Prevedere GOP troppo grandi e salvataggi lenti.

**Beneficio:** RAM stabile e comportamento esplicito quando non è possibile conservare una clip decodificabile. **Tradeoff:** buffer più corto o clip non disponibile in emergenza. **Test:** solo audio, WGC fallita, video senza keyframe, bitrate massimo, save lento.

### R2 - P1: ordine di arrivo scambiato per ordine temporale nel replay

**Evidenza:** `libs/replay-buffer/replay_buffer.cpp:66-95,171-195,238-268,389-394`; `app/recorder/src/main.cpp:1455-1460,1575-1616,2570-2576`.

Audio/video arrivano da produttori distinti; il mutex serializza l'arrivo ma non ordina DTS. Selezione/retention usano front/back e arresto anticipato della scansione. Ordinare al mux avviene troppo tardi per correggere una finestra selezionata male.

**Consiglio:** watermark temporale o reorder limitato per stream; determinare i confini da timestamp validi. **Beneficio:** durata e inizio clip prevedibili. **Costo:** piccola latenza/coda. **Test:** pacchetti fuori ordine con skew, partenza su keyframe e timeline finale.

### R3 - P1: nuovi segmenti disco possono iniziare senza keyframe video

**Evidenza:** `libs/disk-segments/disk_segments.cpp:218-245,264-285`.

Il save chiude il segmento corrente. Il pacchetto successivo, anche audio, può aprirne uno; il primo video può essere inter-frame.

**Consiglio:** attendere un keyframe per aprire il nuovo segmento, con preroll audio limitato e politica documentata. **Beneficio:** segmenti indipendentemente decodificabili. **Test:** save a metà GOP, poi audio e frame non-key, quindi nuovo save e decode integrale.

### R4 - P1: retention disco sospesa per tutta la durata del salvataggio

**Evidenza:** `libs/disk-segments/disk_segments.cpp:154-168,264-314`.

`purge_locked()` non lavora durante `saving`; nuovi segmenti continuano a nascere mentre copia/concatenazione o callback tardano.

**Consiglio:** proteggere soltanto i segmenti dello snapshot, liberare gli altri e imporre tetto byte/free-space. **Beneficio:** spazio temporaneo limitato anche verso dischi lenti. **Costo:** reference tracking e scelta su abort/drop. **Test:** destinazione bloccata per più finestre replay; crescita disco deve stabilizzarsi.

### R5 - P1: I/O mux sincrono sui produttori realtime e stop sul message loop

**Evidenza:** `libs/audio/audio.cpp:240-263`; `libs/recording/recording.cpp:119-143,208-245`; `libs/disk-segments/disk_segments.cpp:218-245`; `app/recorder/src/main.cpp:1575-1616,3251-3268,3374-3386`.

WASAPI chiama l'integrazione prima di ReleaseBuffer. Encode/sink possono arrivare fino alla scrittura disco. Lo stop manuale finalizza sotto lock dal percorso dei comandi.

**Consiglio:** writer dedicati con code di pacchetti codificati a capacità limitata; finalizzazione asincrona e stato “finalizzazione”. Non spostare soltanto il problema in una coda infinita. Su overload continuativo fermare/reportare secondo una policy esplicita.

**Beneficio:** meno gap audio, frame persi e tray bloccata su dischi lenti/pieni. **Test:** latenza ReleaseBuffer, p99 write/stop, code, audio/video e hotkey sotto throttling I/O.

### R6 - P2: copie e allocazioni ripetute dei pacchetti codificati

**Evidenza:** `libs/encoding/encoding.cpp:35-43,419-432`; `libs/encoding/mux_common.cpp:147-166`.

Una copia crea il payload posseduto dal progetto; un'altra allocazione/copia avviene a ogni scrittura mux. La condivisione già esistente tra replay/recording è positiva.

**Consiglio:** ownership refcounted basata su buffer FFmpeg e ref/clone corretti; preservare le regole di consumo di `av_interleaved_write_frame`.

**Beneficio:** meno allocator/memcpy; priorità inferiore al readback dei frame grezzi. **Test:** allocazioni/s e CPU a bitrate alto, race/lifetime durante clear/save/stop.

### R7 - P1: filename recording collidenti e buffer MAX_PATH

**Evidenza:** `libs/recording/recording.cpp:58-72,180-188`.

Nome preciso solo al secondo, senza prenotazione esclusiva, e costruzione in `wchar_t[MAX_PATH]`.

**Consiglio:** path dinamici, suffisso univoco, creazione no-overwrite con retry. **Beneficio:** previene sovrascrittura o failure con start/stop ravvicinati e percorsi lunghi. **Test:** due registrazioni nello stesso secondo, directory lunghe, file già esistente.

### R8 - P1: uscita durante recording non percorre il normale completamento catalogo/bookmark

**Evidenza:** `app/recorder/src/main.cpp:2694-2712,3251-3263,3374-3386`.

`media_stop()` finalizza ma non richiama il normale cataloging/flush bookmark e svuota i bookmark pendenti. Il video può essere recuperato dalla riconciliazione successiva, non i bookmark scartati.

**Consiglio:** unico completamento per stop/exit/autostop, con persistenza o journal recuperabile. **Test:** bookmark, uscita in recording e restart: file, durata, thumbnail, riga catalogo e bookmark devono sopravvivere.

### R9 - P2: errore WASAPI può lasciare stato running obsoleto

**Evidenza:** `libs/audio/audio.cpp:219-231,748-750`; `libs/audio/audio.h:142-143`.

Un errore `GetNextPacketSize` esce dal loop senza azzerare running.

**Consiglio:** stato terminale su ogni uscita, HRESULT e notifica al controller per restart con backoff. **Beneficio:** recupero endpoint anziché cattura apparentemente attiva ma muta. **Test:** disconnessione/cambio dispositivo ed errori simulati.

### R10 - P2: metadati temporali/discontinuità audio scartati

**Evidenza:** `libs/audio/audio.cpp:238-262`; `app/recorder/src/main.cpp:1575-1616`; `libs/recording/recording.cpp:130-142`.

QPC e flag di discontinuity non vengono propagati nella normale integrazione PCM. L'assenza della gestione è visibile; entità di drift/gap dipende da hardware e carico.

**Consiglio:** timeline comune, gestione silenzio/padding/resync e contatori; non correggere a caso alterando i timestamp dei pacchetti.

**Beneficio:** sincronizzazione recuperabile dopo stalli/device switch. **Test:** almeno un'ora con timecode audiovisivo, stalli e cambio endpoint; misurare offset massimo e dopo il recupero.

### R11 - P2: mixer recupera tutto il ritardo in un unico blocco

**Evidenza:** `libs/encoding/encoding.cpp:933-952,979-985`.

Il numero di frame da emettere deriva da tempo reale meno frame già emessi, senza tetto per iterazione. Dopo una pausa lunga del thread/sink può allocare un accumulatore molto grande e generare un burst. Le FIFO per sorgente hanno già una politica di drop, ma non limita questo blocco di uscita.

**Consiglio:** chunk massimo, gestione esplicita di salti temporali e underrun, riallineamento dopo suspend/resume. **Beneficio:** meno picchi CPU/RAM e latenza. **Test:** sospendere il worker o rallentare il sink e tracciare dimensione massima del blocco.

### R12 - P3: clipping duro del mix audio

**Evidenza:** `libs/encoding/encoding.cpp:958-975`.

Le sorgenti vengono sommate e clippate a [-1,1]. È un comportamento definito, non un crash, ma sorgenti forti simultanee possono distorcere.

**Consiglio:** headroom/gain metering e limiter opzionale, non un cambio di volume silenzioso. **Beneficio:** qualità audio migliore. **Costo:** DSP e possibile latenza; validare con picchi simultanei e ascolto comparativo.

## 6. Trim: ulteriori riscontri diretti

### T1 - P1: remux senza conversione dalla timebase di input a quella di output

**Evidenza:** `libs/encoding/trim.cpp:181-254`, in particolare `237-247`.

Le stream timebase vengono inizialmente copiate, ma il muxer può cambiarle durante la scrittura header. I PTS/DTS restano in unità input mentre l'offset concatenazione è calcolato in unità output. Non c'è `av_packet_rescale_ts` in questo percorso; anche duration deve essere convertita.

**Consiglio:** calcolare ancoraggi coerenti, riscalare PTS/DTS/duration da input a output dopo header, applicare offset nella stessa unità. Non correggere DTS invalidi con clamp indiscriminati.

**Beneficio:** durata, velocità, sincronizzazione e concat corretti quando le basi differiscono. **Test:** MP4/MKV con timebase differenti, più segmenti e tracce; `ffprobe -show_packets` e decode completo. Manifestazione dipendente dal muxer/input, non riprodotta qui.

### T2 - P1: trim lossless con preroll ma metadata/bookmark basati sul taglio richiesto

**Evidenza:** `libs/encoding/trim.cpp:178-205,223-232`; `app/recorder/src/main.cpp:978-1008`.

Il seek backward conserva il keyframe precedente a start, ma il catalogo registra `end-start` e i bookmark sottraggono start richiesto, non quello effettivo.

**Consiglio:** l'API trim deve restituire inizio/durata effettivi, oppure offrire trim esatto con ricodifica. Aggiornare bookmark e UI coerentemente e gestire gli errori di aggiornamento.

**Beneficio:** niente durate sbagliate o bookmark spostati di parte di un GOP. **Test:** taglio non-keyframe e confronto contenuto/durata/bookmark, inclusa concatenazione con preroll.

### T3 - P1: fallback reencode salta pacchetti necessari al decoder

**Evidenza:** `libs/encoding/trim.cpp:444-476,480-499`.

Dopo il seek, i pacchetti video prima di start vengono scartati prima di alimentarli al decoder. I successivi frame inter possono dipendere proprio da quei riferimenti. Il filtraggio temporale va effettuato sui frame decodificati, incluso drain e riordino B-frame.

**Consiglio:** decodificare preroll dal keyframe, emettere solo frame nella finestra; preservare timestamp/durata invece di assumere sempre FPS interi uniformi.

**Beneficio:** inizio clip integro e meno errori o frame mancanti. **Test:** tagli tra keyframe con B-frame, 59.94 FPS e input VFR.

### T4 - P1/P2: ricodifica trim accumula tutto in RAM e conserva una sola traccia audio

**Evidenza:** `libs/encoding/trim.cpp:326-327,356-378,502-539`.

`vpkt`/`apkt` mantengono tutti i pacchetti fino a fine encode; poi vengono muxati. Il fallback sceglie un solo `av_find_best_stream` audio mentre il percorso lossless copia tutte le tracce.

**Consiglio:** mux incrementale interleaved con coda limitata; mantenere tutte le tracce o chiedere esplicitamente una scelta prima di una sostituzione distruttiva. Rispettare codec/qualità richiesti e controllare il file prima di sostituire l'originale.

**Beneficio:** memoria non proporzionale alla durata del trim e nessuna perdita silenziosa delle tracce separate. **Test:** trim lungo ad alto bitrate, mic+game separati, errore a metà operazione e controllo private bytes/numero tracce.

## 7. UI, catalogo e attività in background

### U1 - P1: tre scansioni complete della libreria per un reload

**Evidenza:** `app/desktop-ui/ui/src/app.tsx:55-65`; `app/desktop-ui/src-tauri/src/clip_catalog.rs:110-111,199,278-293`; `commands.rs:57-68`.

Lista, giochi e hashtag invocano percorsi che rimaterializzano l'intera libreria; ogni clip comporta metadata file e caricamento cache accessorie. La ricerca parte a ogni carattere senza debounce/generation guard, quindi risposte vecchie possono sovrascrivere quelle nuove.

**Consiglio:** query SQL aggregate per facets, lista paginata, metadata persistiti/cache invalidata, debounce e risultato applicabile solo alla richiesta corrente.

**Beneficio:** meno DB/stat/serializzazione e ricerca stabile su librerie grandi. **Test:** 100/1.000/10.000 clip, query/stat per reload, tempo prima card e risposte fuori ordine.

### U2 - P1: tutte le card aprono un video anche con thumbnail disponibile

**Evidenza:** `app/desktop-ui/ui/src/app.tsx:245-255`; `home/clip-card.tsx:86-150`.

La griglia monta tutte le clip; ciascuna crea un elemento video e chiama load. In assenza thumbnail può partire anche decode o rigenerazione engine, senza gating viewport.

**Consiglio:** virtualizzazione/paginazione, lazy probing tramite visibilità, durata dal catalogo, coda thumbnail con concorrenza limitata. Non cambiare soltanto l'aspetto grafico.

**Beneficio:** meno RAM WebView, I/O e contesa decoder con la cattura. **Test:** 1.000 clip con/senza thumbnail, decoder attivi, private bytes e tempo interattività.

### U3 - P2: collection con query N+1

**Evidenza:** `app/desktop-ui/src-tauri/src/collections.rs:173-204`; `clip_catalog.rs:230-243`.

Ogni membro apre/carica dati catalogo, tag, artwork e stat.

**Consiglio:** batch ID per sorgente, dati condivisi caricati una volta, pruning in transazione. **Beneficio:** apertura collection più rapida. **Test:** connessioni/query/stat con 100 e 1.000 membri.

### U4 - P2: polling host attivo con UI nascosta

**Evidenza:** `app/desktop-ui/src-tauri/src/main.rs:28-38`; `engine_rpc.rs:19-21`; `ui/src/shell/titlebar.tsx:42-55`.

Il watcher nativo interroga l'engine ogni secondo anche a finestra nascosta; la titlebar invece salta le letture nascoste. Le RPC aprono connessioni nuove; il connect non ha timeout esplicito.

**Consiglio:** un solo poll host condiviso, frequenza ridotta/pausa da nascosto e refresh al restore, connect_timeout; valutare eventi push o connessione persistente con riconnessione corretta.

**Beneficio:** meno CPU idle e churn socket/thread. **Test:** CPU/connect/s in stato visibile, minimizzato e nascosto.

### U5 - P2: artwork senza backoff sui fallimenti

**Evidenza:** `app/desktop-ui/src-tauri/src/main.rs:48-54`; `game_catalog.rs:356-377`.

Elementi stale falliti restano stale e sono ritentati nei successivi startup/cicli; nessun limite per batch.

**Consiglio:** last_attempt, backoff con jitter e batch limitato. **Beneficio:** meno rete e worker occupati con cache ampia/offline. **Test:** centinaia di elementi irraggiungibili e verifica retry nel tempo.

### U6 - P1/P2: delete/rename non consistenti tra filesystem e DB

**Evidenza:** `libs/storage/storage.cpp:432-479,811-845,902-921`; `app/desktop-ui/src-tauri/src/clip_catalog.rs:486-514`.

Cancellazione DB committata prima della rimozione file, con errori filesystem ignorati sia in C++ sia nel percorso Rust. File rimasti possono essere reimportati dalla riconciliazione. Rename può cambiare file e poi fallire l'UPDATE DB.

**Consiglio:** staging/quarantena same-volume, rollback/compensazione o journal/tombstone durevole; risposta UI coerente con risultato. Una transazione SQLite da sola non rende atomico il filesystem.

**Beneficio:** meno file orfani, spazio sprecato e clip che riappaiono. **Test:** file lock, permessi negati, disco pieno ed errore SQL dopo rename.

## 8. IPC, updater, logging e affidabilità

### I1 - P1: thread IPC terminati non recuperati fino allo shutdown

**Evidenza:** `libs/ipc/ipc_server.cpp:86-95,246-270,337-343`; `app/desktop-ui/src-tauri/src/engine_rpc.rs:19`; `main.rs:28-38`.

Ogni connessione crea uno std::thread conservato nel vector; la fine del client rimuove il socket ma non fa join/rimozione del thread. Le RPC UI aprono una connessione per richiesta. Rimangono oggetti/handle di thread terminati, non necessariamente interi stack attivi.

**Consiglio:** recupero sicuro dei worker terminati oppure worker pool/event loop limitato. Non usare detach senza lifetime management.

**Beneficio:** arresta crescita handle/RAM nelle sessioni lunghe. **Test:** UI idle 1/8/24 ore, handle count/private bytes/vector e latenza RPC. Il polling visibile può generare circa 1,6 connessioni/s, stima da timer, non misura.

### I2 - P1: input e numero connessioni IPC senza limite

**Evidenza:** `libs/ipc/ipc_server.cpp:86-100,246-270,308`.

Buffer fino a newline illimitato, handler bloccanti per connessione senza cap. Backlog 8 non limita client già accettati. `send` non gestisce output parziale.

**Consiglio:** massimo messaggio, massimo client/work item, deadline e send-all. **Beneficio:** disponibilità anche con client difettoso o abuso locale. **Test:** input senza newline, molti client idle, short writes, JSON malformato.

### I3 - P2: IPC locale senza autenticazione

**Evidenza:** `libs/ipc/ipc_server.cpp:105-239,300-308`.

Scelta già dichiarata dal progetto: un processo locale con accesso alla porta può controllare recording, mutazioni ed exit. Non è una porta pubblica Internet né una vulnerabilità remota dimostrata.

**Consiglio:** definire threat model e valutare named pipe con ACL utente/integrity o token con lifecycle adeguato anche al plugin. Prima di tutto imporre limiti I2.

### A1 - P1, blocco consigliato per la release: autenticità update opzionale

**Evidenza:** `app/updater/src-tauri/src/manifest.rs:25-56`; `download.rs:37-54`.

Dimensione/hash/firma hanno default vuoti e controlli saltati se assenti. Un manifest controllato da un attaccante può quindi indicare payload non firmati. Questo richiede controllo della fonte del manifest/configurazione/canale fidato: il solo bug non dimostra compromissione remota attraverso HTTPS GitHub.

**Consiglio:** firma Ed25519 obbligatoria e campi validati prima del download, URL HTTPS e dimensioni limitate. SHA-256 da solo, se nel manifest alterabile, non prova autenticità. Valutare firma manifest e protezione rollback.

**Beneficio:** aggiornamento realmente autenticato. **Costo:** vecchi manifest incompleti devono fallire chiusi. **Test:** campi mancanti/vuoti/malformati, firme invalide, nessuna applicazione su errore.

### A2 - P1: versione updater discordante

**Evidenza:** `app/updater/src-tauri/Cargo.toml:3` = 1.0.0; `tauri.conf.json:4` = 1.0.1; `src/versions.rs:22-23` legge CARGO_PKG_VERSION.

**Consiglio:** singola fonte o gate di uguaglianza Cargo/Tauri/manifest. **Beneficio:** evita proposta ripetuta dello stesso aggiornamento. **Test:** manifest della versione corrente deve risultare UpToDate.

### A3 - P1: apply componenti senza rollback completo

**Evidenza:** `app/updater/src-tauri/src/apply.rs:60-95`; `main.rs:340-361`.

Spostamenti in .old e sostituzioni file avvengono progressivamente; errore intermedio può lasciare una combinazione di file nuovi, mancanti e vecchi.

**Consiglio:** journal di apply e rollback verificato, swap directory dove possibile; aggiornare versioni persistite solo al completamento effettivo. **Beneficio:** update fallito non rompe l'avvio né reintroduce DLL mancanti. **Test:** errore al file N, crash, file bloccato, disco pieno e riavvio successivo.

### A4 - P2: progress updater emesso per chunk da 64 KiB

**Evidenza:** `app/updater/src-tauri/src/http.rs:155-168`; `main.rs:261-277`.

Ogni chunk prende mutex, serializza stato completo ed emette evento Tauri. A 100 MiB/s il calcolo teorico è circa 1.600 eventi/s, non un benchmark.

**Consiglio:** contatori continui, notifiche 4-10 Hz e sempre sugli eventi terminali. **Beneficio:** meno CPU/render/churn della coda. **Test:** eventi/s e tempo main thread durante download veloce.

### A5 - P2: firma update legge l'intero archivio in RAM

**Evidenza:** `app/updater/src-tauri/src/download.rs:58-95`.

SHA-256 legge a blocchi, poi `verify_signature` usa `std::fs::read` sull'intero ZIP per Ed25519.

**Consiglio:** imporre tetto dimensioni prima/durante download; valutare mapping file o protocollo di firma adatto a verifica streaming solo con migrazione esplicita del formato. Non sostituire arbitrariamente Ed25519 con firma dell'hash, romperebbe il protocollo.

**Beneficio:** picchi RAM più prevedibili sugli update grandi. **Test:** private bytes e I/O con pacchetti grandi, limite dichiarato assente/falso, errore firma.

### A6 - P2: quit può attendere rete gamelist

**Evidenza:** `app/recorder/src/main.cpp:3374-3381`; `libs/gamelist/gamelist.cpp:247-250,493-501`.

Shutdown fa join del worker che può essere dentro WinHTTP sincrono con timeout di 15 secondi per operazione.

**Consiglio:** cancellazione coordinata request/session e completamento controllato, non detach. **Beneficio:** uscita/update più rapidi sotto errori rete. **Test:** DNS/proxy bloccati e tempo close→exit.

### A7 - P2: log ruota solo all'apertura e flush sincrono per scrittura

**Evidenza:** `libs/logging/logging.cpp:41-63,99-133`.

Sessioni verbose lunghe possono superare indefinitamente la soglia dichiarata di 5 MiB.

**Consiglio:** contatore byte e rotazione durante scrittura; batching per log non critici, flush immediato errori. **Beneficio:** spazio limitato e meno I/O. **Costo:** possibile perdita delle ultime righe buffered in crash. **Test:** oltre 20 MiB senza restart, numero file e latenza logging.

## 9. Ottimizzazioni evolutive da sperimentare, non bug dimostrati

1. **Qualità invece di bitrate rigidamente costante:** `libs/encoding/encoding.cpp:283-330` contiene sia quality mode sia CBR, ma il normale percorso usa il bitrate. Offrire un profilo CQP/CRF/VBR per registrazioni locali può migliorare rapporto qualità/spazio. Controindicazione: picchi bitrate/GOP richiedono limiti replay realmente robusti. Confrontare scene di gioco e testo, dimensioni e qualità decodificata; non promettere che AV1 sia sempre la scelta migliore.
2. **Fallback software trasparente:** libaom/libx265 possono costare molto CPU se manca hardware. Mostrare encoder effettivo e avviso; concordare eventuale riduzione risoluzione/FPS invece di cambiarli silenziosamente. Misurare impatto sul frametime del gioco.
3. **Profilo idle con pipeline sospese:** verificare in runtime che game-only/idle/pausa non mantengano attività inutile, senza rompere la funzione clip senza gioco. È una verifica proposta, non una nuova inefficienza dimostrata. Misurare separatamente engine, UI e processi WebView2.
4. **Replay RAM vs disco:** RAM evita scrittura continua ma consuma memoria; disco sposta il costo su I/O/usura e non è gratuitamente più efficiente. A bitrate R e durata T il solo payload è circa R×T/8, più audio e overhead. Esporre durata stimata e consumo, dopo R1/R4.
5. **Preset/scaler/GOP:** tuning per hardware e contenuto, non preset unico “massima qualità”. GOP breve migliora precisione replay ma aumenta overhead; lookahead/B-frame possono aumentare qualità ma latenza/risorse e richiedono trim/reorder corretti.

## 10. Cose già ben impostate da preservare

- Rate limiting WGC prima di readback; downscale GPU quando configurato.
- Buffer latest-frame con swap lato consumer, evitando una copia aggiuntiva.
- Payload codificati condivisi tra replay e recording; snapshot RAM non duplica subito tutti i byte.
- Save replay single-flight su worker; recording attende keyframe a start/resume.
- WASAPI event-driven e mixer con riferimento al tempo reale, da completare con gestione discontinuity/catch-up.
- Cataloghi SQLite WAL/busy timeout; molti comandi Rust già spostati in spawn_blocking.
- Artwork cache-only nel percorso griglia; rete gamelist su worker.
- Runtime-status evita riscritture identiche.
- Stream Deck usa client persistente condiviso e polling moderato.
- Updater protegge estrazione da path traversal e verifica firme quando presenti: rendere questa verifica obbligatoria, non riscriverla senza motivo.

## 11. Piano di misurazione e regressione

### Matrice minima

- NVIDIA, AMD, Intel; hardware encoder disponibile, occupato e assente.
- 1080p60, 1440p high-FPS, 4K60; SDR e HDR; finestra/monitor, resize, minimize, cambio monitor.
- Replay RAM/disco, save durante recording, pausa/resume, exit durante recording e update.
- SSD locale, disco lento/pieno, destinazione irraggiungibile; audio endpoint disconnect/reconnect.
- Librerie 100/1.000/10.000 clip; thumbnail presenti/assenti; UI visibile/nascosta.

### Dati da raccogliere

- CPU engine + UI + processi WebView2; private bytes, working set, handle count nel tempo.
- GPU 3D/copy/video encode, frametime del gioco p50/p95/p99 e capture-to-encode latency.
- Readback/memcpy/sws/encode/mux p95/p99, non solo medie.
- Frame duplicati/persi, packet error, queue depth/overflow, keyframe e durata reale conservata.
- Discontinuity audio e massimo offset A/V durante almeno un'ora e dopo recovery.
- I/O bytes/operazioni, spazio replay temporaneo, query/stat per reload, tempo prima card.

### Strumenti e gate

Windows Performance Recorder/Analyzer, GPUView dove utile, Process Explorer e log perf esistenti. FFprobe per codec/timebase/PTS/duration/color/tracce; decode completo delle clip con FFmpeg per intercettare errori. Test automatici prioritari: dipendenze payload, replay limitato senza video, segmenti keyframe, trim timebase/preroll/multitraccia, IPC lifetime/limiti, update firme/rollback, delete/rename fault injection, librerie grandi.

Non risultava una suite automatica completa nei percorsi esaminati. Le asserzioni sui sorgenti eseguite qui non sono un sostituto di questi test.

## 12. Conclusione

Il difetto storico delle DLL è identificato e le regole di inclusione corrette erano già nel checkout; questa sessione ha rafforzato i gate che devono impedirne la ricomparsa. Serve ancora costruire e provare il pacchetto Windows per dichiarare risolto l'avvio dell'installazione dell'utente.

Per migliorare davvero l'app, le priorità non sono micro-ottimizzazioni isolate: **risorse limitate, timestamp corretti, I/O fuori dai thread realtime e libreria caricata su richiesta**. Dopo queste correzioni, il percorso GPU-residente è la principale opportunità architetturale per ridurre CPU e impatto sulla fluidità del gioco. Tutte le raccomandazioni dell'audit restano non applicate.

---

## Phase 1 progress / evidence - R1, I1, I2 (implementation; review pending)

Only phase 1 is implemented in this working tree. The original audit above is
preserved verbatim; its historical “not applied” statements describe the audit
session, not this appended implementation status. No commit, push, release,
version bump, DLL-workflow rewrite, or user `*.original.md` edit was made.

### Source-proven causes and fixes

- **R1:** original `ReplayBuffer::Impl::can_purge()` required `keyframes > 2`,
  and both byte/time eviction loops depended on it. Audio-only input, no keys,
  a single large GOP or two keys could therefore exceed the caps. Production
  retention now lives in `libs/replay-buffer/packet_ring.h`, exercised directly
  by the standalone tests and used by `ReplayBuffer` under its existing mutex.
  Hard payload capacity is checked with overflow-safe arithmetic; zero/negative
  or overflowing capacities and nonpositive durations disable retention.
  Empty packets are rejected, and a separate **262144 packet cap** bounds
  packet/arrival/timestamp-index metadata. Retention starts only at a video key.
  Pressure removes entire oldest GOPs, including the last GOP if necessary;
  no audio or dependent video is admitted while waiting for the next key.
  An oversized/rejected video packet conservatively invalidates the ring to
  avoid retaining a chain with missing references. Oversized audio is dropped.
- Age enforcement uses both steady-clock residence and the maximum observed
  DTS watermark. A minimum-DTS index detects old packets even behind newer
  arrivals; too-late packets are rejected, so front/back arrival ordering is
  not assumed for the bound. A far-future/skewed stream can conservatively
  shorten/empty replay (tested); this is not a reorder or clock-repair policy.
  Expiration occurs on push, configure, stats/count/memory reads and snapshot,
  not on a new timer: when entirely unused, bounded expired storage may remain
  allocated until the next operation, but cannot be saved as a stale replay.
  Existing zero packet/keyframe stats and empty save result expose unavailability.
- **Memory accounting:** `logical_bytes` is encoded payload in the current RAM
  ring, not private bytes. The single-flight save snapshot shares payload but
  can pin up to one previous ring capacity while the live ring refills; at a
  fixed 512 MiB cap those two holders can therefore retain up to 1 GiB payload,
  plus bounded packet/index/snapshot metadata, allocator overhead, mux buffers
  and unrelated producer/recording memory. Lowering the cap cannot revoke an
  already-running snapshot. No total-process RAM guarantee or benchmark is made.
- **I1:** original accepted connections appended `std::thread` objects forever;
  completion removed only the socket. The accept loop now owns a bounded vector
  of stable client records and joins/removes completed workers every loop,
  including during its 100 ms accept polling interval without new connections.
  No detach is used. Socket close/shutdown ownership shares a lock; start/stop
  serialize lifecycle; stop first joins the nonblocking accept owner, then
  shuts clients and joins workers outside their socket lock. Accepted-client
  allocation/thread-creation and accept-thread creation failures close owned
  sockets rather than leaking or escaping a joinable thread.
- **I2:** original `buf += tmp` grew until LF without a bound, backlog 8 did not
  cap accepted clients, and response `send()` ignored short writes. Named
  limits now enforce **16 accepted workers**, **64 KiB per line** (LF excluded,
  optional CR included), and **JSON depth 64**. Excess clients/oversized lines
  are disconnected. Parsing uses explicit received byte lengths, preserving
  embedded NUL as invalid input. Malformed/deep JSON returns parse error.
  Partial requests get a **30-second first-byte deadline**, not reset by trickle
  traffic; nonblocking send-all has a **30-second total response deadline**.
  There is **no idle timeout**, preserving the Stream Deck shared connection
  and its source-confirmed 5000 ms polling. One callback runs at a time per
  client; pipelined requests are processed sequentially without a growing queue.

### Executed Linux evidence

The root and library CMake files and existing test conventions were inspected;
no native automated suite was present. `tests/phase1` is standalone, adds no
fetched dependency, and uses this host's existing nlohmann-json 3.12.0 package.
IPC tests compile the actual server with small POSIX/Win32 test adapters; replay
tests use the actual production ring with synthetic EncodedPacket payload sizes.

```sh
cmake -S tests/phase1 -B /tmp/monolith-phase1-build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS='-Wall -Wextra -Wpedantic -Werror'
cmake --build /tmp/monolith-phase1-build --parallel
ctest --test-dir /tmp/monolith-phase1-build -V
cmake -S tests/phase1 -B /tmp/monolith-phase1-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS='-Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build /tmp/monolith-phase1-asan --parallel
ctest --test-dir /tmp/monolith-phase1-asan --output-on-failure
```

- Normal: **2/2 passed**, 67.34 s; sanitizer: **2/2 passed**, 69.67 s,
  with no reported AddressSanitizer/UndefinedBehaviorSanitizer diagnostics.
- Replay output: `PASS replay_packet_ring: keyframe starvation, byte/age/count bounds, skew, snapshots, invalid limits`.
- IPC output: `PASS ipc_server: short sends, persistent idle, parse/size/depth limits, 250 reconnects, 16-client cap, trickle deadline, reclamation, 20 shutdown races`.
  Also executes a non-reading-peer send deadline, send interruption and disconnect.
- An initial strict build failed on a missing initializer in the new test;
  it was corrected before the successful runs. No production Windows build
  was attempted or claimed. No dependencies were installed.

### Review and remaining gates

R1/I1/I2 are implemented with Linux regression evidence, **not yet independently
accepted or Windows-verified**. Still required: MSVC `/W4 /permissive-` build,
Winsock disconnect/partial-write/stop races, Windows handle/private-byte soaks,
actual Stream Deck/UI coexistence, FFmpeg decode/save and slow-save validation.
Allocation/thread-creation failure cleanup is implemented but not fault-injected.
In-flight application callbacks (including trim/DB/UI close) remain synchronous;
transport cancellation cannot impose a shutdown deadline on those callbacks.
Sixteen idle local clients can occupy the bounded capacity indefinitely; resource
bounds are not fairness/rate-limiting/authentication guarantees (I3 stays pending).
R2 clip selection/reordering, disk replay R3/R4 and save/mux integration are not
silently marked fixed by the new retention index.

All other audit IDs remain pending: **V1-V8, R2-R12, T1-T4, U1-U6, I3, A1-A7**,
plus the evolutionary experiments in section 9. Parent review is required before
any phase-2 work or authorization to proceed.

---

## Phase 2A progress / evidence - R2, R3, R4, T1-T4 (implementation; review pending)

Phase 1 was independently reviewed and accepted by the parent before this stage;
its earlier “review pending” text is preserved as historical evidence. This stage
implements only the authorized phase 2A media/retention work and the specifically
approved **backend-only configurable disk budget**. Phase 2B durability is not
started. The quota-interrupted partial implementation was inspected and resumed,
not reapplied blindly; the subsequent retry completed tool execution normally.
No alternate model/provider/CLI, nested agents, installs, commits, pushes,
releases or version bumps were used. User-authorized ROADMAP/original-file
removals and the existing DLL/phase-1 changes remain intact.

### Source validation and implemented mapping

- **R2:** confirmed the old `find_clip_start` used `ring.back()` and terminated
  its arrival-order scan at the first cutoff crossing; sorting only at mux time
  could not recover excluded packets. `PacketRing::snapshot()` now scans the
  whole bounded ring, uses the newest video DTS and a temporal keyframe boundary,
  filters the selected interval and sorts before save. Future audio cannot end
  the key search; late old packets do not become a non-key video start. Audio
  tail selection includes retained video presentation timestamps. Stats use
  actual minimum/maximum DTS, not deque endpoints. RAM mux uses one common video
  presentation anchor for every stream, preserving composition delay and A/V
  offsets, with write/finalization errors returned as a failed save. Phase-1
  hard caps remain; pathological clock jumps may still conservatively shorten
  retention, not repair producer clocks.
- **R3:** confirmed `save_clip()` closed the current segment and the next push
  opened a file even for audio/non-key video; opening also reset the start-time
  fields used by rotation. A new disk segment now opens **only** on a video key.
  Audio/inter frames before that key (including after a mid-GOP save) are dropped
  and counted. Segment start is explicit, not a zero-valued timestamp sentinel.
  One DTS origin preserves each stream's PTS/DTS offset; each file records the
  container-origin adjustment needed for correct concatenation. Late prior-GOP
  packets cannot precede the new segment's decode origin.
- **R4:** confirmed purge returned immediately while `saving` was true, retaining
  every newly written segment throughout slow copy/callback work. Disk records
  now have shared ownership: only snapshot inputs stay pinned; expired unpinned
  records continue to be deleted. Inputs are unpinned only after concat closes
  readers, **before** a potentially slow catalog/thumbnail callback. Clear and
  teardown cancel cooperative FFmpeg I/O and join before releasing inputs. Failed
  saves remove their partial output; failed output/segment unlink is retained,
  charged, retried and blocks further admission/save creation. Only exact owned
  files and empty owned session directories are deleted, never recursively.
- **T1:** confirmed input-unit timestamps were written unchanged after a mux
  header could change timebases; independent stream anchors and a DTS clamp
  erased timing information. Trim/concat now rescale PTS, DTS **and duration**
  after header, then subtract the same exact video anchor expressed in each
  destination timebase. No DTS-to-PTS clamp is used. Concat positions are based
  on original segment timeline offsets, not accumulated requested trim lengths;
  incompatible stream parameters fail explicitly.
- **T2:** confirmed the recorder stored `requested_end-requested_start` and
  retimed bookmarks from requested start despite lossless keyframe preroll.
  `TrimResult` returns the actual retained source presentation anchor and the
  finalized, probed output duration. The recorder persists those values and
  uses the shared `retime_bookmark()` mapping; bookmark read/update failures are
  reported instead of ignored. Finalized output stream count/codec/layout are
  checked before success. This does **not** make filesystem replacement and DB
  updates transactional; that remains phase 2B.
- **T3:** confirmed pre-start packets were dropped before decode and frame
  counters/coarsened integer FPS replaced source timing. Fallback now decodes
  preroll, filters presentation-ordered decoded frames (including drain), and
  sends original-precision timestamps/timebase to the encoder. A bounded-memory
  decoder planning pass determines actual frame boundaries before incremental
  encode/audio-copy. Fractional/VFR timing is preserved rather than coerced to CFR.
- **T4:** confirmed whole-clip `vpkt`/`apkt` vectors and `av_find_best_stream`
  selecting only one audio track. They are removed. Fallback uses a matching
  software encoder for the input video codec (H.264/HEVC/AV1), and copies **all**
  audio streams unchanged when muxable. Unsupported codec/container/pixel format
  fails clearly, leaving original replacement unperformed. Video/audio packets
  are muxed incrementally with `av_write_frame`, avoiding a duration/skew-sized
  libavformat interleaver queue. Decoder/encoder threads are bounded. No silent
  alternate-codec or single-audio fallback remains.

### Approved settings/resource and precision decisions

- Persisted setting: **`replay_buffer.disk_budget_mb`**, typed recorder member
  `replay_disk_budget_mb`; default **2048 MiB**, valid integer range
  **512..65536 MiB**. Missing/malformed/float/bool/overflow/out-of-range legacy
  values use the default before integer narrowing. Seed config, native load/save
  and existing live reload/facade plumbing are wired. Rust's existing generic
  JSON-section persistence needs no special-case change. **Settings UI control
  is explicitly deferred to phase 5**, per user decision; no frontend edits.
- This budget counts live + pinned encoded payload. Fixed safeguards: **256 MiB
  segment payload**, **4096 segment records**, **512 MiB free-space reserve for
  producer admission**. One retryable failed-save output may need an additional
  record. Container/index/allocator overhead and successful/in-progress save
  output are extra; this is not a process-RAM or total-filesystem cap.
- Reductions govern the next admission/snapshot and evict unpinned state there;
  configure itself performs no new mux/unlink I/O on the message loop. Existing
  pinned input may temporarily exceed a newly lowered budget. New writes stop
  rather than deleting pinned inputs or restarting manual recording. Drop,
  pressure and deletion-failure stats feed explicit recorder error logging.
- Lossless remains the default **keyframe-aware**, not exact, trim. It retains
  the preceding key and the decode-order prefix needed for in-range B frames;
  future reference frames can extend the requested end (potentially sparse
  beyond the requested interval). Metadata reports actual retained duration.
  Audio remains **packet-boundary**, not sample-exact, with offsets preserved.
- Matroska may omit initial video DTS. A bounded **256-packet / 64 MiB** lookahead
  recovers those DTS from the next known decode timestamp and intervening input
  packet durations in the same timebase. Unresolvable prefixes fail explicitly;
  no arbitrary timestamp clamp or integer-FPS reconstruction is used.

### Executed Linux evidence

Existing FFmpeg CLI/development packages were available; no installation/download
was needed. Standalone `tests/phase2` follows phase 1 rather than changing root
Windows CMake. It compiles the real trim/mux/RAM/disk sources. The small Win32
adapter covers only ASCII fixture paths/time/directories; codec/mux/decode and
filesystem execution are **real Linux integration**. A linker gate pauses the
actual save mux call for deterministic slow-reader/cancellation testing.

```sh
cmake -S tests/phase2 -B /tmp/monolith-phase2-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/monolith-phase2-build --parallel
ctest --test-dir /tmp/monolith-phase2-build --output-on-failure
cmake -S tests/phase2 -B /tmp/monolith-phase2-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build /tmp/monolith-phase2-asan --parallel
ctest --test-dir /tmp/monolith-phase2-asan --output-on-failure
cmake --build /tmp/monolith-phase1-build --parallel
ctest --test-dir /tmp/monolith-phase1-build --output-on-failure
```

- Host: GCC 16.2.1; FFmpeg n9.0.1, libavformat/libavcodec 63.1.101,
  libavutil 61.1.101; existing nlohmann-json 3.12.0.
- Final phase-2 strict build + normal tests: **2/2 PASS, 8.30 s**.
- Final phase-2 ASan/UBSan: **2/2 PASS, 8.89 s**, no sanitizer diagnostics.
- Reused phase-1 regressions: **2/2 PASS, 67.35 s**.
- Media test: 16 MP4/MKV lossless/reencode combinations, non-key cuts, 90/120 kHz
  and millisecond timebases, fractional/VFR B frames, two offset audio tracks,
  mixed-timebase concat, exact bookmark anchor mapping, source-preserving
  unsupported fallback/output-open failure, skewed arrival replay, mid-GOP disk
  save/restart, >100 seconds of new media during a pinned save, cancellation and
  cleanup, failed unlink accounting, budget JSON boundaries, and a **60-second,
  >50 MiB CBR source** incrementally reencoded with all audio and fully decoded.
- Independent ffprobe/frame-hash script output:
  `PASS timeline: 16 trims, lossless frame hashes, fractional/VFR frame PTS/count, audio codec/payload/offsets, actual duration`.
- During development a deprecated test-only path converter failed strict compile
  and was replaced with an explicit ASCII adapter. Tests exposed end-past-file
  acceptance, which was corrected. The lossless frame assertion was refined to
  allow only verified source reference frames beyond the requested end, while
  requiring every requested presentation frame. VFR validation sinks were set
  to demux timebase/passthrough rather than introducing CFR duplicates themselves.
  Final tests above passed; no failed intermediate run is presented as success.

### Remaining gates and pending IDs

**Phase 2A awaits independent review.** No Windows/MSVC, Win32 locking/replacement,
actual settings UI/runtime reload or SQLite transaction execution is claimed.
Also unexecuted: full 512 MiB/4096-record saturation, HEVC/AV1 encoder matrix,
allocation/thread-failure injection, real storage-device stalls, process-memory
benchmarks and crash/power-loss recovery. Cooperative FFmpeg cancellation cannot
interrupt arbitrary blocked filesystem syscalls or completion callbacks instantly.
Existing packet encode sinks/disk finalization/probing remain synchronous (R5).

Phase 2B must address original replacement/temp-path collision and filesystem/DB
catalog/bookmark durability; T2 here fixes timeline values/error reporting only.
All other audit IDs remain pending: **V1-V8, R5-R12, U1-U6, I3, A1-A7**, plus
section-9 experiments and the **phase-5 disk-budget Settings UI control**. No
phase-2B or later work is authorized by this completion report.

Accounting clarification: incremental mux removes whole-clip encoded-packet
vectors and bounds application lookahead; libavformat container indexes/cues
remain library-owned metadata and can scale with output length. No flat total
process-RAM bound is claimed. Sanitizers instrumented the production sources in
the test target, not a rebuilt/instrumented copy of the system FFmpeg libraries.

---

## Phase 2B entry checkpoint - reopened phase-2A concat continuity (review pending)

Full report and CLAUDE.md read before writing. Parent's retained LOW review had
accepted phase 2A with a request for continuous-clock disk-save evidence. That
new executable fixture **reopened acceptance**: uninterrupted source audio lost
packets at internal segment boundaries, independently of the older pressure
fixture's deliberate clock resets. Initial 9-second/two-track reproduction failed
at stream 1 packet 279: expected source DTS 5.952s, output DTS 6.016s; strict build
passed, media failed, dependent timeline test was not run. This is a concrete
regression, not merely a diagnostic from injected reset/priming overlap.

Supervisor narrowed this checkpoint to fix/test that regression, then stop before
starting the larger 2B recovery architecture. R7/R8/U6 are **not implemented**.
Original audit and accepted working-tree changes were preserved.

### Fix and executable coverage

- `libs/encoding/trim.cpp`: concat now supplies an explicit audio window in each
  segment's source timeline. Internal boundaries follow segment ownership (video
  DTS), not each new key's delayed video presentation timestamp. Only the outer
  first boundary uses the actual retained video presentation anchor; the final
  boundary retains existing actual video extent. Packet rescaling/common output
  anchor, B-frame video decode window, `TrimResult`, multitrack handling and
  standalone trim semantics remain unchanged. A SQLite transaction is not involved.
- `tests/phase2/media_test.cpp`: new continuously encoded 21-second fractional/VFR
  B-frame source with 48kHz and offset 44.1kHz AAC tracks, multiple internal segment
  joins. Saved audio payloads must match consecutive source packets, DTS must be
  strictly increasing, intervals and common intertrack offset agree within 2ms
  Matroska rounding. Tail omission is bounded to five outer audio packets. Also
  save mid-GOP, decode it, clear the retained window and check continuity of the
  next-key-started output. No continuity across the intentional post-save key-wait
  gap is claimed. Existing reset/pressure fixtures remain distinct, unchanged.
- Existing 16 trim timeline/frame-hash/all-audio tests still pass; no assertion
  was weakened to accommodate the reproduced loss.

Executed on existing Linux tools/build directories (no installs):

```sh
cmake --build /tmp/monolith-phase2-build --parallel
ctest --test-dir /tmp/monolith-phase2-build --output-on-failure
cmake --build /tmp/monolith-phase2-asan --parallel
ctest --test-dir /tmp/monolith-phase2-asan --output-on-failure
cmake --build /tmp/monolith-phase1-build --parallel
ctest --test-dir /tmp/monolith-phase1-build --output-on-failure
```

Final results: phase2 **2/2 PASS, 11.71s**; ASan/UBSan **2/2 PASS, 11.34s**
(no sanitizer diagnostics); phase1 **2/2 PASS, 67.51s**. Before the fix the new
payload assertion failed twice (second run added exact packet diagnostics).
No Windows/MSVC/pinned-vcpkg or Rust host validation is claimed.

### Approved next architecture, deliberately not partially implemented

Source revalidation confirmed recording MAX_PATH/second-only names, discarded
exit bookmarks, detached catalog jobs consuming global bookmarks, C++/Rust
DB-before-unlink delete, rename-before-SQL and destructive trim-before-bookmark
updates. Parent approved engine-only delete/rename RPC routing (offline library
readable, mutations require running engine, no silent launch/direct fallback),
an engine-owned versioned catalog recovery journal, and a narrow phase3
prerequisite: bounded owned recording control/completion worker with per-session
immutable data, recoverable persistence, explicit accepted vs durable results,
and orderly drain. Full packet mux writer/backpressure remains R5 phase3.
Before implementation inspect all other Rust mutation/reconcile bypasses and
coordinate or route conflicting writes; no single-writer guarantee yet exists.
No recovery schema, worker, routing change or partial durability protocol was
introduced in this checkpoint. All requested 2B filesystem/SQL/crash/exit fault
injection tests remain pending with their implementation.

Required next: retained LOW independent re-review of this focused regression
fix, then resume authorized R7/R8/U6 and the narrow lifecycle prerequisite.
**Phase5 Settings UI remains pending** for persisted disk budget, default 2048MiB.

---

## Phase 2B U6 progress - incomplete, not accepted

A subsequent writer began the approved engine-only delete/rename routing and an
operation-specific SQLite journal implementation. The Rust UI delete/rename
commands now use recorder JSON-RPC and do not fall back/autostart when offline.
The storage implementation adds `file_mutation_journal` prepared/done stages,
quarantine moves and startup compensation/cleanup. This entry is deliberately
not a resolution of U6: it has no production SQLite/filesystem fault-injection
suite, no Windows build, and file identity protection remains incomplete. The
existing `libs/storage/file_mutation.h` scaffold was not a working API and is
not evidence of recovery. R7/R8/trim replacement remain open.

## U6 pragmatic durability completion

U6 routes UI delete/rename exclusively through recorder JSON-RPC; offline engine returns its RPC error. `storage` now journals leaf-only delete/rename operations before no-overwrite moves, uses `.monolith-mutations` quarantine for deletes, commits catalog/tag/bookmark changes with journal stage, and recovers prepared/committed records at open. Ambiguous paths and invalid journal records fail closed. Reconcile removes a missing-video row transactionally. A named per-catalog Windows mutex plus recursive in-process lock guards recovery and mutations.

Executed: Linux production-storage shim test (`tests/u6`), phase2 media tests, `cargo check --offline`, and `git diff --check`. This is not a filesystem/SQLite atomicity proof. Later hardening still needs stable file identity, reparse-point policy, Windows runtime validation, cross-process behavior validation, and broader crash/fault coverage. R7/R8 remain pending.
