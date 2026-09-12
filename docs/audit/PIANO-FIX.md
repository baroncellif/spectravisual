# Piano di correzione — SpectraVisual

Piano operativo per correggere i difetti trovati dall'audit del commit
`1f4df65` ([README.md](README.md)). Ogni passo segue lo schema
**bug/issue → cosa fare → test che devono passare → stato**. I passi sono
ordinati per impatto (danno × frequenza), con i prerequisiti prima.

## Istruzioni per chi esegue il piano (LLM o sviluppatore)

1. **Prima il contesto, poi i fix.** Prima di modificare qualsiasi file leggi
   per intero:
   - questo piano;
   - il report [README.md](README.md) (flussi §4, diagrammi §5, formati §6,
     identità e NQN §7, bug B-01…B-51 e voci minori M-01…M-06 in §8, blast
     radius §9, architettura target §10, domande bloccanti §12);
   - le appendici [A1-funzioni.md](A1-funzioni.md), [A2-campi.md](A2-campi.md),
     [A3-ui-layout-eventi.md](A3-ui-layout-eventi.md),
     [A4-riproduzioni.md](A4-riproduzioni.md);
   - l'harness [repro/harness.c](repro/harness.c), [repro/run_all.sh](repro/run_all.sh),
     [repro/gen_fixtures.py](repro/gen_fixtures.py);
   - **tutto il codice dell'app**: `main.c`, `controller.c`, `loader.c`,
     `predfit.c`, `intensity_fit.c`, `view.c`, `layout.c`, `settings.c`,
     `algorithms.c`, `ui_icons.c`, `plotgpu.cpp`, tutti gli `.h`, `makefile`,
     `README.md` del progetto;
   - per i formati Pickett, i sorgenti in `~/Desktop/Programmi_SP/calpgm`
     (`calcat.c`, `catutil.c`, `ulib.c`, `calfit.c`, `spinv.c`) nei punti citati
     dal report.

   I riferimenti `file:riga` sono del commit `1f4df65`. Prima di ogni passo
   verifica che corrispondano ancora al codice; se un passo precedente li ha
   spostati, rilocalizzali.
