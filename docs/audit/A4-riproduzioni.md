# A4 — Riproduzioni, harness e matrice di regressione

Appendice di [README.md](README.md). Qui sta la prova sperimentale di ogni
affermazione marcata **[RIPR]** nel resto dell'audit.

## A4.1 Come è costruito l'harness

- Sorgente: [repro/harness.c](repro/harness.c). Include **le translation unit
  reali** `main.c`, `controller.c`, `predfit.c` (con `#define main sv_app_main`),
  così le funzioni `static` dell'app sono eseguite senza copie: `set_predictions`,
  `add_spectrum`, `ensure_aux_loaded`, `handle_mouse_down`,
  `run_right_click_peak_find`, `assign_selected_predictions`, `write_inputs`,
  `advanced_commit_edit`, `import_fit_lines`, `predfit_restore_latest`, ecc.
  `loader.c`, `layout.c`, `settings.c`, `view.c`, `intensity_fit.c`,
  `algorithms.c`, `ui_icons.c` sono compilati invariati; solo il renderer ImGui
  è sostituito da no-op ([repro/plotgpu_stub.c](repro/plotgpu_stub.c)), perché
  nulla viene disegnato.
- **Parti copiate** (perché `main()` non è richiamabile): il calcolo del layout
  (`compute_layout`, copia di [main.c:469-511](../../main.c#L469-L511)) e il
  consumo delle code `pending_*` (`pump`, copia di
  [main.c:514-522](../../main.c#L514-L522) e [530](../../main.c#L530)).
- **Click reali**: la selezione nella previsione e il pulsante *Save all*
  passano da `handle_mouse_down` con coordinate calcolate dalla stessa geometria
  dell'app (`update_sidebars`, `ui_as_save`); il picco è preso da
  `run_right_click_peak_find` su uno spettro sintetico.
- **Chiamate dirette** (confine della simulazione): in alcuni scenari la
  selezione è impostata scrivendo `selected_indices` e chiamando
  `assign_selected_predictions` (è esattamente ciò che fa
  [controller.c:681-693](../../controller.c#L681-L693) + [832](../../controller.c#L832)).
- **Isolamento**: ogni scenario fa `chdir` in una propria cartella di lavoro
  temporanea e imposta `settings.data_dir` lì; i file del repository sono solo
  letti (`pred.cat`, `.fit/model.cat`, `assignments_backup.txt`). SPCAT e SPFIT
  sono quelli rilevati dall'app (`autodetect_program`
  [settings.c:248-276](../../settings.c#L248-L276)): `~/Desktop/Programmi_SP/calpgm/{spcat,spfit}`.
- Fixture sintetiche: [repro/gen_fixtures.py](repro/gen_fixtures.py), record
  SPCAT a larghezza fissa come in `calcat.c:700-709` (vedi [README §6.1](README.md#61-cat-esterno-e-modelcat)).
- Esecuzione completa: `sh docs/audit/repro/run_all.sh [cartella]`
  ([repro/run_all.sh](repro/run_all.sh)). I log di questa esecuzione sono in
  [repro/logs/](repro/logs/) (percorsi temporanei sostituiti da `$RUNS`,
  `$FIXTURES`, `$REPO`). Il log di uno scenario corretto da un passo di
  [PIANO-FIX.md](PIANO-FIX.md) è sostituito da quello dell'esecuzione dopo quel
  passo (lo scenario lo indica con **Dopo il passo #n**); gli altri restano quelli
  del commit `1f4df65`.
- Indice AST: [repro/ast_index.py](repro/ast_index.py) genera
  [A1](A1-funzioni.md) e [A2](A2-campi.md) dall'AST JSON di clang.
- **Suite di regressione** (dal passo #0 di [PIANO-FIX.md](PIANO-FIX.md)):
  [tests/test_audit.c](../../tests/test_audit.c) è costruito come l'harness
  (include `main.c`, `controller.c`, `predfit.c` con `#define main sv_app_main`,
  linka gli altri `.c` dell'app e [tests/plotgpu_stub.c](../../tests/plotgpu_stub.c)).
  - `make test` compila `tests/test_audit` ed esegue tutti i test;
    `./tests/test_audit <nome> …` esegue solo quelli indicati; `-v` mostra anche
    ciò che l'app scrive su stdout. Per ogni test la suite stampa PASS, FAIL o
    SKIP e, sotto, le righe "atteso …, ottenuto …"; il codice d'uscita è ≠ 0 se
    c'è almeno un FAIL.
  - Isolamento: ogni test gira in un processo figlio, in una cartella creata con
    `mkdtemp` che è sia la cartella corrente sia `settings.data_dir`; il
    repository è solo letto. La cartella di un test fallito resta su disco e il
    suo percorso viene stampato.
  - Fixture: all'avvio [tests/gen_fixtures.py](../../tests/gen_fixtures.py)
    genera i CAT sintetici (gli stessi di [repro/gen_fixtures.py](repro/gen_fixtures.py))
    in una cartella temporanea; [tests/fixtures/pred_reference.cat](../../tests/fixtures/pred_reference.cat)
    è la copia di `pred.cat` e [tests/fixtures/lin_with_nqn.txt](../../tests/fixtures/lin_with_nqn.txt)
    quella di `assignments_backup.txt` (i nomi `pred.*` sono esclusi da `.gitignore`).
  - SPCAT e SPFIT sono quelli trovati da `autodetect_program`; se mancano, i test
    che li richiedono risultano SKIP.
  - Come nell'harness, il layout (`compute_layout`) e il consumo delle code
    (`pump`) sono copie di `main.c`; clic, trascinamento destro e *Save all*
    passano da `handle_mouse_down/motion/up` con coordinate calcolate dalla
    geometria dell'app.
- **Sanitizer**: tutti gli scenari sono stati rieseguiti con UBSan
  (`-fsanitize=undefined,float-divide-by-zero,float-cast-overflow`): l'unico errore è
  la divisione per zero di [controller.c:1062](../../controller.c#L1062) (B-31)
  ([repro/logs/ubsan/](repro/logs/ubsan/), `_exits.txt` e `pass3.txt`). Guard Malloc
  (`/usr/lib/libgmalloc.dylib`, anche con `MALLOC_PROTECT_BEFORE=1`) su qnfmt, rt3,
  formats, intfit, saveloss e peakfinder: l'unico crash è la lettura prima del buffer
  di Find peaks con larghezza negativa (B-30, exit 139)
  ([repro/logs/gmalloc/](repro/logs/gmalloc/)). AddressSanitizer non è utilizzabile su
  questo sistema (Darwin 25.5): il processo si blocca già all'avvio del runtime
  (`AsanInitInternal` → `StaticSpinMutex::LockSlow`) anche prima di eseguire codice
  dell'app, quindi il blocco non dipende dall'app.

## A4.2 Scenari

Ogni scenario riporta precondizioni, azioni, **oracolo** (che cosa deve restare
identico / che cosa deve cambiare) e osservato.

### R-01 — Parsing di QNFMT e NQN ([qnfmt.log](repro/logs/qnfmt.log))
- Input: `pred.cat` (2312 righe, QNFMT `303`), `.fit/model.cat` (141 righe, QNFMT `1404`),
  fixture a 3/4/5/6 QN, QN in codice lettera, catalogo con spazi finali rimossi.
- Azione: `read_pred_cat_alloc` e istogramma di `n_qn` per decina di J superiore.
- Oracolo: `n_qn` = QNFMT % 10 per **ogni** riga; J=105 letto come 105; il numero di
  righe lette non dipende dagli spazi finali.
- Osservato:
  ```text
  pred.cat      Ju 0..9 -> n_qn=3 (542)   10..19 -> n_qn=1 (1174)   20..29 -> n_qn=2 (594)   30..39 -> n_qn=3 (2)
  5999.2867: n_qn=1 U=(11 0 11 0 0 0) L=(10 1 9 0 0 0)
  model.cat     tutte n_qn=4 (141)
  cat3_303      J=5 ->3, J=11 ->1, J=25 ->2, J=45 ->4, J=72 ->0
  cat4_304      N=3 ->4, N=12 ->1        cat5_305  N=4 ->5, N=15 ->1        cat6_306  N=2 ->6, N=13 ->1
  cat4_1404     sempre 4
  cat_letter    J=105 ("A5") -> U=(0 3 0 ...)
  cat_trim      0 righe lette
  ```
- Esito: **1768/2312 righe di `pred.cat` (76,5 %) hanno NQN sbagliato.** Bug B-01, B-03, B-04.
- **Dopo il passo #1** (log rigenerato): `pred.cat` 2312 righe tutte con `n_qn=3`
  (542 / 1174 / 594 / 2 per decina di J), la riga 5999.2867 con `n_qn=3`;
  `model.cat` 141 righe con `n_qn=4`; fixture 303, 304, 305, 306 con `n_qn` 3, 4, 5, 6
  su ogni riga; `cat_letter` J=105 → U=(105 3 102); `cat_trim` 5 righe lette.
  Test: `test_cat_nqn_from_qnfmt_3qn`, `test_cat_nqn_4_5_6`,
  `test_cat_letter_and_negative_qn`, `test_cat_trailing_spaces_irrelevant`.

### R-17 — Stesso difetto su un `model.cat` generato a specie singola ([qnfmt_single_species.log](repro/logs/qnfmt_single_species.log))
- Input: `model.cat` prodotto da SPCAT via Pred&Fit con 1 specie (`.par` "s 1 1 0" → QNFMT `303`).
- Osservato: J 10..19 → `n_qn=1` (872), 20..29 → 2 (917), 30..39 → 3, 40..49 → 4 (59).
- Esito: il difetto non riguarda solo i CAT esterni; è nascosto nella sessione
  corrente solo perché 3 specie forzano QNFMT `1404`.
- **Dopo il passo #1** (log rigenerato): tutte le 3350 righe con `n_qn=3`
  (543 / 872 / 917 / 959 / 59 per decina di J).

### R-02..R-07 — Round-trip completo catalogo → selezione → picco → assignment → *Save all* → riavvio
Log: [rt3](repro/logs/rt3.log), [rt304](repro/logs/rt304.log), [rt1404](repro/logs/rt1404.log),
[rt5](repro/logs/rt5.log), [rt6](repro/logs/rt6.log), [rtpred](repro/logs/rtpred.log).
- Azioni reali: click nel pannello previsione, `run_right_click_peak_find` su picco
  sintetico (obs − calc = −0,03 MHz), click reale su *Save all*, nuovo `AppState`,
  `set_predictions` (che chiama `ensure_aux_loaded` → `load_existing_assignments`).
- Oracolo: dopo il riavvio la lista deve contenere le **stesse transizioni** (tutti i QN,
  stesso NQN), le stesse frequenze osservate e calcolate e la stessa intensità calcolata.
- Osservato (estratti):
  ```text
  rt3   file:  11 10 ... 5979.970000 5980.000000 7.943282E-05 1
               11 10 ... 5999.256700 5999.286700 1.364897E-04 1
               25  3 24  3 ... 2
               45  2 43  0 44  2 42  0 ... 4        <- quarto QN fittizio 0
        riavvio: 6 in memoria -> 5 ripristinati, 3 transizioni non identiche
                 (le due J=11 fuse in "11 <- 10"; J=25 troncata; J=72 n_qn 0 -> 3)
  rt304 N=12 (F=13 e F=12) scritte "12 11 ... 1" -> fuse in una
  rt1404 4/4 identiche
  rt5, rt6 righe con N>=10 scritte con 1 QN per stato
  rtpred coppia sovrapposta 392.7959 (13 6 7<-14 5 9, 13 6 8<-14 5 10) selezionata con un click,
         scritta due volte "13 14 ... 1", ripristinata come UNA "13 <- 14"
  exp_int dopo il riavvio = intensità calcolata (1.000e+00 -> 1.000e-04)
  ```
- Esito: bug B-01 (causa), B-05 (writer), B-06 (fusione per deduplicazione), B-07 (`exp_int`).

### R-08 — Campo N (NVIB) della terza riga `.par` ([nvib.log](repro/logs/nvib.log))
- Azioni: modifica della riga opzioni nella finestra Advanced (`advanced_commit_edit`),
  `add_species`, `write_inputs`, modifica a mano della sessione e del `.par`, riavvio.
- Oracolo: il valore digitato dall'utente (o scritto a mano nel file) resta.
- Osservato:
  ```text
  1 specie, digitato 's 1 2 0'  -> 's 1 1 0'
  + specie                      -> 's 1 2 0', 's 1 3 0'
  3 specie, digitato 's 1 1 0'  -> 's 1 3 0'     's 1 5 0' -> 's 1 3 0'
  sessione a mano 'hamiltonian s 1 1 0' -> predfit_load_session -> 's 1 3 0'
  model.par a mano 's 1 1 0'   -> al Calculate successivo 's 1 3 0'
  ```
- Esito: bug B-02 (è il "valore riportato a 3" del report).

### R-09 — Formati accettati da `load_existing_assignments` ([formats.log](repro/logs/formats.log))
- Oracolo: ogni formato storico si legge senza scambiare i campi; ciò che non è un
  record valido viene rifiutato.
- Osservato:
  ```text
  new_nqn4           corretto
  legacy16_nqn3      corretto
  legacy16_nqn6      exp=4.0000 U=(3300 2 1 1 3 4)            <- preso per il nuovo formato (16 = 2*6+4)
  legacy14_ei1e-5    exp=0.3375 f=4.0000 U=(1 4 0 0 0 3) L=(1 3 0 0 0 2511)   <- sscanf a 16 campi
  legacy14_ei5.2     exp=0.0000 n_qn=5 ... lin=2.5113e+03     <- ei arrotondato a 5 = NQN
  assignments_backup.txt (.lin + NQN, 422 righe) -> 399 assignment, CalcFreq=0.01 (incertezza),
                     CalcInt=1.0 (peso), 7 righe con exp > 90000 MHz (sentinella non decodificata)
  ```
- Esito: bug B-08.

### R-10 — Selezione stantia dopo un nuovo catalogo ([stale.log](repro/logs/stale.log))
- Azioni: selezione della riga 5999.2867 di A (indice 2), caricamento di B, picco.
- Oracolo: dopo il cambio di catalogo la selezione è vuota oppure punta ancora alla stessa transizione.
- Osservato: l'indice 2 punta a `6033.5894 12 2 10 2 <- 11 2 9 2` di B; l'assignment
  creato a 5999.25 MHz porta quella transizione. Bug B-10.
- **Dopo il passo #3** ([stale.log](repro/logs/stale.log) rigenerato): dopo il caricamento
  di B `n_selected = 0` e il picco non crea alcun assignment. La riga "now points to" del
  log è solo la lettura della vecchia cella dell'array fatta dall'harness: l'app non la
  usa più. Test: `test_selection_cleared_on_catalog_change`,
  `test_selection_indices_in_bounds`.

### R-11 — Pred&Fit consuma assignment di un CAT esterno con NVIB=3 ([pfmix.log](repro/logs/pfmix.log))
- Precondizioni: 3 specie (NVIB forzato 3), Calculate con SPCAT reale (11167 righe, tutte `n_qn=4`);
  4 assignment da `model.cat`, 4 da `pred.cat` (2 con J<10 `n_qn=3`, 2 con J≥10 `n_qn=1`); Fit con SPFIT reale.
- Oracolo: ogni assignment inviato a SPFIT è letto con i suoi QN, oppure l'app lo rifiuta
  con un messaggio; lo stato finale riporta righe rifiutate.
- Osservato:
  ```text
  model.lin   4  1  4  0  3  1  3  0   2511.333800 ...      (8 campi)
              8  2  7  7  2  6         5034.507700 ...      (6 campi)
             11 10                     6905.790500 ...      (2 campi)
  model.fit  Bad Line(  5):   8  2  7  7  2  6  0  0   5034.50770
             Bad Line(  6):   9  1  9  8  1  8  0  0   5650.30790
                 7:  11 10  0  0  0  0  0  0   6905.79050  142775.91517 -999.99999
             2 bad lines · 2 Lines rejected from fit
  stato app  "SPFIT stopped after 2/50 iterations; MICROWAVE RMS = 0.000274 MHz"
  ```
- Esito: 4 assignment su 8 ignorati in silenzio. Bug B-12 (+ B-01, B-02).

### R-12 — Effetti collaterali del fit delle intensità ([intfit.log](repro/logs/intfit.log))
- Precondizioni: catalogo generato con 2 specie, concentrazione della specie 1 = 0,1;
  6 assignment della specie 0 su picchi sintetici proporzionali al modello.
- Azioni: click reale su *Run fit*; poi T rot digitata nella command bar; poi la stessa
  T nel pannello Pred&Fit.
- Oracolo: il fit non modifica il modello Pred&Fit né le intensità delle altre specie
  senza un'azione di "applica"; la stessa temperatura inserita da due punti dà lo stesso catalogo.
- Osservato:
  ```text
  Run fit        rot 2.000 -> 2.023; species0.T e predfit.temp_k -> 2.023
  riga specie 1  rapporto lin/base  0.10 -> 0.98       (concentrazione persa)
  T rot=2 (command bar)   rapporto 1.0
  T rot=2 (Pred&Fit)      rapporto 0.1
  ```
- Esito: bug B-11, B-13.

### R-13 — Quale file ricostruisce la lista al riavvio ([restore.log](repro/logs/restore.log))
- Precondizioni: `data_dir` ≠ CWD; 3 assignment, il secondo escluso dal fit; *Save all*; `write_inputs(for_fit)`.
- Oracolo: la lista ripristinata è la stessa in entrambe le modalità di avvio, con frequenze
  calcolate, intensità ed esclusioni.
- Osservato:
  ```text
  avvio SENZA .cat  lista da model.lin: pred f=0.0000, lin=0, exp_int=0; esclusione conservata
  avvio CON .cat    lista da data_dir/assignments.txt: frequenze e intensità presenti; fit=1 per tutte (esclusione persa)
  ```
- Esito: bug B-15, B-16.
- **Dopo il passo #2** ([restore.log](repro/logs/restore.log) rigenerato): senza `.cat` la
  lista viene da `data_dir/assignments.txt`, con frequenze calcolate e intensità, e
  l'esclusione è conservata (dal `.lin`); con `.cat` la lista è la stessa ma
  l'esclusione è persa (B-16, passo #8). Test: `test_restore_reads_data_dir_list`.

### R-14 — Undo dopo una modifica della lista ([undo.log](repro/logs/undo.log))
- Azioni: esclusione di `4 0 4 <- 3 0 3`, Fit, cancellazione della riga 0, Undo.
- Oracolo: dopo l'Undo l'esclusione resta sulla stessa transizione.
- Osservato: dopo l'Undo `4 0 4` è incluso e `5 1 5 <- 4 1 4` è escluso. Undo esegue solo SPCAT.
  Bug B-17.

### R-15 — Sentinella 90000 MHz e incertezza `.lin` ([sent001.log](repro/logs/sent001.log), [sent05.log](repro/logs/sent05.log))
- Azioni: una riga esclusa; Fit con incertezza 0,01 MHz e con 0,5 MHz.
- Oracolo: la riga esclusa non entra nel fit per nessun valore ammesso dell'incertezza.
- Osservato:
  ```text
  0.01 MHz   "***** NEXT LINE NOT USED IN FIT", RMS 0.000354 MHz
  0.5  MHz   riga 93139 MHz usata; "Fit Diverging"; RMS ~30000 MHz; A 1151.36 -> 24198.65 MHz; C -> 7385.9 MHz
  ```
- Esito: bug B-09.

### R-16 — Restore di `.int` e specie ([rint.log](repro/logs/rint.log))
- Precondizioni: specie attiva 1 con Tred=5 K e μ=(0,4 0,3 0,5); TCAT esplicita 1 K; FQLIM e MAXV automatici.
- Oracolo: dopo il riavvio le specie hanno i propri valori e i campi automatici restano automatici.
- Osservato:
  ```text
  prima    int{temp=1 fqlim=0 maxv=-1}; specie1 T=5 mu=(0.4 0.3 0.5)
  dopo     int{temp=1 fqlim=8 maxv=1};  specie1 T=1 mu=(0.75 0.21 1.14)   <- Tcat e dipoli dello stato 0
  + specie MAXV resta 1 (lo stato 2 non verrà calcolato da SPCAT)
  ```
- Esito: bug B-18.

### Secondo e terzo passaggio (R-18…R-40)

Stessa costruzione: translation unit reali, cartelle temporanee, SPCAT/SPFIT reali
dove servono. Gli scenari che passano da eventi SDL (R-24, R-38) inizializzano solo
il sottosistema eventi (`SDL_INIT_EVENTS`) e inviano gli eventi con `SDL_PushEvent`
a `handle_app_events`, lo stesso gestore del ciclo principale.

### R-18 — Cartella dati con spazi ([spaces.log](repro/logs/spaces.log))
- Azione: `data_dir` = `$RUNS/with space`, modello a una specie, Calculate.
- Oracolo: SPCAT eseguito, `model.cat` prodotto.
- Osservato:
  ```text
  sh: line 0: cd: $RUNS/with: No such file or directory
  Calculate -> 0, status: SPCAT failed (exit 256).
  model.var written: yes; model.cat produced by SPCAT: no
  ```
- Esito: bug B-28, M-01.

### R-19 — Rimozione di uno spettro che precede quello attivo ([remove_active.log](repro/logs/remove_active.log))
- Azione: spettri A, B, C; B attivo con offset 0,5 MHz; `remove_spectrum(A)` (la × della riga A).
- Oracolo: resta attivo B, con il suo offset.
- Osservato: `after removing A: n=2 active=1 (C.txt), mirrored exp_path=…/C.txt, offset=0.00`.
- Esito: bug B-29.

### R-20 — Find peaks ([peakfinder.log](repro/logs/peakfinder.log), [gmalloc/peakfinder.before.log](repro/logs/gmalloc/peakfinder.before.log))
- Azione: `run_peak_finder` su 2000 punti con righe alte 5 ogni 97 punti; finestra di
  rumore 10 e 1000; stesse righe su baseline 10; larghezza −5, anche con Guard Malloc.
- Oracolo: la finestra di rumore cambia la soglia; lo stesso spettro spostato in
  verticale dà gli stessi picchi; una larghezza negativa viene rifiutata.
- Osservato:
  ```text
  baseline 1:  noise window 10 -> 21 peaks, noise window 1000 -> 21 peaks
  baseline 10 (senza rumore) -> 0 peaks
  larghezza -5 con libgmalloc e MALLOC_PROTECT_BEFORE=1 -> exit 139 (SIGSEGV)
  ```
- Esito: bug B-30.

### R-21 — Pan verticale con la sola previsione ([divzero.log](repro/logs/divzero.log), [ubsan/divzero.log](repro/logs/ubsan/divzero.log))
- Azione: solo `cat3_303.cat`; layout calcolato come in `main` (`exp_h = 0`); tasto W; poi uno spettro.
- Oracolo: `vymin`/`vymax` finiti.
- Osservato: UBSan `controller.c:1062:51: runtime error: division by zero`; `vymax=-inf`,
  anche dopo il caricamento dello spettro.
- Esito: bug B-31.

### R-22 — *Run fit* delle intensità che fallisce ([fitfail.log](repro/logs/fitfail.log))
- Azione: modello a 2 specie (concentrazione della seconda 0,1), Calculate; spettro
  lontano da tutte le righe; clic reale su *Run fit* senza assignment; poi 3 assignment
  fuori dalla traccia, μ red b azzerato, di nuovo *Run fit*.
- Oracolo: un fit rifiutato non cambia intensità né μ red.
- Osservato:
  ```text
  rapporto della specie 1: 0.100 -> 1.000 dopo "Need spectrum, catalog, and at least 2 assignments."
  mu red (0.750 0.000 1.140) -> (0.750 0.210 1.140) dopo "No 2 positive assigned areas in the active trace."
  ```
- Esito: bug B-32.

### R-23 — Dipoli nei tre campi ([mu_input.log](repro/logs/mu_input.log))
- Azione: μb = 0 e μc = −1,141963 nel pannello Pred&Fit, in Advanced › Species e in
  Intensity analysis (commit reali di `commit_text_input` e `advanced_commit_edit`).
- Oracolo: la stessa regola ovunque, con messaggio sui valori rifiutati.
- Osservato: Pred&Fit rifiuta 0 e il negativo; Advanced rifiuta il negativo; Intensity
  analysis accetta entrambi e propaga a Pred&Fit e alla specie il negativo, non lo 0.
- Esito: bug B-33.

### R-24 — Drop di tre spettri insieme ([multidrop.log](repro/logs/multidrop.log))
- Azione: tre `SDL_DROPFILE` nella stessa coda, `handle_app_events`, consumo della coda come in `main`.
- Oracolo: 3 spettri caricati.
- Osservato: `n_spectra = 1: s3.txt`.
- Esito: bug B-34.

### R-25 — `work_dir` all'avvio e *Restore defaults* ([workdir.log](repro/logs/workdir.log))
- Azione: sequenza di `main` (`init_app_defaults`, `settings_init`,
  `settings_apply_defaults`) con `data_dir` impostata; poi `settings_restore_defaults`
  (pulsante *Restore defaults*).
- Oracolo: `work_dir` = `data_dir/.fit`; programmi e cartella dati conservati.
- Osservato: `predfit.work_dir = '.fit'` contro `'$RUNS/n2/workdir/.fit'`; dopo il
  ripristino `spcat='' spfit='' data_dir=''`.
- Esito: bug B-35, B-36.

### R-26 — Estensione `.CAT` maiuscola ([uppercase.log](repro/logs/uppercase.log))
- Azione: `cat3_303.cat` copiato come `PRED.CAT`; `path_looks_like_cat`; caricamento come farebbe il drop.
- Oracolo: riconosciuto come catalogo.
- Osservato: `path_looks_like_cat = 0`; caricato come spettro di 6 punti, primo punto (3000,1; 0,001).
- Esito: bug B-37.

### R-27 — Parametri con ID 0 e duplicati ([param0.log](repro/logs/param0.log), [paramdup.log](repro/logs/paramdup.log))
- Azione: modello a una specie, 6 assignment; *+ parameter* seguito da Esc (ID 0),
  oppure *+ parameter* con ID 10000; Fit.
- Oracolo: la riga viene rifiutata prima di SPFIT.
- Osservato: ID 0 → NPAR 4, SPFIT fitta un parametro `0` (`-1(-2147483648)E-15`);
  ID 10000 doppio → RMS 2566 MHz, "Fit Diverging: restore parameters".
- Esito: bug B-38.

### R-28 — Avvio con un `.cat` e modello Pred&Fit ([clicat.log](repro/logs/clicat.log))
- Azione: sessione 1 con A, B, C e DJ (ID 200, incertezza 0), Calculate. Sessione 2a:
  avvio con `pred.cat` e uno spettro (sequenza di `main`: `predfit_load_session`,
  `set_predictions`, `add_spectrum`, salvataggio della sessione). Sessione 3a: riavvio
  senza `.cat`. Sessione 2b: avvio con `.cat` e Calculate.
- Oracolo: stessi parametri e incertezze in tutte le sessioni; `model.var` mai riscritto con i default.
- Osservato:
  ```text
  2a: A B C = 10000 1000 900, n_param = 3
  3a: param 200 = -7e-06, error 1      (era 0: fissato)
  2b: model.var con 10000 / 1000 / 900
  ```
- Esito: bug B-39.

### R-29 — Riavvio da un'altra cartella e *Save all* ([saveloss.log](repro/logs/saveloss.log))
- Azione: sessione 1 in `data_dir` con 3 assignment da `cat3_303.cat` (J = 5, 11, 25),
  *Save all* reale, `write_inputs(1)`, `model.cat` presente. Sessione 2 con CWD diversa
  da `data_dir` e senza `.cat`: restore, poi *Save all* reale.
- Oracolo: le 3 righe di `assignments.txt` sopravvivono.
- Osservato: sessione 2 con 1 assignment (frequenza prevista 0, `exp_int` 0);
  `assignments.txt` riscritto con una riga.
- Esito: bug B-40.
- **Dopo il passo #2** ([saveloss.log](repro/logs/saveloss.log) rigenerato): sessione 2
  con 3 assignment; *Save all* riscrive le stesse 3 righe. Dal passo #1 questo scenario
  non produce più righe troncate: il caso con i file troncati scritti da `1f4df65` è
  coperto da `test_save_all_after_restore_no_loss` e `test_restore_keeps_short_lin_rows`
  (nessuna riga persa, righe corte marcate da riassegnare, copia `.bak` del file
  precedente).

### R-30 — Export del fit delle intensità dopo una cancellazione ([exportstale.log](repro/logs/exportstale.log))
- Azione: 6 assignment su uno spettro sintetico, `intensity_fit_run`, export;
  cancellazione dell'assignment 0; secondo export senza nuovo fit.
- Oracolo: export rifiutato, oppure valori ancora associati alle proprie transizioni.
- Osservato: ogni riga del secondo export porta i valori della riga che la precedeva nel primo.
- Esito: bug B-41.

### R-31 — Spettro in ordine decrescente ([descending.log](repro/logs/descending.log), [descending2.log](repro/logs/descending2.log))
- Azione: lo stesso spettro (righe a 3000,0 e 3001,0 MHz, alte 1 e 5) scritto in
  ordine crescente e decrescente; `set_predictions`, `add_spectrum`, trascinamento
  destro su 2999,8–3000,2 MHz; 2 assignment e `intensity_fit_run`.
- Oracolo: stessi risultati nei due ordini.
- Osservato:
  ```text
  binary_search_lower(2999.8) = 0, binary_search_upper(3000.2) = 1501
  crescente:   3000.0000 MHz; intensity fit 2/2 lines used
  decrescente: 3001.0000 MHz; "No 2 positive assigned areas in the active trace."
  ```
- Esito: bug B-42.

### R-32 — Rimozione e aggiunta di specie ([speciesdel.log](repro/logs/speciesdel.log))
- Azione: 3 specie (Mono v0, Donor v1, Accep v2) con costanti proprie, attiva Donor;
  clic reale sulla × della riga 0 in Advanced › Species; × dell'ultima riga; *+ species*.
- Oracolo: resta attiva Donor; la specie nuova parte dal seme della specie attiva.
- Osservato: dopo la prima rimozione è attiva Accep (A da 907,5 a 1100); la specie
  nuova (v = 2) ha A B C 1100/300/280, cioè le costanti di Accep.
- Esito: bug B-43.

### R-33 — Persistenza dell'incertezza `.lin` ([lineerr.log](repro/logs/lineerr.log))
- Azione: incertezza 0,05 MHz, salvataggio della sessione, nuovo stato caricato come all'avvio.
- Oracolo: 0,05 MHz.
- Osservato: 0,0100 MHz.
- Esito: bug B-44.

### R-34 — Colonna ERR dopo Fit e dopo Calculate ([errcol.log](repro/logs/errcol.log))
- Azione: modello a una specie, Calculate; 8 assignment, Fit; Calculate con gli stessi
  parametri; massimo della colonna ERR di `model.cat` a ogni passo.
- Oracolo: ERR coerente con le incertezze del modello corrente.
- Osservato: 999,9999 → 39,0301 → 999,9999 MHz.
- Esito: bug B-45.

### R-35 — Baseline e T rot ([baseline.log](repro/logs/baseline.log))
- Azione: 10 righe simulate a T rot = 2 K su una baseline costante pari a 0, 0,5 %,
  2 % e 10 % della riga più forte; `intensity_fit_run` con fit di T.
- Oracolo: T rot ≈ 2 K per ogni baseline.
- Osservato: 2,048 / 3,316 / 4,436 / 5,294 K.
- Esito: bug B-46.

### R-36 — Riassegnazione di una riga esclusa ([reassign.log](repro/logs/reassign.log))
- Azione: assignment escluso (`fit_enabled = 0`, come in Advanced › Lines), poi
  assegnato a un altro picco con lo stesso percorso della UI.
- Oracolo: resta escluso.
- Osservato: `fit_enabled = 1`.
- Esito: bug B-47.

### R-37 — Tempo di caricamento di `assignments.txt` ([perf.log](repro/logs/perf.log))
- Azione: file con 250, 500, 1000 e 2000 righe distinte; `load_existing_assignments`.
- Oracolo: tempo proporzionale al numero di righe.
- Osservato: 6,3 / 45,4 / 348,2 / 2706,1 ms.
- Esito: bug B-48.

### R-38 — Tastiera tra finestre ([textleak.log](repro/logs/textleak.log), [keyleak.log](repro/logs/keyleak.log))
- Azione: eventi con il `windowID` della finestra Advanced (7) o Settings (9), nessuna
  cella in modifica, elaborati da `handle_app_events`. (a) Campo Offset in modifica,
  testo "12" + Invio. (b) Nessun campo attivo, tasti Backspace, D, R.
- Oracolo: la finestra principale non cambia.
- Osservato:
  ```text
  (a) Offset principale = 12.0000 MHz
  (b) Advanced e Settings: n_peaks 2 -> 1; Intensity analysis 0 -> 1; vista 2999.900-3000.100 -> 2999.000-3001.000
  ```
- Esito: bug B-49, U-03.

### R-39 — Vista iniziale con catalogo e spettro ([yrange.log](repro/logs/yrange.log))
- Azione: spettro con y tra 1e-6 e 3,1e-5 e `cat3_303.cat` nei due ordini (riga di
  comando: prima il catalogo, [main.c:429-430](../../main.c#L429-L430); drop: prima lo spettro).
- Oracolo: asse Y sull'intervallo dello spettro in entrambi i casi.
- Osservato: catalogo → spettro `vymin=0 vymax=1`; spettro → catalogo `1e-06…3.1e-05`.
- Esito: bug B-50.

### R-40 — Cancellazione della riga A ([noA.log](repro/logs/noA.log), [noA2.log](repro/logs/noA2.log))
- Azione: modello a una specie; `delete_parameter(0)` (× della riga 10000); Calculate.
  In una seconda esecuzione A = 1151,3604 digitato nel pannello Pred&Fit prima di Calculate.
- Oracolo: il valore mostrato è quello usato, oppure Calculate viene rifiutato.
- Osservato: pannello A = 1151,3604, `model.var` con NPAR 2 (B, C); SPCAT: 5218 righe
  da 0,0001 MHz, "roll-over at K", stato "SPCAT complete"; l'A digitato non crea la riga.
- Esito: bug B-51.

## A4.3 Matrice di regressione proposta

Ogni riga è un test automatizzabile con lo stesso harness (o con test unitari
su `loader.c`) e ha un oracolo esplicito. "=" significa *deve restare identico*,
"Δ" *deve cambiare come indicato*. La colonna *Test* riporta la funzione di
[tests/test_audit.c](../../tests/test_audit.c) che copre il caso; è aggiornata a
ogni passo di [PIANO-FIX.md](PIANO-FIX.md).

| # | Caso | Input | Oracolo | Test |
|---|---|---|---|---|
| T-01 | CAT esterno 3 QN, J<10 e J≥10 | `cat3_303.cat`, `pred.cat` | = `n_qn` 3 su tutte le righe; = QN interi | `test_cat_nqn_from_qnfmt_3qn` |
| T-02 | CAT 4 QN (spin, QNFMT 304) e (stato, 1404) | `cat4_304.cat`, `cat4_1404.cat` | = `n_qn` 4; = F / v nel quarto campo | `test_cat_nqn_4_5_6`, `test_baseline_cat1404_nqn4` |
| T-03 | CAT 5/6 QN | `cat5_305.cat`, `cat6_306.cat` | = `n_qn` 5/6 | `test_cat_nqn_4_5_6` |
| T-04 | QN ≥ 100 e ≤ −10 (codice lettera) | `cat_letter.cat` + riga con `a1` | = J 105; = −11 | `test_cat_letter_and_negative_qn` |
| T-05 | Righe senza spazi finali | `cat_trim.cat` | = numero di righe lette | `test_cat_trailing_spaces_irrelevant` |
| T-06 | Transizioni uguali nei primi 3 QN, diverse nei successivi | `cat4_304` (F), `cat4_1404` (v), `cat5_305` | = due assignment distinti dopo save/reopen | — |
| T-07 | Save/reopen `assignments.txt` | R-02..R-07 | = QN, NQN, ObsFreq, CalcFreq, CalcInt | `test_baseline_roundtrip_qnfmt1404` (solo QNFMT 1404) |
| T-08 | Riassegnazione della stessa transizione a un altro picco | 2 picchi, 1 transizione | Δ solo ObsFreq; = numero di assignment | `test_baseline_reassign_updates_obsfreq` |
| T-09 | Blend: due transizioni allo stesso picco | coppia 392.7959 di `pred.cat` | = 2 assignment con la stessa ObsFreq; righe consecutive nel `.lin` | — |
| T-10 | CAT esterno prima e dopo Pred&Fit | `pred.cat` dopo Calculate | = `cat_temp_k`=0 e intensità grezze; = selezione vuota dopo il cambio | `test_selection_cleared_on_catalog_change`, `test_selection_indices_in_bounds` |
| T-11 | Riga opzioni `.par`: NVIB 1, 2, 3, 5 con 1 e 3 specie | Advanced | = valore digitato, oppure errore esplicito di incoerenza | — |
| T-12 | Calculate → Fit → Undo → Fit | modello 1 specie | = lista assignment; = flag di esclusione per transizione | — |
| T-13 | Più specie, specie esclusa (PRED off), rimozione specie | 3 specie | = `.int` coerente; = parametri della specie rimossa gestiti in modo esplicito | — |
| T-14 | Restore con e senza `.cat` sulla riga di comando, `data_dir` ≠ CWD | R-13 | = stessa lista nelle due modalità | `test_restore_reads_data_dir_list` |
| T-15 | Incertezza `.lin` 0,001–5 MHz con righe escluse | R-15 | = righe escluse fuori dal fit | — |
| T-16 | Assignment di CAT diverso dal modello → Fit | R-11 | Δ rifiuto esplicito (o conversione documentata); = nessuna riga "Bad Line" silenziosa | — |
| T-17 | Fit intensità con CAT esterno, senza Pred&Fit | `pred.cat` + spettro | = modello Pred&Fit invariato | — |
| T-18 | Fit intensità con `model.cat` multi-specie | R-12 | = concentrazioni delle altre specie; = stesso risultato qualunque sia il campo usato per T | — |
| T-19 | Restore `.int` con specie attiva ≠ 0 | R-16 | = Tred e μ di ogni specie; = campi automatici | — |
| T-20 | Legacy `assignments.txt` a 14/16 campi | R-09 | = campi letti correttamente o riga rifiutata con messaggio | — |
| T-21 | `data_dir` con spazi e metacaratteri di shell | R-18 | = Calculate e Fit riusciti; = nessuna parte del percorso interpretata dalla shell | — |
| T-22 | Rimozione di uno spettro prima e dopo quello attivo | R-19 | = spettro attivo per identità, con offset e smoothing | — |
| T-23 | Find peaks con baseline 0 e 10, rumore noto, larghezza ≤ 0 | R-20 | = stessi picchi al variare della baseline; Δ larghezza ≤ 0 rifiutata; = nessuna lettura fuori limite con Guard Malloc | — |
| T-24 | Pan verticale con la sola previsione | R-21 | = `vymin`/`vymax` finiti; = nessun errore UBSan | — |
| T-25 | *Run fit* che fallisce | R-22 | = intensità, μ red, T rot e specie di Pred&Fit invariati | — |
| T-26 | μ = 0 e μ < 0 nei tre campi | R-23 | = stessa regola e stesso messaggio in tutti i campi | — |
| T-27 | Drop di N file in un gesto | R-24 | = N file caricati | — |
| T-28 | `data_dir` impostata, avvio con `.cat` | R-25 | = `work_dir` = `data_dir/.fit` prima del primo Calculate | — |
| T-29 | *Restore defaults* | R-25 | = programmi SPCAT/SPFIT e `data_dir` invariati, oppure richiesta esplicita | — |
| T-30 | Estensioni `.CAT` e `.Cat` | R-26 | = aperti come cataloghi | — |
| T-31 | Parametro con ID 0 o duplicato | R-27 | Δ rifiuto con messaggio; = `.par` senza righe non valide | — |
| T-32 | Avvio con `.cat` dopo un Fit, poi Calculate | R-28 | = parametri e incertezze (anche "fissato") di `model.var`; = `model.var` mai riscritto con i default | — |
| T-33 | Riavvio da CWD ≠ `data_dir`, poi *Save all* | R-29 | = numero e contenuto delle righe di `assignments.txt` | `test_save_all_after_restore_no_loss` |
| T-34 | Export del fit delle intensità dopo una cancellazione | R-30 | Δ export rifiutato o ricalcolato; = aree associate alla transizione giusta | — |
| T-35 | Spettro in ordine decrescente | R-31 | = stessa frequenza misurata e stesse aree del file crescente | `test_baseline_right_drag_ascending` (solo file crescente) |
| T-36 | Rimozione di una specie prima dell'attiva; *+ species* dopo una rimozione | R-32 | = specie attiva per identità; = seme preso dalla specie attiva, o eredità dichiarata | — |
| T-37 | Incertezza `.lin` dopo il riavvio | R-33 | = valore digitato | — |
| T-38 | Colonna ERR di `model.cat` dopo Fit e dopo Calculate | R-34 | = ERR coerente con le incertezze fittate, o dichiaratamente a priori | — |
| T-39 | Aree con baseline da 0 a 10 % | R-35 | = T rot entro l'errore statistico | — |
| T-40 | Riassegnazione di una riga esclusa | R-36 | = esclusione conservata | — |
| T-41 | Caricamento di 5000 righe | R-37 | = tempo lineare (meno di 1 s) | — |
| T-42 | Tasti e testo in Advanced e Settings | R-38 | = stato della finestra principale invariato | — |
| T-43 | Avvio `spettro.txt catalogo.cat` con y ≪ 1 | R-39 | = asse Y sull'intervallo dello spettro | — |
| T-44 | Cancellazione della riga A | R-40 | Δ Calculate rifiutato o riga ricreata; = valore mostrato uguale al valore usato | — |
