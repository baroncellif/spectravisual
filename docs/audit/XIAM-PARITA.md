# Parità XIAM ↔ Pred&Fit

Audit della finestra XIAM (`xiam.c`) contro la finestra Pred&Fit Advanced
(`predfit.c`). Obiettivo: le due finestre devono avere **la stessa struttura,
le stesse sottofinestre e le stesse voci**; la sola differenza ammessa è
l'insieme dei *parametri* — nativi XIAM (GHz, `fit`/`dqu`/`dqx`, blocchi I–VIII)
invece che Pickett (`.par`/`.var`/`.int`, id numerici).

Ogni voce ha la forma **cosa manca → cosa fare → test**. Lo stato è aggiornato
mano a mano; un passo si chiude solo con `make test` verde.

## 0. Struttura della finestra

| | Pred&Fit | XIAM (prima) |
|---|---|---|
| Schede | 6: Parameters, Lines, Fitting, **States**, Hamiltonians, Simulation | 5: manca States |
| Navigatore | elenco gerarchico + H su/giù, + Hamiltonian, Rename, Duplicate, Delete (con conferma) | solo "+ Open .xi" e "Reload active .xi" |
| Scroll navigatore | sì, con clamp | assente |
| Editor di campo | testo completo (Home/End/Delete/selezione shift/⌘A⌘C⌘X⌘V, caret al click) | solo Backspace e ←/→ |
| Hover di riga | sì (Lines, Fitting) | assente |

## 1. Bug di contabilità dello scroll  — *X-01*

* `xiam_handle_event` calcola il limite dei parametri su `(u.rows.h - 64)/row_h`
  mentre `xiam_render_parameters` disegna `(u.table.h - 97)/row_h` righe: il
  limite è sfalsato di una riga.
* Le schede Hamiltonians e Simulation incrementano lo scroll **senza limite
  superiore**: si scorre nel vuoto.
* L'elenco del navigatore è disegnato da `i = 0` senza offset: oltre ~9
  Hamiltoniane le righe finiscono fuori dal clip e non sono più raggiungibili.

→ derivare il numero di righe visibili in un solo posto, aggiungere i clamp,
dare al navigatore il suo scroll.
**Test**: `test_xiam_scroll_limits_follow_the_rendered_rows`.

## 2. Parametri non aggiungibili né eliminabili — *X-02*

Pred&Fit ha `+ parameter` e la `×` per riga (`add_parameter`,
`delete_parameter`). XIAM non ha nessuno dei due: l'elenco è esattamente quello
che il `.xi` dichiarava.