2. **Un passo alla volta, nell'ordine.** Non accorpare passi e non correggere
   nient'altro nello stesso commit. Un difetto nuovo trovato strada facendo va
   annotato in [Difetti nuovi](#difetti-nuovi-trovati-durante-i-fix), con la
   prova, e non corretto in quel passo.
3. **Ciclo di ogni passo:**
   1. scrivi le funzioni di test elencate nel passo (nella suite creata al
      passo #0) e verifica che **falliscano** sul codice attuale: dimostrano il
      difetto. Se una passa già, fermati e capisci perché;
   2. implementa la correzione descritta in *Cosa fare*: la più piccola che fa
      passare i test, senza refactor non richiesti;
   3. compila: `make clean && make` se hai toccato un `.h`, altrimenti `make`;
   4. `make test`: devono passare i test del passo e **tutta** la suite;
   5. `sh docs/audit/repro/run_all.sh <cartella temporanea>`: gli scenari
      R-xx del passo devono mostrare il comportamento corretto; sostituisci i
      loro log in `docs/audit/repro/logs/` (percorsi temporanei riscritti come
      `$RUNS`, `$FIXTURES`, `$REPO`);
   6. smoke test: il binario nel PATH è `./spectravisual` nella radice del
      repository (lo firma già `make`). Lancialo dalla radice con uno spettro e
      un catalogo in background, attendi 2 s e controlla con `kill -0` che sia
      vivo. **Non usare `make deploy`** (vedi #0);
   7. **aggiorna la documentazione con la nuova logica:**
      - nel report, in cima alla scheda di ogni bug corretto:
        `**Stato: risolto** in <hash> — nuova logica: …` (la diagnosi originale
        resta sotto, come storia); poi le sezioni che descrivono il
        comportamento cambiato (flussi §4, diagrammi §5, formati §6, identità
        §7, blast radius §9) e l'executive summary se cita il bug;
      - appendici: rigenera A1.3 e A2.3 ([tools/README.md](tools/README.md)) e
        aggiorna a mano A1.1, A1.2, A2.1, A2.2 per le funzioni e i campi toccati;
        A3 se cambiano UI o eventi; in A4 l'esito dello scenario dopo la
        correzione e, nella matrice, il nome della funzione di test per ogni T-xx;
      - il `README.md` del progetto se cambia un comportamento visibile;
      - questo piano: **Stato** del passo → fatto, con hash del commit, data e
        test aggiunti;
      - controlli e HTML: `python3 docs/audit/tools/check_links.py`, validazione
        Mermaid, rigenerazione di `docs/audit/audit.html` ([tools/README.md](tools/README.md)).
        L'HTML resta locale e non va pubblicato;
   8. **commit** del passo: codice, test e documentazione insieme, un commit per
      passo. Messaggio `Fix #<n>: <titolo> (B-xx, …)` e nel corpo i test aggiunti.
      L'hash va poi scritto nello *Stato* del passo, con un piccolo commit
      `Docs: stato del passo #<n>` oppure insieme al passo successivo.
4. **Domande bloccanti.** Alcuni passi dipendono da scelte D1–D10 (report §12).
   Quando arrivi a un passo *bloccato da Dx* e la risposta non è nella tabella
   qui sotto, chiedila all'utente e registrala con la data. Non decidere al suo
   posto; la parte del passo che non dipende dalla risposta si può fare.
5. **Vincoli sempre validi** (report, *Violazioni dei vincoli*): un `.cat`
   esterno si apre e si assegna senza Pred&Fit (1); il flusso catalogo →
   assignment → `assignments.txt` è autonomo (2); NQN è proprietà della riga CAT
   (3); Pred&Fit è opt-in (4); il fit delle intensità non muta dati senza
   un'azione esplicita (5); `assignments.txt` resta nell'ordine di un record
   `.lin` con NQN finale (6).
6. **Branch.** Lavora su `fix/audit`, creato da `ui/workbench` al passo #0 (salvo
   diversa indicazione dell'utente). Niente push e niente riscrittura della
   storia senza richiesta.
7. **Se un test non passa** con la correzione descritta, fermati e riporta test,
   output e ipotesi all'utente: non allentare il test.

## Risposte alle domande bloccanti

Le domande complete sono nel report ([§12](README.md#12-domande-bloccanti)).

| # | Domanda (breve) | Passi | Risposta | Data |
|---|---|---|---|---|
| D1 | Le specie sono molecole diverse o stati vibrazionali dello stesso Hamiltoniano? | #5, #14, #14b | Il progetto contiene più **Hamiltoniani**. Ogni Hamiltoniano possiede un solo `.par/.var/.lin/.int`, un solo SPFIT/SPCAT e uno o più stati; gli stati di quello Hamiltoniano condividono T rot, range e cut della sola carta di controllo `.int`, ma hanno dipoli e concentrazione separati. Stati accoppiati (tunneling incluso) devono quindi appartenere allo stesso Hamiltoniano; specie davvero indipendenti sono Hamiltoniani distinti. Per #5, con NVIB troppo piccolo: Calculate e Fit rifiutano con il minimo richiesto. | 2026-09-12 |
| D2 | Assignment con forma dei QN diversa dal modello: rifiutarli o convertirli, e con quale regola? | #6 (la conversione) | — | — |
| D3 | Dove salvare le esclusioni dal fit (file di Pred&Fit con chiave = identità)? | #8 | — | — |
| D4 | CalcIntensity in `assignments.txt`: intensità del catalogo o intensità mostrata? | #9 | — | — |
| D5 | Fit delle intensità con più specie: parametri per specie o una specie scelta? Risultati applicati a Pred&Fit solo su richiesta? | #9 | — | — |
| D6 | Servono cataloghi con NQN = 0 (10 QN per stato) o NQN > 6? | #1 | Non servono: le righe con NQN 0 o > 6 non vengono caricate e il loro numero compare nel messaggio di stato. | 2026-09-11 |
| D7 | All'avvio Pred&Fit si ripristina da solo o solo su richiesta? | #21 | — | — |
| D8 | Undo ripristina anche la lista degli assignment? | #8 | — | — |
| D9 | Baseline delle aree del fit delle intensità: quale stima? | #10 | — | — |
| D10 | I momenti di dipolo possono essere nulli o negativi in tutti i campi? | #15 | — | — |

## Ordine dei passi

#1–#8 correggono corruzione o perdita di dati (catalogo, lista, file, modello,
fit); #9–#15 risultati scientifici sbagliati (intensità, specie, parametri);
#16–#20 comportamenti che inducono errori (tastiera, spettro attivo,
impostazioni, percorsi, Find peaks); #21–#24 architettura, prestazioni e UI;
#25 aggiunge l'importazione esplicita di un modello Pickett esterno, dopo che le
funzioni di base di Pred&Fit saranno state stabilizzate.

| # | Titolo | Bug/issue | Impatto | Bloccato da |
|---|---|---|---|---|
| #0 | Rete di sicurezza: suite di test e strumenti | prerequisito | — | — |
| #1 | Parser dei cataloghi | B-01, B-03, B-04, B-25 | critico: NQN sbagliato sul 76,5 % di `pred.cat` (P0.2, P0.3) | D6 (solo NQN 0 o > 6) |
| #2 | Nessuna perdita di assignment | B-40, B-15, B-23, U-06 | critico: perdita definitiva della lista | — |
| #3 | Selezione azzerata al cambio di catalogo | B-10 | alto: transizione sbagliata assegnata in silenzio | — |
| #4 | Integrità di `assignments.txt` | B-05, B-06, B-07, B-08 | alto: file corrotto o frainteso (vincolo 6) | — |
| #5 | NVIB non più riscritto | B-02 | critico: P0.1, forma dei QN di `model.cat` | D1 (parziale) |
| #6 | Fit con NQN coerenti e diagnostica di SPFIT | B-12, B-22, U-08 | critico: fit sbagliati senza avviso (P1) | D2 (solo conversione) |
| #7 | Modello Pred&Fit all'avvio con un `.cat` | B-39, B-35 | alto: modello fittato sovrascritto | — |
| #8 | Esclusioni dal fit per identità | B-09, B-16, B-17, B-47 | alto: righe escluse che entrano nel fit | D3, D8 |
| #9 | Intensità: un solo ricalcolo, nessuna propagazione | B-11, B-13, B-32, B-27, B-21 | alto: intensità e Pred&Fit modificati (P2) | D4, D5 (parziale) |
| #10 | Baseline nelle aree | B-46 | alto: T rot distorta | D9 |
| #11 | Restore del `.int` | B-18 | alto: T e μ delle specie sovrascritti al riavvio | fatto `b9abf1a` |
| #12 | Spettri in ordine decrescente | B-42 | alto se i file sono decrescenti | fatto `0b0aaec` |
| #13 | Fit delle intensità: ricerca per identità ed export | B-20, B-41, M-03 | medio | — |
| #14 | Specie | B-43, B-24, B-19 | medio | D1 |
| #14b | Specie come molecole distinte | richiesta di D1 | medio | conferma del progetto |
| #15 | Validazione di parametri e dipoli | B-38, B-51, B-33, U-15 | medio | D10 (parziale) |
| #16 | Tastiera e testo tra finestre | B-49, U-03, U-13, U-02 | medio | fatto `656a1ab` |
| #17 | Spettro attivo e coda di caricamento | B-29, B-14, B-34, U-17 | medio | fatto |
| #18 | Impostazioni e persistenza di Pred&Fit | B-36, U-16, B-44, B-45, M-06 | medio | parziale `b98d08f` |
| #19 | Percorsi con spazi ed esito dei processi | B-28, M-01 | medio | fatto `79f231c` |
| #20 | Find peaks | B-30, U-04 | medio (lettura fuori dal buffer) | fatto `a2b91ff` |
| #21 | Pred&Fit opt-in | B-26 | medio (vincolo 4) | D7 |
| #22 | Prestazioni del caricamento della lista | B-48 | basso | parziale `7169e8b` |
| #23 | Vista iniziale, estensioni, input numerici | B-31, B-50, U-14, B-37, M-02, U-18 | basso | — |
| #24 | UI e pulizia | U-01, U-05, U-07, U-09, U-10, U-11, U-12, M-04, M-05 | basso | — |
| #25 | Importazione esplicita di input SPFIT/SPCAT | richiesta utente 2026-09-12 | medio: adozione di modelli esterni senza ricostruzione manuale | dopo #21 e funzioni base Pred&Fit stabili |

Copertura dei problemi segnalati: **P0.1** → #5; **P0.2** → #1, #4, #6;
**P0.3** → #1; **P1** → #2, #6, #7, #8, #11, #14, #15, #18, #19, #21;
**P2** → #9, #10, #13.

## Passi

### #0 — Rete di sicurezza: suite di test e strumenti

- **Bug/issue**: nessuna suite eseguibile (`tests/test_core` è un binario senza
  sorgente, non ricompilabile); `make deploy` copia il binario dentro
  `../spectravisual/`, che oggi è una cartella con un altro repository, mentre il
  binario nel PATH è già `./spectravisual` nella radice; la documentazione di
  audit non è ancora nel repository.
- **Cosa fare**:
  1. crea il branch `fix/audit` da `ui/workbench` e committa `docs/audit/` così
     com'è (report, appendici, `repro/`, `tools/`, questo piano);
  2. crea `tests/test_audit.c` sul modello di [repro/harness.c](repro/harness.c):
     include le translation unit reali `main.c`, `controller.c`, `predfit.c` con
     `#define main sv_app_main`, linka gli altri `.c` dell'app e
     `tests/plotgpu_stub.c` (copia di [repro/plotgpu_stub.c](repro/plotgpu_stub.c));
  3. runner minimo: tabella `{nome, funzione}`; `./tests/test_audit` esegue
     tutto, `./tests/test_audit <nome>` un solo test; stampa PASS/FAIL/SKIP con
     atteso e ottenuto; codice d'uscita ≠ 0 se c'è un FAIL. Ogni test lavora in
     una propria cartella temporanea (`mkdtemp`), con `settings.data_dir` lì, e
     non scrive mai nel repository; i test che richiedono SPCAT/SPFIT diventano
     SKIP solo se `autodetect_program` non li trova;
  4. fixture: `tests/gen_fixtures.py` (da [repro/gen_fixtures.py](repro/gen_fixtures.py))
     genera i CAT sintetici nella cartella del test; copia l'attuale `pred.cat` in
     `tests/fixtures/pred_reference.cat` (i nomi `pred.*` sono ignorati da
     `.gitignore`) e `assignments_backup.txt` in `tests/fixtures/lin_with_nqn.txt`;
  5. `makefile`: target `test` (compila ed esegue la suite); target `deploy`
     rimosso, oppure sostituito da un controllo che il binario del PATH sia
     `./spectravisual`; `tests/test_core` rimosso dal repository;
  6. in [A4.1](A4-riproduzioni.md#a41-come-è-costruito-lharness) spiega come si
     esegue la suite.
- **Test che devono passare** (comportamenti oggi corretti, da non rompere):
  - `test_baseline_cat1404_nqn4`: `cat4_1404.cat` → tutte le righe con
    `n_qn = 4` e QN interi corretti;
  - `test_baseline_roundtrip_qnfmt1404`: scenario R-02..R-07 con
    `cat4_1404.cat` → stessa lista dopo *Save all* e riavvio;
  - `test_baseline_reassign_updates_obsfreq` (T-08): stessa transizione su un
    secondo picco → cambia solo ObsFreq, numero di assignment invariato;
  - `test_baseline_right_drag_ascending`: trascinamento destro su uno spettro
    crescente → 3000,0000 MHz (R-31, parte crescente);
  - `test_baseline_calculate_single_species`: Calculate con un modello a una
    specie produce `model.cat`;
  - `make test` funziona partendo da un clone pulito.
- **Stato**: ☑ fatto il 2026-09-11 — commit: `dcff7bc` —
  test aggiunti: `test_baseline_cat1404_nqn4`, `test_baseline_roundtrip_qnfmt1404`,
  `test_baseline_reassign_updates_obsfreq`, `test_baseline_right_drag_ascending`,
  `test_baseline_calculate_single_species` (5 PASS, anche su una copia pulita
  dell'indice git). `deploy` rimosso dal `makefile`, `tests/test_core` rimosso dal
  repository. `run_all.sh` sul codice invariato: log uguali a quelli dell'audit
  salvo date di SPCAT/SPFIT, percorsi temporanei e tempi di `perf`.

### #1 — Parser dei cataloghi: NQN, QN in codice lettera, righe corte, campi a larghezza fissa

- **Bug/issue**: [B-01](README.md#b-01) NQN letto dalle colonne sbagliate
  (cifra delle decine di J con QNFMT a tre cifre), [B-03](README.md#b-03) righe
  senza spazi finali scartate, [B-04](README.md#b-04) QN ≥ 100 e ≤ −10 letti come
  0, [B-25](README.md#b-25) FREQ ed ERR che si toccano. Issue P0.2 e P0.3;
  vincoli 1 e 3.
- **Cosa fare**:
  - `parse_cat_quantum_numbers` [loader.c:54-65](../../loader.c#L54-L65): copiare
    le colonne 52–55 (QNFMT) in un buffer di 4 caratteri + `'\0'` e convertirle con
    `strtol`; NQN = QNFMT % 10. NQN 0 (10 QN) e NQN > 6 dipendono da D6: finché
    manca la risposta, la riga non viene caricata e il numero di righe scartate
    compare nel messaggio di stato;
  - `parse_qn2` [loader.c:11-15](../../loader.c#L11-L15): decodifica come
    `readqn` (`calpgm/catutil.c:10-60`): lettera maiuscola = centinaia (`A0` = 100),
    minuscola = negativi da −10 (`a0` = −10, `a1` = −11), `-d` = −d;
  - `read_pred_cat` / `read_pred_cat_alloc` [loader.c:239](../../loader.c#L239),
    [291](../../loader.c#L291): accettare le righe lunghe almeno fino a QNFMT (55
    caratteri) e trattare come vuoti i campi QN mancanti;
  - campi numerici [loader.c:41](../../loader.c#L41), [241](../../loader.c#L241),
    [293](../../loader.c#L293): lettura a colonne fisse come
    `calpgm/calcat.c:700-709` (FREQ 13, ERR 8, LGINT 8, DR 2, ELO 10, GUP 3,
    TAG 7, QNFMT 4), non con `"%lf %lf %lf"`.
- **Test che devono passare**:
  - `test_cat_nqn_from_qnfmt_3qn` (T-01): `cat3_303.cat` e
    `pred_reference.cat` → `n_qn = 3` su tutte le righe; la riga 5999,2867 MHz →
    U = (11 0 11), L = (10 1 9);
  - `test_cat_nqn_4_5_6` (T-02, T-03): `cat4_304` e `cat4_1404` → 4 con F o v nel
    quarto campo; `cat5_305` → 5; `cat6_306` → 6;
  - `test_cat_letter_and_negative_qn` (T-04): `A5` → 105, `a1` → −11, `-5` → −5;
  - `test_cat_trailing_spaces_irrelevant` (T-05): stesso numero di righe e stessi
    valori con e senza spazi finali;
  - `test_cat_fixed_width_numbers`: riga con `6348.1049158.2229` → FREQ 6348,1049,
    ERR 158,2229;
  - `test_cat_invalid_nqn_reported`: riga con NQN 0 → non caricata e contata nel
    messaggio (finché D6 non dice altro).
- **Documentazione**: report §6.1, §7.2, schede B-01/B-03/B-04/B-25, punto 1
  dell'executive summary; A4 R-01 e R-17.
- **Stato**: ☑ fatto il 2026-09-11 — commit: `fbdc645` —
  test aggiunti: `test_cat_nqn_from_qnfmt_3qn`, `test_cat_nqn_4_5_6`,
  `test_cat_letter_and_negative_qn`, `test_cat_trailing_spaces_irrelevant`,
  `test_cat_fixed_width_numbers`, `test_cat_invalid_nqn_reported` (suite 11/11 PASS).
  Nuove funzioni in `loader.h`: `parse_cat_record`, `read_pred_cat_alloc_counted`.
  Con D6 le righe con NQN 0 o > 6 sono scartate e contate in `status_message` ed
  `error_message`. R-01 e R-17 rieseguiti: nessuna riga con NQN errato.

### #2 — Nessuna perdita di assignment: restore da `data_dir`, righe `.lin` corte, scrittura sicura, salvataggio automatico

- **Bug/issue**: [B-40](README.md#b-40) riavvio da un'altra cartella + *Save all*
  → 2 assignment su 3 persi; [B-15](README.md#b-15) `import_fit_lines` legge
  `assignments.txt` dalla CWD; [B-23](README.md#b-23) nessun salvataggio
  automatico ed errori di scrittura silenziosi; U-06 (*Save all* ed *Export list*
  senza esito in UI).
- **Cosa fare**:
  - `import_fit_lines` [predfit.c:893](../../predfit.c#L893): percorso da
    `settings_data_file(s, "assignments.txt", …)`;
  - `read_lin_rows` [predfit.c:814-841](../../predfit.c#L814-L841): non scartare
    le righe con meno di 6 campi QN; importarle marcate "da riassegnare" (NQN non
    affidabile) e riportarne il numero nello stato;
  - una sola funzione di scrittura di `assignments.txt`, usata da *Save all*
    [controller.c:283-319](../../controller.c#L283-L319): file temporaneo +
    `rename`, copia della versione precedente in `assignments.txt.bak`, errore in
    `error_message` se `fopen`, `fclose` o `rename` falliscono; lo stesso
    trattamento degli errori per *Export list*
    [controller.c:340-347](../../controller.c#L340-L347);
  - salvataggio automatico con quella funzione dopo ogni aggiunta, aggiornamento e
    cancellazione di un assignment; *Save all* resta.
- **Test che devono passare**:
  - `test_restore_reads_data_dir_list` (T-14): avvio senza `.cat` con
    CWD ≠ `data_dir` → stessa lista (numero, QN, frequenze) dell'avvio con `.cat`;
  - `test_restore_keeps_short_lin_rows`: `model.lin` con righe da 2 e 4 campi
    QN → nessuna riga persa, righe marcate e contate nel messaggio;
  - `test_save_all_after_restore_no_loss` (T-33, R-29): scenario R-29 →
    `assignments.txt` con le 3 righe; `assignments.txt.bak` uguale al contenuto
    precedente;
  - `test_save_all_reports_write_error` (U-06): `data_dir` inesistente o non
    scrivibile → `error_message` non vuoto, file precedente intatto;
  - `test_autosave_on_assign_update_delete`: dopo assegnazione, riassegnazione e
    cancellazione il file su disco coincide con la lista in memoria, senza *Save all*.
- **Documentazione**: report §4 (flussi 1, 2, 8), §5.7, §6.2, schede
  B-15/B-23/B-40; A3 U-06; A4 R-13 e R-29.
- **Stato**: ☑ fatto il 2026-09-11 — commit: `04e2e6f` —
  test aggiunti: `test_restore_reads_data_dir_list`, `test_restore_keeps_short_lin_rows`,
  `test_save_all_after_restore_no_loss`, `test_save_all_reports_write_error`,
  `test_autosave_on_assign_update_delete` (suite 16/16 PASS). Unico writer
  `save_assignments` in `controller.c` (file temporaneo, `rename`, copia `.bak`,
  errori in `error_message`), chiamato anche dopo ogni aggiunta, riassegnazione e
  cancellazione; nuovo campo `Assignment.needs_reassign` (riga in ambra nel pannello)
  per le righe `.lin` con meno di 3 QN per stato. Di U-06 resta l'esito positivo
  non visibile con dati caricati (passo #24). R-13 e R-29 rieseguiti; l'etichetta di
  R-13 nell'harness ora dice `data_dir`.

### #3 — Selezione azzerata a ogni cambio di catalogo

- **Bug/issue**: [B-10](README.md#b-10): dopo drop di un `.cat`, Calculate, Fit,
  Undo o restore gli indici selezionati puntano ad altre righe, e il picco
  successivo assegna la transizione sbagliata.
- **Cosa fare**: in `set_predictions` [main.c:288-329](../../main.c#L288-L329)
  azzerare `n_selected` (oppure rimappare la selezione per identità NQN + QN sul
  nuovo catalogo); controllo `idx < n_pred` in [view.c:1815](../../view.c#L1815) e
  in `assign_selected_predictions` [controller.c:836-852](../../controller.c#L836-L852).
- **Test che devono passare**:
  - `test_selection_cleared_on_catalog_change` (T-10, R-10): indice 2 selezionato
    in `cat3_303.cat`, poi `set_predictions(cat4_1404.cat)` e trascinamento
    destro → nessun assignment creato (con la rimappatura: quello della stessa
    identità, oppure nessuno);
  - `test_selection_indices_in_bounds`: dopo ogni `set_predictions` nessun indice
    selezionato è ≥ `n_pred`.
- **Documentazione**: report §4 flusso 4, §5.2, scheda B-10; A4 R-10.
- **Stato**: ☑ fatto il 2026-09-11 — commit: `7d6ece4` —
  test aggiunti: `test_selection_cleared_on_catalog_change`,
  `test_selection_indices_in_bounds` (suite 18/18 PASS). Scelta: azzerare la
  selezione in `set_predictions` (non rimapparla); il controllo `idx < n_pred`
  c'era già in `assign_selected_predictions`, aggiunto nella card del render
  (`view.c`). R-10 rieseguito.

### #4 — Integrità di `assignments.txt`: niente NQN inventati, niente fusioni silenziose, formato riconoscibile

- **Bug/issue**: [B-05](README.md#b-05) fallback `nq = 3` e zeri scritti come
  QN; [B-06](README.md#b-06) transizioni diverse fuse al riavvio;
  [B-07](README.md#b-07) `exp_int` riceve CalcIntensity al riavvio;
  [B-08](README.md#b-08) formato ambiguo con `.lin` e con i formati precedenti.
  Vincolo 6.
- **Cosa fare**:
  - writer [controller.c:300-314](../../controller.c#L300-L314): nessun
    fallback 3; un assignment con NQN non valido non viene scritto e compare in un
    messaggio ("N assignment da riassegnare");
  - reader [loader.c:579-667](../../loader.c#L579-L667): il formato nuovo si
    riconosce dall'intestazione già scritta dal writer, a cui si aggiunge un numero
    di versione (le righe di dati restano come vuole il vincolo 6); senza
    intestazione si provano prima il formato legacy a 14 campi e poi quello a 16
    (oggi il ramo a 14 è irraggiungibile); un `.lin` (incertezza e peso al posto di
    CalcFreq e CalcIntensity, sentinella 9xxxx) viene rifiutato con messaggio;
  - collisioni in lettura (stessa identità due volte) contate e segnalate, non
    fuse in silenzio; righe troncate da B-01 (NQN 1–2 con J ≥ 10) marcate "da
    riassegnare";
  - `exp_int` delle righe lette = 0, mai CalcIntensity.
- **Test che devono passare**:
  - `test_roundtrip_every_qnfmt` (T-06, T-07): scenario R-02..R-07 con QNFMT 303,
    304, 305, 306, 1404 e con `pred_reference.cat` → dopo *Save all* e riavvio
    stessi QN, NQN, ObsFreq, CalcFreq, CalcIntensity; nessuna fusione;
  - `test_blend_pair_survives_reload` (T-09): la coppia a 392,7959 MHz di
    `pred_reference.cat` → 2 assignment dopo il riavvio;
  - `test_save_skips_invalid_nqn`: assignment con `n_qn = 0` → non scritto,
    messaggio;
  - `test_reload_reports_collisions`: file con due righe della stessa identità →
    conteggio nel messaggio, nessuna sostituzione silenziosa;
  - `test_reload_exp_int_zero`: `exp_int` = 0 dopo il riavvio;
  - `test_reader_rejects_lin_file` (T-20): `lin_with_nqn.txt` → 0 assignment,
    messaggio;
  - `test_reader_legacy_14_and_16` (T-20): righe legacy a 14 e 16 campi, anche con
    NQN 6 e con ExpInt ≈ 5, lette correttamente.
- **Documentazione**: report §6.2, §7.1, §7.3, schede B-05…B-08; A4 R-02..R-07
  e R-09.
- **Stato**: ☑ fatto il 2026-09-11 — commit: `e428db6` —
  test aggiunti: `test_roundtrip_every_qnfmt`, `test_blend_pair_survives_reload`,
  `test_save_skips_invalid_nqn`, `test_reload_reports_collisions`,
  `test_reload_exp_int_zero`, `test_reader_rejects_lin_file`,
  `test_reader_legacy_14_and_16` (suite 25/25 PASS). `test_roundtrip_every_qnfmt` e
  `test_blend_pair_survives_reload` passavano già prima della correzione: la causa di
  B-05/B-06 per i cataloghi validi era B-01, corretto al passo #1; restano come test
  di regressione. Nuove funzioni: `load_assignments_file`,
  `assignment_file_message` (`loader.h`, con `AssignmentFileReport` e
  `ASSIGNMENT_FORMAT`). Intestazione scritta: `# SpectraVisual assignments,
  format 1: …`; quella senza versione di `1f4df65` resta leggibile, e il file
  `assignments.txt` nella radice (una riga `11 10 … 1`) viene letto e marcato da
  riassegnare. Una riga senza NQN non è più scritta: resta in memoria marcata e il
  messaggio lo dice, ma sparisce dal file al primo salvataggio (l'ultima copia sta
  in `assignments.txt.bak`). R-02..R-07 e R-09 rieseguiti.

### #5 — NVIB: il valore dell'utente non viene più riscritto

- **Bug/issue**: [B-02](README.md#b-02): il terzo token della riga opzioni
  (NVIB) è sempre riportato a `max(state_index)+1` da cinque writer. È il
  problema P0.1 ("N riportato a 3").
- **Cosa fare**: `set_hamiltonian_nstates` e `update_hamiltonian_nstates`
  [predfit.c:112-143](../../predfit.c#L112-L143) non modificano più la riga;
  togliere le chiamate in [predfit.c:355](../../predfit.c#L355),
  [623](../../predfit.c#L623), [647](../../predfit.c#L647),
  [1228-1238](../../predfit.c#L1228-L1238), [1662](../../predfit.c#L1662).
  Validazione in `write_inputs`: con NVIB minore del numero di stati delle specie
  incluse, Calculate e Fit vengono rifiutati con un messaggio che indica il valore
  minimo (se alzarlo automaticamente, con avviso, dipende da D1). CHR, SPIND e gli
  altri token restano sempre invariati.
- **Test che devono passare**:
  - `test_nvib_typed_value_kept` (T-11, R-08): `s 1 2 0` e `s 1 5 0` con 1 e 3
    specie → invariati dopo il commit, `write_inputs`, salvataggio e caricamento
    della sessione;
  - `test_nvib_too_small_rejected`: NVIB 1 con 3 specie → Calculate rifiutato con
    messaggio, nessun file Pickett scritto;
  - `test_option_line_other_tokens_kept`: CHR, SPIND e la forma con le virgole
    invariati.
- **Bloccato da**: D1, solo per la regola con NVIB troppo piccolo.
- **Documentazione**: report executive summary punto 2, §4 flusso 5, §5.9, §6.4,
  scheda B-02; A4 R-08.
- **Stato**: ☑ fatto il 2026-09-11 — commit: `9f5ee42` —
  test aggiunti: `test_nvib_typed_value_kept`, `test_nvib_too_small_rejected`,
  `test_option_line_other_tokens_kept` (suite 28/28 PASS). `hamiltonian_line`
  resta testuale e invariata; `write_inputs` legge NVIB senza normalizzarlo e
  rifiuta Calculate/Fit prima di creare `.fit` se è minore del massimo stato
  delle specie PRED incluse. R-08 rieseguito.

### #6 — Fit: nessuna riga con NQN diverso dal modello; diagnostica di SPFIT visibile

- **Bug/issue**: [B-12](README.md#b-12) assignment con NQN diverso dal modello
  mandati a SPFIT ("Bad Line", righe calcolate a 142 775 MHz, mentre lo stato
  dice "RMS 0.000274"); [B-22](README.md#b-22) "Bad Line", "Lines rejected",
  "NEXT LINE NOT USED IN FIT", "Fit Diverging" ignorati; U-08 (righe escluse
  mostrate come "reassigned — run Fit", rifiutate come "not fitted yet").
- **Cosa fare**: in `write_inputs` [predfit.c:629-710](../../predfit.c#L629-L710)
  il NQN del modello è il QNFMT % 10 del `model.cat` prodotto dall'ultimo
  Calculate con la stessa riga opzioni (se manca o la riga è cambiata, prima
  Calculate); ogni assignment incluso con NQN diverso blocca il Fit, con l'elenco
  delle righe (conversione solo con la regola di D2). `fit_summary`
  [predfit.c:743-764](../../predfit.c#L743-L764) conta le quattro diagnostiche e le
  riporta nello stato; la tabella *Fitting*
  [predfit.c:1950-1997](../../predfit.c#L1950-L1997) mostra "esclusa", "rifiutata
  da SPFIT", "non letta".
- **Test che devono passare**:
  - `test_fit_rejects_nqn_mismatch` (T-16, R-11): 3 specie (NQN 4) e assignment
    con NQN 3 → Fit rifiutato, `model.lin` non scritto, righe elencate;
  - `test_fit_status_counts_spfit_diagnostics`: `model.fit` di fixture con le
    quattro diagnostiche → conteggi nello stato;
  - `test_fitting_tab_row_states` (U-08): stato corretto per riga esclusa,
    rifiutata e usata.
- **Bloccato da**: D2, solo per la conversione (il rifiuto non è bloccato).
- **Documentazione**: report executive summary punto 3, §4 flusso 5, §7, schede
  B-12/B-22; A3 U-08; A4 R-11.
- **Stato**: ☑ fatto il 2026-09-11 — commit: `deef2f7` —
  test aggiunti: `test_fit_rejects_nqn_mismatch`,
  `test_fit_status_counts_spfit_diagnostics`, `test_fitting_tab_row_states`
  (suite 31/31 PASS). `current_model_nqn` accetta il catalogo soltanto quando
  la riga opzioni salvata in `model.par` coincide con quella corrente; il Fit
  rifiuta le righe incluse incompatibili prima di `model.lin`. R-11 rieseguito;
  la conversione resta rinviata a D2.

### #7 — Modello Pred&Fit all'avvio con un `.cat` e cartella di lavoro

- **Bug/issue**: [B-39](README.md#b-39) con un `.cat` sulla riga di comando il
  modello torna ai default, i parametri fissati tornano liberi e un Calculate
  sovrascrive il `model.var` fittato; [B-35](README.md#b-35) `work_dir` calcolata
  prima di leggere le impostazioni.
- **Cosa fare**: la sessione salva anche il valore dei parametri (riga
  compatibile con v3, per esempio `param <id> <valore> <incertezza>`)
  [predfit.c:185-238](../../predfit.c#L185-L238); `predfit_load_session`
  [predfit.c:345-347](../../predfit.c#L345-L347) aggiunge le righe che mancano in
  memoria invece di ignorarle; nessun salvataggio della sessione Pred&Fit
  all'avvio se non è stata caricata o modificata [main.c:450](../../main.c#L450);
  `work_dir` ricalcolata dopo `settings_init`
  [main.c:369](../../main.c#L369)–[398](../../main.c#L398), o `fit_root()` a ogni uso.
- **Test che devono passare**:
  - `test_launch_with_cat_keeps_model` (T-32, R-28): dopo un Fit con DJ fissato,
    avvio con `.cat` → parametri, valori e incertezze uguali a quelli di prima;
    Calculate non scrive i default;
  - `test_session_load_adds_missing_param_rows`: righe della sessione assenti in
    memoria → aggiunte con valore e incertezza;
  - `test_startup_does_not_rewrite_session`: avvio e uscita senza usare Pred&Fit →
    file di sessione identico byte per byte;
  - `test_workdir_after_settings` (T-28, R-25): `work_dir` = `data_dir/.fit`
    subito dopo l'avvio.
- **Documentazione**: report §4 flussi 5 e 8, §5.7, §6.7, schede B-35/B-39;
  A4 R-25 e R-28.
- **Stato**: ☑ fatto il 2026-09-11 — commit: `4c47c47` —
  test aggiunti: `test_launch_with_cat_keeps_model`,
  `test_session_load_adds_missing_param_rows`,
  `test_startup_does_not_rewrite_session`, `test_workdir_after_settings`
  (suite 35/35 PASS). Le righe `param` v3 conservano valore e incertezza e
  restano compatibili con le vecchie righe `ID incertezza`; R-25 e R-28
  rieseguiti. B-36 non è incluso in questo passo.

### #8 — Esclusioni dal fit: fuori dal `.lin` e salvate per identità

- **Bug/issue**: [B-09](README.md#b-09) con incertezza `.lin` ≥ 0,09 MHz la riga
  "esclusa" entra nel fit (A da 1151 a 24 199 MHz); [B-16](README.md#b-16) flag
  ricostruiti per frequenza, righe solo-`.lin` con NQN = campi/2;
  [B-17](README.md#b-17) Undo ripristina le esclusioni per posizione;
  [B-47](README.md#b-47) riassegnare una riga esclusa la reinclude.
- **Cosa fare**: `write_inputs` [predfit.c:688-707](../../predfit.c#L688-L707)
  non scrive le righe escluse e NLINE conta solo quelle incluse; le esclusioni
  stanno in un file di Pred&Fit con chiave = identità NQN + QN (D3; proposta
  `.fit/exclusions.txt`), letto al restore al posto della sentinella
  [predfit.c:899-943](../../predfit.c#L899-L943); lo snapshot di Undo salva le
  identità escluse [predfit.c:433-479](../../predfit.c#L433-L479) (D8 decide se
  ripristina anche la lista); `add_or_update_assignment` conserva `fit_enabled`
  [loader.c:559](../../loader.c#L559).
- **Test che devono passare**:
  - `test_excluded_rows_absent_from_lin` (T-15, R-15): con incertezza da 0,001 a
    5 MHz le righe escluse non sono nel `.lin` e non entrano nel fit;
  - `test_exclusions_persist_by_identity` (T-12, T-14): riavvio con e senza `.cat`
    → stesse esclusioni sulle stesse transizioni;
  - `test_undo_exclusions_by_identity` (T-12, R-14): esclusione, Fit,
    cancellazione della riga 0, Undo → esclusione sulla stessa transizione;
  - `test_reassign_keeps_exclusion` (T-40, R-36).
- **Bloccato da**: D3 (dove salvare), D8 (portata di Undo).
- **Documentazione**: report executive summary punto 5, §5.8, §6.3, schede
  B-09/B-16/B-17/B-47; A4 R-14, R-15, R-36.
- **Stato**: ☑ fatto il 2026-09-11 — commit: `5986dc6` —
  D3: `.fit/exclusions.txt`, file versionato e atomico con NQN + 12 QN;
  righe malformate ignorate senza toccare gli assignment. D8: Undo ripristina
  solo modello ed esclusioni per identità, mai la lista. Advanced › Lines e
  › Fitting distinguono clic (include/esclude dal Fit) e × (elimina dalla lista
  e da `assignments.txt`). Test aggiunti: `test_excluded_rows_absent_from_lin`,
  `test_exclusions_persist_by_identity`, `test_undo_exclusions_by_identity`,
  `test_reassign_keeps_exclusion`, `test_malformed_exclusions_are_safe`
  (suite 40/40 PASS).

### #9 — Intensità: una sola funzione di ricalcolo, nessuna propagazione verso Pred&Fit, fit fallito senza effetti

- **Bug/issue**: [B-11](README.md#b-11) ricalcolo a specie singola su un catalogo
  multi-specie (concentrazioni perse); [B-13](README.md#b-13) Intensity analysis
  e fit delle intensità scrivono nella specie attiva di Pred&Fit;
  [B-32](README.md#b-32) un *Run fit* rifiutato modifica intensità e μ red;
  [B-27](README.md#b-27) CalcIntensity salvata dipende dalla vista;
  [B-21](README.md#b-21) specie e concentrazioni ignorate dal modello di
  intensità. Issue P2; vincolo 5.
- **Cosa fare**: una funzione `recompute_display_intensities(AppState *)` usata da
  tutte le route: campi T cat, T rot e μ
  [controller.c:913-947](../../controller.c#L913-L947), *Run fit*
  [controller.c:438-446](../../controller.c#L438-L446),
  `predfit_publish_shared_state` e `predfit_adopt_generated_catalog`
  [predfit.c:363-431](../../predfit.c#L363-L431); sceglie il percorso per specie
  quando il catalogo è generato. Togliere le chiamate automatiche a
  `predfit_adopt_shared_state` ([controller.c:926](../../controller.c#L926),
  [940](../../controller.c#L940), [intensity_fit.c:311](../../intensity_fit.c#L311))
  e sostituirle con un'azione esplicita "Applica a Pred&Fit" (D5). *Run fit*
  ricalcola solo se `intensity_fit_run` restituisce 1; il riempimento di μ red va
  dopo i controlli e su una copia locale
  [intensity_fit.c:270-282](../../intensity_fit.c#L270-L282). CalcIntensity di
  *Save all* secondo D4; modello del fit per specie secondo D5.
- **Test che devono passare**:
  - `test_intensity_recompute_keeps_concentration` (T-18, R-12): specie con
    concentrazione 0,1 → rapporto 0,1 dopo *Run fit*, dopo T rot e dopo μ red;
  - `test_intensity_fit_does_not_touch_predfit` (T-17): parametri, T e μ delle
    specie di Pred&Fit invariati dopo *Run fit* e dopo i campi dell'Intensity
    analysis;
  - `test_failed_run_fit_no_side_effects` (T-25, R-22);
  - `test_calcintensity_independent_of_view` (B-27, dopo D4): lo stesso
    assignment salvato prima e dopo aver cambiato T rot → stessa CalcIntensity;
  - `test_intensity_fit_multispecies` (B-21, dopo D5).
- **Bloccato da**: D4, D5 (una sola funzione, nessuna propagazione e fit fallito
  senza effetti non sono bloccati).
- **Documentazione**: report executive summary punto 6, §4 flusso 7, §5.5, §5.9,
  schede B-11/B-13/B-21/B-27/B-32; A2.1; A4 R-12 e R-22.
- **Stato**: ◐ sospeso per decisione di progetto il 2026-09-12 — sono state
  introdotte soltanto le protezioni di confine: il fit è transazionale (un
  rifiuto non modifica T rot o μ red), un risultato non viene adottato dalla
  specie Pred&Fit e il ricalcolo della visualizzazione conserva le
  concentrazioni di tutte le specie dei cataloghi generati.
  `test_intensity_fit_does_not_touch_predfit` e
  `test_intensity_recompute_keeps_concentration` coprono questi casi (suite
  69/69 PASS). Il design e l'implementazione del fit delle intensità restano
  deliberatamente fermi finché Pred&Fit non sarà stato definito e stabilizzato;
  solo allora si decideranno D4/D5 e si affronteranno
  B-11/B-13/B-21/B-27/B-32.

### #10 — Aree del fit delle intensità con baseline sottratta

- **Bug/issue**: [B-46](README.md#b-46): con una baseline pari allo 0,5 % della
  riga più forte T rot passa da 2,05 a 3,3 K.
- **Cosa fare**: `integrate_area` [intensity_fit.c:90-112](../../intensity_fit.c#L90-L112)
  sottrae una baseline stimata secondo D9 (proposta: retta tra le medie di due
  finestre laterali larghe quanto la semi-ampiezza, appena fuori dalla finestra
  di integrazione); la baseline compare nell'export.
- **Test che devono passare**:
  - `test_intensity_area_baseline_invariant` (T-39, R-35): baseline 0, 0,5 %, 2 %
    e 10 % → T rot = 2 K entro ±0,1 K, stesse righe usate.
- **Bloccato da**: D9.
- **Documentazione**: report §4 flusso 7, scheda B-46; A4 R-35.
- **Stato**: ☐ non fatto — commit: —

### #11 — Restore del `.int` senza toccare le specie

- **Bug/issue**: [B-18](README.md#b-18): al riavvio la specie attiva riceve TCAT
  e i dipoli dello stato 0; FQLIM e MAXV automatici diventano valori fissi.
- **Cosa fare**: `import_int_settings` [predfit.c:770-799](../../predfit.c#L770-L799)
  legge dal `.int` solo i campi che la sessione non ha, non scrive `temp_k` e `mu`
  della specie attiva e conserva i valori automatici (FQLIM 0, MAXV −1); nessuna
  `store_active_species` subito dopo [predfit.c:958](../../predfit.c#L958).
- **Test che devono passare**:
  - `test_restore_int_keeps_species_and_auto_fields` (T-19, R-16): specie attiva
    1 con T 5 K e μ (0,4 0,3 0,5), FQLIM e MAXV automatici → identici dopo il
    riavvio; aggiungendo una specie MAXV resta automatico.
- **Documentazione**: report §4 flusso 6, §6.5, scheda B-18; A4 R-16.
- **Stato**: ☑ fatto il 2026-09-12 — commit codice: `b9abf1a` — test aggiunto:
  `test_restore_int_keeps_species_and_auto_fields` (suite 41/41 PASS). La riga
  `int2` della sessione moderna prevale integralmente; `model.int` fa da fallback
  soltanto per sessioni senza quella riga e non modifica mai T o dipoli della specie.

### #12 — Spettri in ordine di frequenza decrescente

- **Bug/issue**: [B-42](README.md#b-42): il trascinamento destro misura il picco
  più alto di tutto lo spettro, il fit delle intensità non trova aree; Find peaks
  e Tab usano le stesse ricerche binarie.
- **Cosa fare**: `read_data_alloc` [loader.c:462-519](../../loader.c#L462-L519):
  se x non è crescente, ordinare i punti per x e dirlo nello stato; rifiutare con
  messaggio i file non monotoni (x ripetute o disordinate).
- **Test che devono passare**:
  - `test_descending_spectrum_same_results` (T-35, R-31): file crescente e
    decrescente → stessa frequenza misurata (3000,0000 MHz) e stesse aree.
- **Documentazione**: report §6.8, scheda B-42; A4 R-31.
- **Stato**: ☑ fatto il 2026-09-12 — commit codice: `0b0aaec` — test aggiunti:
  `test_descending_spectrum_same_results`, `test_nonmonotonic_spectrum_rejected`
  (suite 43/43 PASS). I file strettamente decrescenti sono riordinati e segnalati;
  frequenze duplicate o mescolate sono rifiutate.

### #13 — Fit delle intensità: ricerca per identità ed export coerente

- **Bug/issue**: [B-20](README.md#b-20) righe ritrovate con la frequenza della
  copia e scartate senza messaggio; [B-41](README.md#b-41) export disallineato
  dopo una modifica della lista; M-03 righe vicine ai bordi della traccia
  scartate in silenzio.
- **Cosa fare**: `current_pred_line` [intensity_fit.c:56-76](../../intensity_fit.c#L56-L76)
  cerca per identità, come `current_assignment_prediction`; i risultati vengono
  invalidati (`intfit_has_result = 0`) a ogni modifica della lista, oppure
  indicizzati per identità [intensity_fit.c:312-364](../../intensity_fit.c#L312-L364);
  il messaggio riporta le righe scartate con il motivo.
- **Test che devono passare**:
  - `test_intensity_lookup_after_fit`: dopo un Fit (frequenze cambiate) tutte le
    righe assegnate vengono trovate;
  - `test_intensity_export_after_list_change` (T-34, R-30): dopo una
    cancellazione l'export viene rifiutato finché non si rifà il fit, oppure ogni
    riga resta associata alla propria transizione;
  - `test_intensity_skipped_lines_counted` (M-03).
- **Documentazione**: schede B-20, B-41, M-03; A4 R-30.
- **Stato**: ☐ non fatto — commit: —

### #14 — Specie: rimozione, aggiunta e posizione dello stato vibrazionale

- **Bug/issue**: [B-43](README.md#b-43) la specie attiva cambia quando si rimuove
  una riga precedente e una specie nuova eredita costanti orfane;
  [B-24](README.md#b-24) i parametri della specie rimossa restano;
  [B-19](README.md#b-19) la specie viene dedotta dal quarto QN anche quando è F.
- **Cosa fare**: rimozione [predfit.c:1658-1663](../../predfit.c#L1658-L1663):
  `active_species--` se la riga rimossa la precede; i parametri dello stato
  rimosso si eliminano o si conservano secondo D1, dicendolo nello stato;
  `add_species` [predfit.c:595-627](../../predfit.c#L595-L627): seme dalla specie
  attiva, parametri orfani mai riusati in silenzio; `PredLine` conserva QNFMT
  intero e il ricalcolo per specie [loader.c:390-427](../../loader.c#L390-L427)
  ricava la posizione di v da QNFMT (`calpgm/calcat.c:289-294`).
- **Test che devono passare**:
  - `test_species_remove_keeps_active` (T-36, R-32);
  - `test_species_add_seeds_from_active` (T-36);
  - `test_species_remove_params_handled` (T-13);
  - `test_species_state_position_from_qnfmt`: QNFMT 304 (spin) con F ≠ 0 →
    nessuna specie ricavata da F.
- **Bloccato da**: D1.
- **Documentazione**: report §4 flusso 6, schede B-19/B-24/B-43; A4 R-32.
- **Stato**: ☐ non fatto — commit: —

### #14b — Progetto Pred&Fit: Hamiltoniani indipendenti e stati condivisi

- **Decisione di progetto (2026-09-12)**: non esistono due "tipi" di specie.
  L'unità eseguibile è l'**Hamiltoniano**; uno stato è sempre figlio di un
  Hamiltoniano. Il progetto è quindi:

  ```text
  Pred&Fit project
  ├─ Hamiltoniano H1 → H1.par / H1.var / H1.lin / H1.int → SPFIT/SPCAT H1
  │  ├─ Stato 0     → dipoli μa, μb, μc; concentrazione esterna c0
  │  ├─ Stato 1     → dipoli μa, μb, μc; concentrazione esterna c1
  │  └─ Stato 2     → dipoli μa, μb, μc; concentrazione esterna c2
  └─ Hamiltoniano H2 → H2.par / H2.var / H2.lin / H2.int → SPFIT/SPCAT H2
     ├─ Stato 0     → dipoli; c0
     └─ Stato 1     → dipoli; c1
  ```

  In ciascun `H.int`, FLAGS/TAG/QROT/FBGN/FEND/STR0/STR1/FQLIM/TEMP/MAXV sono
  comuni. Le sole carte ripetute sono `IDIP,DIPOLE`, una terna per stato: per
  esempio `1/2/3` per v=0, `111/112/113` per v=1. Questo segue direttamente il
  formato di SPCAT documentato in *spinv15 annotated* §"Format of the .int
  File" e nel CRIB sheet. La concentrazione non è un parametro Pickett e non va
  scritta nel `.int`: è un moltiplicatore SpectraVisual applicato dopo SPCAT.
  Per una transizione fra stati diversi useremo la concentrazione dello **stato
  inferiore**, perché è la sua popolazione a pesare l'assorbimento; per le righe
  diagonali coincide naturalmente con lo stato della riga.

- **Cosa fare**:
  1. estrarre da `PredFitState` una struttura `HamiltonianModel` con nome/ID,
     parametri e riga opzioni, impostazioni `.int` (inclusi T rot e cut), stati,
     storico SPFIT e directory `.fit/Hxx/`; `PredFitState` conserva la lista e
     l'Hamiltoniano attivo per l'editor;
  2. uno `StateModel` contiene solo `state_index`, nome, abilitazione alla
     predizione, `mu[3]` e `concentration` (default 1). Non contiene T rot,
     cutoff, range o file propri;
  3. associare ogni `Assignment` e ogni riga di catalogo generata al suo
     `hamiltonian_id`; Fit scrive nell'unico `H.lin` esclusivamente gli
     assignment di H. Le righe restano miste nello stesso `.lin` quando gli
     stati sono accoppiati, preservando i fit di tunneling;
  4. Calculate/Fit operano su H attivo oppure, nel comando "all", una volta per
     Hamiltoniano. I CAT risultanti sono uniti solo per la visualizzazione,
     senza perdere provenienza; T rot/cut/dipoli di H1 non possono influire su
     H2;
  5. la sessione serializza tutti gli Hamiltoniani, i loro stati e la proprietà
     di ogni assignment. La migrazione della sessione v3 crea H1 dal modello
     unico corrente;
  6. l'editor mostra prima la lista degli Hamiltoniani (aggiungi, duplica,
     rinomina, seleziona), poi i parametri e gli stati dell'H selezionato. Il
     vecchio "Species" diventa esplicitamente "States of Hx".

- **Sequenza di implementazione**:
  1. consolidare subito il singolo Hamiltoniano: un solo `.int`, T rot comune,
     dipoli e concentrazioni per stato, migrazione delle sessioni esistenti;
  2. introdurre il contenitore `HamiltonianModel` e la migrazione H1 senza
     cambiare ancora l'interfaccia di calcolo;
  3. aggiungere selezione/creazione H e directory per-H;
  4. aggiungere provenienza agli assignment/CAT e Calculate/Fit per-H;
  5. solo dopo riprendere il fit delle intensità: potrà fittare le
     concentrazioni esterne per stato, mai riscrivere `H.int`.
- **Test che devono passare**:
  - `test_multistate_writes_one_int_with_shared_trot`: H con due stati → solo
    `model.int`, una TEMP comune e IDIP 1/2/3 + 111/112/113; concentrazione
    assente dal file;
  - `test_two_hamiltonians_keep_independent_int_controls`: H1/H2 hanno T rot e
    cut diversi → due `.int`, ciascuno con solo i propri valori;
  - `test_fit_per_hamiltonian_uses_own_lines`: il `.lin` di H contiene solo gli
    assignment di H ma conserva insieme tutti i suoi stati;
  - `test_multihamiltonian_session_roundtrip`: stati, concentrazioni e ownership
    degli assignment sopravvivono a save/load;
  - `test_interstate_line_uses_lower_state_concentration`.
- **Bloccato da**: — (design confermato dall'utente).
- **Documentazione**: report §4 flussi 5 e 6, §6.4, §6.5, §6.7, §10; A4 scenario nuovo.
- **Stato**: ◐ fasi 1–4 parzialmente implementate il 2026-09-12 — il singolo
  Hamiltoniano scrive un solo `.int` multi-stato con T rot comune; le sessioni
  precedenti con Tred per specie sono migrate allo stato selezionato. Il nuovo
  contenitore `HamiltonianModel` conserva H indipendenti, selezionabili e
  creati nuovi o duplicati dalla sidebar permanente di Advanced; la stessa
  gerarchia H → stati resta visibile in Parameters, Lines, Fitting e States.
  "Add Hamiltonian" crea il modello Pickett di default, mentre "Duplicate"
  resta esplicitamente il template dell'H selezionato. Una sessione v5 salva
  tutti gli H in record `h4*`. Dal secondo H in poi i file Pickett vivono in `.fit/Hxxx/`.
  `assignments.txt` formato 2 conserva inoltre `HamiltonianID`, e un Fit scrive
  solo gli assignment del modello attivo. Test aggiunti:
  `test_multistate_writes_one_int_with_shared_trot`,
  `test_hamiltonian_switch_keeps_independent_models`,
  `test_multihamiltonian_session_roundtrip`,
  `test_two_hamiltonians_keep_independent_int_controls`,
  `test_assignment_owner_keeps_same_qn_in_two_hamiltonians`,
  `test_add_hamiltonian_starts_fresh_and_keeps_source` (suite 71/71 PASS).
  Restano l'unione dei CAT per la visualizzazione e il comando "Calculate all"
  per tutti gli H; report ed esclusioni SPFIT sono già separati per H.

### #15 — Validazione di parametri e dipoli

- **Bug/issue**: [B-38](README.md#b-38) parametri con ID 0 o duplicati arrivano a
  SPFIT; [B-51](README.md#b-51) cancellata la riga A, il pannello mostra ancora A
  e SPCAT calcola senza A; [B-33](README.md#b-33) tre regole diverse per i dipoli;
  U-15.
- **Cosa fare**: `add_parameter` [predfit.c:1346-1351](../../predfit.c#L1346-L1351)
  aggiunge la riga solo al commit di un ID valido (> 0 e non duplicato, altrimenti
  messaggio) [predfit.c:1272-1289](../../predfit.c#L1272-L1289); Calculate e Fit
  rifiutati se manca A, B o C di uno stato calcolato, oppure il campo del pannello
  ricrea la riga [predfit.c:506-522](../../predfit.c#L506-L522),
  [1355-1363](../../predfit.c#L1355-L1363),
  [controller.c:952-959](../../controller.c#L952-L959); un solo validatore per μ
  (regola D10) usato da pannello Pred&Fit, Advanced e Intensity analysis, con
  messaggio sui valori rifiutati.
- **Test che devono passare**:
  - `test_param_id_zero_or_duplicate_rejected` (T-31, R-27);
  - `test_calculate_requires_abc` (T-44, R-40);
  - `test_dipole_rules_consistent` (T-26, R-23).
- **Bloccato da**: D10, solo per la regola dei dipoli.
- **Documentazione**: schede B-33, B-38, B-51; A3 U-15; A4 R-23, R-27, R-40.
- **Stato**: ◐ fix parziale il 2026-09-12 — commit codice: `a3934e5` — test
  aggiunti: `test_param_id_zero_or_duplicate_rejected`,
  `test_calculate_requires_abc` (suite 45/45 PASS). ID positivi/unici e A/B/C
  per ogni specie calcolata sono obbligatori prima dei file Pickett; la regola
  comune per i dipoli resta bloccata da D10.

### #16 — Tastiera e testo tra finestre

- **Bug/issue**: [B-49](README.md#b-49) testo e Invio digitati in Advanced o
  Settings confermano il campo attivo della finestra principale; U-03 scorciatoie
  della finestra principale eseguite da tasti premuti nelle altre finestre; U-13;
  U-02 Cmd/Ctrl più un tasto esegue l'azione del tasto semplice.
- **Cosa fare**: `handle_app_events` [controller.c:150-237](../../controller.c#L150-L237)
  ignora `SDL_TEXTINPUT` e `SDL_KEYDOWN` con `windowID` diverso da quello della
  finestra principale, tranne le scorciatoie globali volute (Cmd+F, Cmd+B),
  dichiarate esplicitamente; il campo attivo viene confermato quando una finestra
  secondaria prende il focus; `handle_keydown`
  [controller.c:986-1053](../../controller.c#L986-L1053) con Cmd/Ctrl esegue solo
  le combinazioni previste.
- **Test che devono passare**:
  - `test_text_from_secondary_window_ignored` (T-42, R-38 a);
  - `test_keys_from_secondary_window_ignored` (T-42, R-38 b);
  - `test_cmd_modified_keys_not_plain_actions` (U-02).
- **Documentazione**: report §5.4, scheda B-49; A3 A3.8, A3.9, U-02, U-03, U-13;
  A4 R-38.
- **Stato**: ☑ fatto il 2026-09-12 — commit codice: `656a1ab` — test aggiunti:
  `test_text_from_secondary_window_ignored`, `test_keys_from_secondary_window_ignored`,
  `test_cmd_modified_keys_not_plain_actions` (suite 48/48 PASS).

### #17 — Spettro attivo e coda di caricamento

- **Bug/issue**: [B-29](README.md#b-29) rimuovere uno spettro precedente cambia
  lo spettro attivo; [B-14](README.md#b-14) coda a slot unico (richieste perse
  nello stesso frame); [B-34](README.md#b-34) drop di più file: solo l'ultimo;
  U-17.
- **Cosa fare**: `remove_spectrum` [main.c:331-342](../../main.c#L331-L342)
  decrementa `active_spec` se l'indice rimosso lo precede; `pending_pred_path`,
  `pending_spec_path`, `pending_load` ([types.h:334-348](../../types.h#L334-L348),
  [controller.c:218-226](../../controller.c#L218-L226),
  [main.c:518-522](../../main.c#L518-L522)) diventano una coda FIFO di richieste,
  consumata per intero a ogni frame.
- **Test che devono passare**:
  - `test_remove_spectrum_keeps_active` (T-22, R-19);
  - `test_drop_many_files_loads_all` (T-27, R-24);
  - `test_calculate_and_drop_same_frame`: nessuna richiesta persa, ordine
    rispettato.
- **Documentazione**: report §5.2, schede B-14/B-29/B-34; A3 U-17; A4 R-19 e R-24.
- **Stato**: ☑ fatto il 2026-09-12 — commit codice: `ccc80ba`, `ff5c7d4` — test
  aggiunti: `test_remove_spectrum_keeps_active`, `test_drop_many_files_loads_all`,
  `test_calculate_and_drop_same_frame` (suite 51/51 PASS). La rimozione conserva
  l’identità della traccia attiva; una FIFO da 32 richieste conserva invece tutti
  i drop e i cataloghi prodotti da Calculate/Fit, nell’ordine ricevuto. Anche una
  sessione aperta dalla coda non scarta più le richieste che la seguono.

### #18 — Impostazioni e persistenza di Pred&Fit

- **Bug/issue**: [B-36](README.md#b-36) *Restore defaults* svuota SPCAT, SPFIT e
  la cartella dati (U-16); [B-44](README.md#b-44) l'incertezza `.lin` non
  sopravvive al riavvio; [B-45](README.md#b-45) Calculate sovrascrive il `.var`
  di SPFIT e la colonna ERR perde significato; M-06 export e screenshot nella CWD.
- **Cosa fare**: `settings_restore_defaults` [settings.c:56-97](../../settings.c#L56-L97)
  non azzera programmi e cartella dati, oppure il pulsante
  [settings.c:966-972](../../settings.c#L966-L972) ripete `autodetect_program` e
  chiede conferma; l'incertezza `.lin` va nella sessione
  [predfit.c:185-238](../../predfit.c#L185-L238) e `settings_apply_defaults`
  [settings.c:299](../../settings.c#L299) non la sovrascrive quando la sessione la
  contiene; Calculate non sovrascrive il `.var` prodotto da SPFIT se i parametri
  non sono cambiati, oppure scrive su un file separato
  [predfit.c:667-675](../../predfit.c#L667-L675); `intensity_fit.ifit` e
  `spectravisual_export.bmp` in `data_dir` ([controller.c:448](../../controller.c#L448),
  [main.c:358](../../main.c#L358)).
- **Test che devono passare**:
  - `test_restore_defaults_keeps_paths` (T-29, R-25);
  - `test_line_error_persists` (T-37, R-33);
  - `test_calculate_after_fit_keeps_fitted_var` (T-38, R-34): dopo Fit e
    Calculate con gli stessi parametri la colonna ERR resta quella del fit;
  - `test_exports_go_to_data_dir` (M-06).
- **Documentazione**: report §5.7, §6.4, §6.8, schede B-36/B-44/B-45, M-06; A3
  U-16; A4 R-25, R-33, R-34.
- **Stato**: ◐ fix parziale il 2026-09-12 — commit codice: `b98d08f` — test
  aggiunti: `test_restore_defaults_keeps_paths`, `test_line_error_persists`,
  `test_calculate_after_fit_keeps_fitted_var`, `test_exports_go_to_data_dir`
  (suite 55/55 PASS). *Restore defaults* mantiene SPCAT/SPFIT/cartella dati;
  l’incertezza delle osservazioni `.lin` è serializzata nella sessione; un
  Calculate con modello invariato conserva le incertezze stimate da SPFIT in
  `model.var`; gli export correnti usano `data_dir`. Rimane intenzionalmente
  separato N-02: il salvataggio della sessione deve diventare solo esplicito,
  tramite il futuro pulsante **Save session** richiesto dall’utente.

### #19 — Percorsi con spazi ed esito dei processi esterni

- **Bug/issue**: [B-28](README.md#b-28) con uno spazio in `data_dir` SPCAT e SPFIT
  non partono; M-01 lo stato mostra "exit 256" invece del codice d'uscita.
- **Cosa fare**: sostituire `system("cd %s && …")`
  [predfit.c:717-721](../../predfit.c#L717-L721), [985](../../predfit.c#L985),
  [1006](../../predfit.c#L1006), [1010](../../predfit.c#L1010) con `fork` +
  `chdir(work_dir)` + `execv` (o `posix_spawn`), senza shell; nello stato
  `WEXITSTATUS` o il segnale.
- **Test che devono passare**:
  - `test_spaces_and_quotes_in_data_dir` (T-21, R-18): `data_dir` con uno spazio e
    un apostrofo → Calculate e Fit riusciti;
  - `test_process_exit_status_decoded` (M-01): un programma che esce con 1 → "exit 1".
- **Documentazione**: report §5.1, §5.8, scheda B-28, M-01; A4 R-18.
- **Stato**: ☑ fatto il 2026-09-12 — commit codice: `79f231c` — test aggiunti:
  `test_spaces_and_quotes_in_data_dir`, `test_process_exit_status_decoded`
  (suite 57/57 PASS). SPCAT/SPFIT partono con `fork` + `chdir` + `execv`, quindi
  né spazi né apostrofi attraversano una shell; gli errori riportano il vero
  codice di uscita o il segnale ricevuto.

### #20 — Find peaks

- **Bug/issue**: [B-30](README.md#b-30) soglia calcolata sul segnale grezzo,
  finestra di rumore ignorata, larghezza negativa che legge fuori dal buffer;
  U-04 Find peaks ignora l'offset della traccia.
- **Cosa fare**: `run_peak_finder` [algorithms.c:73-139](../../algorithms.c#L73-L139)
  stima il rumore (RMS o MAD dei residui) su `noise_pts` punti e applica la
  soglia al rumore; larghezza tra 1 e il numero di punti in vista, controllata al
  commit [controller.c:890-892](../../controller.c#L890-L892); finestra di ricerca
  convertita con `exp_offset` come nel trascinamento destro
  ([controller.c:335](../../controller.c#L335) contro [775-776](../../controller.c#L775-L776)).
- **Test che devono passare**:
  - `test_peakfinder_baseline_invariant` (T-23, R-20): baseline 1 e 10 → stessi
    picchi;
  - `test_peakfinder_noise_window_used`: rumore noto → soglia dipendente da
    `noise_pts`;
  - `test_peakfinder_width_bounds`: larghezza ≤ 0 rifiutata; nessuna lettura fuori
    limite, verificata anche con Guard Malloc (`DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib
    MALLOC_PROTECT_BEFORE=1 ./tests/test_audit test_peakfinder_width_bounds`);
  - `test_find_peaks_respects_offset` (U-04).
- **Documentazione**: scheda B-30; A3 U-04; A4 R-20.
- **Stato**: ☑ fatto il 2026-09-12 — commit codice: `a2b91ff` — test aggiunti:
  `test_peakfinder_baseline_invariant`, `test_peakfinder_noise_window_used`,
  `test_peakfinder_width_bounds`, `test_find_peaks_respects_offset` (suite
  61/61 PASS). La soglia usa la MAD dei residui rispetto a due finestre locali
  di rumore; gli indici e gli input sono limitati alla vista; Find traduce le
  coordinate visuali nell’asse grezzo prima di memorizzare il picco.

### #21 — Pred&Fit opt-in

- **Bug/issue**: [B-26](README.md#b-26): Pred&Fit si attiva da solo (restore
  automatico se esiste `.fit/model.cat`) e scrive `.fit/` a ogni caricamento di
  spettro e all'uscita. Vincolo 4.
- **Cosa fare**: secondo D7, nessun restore automatico senza richiesta, oppure
  una preferenza esplicita in Settings [main.c:401](../../main.c#L401);
  `predfit_save_session` solo dopo un'azione di Pred&Fit, non a ogni caricamento
  o uscita ([main.c:450](../../main.c#L450), [521](../../main.c#L521),
  [526-527](../../main.c#L526-L527), [545](../../main.c#L545)); sessione generale
  (spettri, vista) separata dal modello Pred&Fit (report §10).
- **Test che devono passare**:
  - `test_cat_workflow_creates_no_fit_dir` (vincolo 4): aprire `.cat` e spettro,
    assegnare, salvare, uscire → nessuna cartella `.fit/` creata;
  - `test_no_auto_restore_without_request` (secondo D7).
- **Bloccato da**: D7.
- **Documentazione**: report tabella dei vincoli, §4 flussi 1 e 8, §5.2, §5.7,
  scheda B-26; A4 R-13.
- **Stato**: ☐ non fatto — commit: —

### #22 — Prestazioni del caricamento della lista

- **Bug/issue**: [B-48](README.md#b-48): caricamento di `assignments.txt` con
  costo cubico (2000 righe 2,7 s).
- **Cosa fare**: `load_existing_assignments` [loader.c:609-667](../../loader.c#L609-L667)
  accumula le righe e deduplica una sola volta alla fine ("vince l'ultima
  occorrenza"); indice per identità in `add_or_update_assignment`
  [loader.c:549-572](../../loader.c#L549-L572).
- **Test che devono passare**:
  - `test_load_5000_rows_fast` (T-41, R-37): 5000 righe in meno di 1 s, con lo
    stesso risultato della versione attuale su 250 righe.
- **Documentazione**: scheda B-48; A4 R-37.
- **Stato**: ◐ fix parziale il 2026-09-12 — commit codice: `7169e8b`. Il lettore
  accumula le righe valide e deduplica una sola volta alla fine, conservando
  l’ultima occorrenza come prima. Il benchmark dedicato di 5000 righe resta da
  aggiungere prima di dichiarare il requisito prestazionale definitivamente chiuso.

### #23 — Vista iniziale, estensioni e input numerici

- **Bug/issue**: [B-31](README.md#b-31) pan verticale senza spettro: divisione
  per zero e asse a −∞; [B-50](README.md#b-50) asse Y 0…1 quando il catalogo
  arriva prima dello spettro (U-14); [B-37](README.md#b-37) `.CAT` maiuscolo
  aperto come spettro; M-02 campi numerici senza validazione, con *Jump* che
  accetta min > max (U-18).
- **Cosa fare**: `handle_keydown` [controller.c:1062](../../controller.c#L1062)
  ignora i tasti Y se `exp_h ≤ 0`; `add_spectrum` [main.c:266-279](../../main.c#L266-L279)
  inizializza Y dal primo spettro anche se il catalogo è arrivato prima, e X come
  unione degli intervalli; `path_looks_like_cat`
  [controller.c:980-983](../../controller.c#L980-L983) e [main.c:392](../../main.c#L392)
  usano `strcasecmp`; `commit_text_input` [controller.c:882-973](../../controller.c#L882-L973)
  legge con `strtod`/`strtol` controllando il resto della stringa, con limiti per
  campo e min < max per *Jump* e filtri.
- **Test che devono passare**:
  - `test_vertical_pan_without_spectrum` (T-24, R-21): valori finiti, nessun
    errore UBSan;
  - `test_initial_view_from_first_spectrum` (T-43, R-39);
  - `test_uppercase_cat_extension` (T-30, R-26);
  - `test_numeric_fields_rejected` (M-02, U-18).
- **Documentazione**: schede B-31, B-37, B-50, M-02; A3 U-14 e U-18; A4 R-21,
  R-26, R-39.
- **Stato**: ☐ non fatto — commit: —

### #24 — UI e pulizia

- **Bug/issue**: U-01 con 13 o più assignment l'ultimo non si vede; U-05 campo
  *Start* di Pred&Fit senza effetto; U-07 marcatori "assigned" presi da
  `assigned.lin` nella CWD; U-09 il risultato di *Measure* non resta a schermo;
  U-10 Help descrive D come "Dipole moments"; U-11 `README.md` del progetto non
  allineato; U-12 codice UI morto; M-04 catena `else if` interrotta; M-05 "/50"
  duplicato (dettagli in [A3.13](A3-ui-layout-eventi.md#a313-difetti-e-incongruenze-ui-rilevati)
  e nelle [voci minori](README.md#voci-minori)).
- **Cosa fare**: scroll massimo = `n` − righe visibili
  ([controller.c:850](../../controller.c#L850), [876](../../controller.c#L876),
  [ui_panels.h:190](../../ui_panels.h#L190)); campo *Start* collegato alla
  frequenza minima o rimosso; marcatori "assigned" dagli assignment correnti;
  risultato di *Measure* visibile fino alla misura successiva
  ([controller.c:646-648](../../controller.c#L646-L648)); testi di Help
  ([view.c:1649](../../view.c#L1649)) e del README del progetto
  ([README.md:86-92](../../README.md#L86-L92)) allineati al comportamento;
  rimozione del codice morto elencato in U-12; `else if` in
  [controller.c:889](../../controller.c#L889); NITR in una costante usata da
  [predfit.c:669](../../predfit.c#L669) e [763](../../predfit.c#L763).
- **Test che devono passare**:
  - `test_assignment_list_shows_last_row` (U-01);
  - `test_pf_start_field_effect` (U-05);
  - `test_assigned_markers_from_list` (U-07);
  - build senza warning nuovi con `-Wall` (U-12, M-04, M-05); U-09, U-10 e U-11
    verificati a vista e annotati nel commit.
- **Documentazione**: A3 A3.13 (stato di U-01…U-18); `README.md` del progetto.
- **Stato**: ☐ non fatto — commit: —

### #25 — Importazione esplicita di un modello SPFIT/SPCAT esterno

- **Richiesta**: l'utente deve poter consegnare al programma i file di input
  Pickett già esistenti, senza dover ricreare a mano parametri, righe di fit e
  opzioni. La proposta di convenzione è una cartella
  `data_dir/.fit/load/` contenente un sottoinsieme di `model.par`, `model.var`,
  `model.lin` e `model.int` (oppure file con lo stesso suffisso e un prefisso
  comune). Non è un restore automatico: l'import parte solo da un'azione
  esplicita nella UI Pred&Fit, ad esempio **Import model**.
- **Cosa fare**:
  - rilevare e mostrare il contenuto di `.fit/load/` solo dopo il comando
    esplicito; accettare anche una scelta di cartella/file dalla UI, se il
    backend dei dialog lo consente;
  - validare ogni file prima di cambiare lo stato: `.par/.var` devono avere un
    modello e parametri Pickett leggibili, `.lin` righe con NQN compatibile,
    `.int` opzioni e specie coerenti. Un set parziale è importabile, ma le
    funzioni che richiedono dati mancanti (Calculate/Fit) devono dirlo e non
    creare file nuovi;
  - caricare i dati nei normali contesti in memoria (parametri/opzioni,
    assignment, impostazioni di intensità), passandoli per gli stessi
    validatori e deduplicazioni dell'editing UI; non trattare il testo esterno
    come stato fidato;
  - mostrare un riepilogo e richiedere conferma prima di sostituire un modello
    Pred&Fit o assignment già presenti. In caso di errore l'import deve essere
    transazionale: nessuna modifica parziale dello stato attivo;
  - dopo la conferma, copiare i file validati nella directory di lavoro
    `.fit/` con nomi canonici, mantenendo in `load/` gli originali come input
    non modificato. `load/` non viene mai usata come directory di output da
    Calculate/Fit;
  - aggiornare catalogo, selezioni e rendering soltanto quando esiste un
    catalogo importato o dopo un Calculate esplicito. Nessun SPCAT/SPFIT viene
    eseguito dall'operazione di importazione stessa.
- **Test che devono passare**:
  - `test_import_complete_pickett_model`: import di `.par/.var/.lin/.int`
    valido → stato Pred&Fit e assignment corrispondenti, senza eseguire
    programmi esterni;
  - `test_import_partial_model_reports_missing_requirements`: set parziale →
    le parti disponibili sono caricate, mentre Calculate/Fit riportano in modo
    esplicito il file o il dato mancante;
  - `test_import_invalid_model_is_transactional`: un file malformato o NQN
    incompatibile non modifica il modello o gli assignment già attivi;
  - `test_import_requires_confirmation_before_replace`: un modello attivo non
    viene sovrascritto senza conferma;
  - `test_import_load_directory_never_overwritten`: Calculate/Fit scrivono
    solo in `.fit/`, mai in `.fit/load/`.
- **Documentazione**: README (workflow Pred&Fit), A3 (controllo UI e messaggi),
  A4 (persistenza e rami di errore); aggiungere fixture Pickett minime nei test.
- **Stato**: ☐ pianificato su richiesta utente il 2026-09-12 — da iniziare solo
  dopo la stabilizzazione delle funzioni base di Pred&Fit e dopo #21.

## Dopo i fix (facoltativo)

Il refactor verso l'architettura target (report [§10](README.md#10-architettura-target):
catalogo, assignment, Pred&Fit e intensità come contesti separati) si fa solo
dopo #24 e solo su richiesta dell'utente. Criterio: tutta la suite verde e i
vincoli 1–6 verificati da test dedicati.

## Difetti nuovi trovati durante i fix

| # | Descrizione | Prova | Trovato al passo | Passo in cui correggerlo |
|---|---|---|---|---|
| N-01 | Il numero di righe scartate dal passo #1 sta in `error_message`, che `add_spectrum` azzera a ogni caricamento riuscito ([main.c:282](../../main.c#L282)). All'avvio `spectravisual spettro.txt catalogo.cat` il catalogo è caricato prima dello spettro, quindi l'avviso sparisce subito. È lo stesso canale unico, sovrascritto da qualsiasi messaggio, di U-06; dal passo #4 vale anche per il resoconto della lettura di `assignments.txt`. | lettura di `add_spectrum` e dell'ordine di caricamento in `main` (prima il `.cat`, poi gli spettri) | #1 | #24 |
| N-02 | La sessione Pred&Fit viene ancora salvata automaticamente da Calculate/Fit (`write_inputs`), quindi un esperimento o un modello sbagliato può sovrascrivere uno stato buono. Richiesta utente: eliminare gli autosalvataggi e aggiungere un pulsante **Save session** nella finestra principale, accanto a Export, che sia l'unico writer esplicito della sessione completa. | richiesta utente, 2026-09-11; writer `predfit_save_session` in `write_inputs` | #7 | nuovo passo, dopo una decisione su D7/sessione generale |
| N-03 | L'export deve produrre un PNG con una specifica visiva da definire; l'attuale screenshot esporta `spectravisual_export.bmp` nella CWD. | richiesta utente, 2026-09-11; `save_screenshot` in `main.c` | #7 | nuovo passo, dopo la specifica PNG |
