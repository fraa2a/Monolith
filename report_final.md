# Report finale + Piano di implementazione — Clip a durata/velocità doppia (2×)

> Stato: **causa identificata, soluzione a lungo termine definita.** Questo documento
> sostituisce/estende `report.md`. Pronto per implementazione.

---

## 1. Sintesi esecutiva

**Sintomo.** Le clip salvate dal replay buffer hanno la durata totale corretta (fissata
dall'audio), ma i **fotogrammi video** sono compressi nella **prima metà** della clip e
scorrono a **velocità doppia**. Riproducibile su AMD (`hevc_amf`) e NVIDIA (`h264_nvenc`).

**Causa radice (confermata dall'analisi del codice).** Il PTS video è un **contatore
sintetico** (`next_pts++`, un incremento per ogni `push_bgra`), in timebase `1/fps`. La
correttezza di questo schema dipende interamente dall'assunzione che il **pacer consegni
esattamente `fps` frame al secondo reali**. Questa assunzione è **falsa**: il pacer
(`pacer_thread_proc`) non è agganciato a un orologio reale — fa `WaitForSingleObject(interval_ms)`
e per ogni tick esegue, in sequenza, una copia dell'intero frame BGRA (~8 MB a 1080p),
una `sws_scale` (conversione colore, costosa) e l'encode. Il tempo di lavoro per tick si
somma all'attesa, e la risoluzione di default del timer di Windows (~15,6 ms) arrotonda le
attese brevi. Il risultato è che il pacer emette **circa la metà** dei frame attesi al
secondo, ma li **etichetta come se fossero a cadenza piena**.

**Perché esattamente questo sintomo:**

| Osservazione | Spiegazione |
|---|---|
| Video a 2× | N frame reali su `T` secondi vengono etichettati con passo `1/fps` come se fossero `N/fps` secondi. Se `N ≈ (fps/2)·T`, la durata video risulta `T/2` → 2× di velocità. |
| Video solo nella prima metà | La durata video = `(numero frame)/fps = T/2`; gli ultimi frame finiscono a metà timeline. |
| Audio corretto su tutta la durata | L'`AudioEncoder` **non** usa un contatore sintetico: avanza `next_pts += frame_size` sui **campioni reali** consumati (timebase `1/sample_rate`, `encoding.cpp:634`). Quindi l'audio dura esattamente il wall-clock reale e fissa la durata del contenitore. |
| Indipendente dal vendor encoder | Il difetto è a monte degli encoder, nel pacing/timestamping, non nell'encoder. |

**Conclusione.** Il problema **non** è nel muxing (`replay_buffer.cpp` e `recording.cpp`
sono corretti) né nel timebase dell'encoder. È un problema di **clock**: il timestamp video
non è ancorato al tempo reale. La correzione "OBS-style" del commit `a0e255a` ha copiato la
*formula* di OBS (`dts_usec = dts·1e6/fps`) ma non la *precondizione* che la rende valida —
ovvero un **CFR agganciato a un orologio reale**.

---

## 2. Perché il fix `a0e255a` non bastava

OBS è corretto perché il suo `video_output` garantisce un **CFR real-time-accurate**: a ogni
iterazione calcola, da un orologio monotòno ad alta risoluzione, quanti slot-frame sono
trascorsi e **duplica/scarta** frame per emetterne esattamente `fps` al secondo. Solo *grazie
a questa precondizione* la formula `dts_usec = dts·1e6/fps` e `frame->pts = frame_number`
risultano corrette.

Monolith ha adottato la formula ma il pacer **non** garantisce la precondizione: un
incremento di PTS per ogni iterazione del loop, indipendentemente da quanto tempo reale è
passato. Appena il loop va più lento del previsto (cosa garantita dal costo copy+scale+encode
e dalla granularità del timer), PTS e wall-clock divergono → errore di velocità.

---

## 3. Architettura della soluzione a lungo termine

Replicare fedelmente il modello OBS, in tre parti. La **Parte A** è la correzione risolutiva;
B e C sono difese di robustezza/correttezza che eliminano intere classi di bug correlati.

### Parte A — Pacer CFR agganciato a orologio reale (risolutiva)

Riscrivere il pacer in modo che il **numero di frame emessi sia funzione del tempo reale
trascorso**, non del numero di iterazioni del loop.

- All'avvio: `start_qpc = QueryPerformanceCounter()`, `qpc_freq = QueryPerformanceFrequency()`.
- A ogni iterazione: `target = llround((now_qpc - start_qpc) * fps / qpc_freq)`.
  Questo è il numero di frame che *dovrebbero* esistere a questo istante.
- Emettere frame finché `frames_emitted < target`, usando **l'ultimo frame disponibile**
  (duplicazione) quando non ne arrivano di nuovi, e **saltando** i frame in eccesso se il loop
  recupera. `frame->pts = frames_emitted` (rimane un contatore, ma ora **clock-locked**).
- Mantenere `next_pts` lato encoder coerente con questo contatore (il PTS arriva dal pacer).

Effetto: dopo `T` secondi reali sono stati emessi **esattamente** `T·fps` frame con PTS
corretti → velocità e durata corrette **indipendentemente** da jitter del timer, costo
di encode o FPS configurato. La formula `dts_usec = dts·1e6/fps` esistente torna valida.

Migliorie di pacing incluse:
- `timeBeginPeriod(1)` / `timeEndPeriod(1)` attorno alla vita del pacer per ridurre il jitter.
- Sostituire `WaitForSingleObject(interval)` con un **waitable timer ad alta risoluzione**
  (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) o un'attesa con target assoluto.
- Eliminare il troncamento intero `1000/fps` (a 60 fps dà 16 → +4% di velocità anche con timer
  perfetto): lavorare in QPC/microsecondi.

### Parte B — Timebase di output dell'encoder autoritativo (difensiva)

Per blindare il caso (ipotizzato in `report.md`) in cui un encoder HW riscrive
`ctx->time_base` dopo `avcodec_open2`:

- Dopo `avcodec_open2`, **leggere** `impl_->ctx->time_base` e memorizzarlo come `pkt_tb`
  autoritativo.
- Usare `pkt_tb` (non `cfg_fps` assunto) per: `ep.tb_num/tb_den`, il calcolo di `ep.dts_usec`,
  e `vsp.tb_num/tb_den`.
- Il muxer (`write_clip`, `recording.cpp`) già rescala `src_tb → dst_tb` con
  `av_packet_rescale_ts`; passandogli il timebase reale, qualunque scelta dell'encoder viene
  gestita correttamente.

> Nota: con la Parte A attiva, il sintomo 2× è risolto anche senza B. La Parte B è una
> garanzia di correttezza che rende il pipeline immune a encoder "creativi" e va comunque
> implementata. Il log diagnostico (`monolith_enc_diag.txt`) serve a confermare quale timebase
> riportano effettivamente `h264_nvenc`/`hevc_amf`, ma **non è bloccante** per il fix.

### Parte C — Allineamento A/V con offset comune (difensiva, sync)

Bug latente trovato durante l'analisi: in `write_clip` (`replay_buffer.cpp:326–348`) gli offset
PTS sono calcolati **indipendentemente per ogni stream** (ogni stream azzerato sul proprio
primo pacchetto). Lo stesso accade in `recording.cpp` (`input_anchor` per-stream). Se il primo
pacchetto video (un keyframe dopo il trim) e il primo pacchetto audio non corrispondono allo
stesso istante reale, A e V vengono forzati entrambi a 0 → **disallineamento A/V** fino alla
differenza di trim.

Correzione (modello OBS): scegliere un **unico riferimento temporale comune** (es. il minimo
`dts_usec` tra *tutti* i pacchetti mantenuti) e sottrarre a ogni stream l'equivalente di quel
riferimento nel proprio timebase, invece di azzerare ciascuno sul proprio primo pacchetto.

---

## 4. Piano di implementazione (file per file)

### 4.1 `libs/encoding/encoding.h`
- `VideoEncoder::push_bgra(...)`: aggiungere parametro timestamp opzionale, es.
  `void push_bgra(const uint8_t* bgra, int stride, int width, int height, int64_t pts);`
  (oppure mantenere la firma e gestire il PTS internamente se il pacer resta sorgente del
  contatore — vedi 4.3). Scelta consigliata: **il pacer fornisce il PTS** (frame index
  clock-locked), così la sorgente di verità del tempo è una sola.

### 4.2 `libs/encoding/encoding.cpp`
1. **Parte B:** in `VideoEncoder::Impl` aggiungere `AVRational pkt_tb{0,0};`.
   Dopo `avcodec_open2` riuscita (intorno a `encoding.cpp:262–270`):
   ```cpp
   impl_->pkt_tb = impl_->ctx->time_base;          // timebase reale di output
   impl_->vsp.tb_num = impl_->pkt_tb.num;
   impl_->vsp.tb_den = impl_->pkt_tb.den;
   ```
   Mantenere comunque un fallback a `{1, cfg_fps}` se `pkt_tb` è degenere (`num<=0||den<=0`).
2. In `drain_video` (`encoding.cpp:309–340`): calcolare `dts_usec` e `tb_num/tb_den` da
   `impl_->pkt_tb` invece di `cfg_fps`:
   ```cpp
   ep.dts_usec = av_rescale_q(pkt->dts, impl->pkt_tb, AVRational{1,1000000});
   ep.tb_num = impl->pkt_tb.num;
   ep.tb_den = impl->pkt_tb.den;
   ```
3. **Parte A (lato encoder):** `push_bgra` usa il PTS fornito dal pacer
   (`frame->pts = pts_in;`) invece di `next_pts++`. Mantenere `next_pts` solo come fallback.

### 4.3 `app/recorder/src/main.cpp` — riscrittura del pacer (cuore del fix)
- `PacerFrame`: aggiungere `int64_t timestamp_qpc = 0;` (propagare dal WGC callback, che già
  ha `f.timestamp_qpc`).
- `pacer_push_frame(...)`: aggiungere param `int64_t qpc` e salvarlo in `g_pacer_shared`.
- `pacer_start()`:
  - `timeBeginPeriod(1);`
  - salvare `g_qpc_freq = QueryPerformanceFrequency()`, `g_pacer_start_qpc = QueryPerformanceCounter()`, `g_frames_emitted = 0`.
  - creare un waitable timer ad alta risoluzione (fallback a `WaitForSingleObject` se non
    disponibile su OS più vecchi).
- `pacer_thread_proc()`: sostituire "una emissione per tick" con il loop CFR clock-locked:
  ```cpp
  int64_t now = QueryPerformanceCounter();
  int64_t target = llround(double(now - start_qpc) * fps / qpc_freq);
  while (g_frames_emitted < target) {
      g_video_enc.push_bgra(local_bgra..., g_frames_emitted); // PTS = indice frame
      g_frames_emitted++;
  }
  ```
  (duplica l'ultimo frame quando non ne arrivano di nuovi; se `target` salta avanti, recupera).
- `pacer_stop()`: `timeEndPeriod(1);`, chiudere il timer.
- Correggere `g_pacer_interval_ms = 1000 / fps` → usare un periodo in QPC/µs senza
  troncamento (es. tick del waitable timer a `interval_100ns = 10'000'000 / fps`).

### 4.4 `libs/replay-buffer/replay_buffer.cpp` — Parte C
- In `write_clip` (`:326–387`) sostituire gli offset per-stream con un **offset comune**:
  calcolare `int64_t ref_usec = min(dts_usec)` su tutti i pacchetti mantenuti, poi per ogni
  pacchetto `pkt->pts/dts -= av_rescale_q(ref_usec, {1,1000000}, src_tb)` prima del
  `av_packet_rescale_ts`. In alternativa, mantenere offset per-stream ma derivarli tutti da
  `ref_usec` comune (stesso istante reale per ogni stream).

### 4.5 `libs/recording/recording.cpp` — Parte C (coerenza)
- Applicare lo stesso principio di offset comune in `write_packet` (`:196–225`): l'anchor non
  deve essere per-stream indipendente ma derivato da un riferimento condiviso al primo
  pacchetto in assoluto, così la registrazione manuale ha lo stesso A/V sync corretto.

### 4.6 Pulizia
- Rimuovere il logging diagnostico (`diag_log`, `[enc_diag] ...`) da `encoding.cpp` una volta
  validato il fix, **oppure** metterlo dietro una variabile d'ambiente per debug futuri.

---

## 5. Piano di validazione

1. **Conferma empirica della causa (opzionale ma consigliata):** generare una clip con il
   logging attuale attivo e ispezionare `%TEMP%\monolith_enc_diag.txt`:
   - se i `dts` video sono `0,1,2,...` ma molti meno di `fps·durata`, conferma "pacer
     sotto-consegna" (Parte A).
   - annotare il `time_base` reale di `h264_nvenc`/`hevc_amf` per dimensionare la Parte B.
2. **Test di velocità/durata:** registrare una scena con un cronometro a schermo o un
   metronomo. La clip salvata deve avere video a velocità 1× e durata == configurata.
   Ripetere a **30/60/120 fps** e con buffer **10/30/60 s**.
3. **A/V sync (Parte C):** clap/flash test — un evento audio+visivo simultaneo deve restare
   allineato (< 1 frame) nella clip.
4. **Carico:** verificare a 1080p/1440p/4K che il pacer mantenga il CFR (duplicazione sotto
   carico anziché rallentamento) e che la CPU resti accettabile.
5. **Regressione manual-recording:** confermare che `recording.cpp` produca file corretti
   (durata, velocità, sync) dopo la Parte C.
6. **Cross-encoder:** ripetere 2–4 su NVENC, AMF e fallback `libx264` (sw) per assicurare
   l'indipendenza dal vendor.

---

## 6. Riepilogo file coinvolti

| File | Modifica | Parte |
|---|---|---|
| `app/recorder/src/main.cpp` | Riscrittura pacer CFR clock-locked, `timeBeginPeriod`, waitable timer, propagazione QPC | A |
| `libs/encoding/encoding.cpp` | PTS dal pacer; `pkt_tb` autoritativo; `dts_usec`/`tb` da `pkt_tb` | A, B |
| `libs/encoding/encoding.h` | Firma `push_bgra` con PTS | A |
| `libs/replay-buffer/replay_buffer.cpp` | Offset A/V comune in `write_clip` | C |
| `libs/recording/recording.cpp` | Offset A/V comune in `write_packet` | C |
| `libs/capture/capture.*` | Nessuna modifica (QPC già presente) | — |
| `libs/audio/audio.*` | Nessuna modifica (timing già corretto) | — |

---

## 7. Ordine di esecuzione consigliato

1. **Parte A** (pacer CFR) — risolve il 2× da solo. Validare con §5.2.
2. **Parte B** (timebase autoritativo) — blindatura encoder. Validare con log §5.1 + cross-encoder §5.6.
3. **Parte C** (offset A/V comune) — corregge il sync latente. Validare con §5.3 + §5.5.
4. **Pulizia** logging diagnostico.