→ `+ parameter` inserisce una nuova dichiarazione in **blocco III** di
`source_lines` (è da lì che `xiam_write_input` riscrive i valori: un parametro
senza `source_line` non arriverebbe mai a XIAM) e apre subito l'editor del nome;
la `×` commenta la dichiarazione con `\` e toglie la riga dalla tabella,
insieme all'eventuale direttiva `fit`/`dqu`/`dqx`. Nome editabile in cella.
**Test**: `test_xiam_parameter_added_and_removed_reaches_the_input`.

## 3. Editor di testo ridotto — *X-03*

`xiam_handle_event` gestisce solo Invio/Esc/Tab/Backspace/←/→. Mancano
Canc, Home/End, la selezione con shift, ⌘A/⌘C/⌘X/⌘V e il posizionamento del
caret cliccando nel campo già in modifica.

→ replicare `advanced_edit_event`.
**Test**: `test_xiam_field_editor_matches_predfit`.

## 4. Navigatore senza gestione del progetto — *X-04*

Mancano: rinomina (doppio click e pulsante), duplica, rimuovi, riordina su/giù.
`xiam_remove_active_hamiltonian` esiste ma è raggiungibile solo dalla scheda
Hamiltonians.

→ stessa pulsantiera di Pred&Fit. La duplicazione copia `source_lines`,
parametri e controlli con un `model_id` nuovo (le assegnazioni restano del
modello originale, come in Pred&Fit).
**Test**: `test_xiam_navigator_manages_the_project`.

## 5. Scheda States assente — *X-05*

In Pred&Fit la scheda States contiene il record di controllo `.int` (FLAGS,
TAG, QROT, FBGN, FEND, STR0, FQLIM, TROT, MAXV, sigma) più una riga per stato
con USE/PRED/nome/v/mu a/mu b/mu c/concentrazione.

L'equivalente nativo XIAM esiste e non è esposto da nessuna parte:
* **blocco II** — record di controllo (`ncyc`, `ints`, `ntop`, `nfold`,
  `reduc`, `print`, `ndata`, `maxm`, `maxvm`, `convg`, `spin`, `spin2`);
* **blocco V** — simmetrie torsionali (`/A1 S 0`, `/E2 S 1`, …);
* **blocco VI** — stati torsionali (`V 0`, `V 1`, …).

→ nuova scheda in posizione 3 (stesso ordine di Pred&Fit): celle di controllo
del blocco II in alto, righe = simmetrie e stati torsionali, con la casella
USE che commenta/decommenta la riga nativa e i valori (sigma, v) editabili.
**Test**: `test_xiam_states_tab_edits_blocks_ii_v_vi`.

## 6. Fitting senza Undo e senza click di riga — *X-06*

Pred&Fit ha `Fit` + `Undo fit` (`predfit_undo_last_fit`), la `×` per riga e
l'hover. XIAM ha `Fit` / `Predict updated model` / `Save fit .xi…`, nessun
undo e **nessuna gestione del click** nelle righe dei residui.

→ storia dei parametri prima di ogni fit (`Undo fit` ripristina valori ed
errori), click di riga che include/esclude, `×` che elimina, hover.
**Test**: `test_xiam_undo_fit_restores_the_parameters`.

## 7. Hamiltonians senza sessione né import — *X-07*

Pred&Fit: `Save session` (con `*` se sporca), `Restore session`,
`Import .fit/load`. XIAM: `+ Open .xi`, `Reload`, `Remove`.
`xiam_import_load_dir` esiste già ma **non è raggiungibile dalla UI**.

→ aggiungere i tre pulsanti di Pred&Fit accanto a quelli specifici di XIAM.
**Test**: `test_xiam_hamiltonians_tab_saves_and_imports`.

## 8. Simulation ridotta — *X-08*

Pred&Fit: casella per H e per stato, pastiglia colore della traccia,
interruttori mu a/b/c, `Traces: …`, Check all / Uncheck all, Simulate.
XIAM: solo la casella per H.

→ colore di traccia per Hamiltoniana XIAM (`trace_color` in
`XiamHamiltonian`, esposto al broadening con un filtro per solo
`hamiltonian_id`), interruttori `mu_x`/`mu_y`/`mu_z` sui parametri nativi,
pulsante `Traces:` condiviso con Pred&Fit.
**Test**: `test_xiam_simulation_traces_and_dipoles`.

## 9. Differenze che restano, e perché

* **Errori dei parametri**: in Pred&Fit sono editabili perché il `.var` li
  porta in ingresso; in XIAM l'incertezza esiste solo come *uscita* del report
  (`Parameters and Errors`), quindi la colonna resta in sola lettura.
* **ID numerico dei parametri**: Pickett identifica le costanti con l'id del
  `.par`; XIAM con il nome nativo (case-sensitive: `DJ` ≠ `dj`). La colonna ID
  è sostituita dal nome editabile.
* **QROT / TROT / MAXV**: sono voci `.int`; l'equivalente XIAM è `temp` più i
  controlli del blocco II, che la scheda States espone al loro posto.
* **Stati**: in Pickett uno stato è un figlio dell'Hamiltoniana con dipoli e
  concentrazione propri; in XIAM simmetrie e stati torsionali restano dentro il
  modello e condividono `mu_x/mu_y/mu_z`, per cui le righe di States non hanno
  colonne di dipolo per riga.
