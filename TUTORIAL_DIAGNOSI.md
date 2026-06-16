# Tutorial: Analisi dei log diagnostici — Bug durata clip 2×

## Prerequisiti

- Monolith compilato con il logging diagnostico **già inserito** (commit corrente)
- `Sysinternals DebugView` ([download](https://learn.microsoft.com/en-us/sysinternals/downloads/debugview)) — **OPZIONALE**, solo se vuoi vedere i log in tempo reale

---

## Passo 1: Generare una clip con logging

1. **Lancia Monolith** normalmente (tray icon)
2. **Configura** durata buffer = 10 secondi (più breve = diagnosi più rapida)
3. **Lascia registrare** il buffer per almeno 15-20 secondi (così il buffer ha materiale per salvarsi)
4. **Salva una clip** (tasto rapido o menu contestuale)
5. **Chiudi Monolith** (questo forza il flush del file di log)

---

## Passo 2: Trovare il file di log

Il diagnostico scrive in:

```
%TEMP%\monolith_enc_diag.txt
```

Apri Explorer e incolla `%TEMP%` nella barra degli indirizzi. Cerca `monolith_enc_diag.txt`.

In alternativa, apri PowerShell:

```powershell
notepad "$env:TEMP\monolith_enc_diag.txt"
```

> **Attenzione:** ogni avvio di Monolith **appende** al file. Se fai più test, svuotalo prima:
> ```powershell
> Clear-Content "$env:TEMP\monolith_enc_diag.txt"
> ```

---

## Passo 3: Leggere il log

### 3.1 Timebase dopo avvio encoder

Cerca righe con `[enc_diag] video time_base after avcodec_open2`:

```
[enc_diag] video time_base after avcodec_open2: 1/60  (cfg_fps=60, encoder=hevc_amf)
                                    ^^^^
                                    Questo è ciò che conta
```

| Cosa vedi | Significato |
|-----------|-------------|
| `1/60` | L'encoder HA MANTENUTO il nostro timebase |
| `1/1000000` | L'encoder è passato a microsecondi (tipico NVENC/AMF) |
| `1/120` | L'encoder ha DOPPIATO il timebase (possibile 2×?) |
| `1001/60000` | Timebase NTSC-like per 59.94fps (raro) |

### 3.2 Timebase audio (controllo)

```
[enc_diag] audio time_base after avcodec_open2 (track 1): 1/48000  (cfg_sample_rate=48000)
```

Se l'audio è `1/48000` (atteso), conferma che il problema è **solo video**.

### 3.3 Primi pacchetti video

```
[enc_diag] video pkt #0: dts=0 pts=0 dts_usec=0 cfg_fps=60
[enc_diag] video pkt #1: dts=X pts=Y dts_usec=Z cfg_fps=60
```

**Formula usata per dts_usec:** `floor(pkt->dts × 1'000'000 / cfg_fps)`

| Scenario | dts #0 | dts #1 | dts_usec #1 | Diagnosi |
|----------|--------|--------|-------------|----------|
| ✅ Normale | 0 | 1 | 16.666 | Timebase non cambiato → fix OBS OK, bug altrove |
| ⚠️ DTS riscalati | 0 | 16666 | 277.766.666 | Timebase cambiato a {1,1000000}, encoder riscala! |
| ❌ DTS = 0 sempre | 0 | 0 | 0 | Encoder non produce DTS validi → time cap NON funziona |
| ❓ DTS duplicati | 0 | 0 | 0 | `avcodec_send_frame` torna EAGAIN, PTS non incrementato |

### 3.4 Primi pacchetti audio (controllo)

```
[enc_diag] audio pkt #0 (track 1): dts=0 pts=0 dts_usec=0 sr=48000
[enc_diag] audio pkt #1 (track 1): dts=1024 pts=1024 dts_usec=21333 sr=48000
```

**Valori attesi:** `dts` incrementa di `frame_size` (tipicamente 1024 per AAC), `dts_usec` = `1024 × 1M / 48000 = 21333μs` = ~21ms. Se l'audio è corretto e il video no, il problema è nell'encoder video.

---

## Passo 4: Catturare con DebugView (opzionale)

Se preferisci vedere i log in tempo reale senza cercare il file:

1. **Scarica** [DebugView](https://learn.microsoft.com/en-us/sysinternals/downloads/debugview)
2. **Avvia** `DbgView.exe` come **amministratore** (per catturare output kernel e win32)
3. **Filtro** (CTRL+L): includi `enc_diag`
4. **Genera la clip** in Monolith
5. I log appaiono in tempo reale in DebugView
6. **Salva** (CTRL+S) il log come file

---

## Passo 5: Cosa inviarmi

Il contenuto del file `%TEMP%\monolith_enc_diag.txt` (puoi copiarlo direttamente). In alternativa, uno screenshot di DebugView con le righe `[enc_diag]`.

---

## Riferimenti

| File | Cosa contiene |
|------|---------------|
| `libs/encoding/encoding.cpp:262-270` | Log timebase video dopo `avcodec_open2` |
| `libs/encoding/encoding.cpp:325-335` | Log pacchetti video in `drain_video()` |
| `libs/encoding/encoding.cpp:478-487` | Log timebase audio dopo `avcodec_open2` |
| `libs/encoding/encoding.cpp:529-540` | Log pacchetti audio in `drain_audio()` |
| `libs/encoding/encoding.cpp:23-31` | Funzione `diag_log()` che scrive il file |
