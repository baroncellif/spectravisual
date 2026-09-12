# Audit forense: assignment, Pred&Fit e fitting delle intensità

## Mandato per l'LLM / software engineer

Esegui un audit **completo, end-to-end e basato sul codice** dei problemi
descritti qui sotto. Questo incarico non autorizza correzioni immediate: prima
va prodotta una mappa tecnica affidabile dell'intero sistema, con prove
puntuali di dove ogni dato nasce, viene trasformato, viene salvato e viene
riutilizzato.

Non limitarti alle funzioni che contengono i nomi `assignment`, `fit`, `lin`,
`cat`, `NQN` o `SPFIT`. Segui tutte le dipendenze indirette: eventi UI, handler
click/tastiera, stato globale dell'app, caricamento/salvataggio delle sessioni,
selezione righe, rendering, normalizzazione, spettri sperimentali, cataloghi,
cache, file `.fit`, import/export e codice che scrive gli stessi campi.

Lo scopo è capire **come si collegano i bug**, non indovinare una causa locale.

## Requisito aggiuntivo: cartografia completa dell'applicazione

L'audit non è limitato ai problemi P0/P1/P2. Deve produrre una mappatura dello
stato attuale dell'intero programma, come un disegno tecnico di un macchinario:
non solo i componenti principali, ma anche collegamenti, viti, parti apparentemente
secondarie, diramazioni rare, codice difettoso e comportamenti ereditati.

In particolare, devi mappare **tutto ciò che esiste nel codice**, anche quando
non sembra coinvolto direttamente nei bug:

- ogni finestra, pannello, tab, layout, widget, tasto, scorciatoia, click,
  drag, scroll, hover, input di testo, menu, messaggio di stato ed errore;
- ogni azione utente e la sua catena completa fino a handler, funzioni,
  mutazioni di memoria, file, cache, rendering e conseguenze collaterali;
- ogni funzione, struttura, campo, variabile globale/statica, callback, timer,
  cache, flag e percorso di inizializzazione/distruzione;
- tutti i flussi di caricamento, salvataggio, autosave, restore, import/export,
  previsione, assegnazione, fitting, normalizzazione, broadening, rendering,
  zoom e selezione;
- ogni file letto, scritto o soltanto controllato, incluse directory di lavoro,
  file temporanei e compatibilità con formati vecchi;
- codice non raggiunto, duplicato, incompleto, apparentemente inutile o già
  sbagliato: va comunque documentato nel comportamento corrente, non omesso o
  “ripulito” mentalmente.

### Diagramma completo obbligatorio

Consegna sia una spiegazione testuale sia diagrammi disegnati in un formato
versionabile e renderizzabile nel repository (preferibilmente Mermaid; se un
diagramma diventasse illeggibile, suddividilo in diagrammi collegati, con ID
coerenti e riferimenti incrociati).

I diagrammi devono includere almeno:

1. **Mappa generale dell'architettura**: moduli, file, sottosistemi, strutture
   dati e relazioni di lettura/scrittura.
2. **Macchina a stati dell'app**: avvio, nessun dato, spettro caricato, CAT
   caricato, righe selezionate, assignment presenti, Pred&Fit attivo,
   previsione/fit in corso, restore e chiusura.
3. **Mappa completa della UI e dei layout**: ogni finestra/pannello/tab,
   controllo visuale, azione possibile e handler raggiunto. Indica quali
   layout vengono calcolati, da quali dimensioni/flag dipendono e chi renderizza
   ogni elemento.
4. **Diagramma evento → effetto** per ogni classe di input: mouse, tastiera,
   testo, scroll, resize, apertura/chiusura finestre e caricamento file.
5. **Data-flow dei dati sperimentali, cataloghi, assignment, Pred&Fit e
   fitting intensità**, fino ai file e di ritorno al rendering.
6. **Call graph operativo**: una rete delle funzioni rilevanti per ciascun
   percorso utente, comprese chiamate indirette e punti in cui uno stesso campo
   è letto/scritto.
7. **Mappa della persistenza**: dati solo in memoria, dati serializzati, chi li
   ripristina e fallback/errori.
8. **Mappa degli errori e rami anomali**: controlli falliti, fallback,
   sentinelle, casi legacy e percorsi che producono stato parziale o incoerente.

### Relazioni, precedenze e sovrascritture obbligatorie

I diagrammi non devono essere una collezione di blocchi isolati. Devono rendere
esplicite le **relazioni causali e di dipendenza** tra ogni oggetto, stato,
file e funzione. Per ogni freccia indica almeno: direzione, tipo di relazione,
funzione che la realizza e se il passaggio legge, copia, trasforma, invalida,
aggiorna o sovrascrive il dato.

La parola **ogni** è letterale: non devi mappare solo dati, file e funzioni,
ma le relazioni tra *qualsiasi* elemento presente nel programma. Quindi devono
comparire, quando esistono, anche relazioni tra:

- UI, widget, finestre, pannelli, tab, layout, geometrie, resize e rendering;
- eventi, scorciatoie, focus, selezione, hover, scroll, drag e stato della UI;
- condizioni, flag, rami, valori sentinella, fallback, messaggi e percorsi di
  errore;
- inizializzazione, loop principale, polling eventi, aggiornamento, cache,
  invalidazione, ordine temporale e teardown;
- configurazione, percorsi, build, librerie esterne, eseguibili SPFIT/SPCAT e
  loro input/output;
- codice legacy, duplicato, morto o apparentemente scollegato e il resto del
  sistema che può ancora raggiungerlo, dipenderne o subirne gli effetti.

Per ogni relazione devi specificare **perché esiste**, **quando è attiva**,
**quale direzione segue**, **che cosa la attiva**, **che cosa modifica** e
**quali altre relazioni innesca**. Una relazione può essere di controllo,
temporale, visiva, geometrica, di ownership, di dipendenza, di dati, di cache,
di persistenza, di errore o di side effect: tutte vanno classificate e rese
visibili. Non accettare nodi isolati senza una motivazione esplicita.

Questo vale anche e soprattutto per le **funzioni**: non basta elencarle per
file. Per ogni funzione, inclusi helper `static`, callback, funzione di
rendering, parser/writer, wrapper e codice di errore, mappa:

| Funzione | Chiamanti / eventi d'origine | Funzioni chiamate | Input | Output | Stato letto | Stato scritto | File/UI/cache toccati | Effetti indiretti |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |

Il call graph deve essere completo e bidirezionale: da ogni azione utente si
deve poter scendere fino alle primitive che compiono il lavoro, e da ogni
funzione si deve poter risalire a tutti gli eventi, timer o restore che possono
chiamarla. Per ogni chiamata, distingui una normale dipendenza di controllo da
una dipendenza di dati o da un effetto collaterale su stato condiviso.

Evidenzia in particolare funzioni che:

- leggono e scrivono lo stesso campo; 
- ricevono una copia di un dato mentre un'altra funzione modifica l'originale;
- aggiornano implicitamente stato globale o cache;
- hanno più chiamanti appartenenti a route diverse;
- vengono chiamate sia dalla UI sia da restore/autosave/fitting;
- ricostruiscono, deduplicano o sovrascrivono una lista già costruita da altre
  funzioni;
- modificano layout/rendering oppure dipendono da essi per decidere logica di
  dominio.

Per ogni funzione coinvolta in una relazione critica, mostra nel diagramma
anche le frecce **funzione → funzione**, **funzione → campo**, **campo →
funzione**, **funzione → file/UI** e **file/UI → funzione**. Devono quindi
risultare visibili non solo le relazioni tra file, ma anche, per esempio,
quale funzione crea `Assignment`, quale la sostituisce, quale la serializza,
quale rigenera `.lin`, quale rilegge `.lin`, quale aggiorna i parametri e quale
usa quei parametri per chiamare SPCAT/SPFIT.

Deve essere possibile seguire in entrambi i sensi, con riferimenti al codice,
catene del tipo:

```text
CAT caricato
  → PredLine / NQN
  → selezione grafica
  → click su un picco sperimentale
  → Assignment in memoria
  → assignments.txt
  → model.lin
  → SPFIT + parametri / Hamiltoniano
  → model.fit / model.par / model.var
  → SPCAT
  → model.cat
  → PredLine e rendering aggiornati
```

e anche catene concorrenti o cicliche del tipo:

```text
assignment creati dall'utente ─┐
assignment ripristinati da assignments.txt ─┼→ lista assignment in memoria
righe ripristinate da model.lin ────────────┘          │
                                                        ├→ deduplicazione / flag fit
                                                        ├→ writer model.lin
                                                        └→ writer assignments.txt
```

Per ogni nodo condiviso, devi specificare chiaramente:

1. tutte le fonti possibili che lo alimentano;
2. l'ordine temporale dei writer;
3. chi vince quando due fonti non concordano;
4. se un writer distrugge, invalida o rende stantio un dato precedente;
5. quali componenti successivi ricevono il cambiamento;
6. come il comportamento cambia tra avvio normale, restore, Calculate, Fit,
   undo, apertura di un CAT esterno e chiusura/riapertura dell'app.

Mostra in modo particolare, senza lasciare impliciti i legami:

- `assignments.txt` ↔ assignment in memoria ↔ `model.lin`;
- CAT esterno / `model.cat` ↔ `PredLine` ↔ selezione ↔ assignment;
- assignment + parametri + Hamiltoniano + opzioni `.int` → writer input
  Pred&Fit → SPFIT/SPCAT → file di output → stato/rendering;
- parametri definiti/modificati nella UI ↔ `.par/.var` ↔ Pred&Fit ↔ catalogo
  ricalcolato;
- fitting intensità ↔ spettro sperimentale ↔ assignment ↔ catalogo ↔ dipoli,
  temperature e concentrazioni;
- caricamento da file, salvataggio manuale, autosave/restore e cache, inclusi
  casi in cui un file può sovrascrivere o prevalere su una modifica in memoria.

Evidenzia graficamente i cicli, le relazioni molti-a-uno, le fonti concorrenti
e ogni passaggio in cui esiste ambiguità sulla fonte autorevole. Se ad esempio
`assignments.txt` e `model.lin` possono entrambi ricostruire una lista, il
diagramma deve dichiarare quale è la fonte di verità in ogni scenario e mostrare
la funzione che risolve il conflitto. Non basta dire che i componenti “sono
collegati”: ogni collegamento deve essere tracciato, spiegato e verificabile.

Ogni nodo del diagramma deve avere un identificatore che rimandi a una sezione
testuale con file, funzione e linea/intervallo di linee. Se una zona del codice
non può essere mappata con certezza, dichiarala come "non verificata" e spiega
quali dati mancano: non ometterla.

Il risultato deve permettere a un altro engineer di capire come funziona oggi
il programma senza aprire subito il sorgente, e di risalire da ogni dettaglio
del diagramma al codice corrispondente. Solo dopo questa cartografia totale si
potrà decidere in sicurezza come migliorarlo.

## Vincoli funzionali non negoziabili

Usa questi requisiti come oracolo per trovare le violazioni:

1. L'app deve poter aprire qualsiasi `.cat` / `pred.cat` e usarlo per
   visualizzare e assegnare transizioni senza aver aperto Pred&Fit, senza
   generare un `.par`, senza imporre un Hamiltoniano e senza dipendere da
   SPFIT/SPCAT.
2. Il flusso **catalogo caricato → selezione righe → picco sperimentale →
   assignment → `assignments.txt`** è autonomo. Non deve essere modificato da
   Hamiltoniano, numero di stati, `NQN`, file `.par/.var/.lin/.int`, `.fit` o
   sessione Pred&Fit.
3. `NQN` è proprietà della singola riga CAT, letta da `QNFMT`: è il numero di
   QN stampati per ciascuno stato in quella riga. Non è un parametro
   dell'Hamiltoniano, non deriva dalla terza riga del `.par` e non deve essere
   forzato dal modello Pred&Fit.
4. Pred&Fit è un flusso separato e opt-in. Solo un'azione esplicita può
   generare/importare `.par`, `.var`, `.int`, `.lin`, `.fit`, `.out`, `.bak` e
   `model.cat` nella propria area. Può consumare assignment come input, ma non
   deve corromperli o reinterpretarli mentre l'app è in normale modalità di
   assegnazione.
5. Il fitting delle intensità è un terzo flusso. Può avere interfacce chiare
   con catalogo e assignment, ma non deve dipendere accidentalmente dai dettagli
   interni Pred&Fit né mutare dati senza un'azione esplicita e tracciabile.
6. `assignments.txt` deve avere l'ordine di un record `.lin`:

   ```text
   QN superiori  QN inferiori  frequenza osservata  frequenza calcolata  intensità calcolata  NQN
   ```

   La frequenza osservata occupa il campo della frequenza del `.lin`; frequenza
   calcolata e intensità calcolata sostituiscono incertezza e peso. `NQN` può
   restare come ultimo campo tecnico per ricostruire la forma esatta della riga
   al riavvio. Il formato non può assumere sempre tre QN per stato.

## Problemi segnalati

### P0 — assignment da catalogo bloccati/corrotti

1. La creazione di `assignments.txt` si è incasinata con la nuova route di
   fitting. In particolare, quando nel `.par` si tenta di cambiare `N`, il
   valore viene riportato automaticamente a `3`.
2. Caricando un `pred.cat` con un numero di QN diverso da quello previsto
   dall'Hamiltoniano fissato, la creazione degli assignment si incasina. Il
   sospetto iniziale è un uso improprio di `NQN`, ma è un'ipotesi da verificare.
3. Gli assignment devono funzionare anche per cataloghi esterni la cui forma di
   QN non corrisponde a nessun modello Pred&Fit attivo.

### P1 — confini confusi della route Pred&Fit

La route di fitting mescola catalogo caricato, assignment, file/modello
Pred&Fit e stato di sessione. Identifica ogni punto in cui Pred&Fit legge o
modifica dati del flusso assignment, inclusi effetti dopo fit, undo, restore,
caricamento di `model.cat` o riapertura dell'app.

### P2 — fitting delle intensità scollegato

Il fitting delle intensità appare scollegato. Va definito esattamente quali
input consuma e quale output modifica: spettro sperimentale, catalogo,
assignment, temperatura, dipoli, concentrazioni, normalizzazione e UI.

## Metodo obbligatorio di analisi

### 1. Inventario del repository

1. Elenca sorgenti, header, build script, fixture e file di stato coinvolti
   direttamente o indirettamente nei tre flussi.
2. Per ciascun file: responsabilità reale, funzioni principali e strutture
   dati lette/scritte.
3. Cerca sia nomi diretti sia accessi indiretti ai campi di `AppState`,
   `PredFitState`, `Assignment`, `PredLine`, impostazioni e cache.
4. Evidenzia codice duplicato che confronta transizioni, interpreta QN,
   riscalcola intensità, serializza dati o aggiorna lo stesso stato da più
   punti.

### 2. Mappa dello stato in memoria

Produci una tabella con questa forma:

| Campo / struttura | Proprietario logico | Writer | Reader | Durata | Rischio |
| --- | --- | --- | --- | --- | --- |

Includi almeno:

- `pred_lines`, `n_pred`, `pred_path`, `pending_pred_path`, `pending_load`,
  catalogo attivo e scale/massimi di intensità;
- `Assignment`, lista/numero assignment, selezione, frequenza osservata,
  frequenza calcolata, intensità e flag fit;
- tutti i QN e `n_qn`/`NQN` in `PredLine`;
- parametri Pred&Fit, Hamiltoniano, specie, `.int`, flag catalogo generato,
  cache/report, history/undo e directory di lavoro;
- stato del fitting intensità;
- dipoli, `Tcat`, `Tred`, concentrazione, intensità lineare/logaritmica;
- variabili statiche/globali e cache.

Separa il proprietario logico dalla memoria fisica. Segnala ogni campo con più
writer non coordinati.

### 3. Mappa dei flussi end-to-end

Disegna i flussi seguenti, indicando funzioni, campi mutati, file, eventi UI e
condizioni di ramo:

1. Avvio senza Pred&Fit → apertura spettro → apertura CAT esterno → selezione
   transizione → click su picco → Save all.
2. CAT esterno con 3 QN → assignment → riavvio → restore assignment.
3. Stesso flusso con 4, 5 e 6 QN per stato, inclusi stato vibrazionale e/o
   iperfine se disponibili nei fixture.
4. CAT esterno aperto dopo un Pred&Fit nello stesso processo: flag, cache,
   temperature e trasformazioni residue.
5. Pred&Fit: creazione modello → modifica Hamiltoniano/numero stati →
   Calculate → `model.cat` → assignment → Fit → SPCAT.
6. Pred&Fit con più specie/stati, specie incluse/escluse, undo e restore `.fit`.
7. Fitting intensità con CAT esterno e `model.cat`, prima/dopo modifiche a
   Tcat/Tred/dipoli/concentrazione.
8. Salvataggio e riapertura completa: spectra, CAT, assignment,
   `.fit/spectravisual.state`, `model.lin` e output fit.

Per ogni flusso usa questa forma:

```text
azione utente
  → handler evento
  → funzione di dominio
  → campi di stato modificati
  → file letto/scritto
  → rendering/cache/altre conseguenze
```

### 4. Audit di formati e serializzazione

Confronta parser e writer di:

- CAT esterno e `model.cat`;
- `assignments.txt`, incluso formato nuovo e compatibilità precedente;
- `.lin` scritto per SPFIT e `.lin` riletto nel restore;
- `.par` e `.var`, inclusa intestazione e terza riga Hamiltoniana;
- `.int` e file per specie;
- `.fit`, `.out`, `.bak` e stato di sessione.

Per ogni formato documenta layout, unità, semantica, funzione writer/reader,
spazi fissi/padding/sentinelle/commenti, QN usati in lettura e scrittura,
perdite di informazione, compatibilità con file vecchi e divergenze dal formato
SPFIT/SPCAT effettivo.

Verifica in particolare se il valore `N` della terza riga `.par` viene confuso
con `NQN`, numero di stati, massimo di QN o rendering. Identifica funzione e
percorso di controllo che lo riporta a `3`, se accade davvero.

### 5. Audit dell'identità delle transizioni

Definisci e verifica cosa rende una transizione “la stessa” in:

- selezione grafica;
- deduplicazione e riassegnazione;
- matching assignment ↔ CAT corrente;
- matching assignment ↔ `.lin` al restore;
- righe fit e righe visualizzate;
- fitting intensità.

Controlla se l'identità usa erroneamente frequenza, solo primi 3 QN, 12 QN con
zeri fittizi, `NQN`, stato vibrazionale o ordine del file. Elenca collisioni e
duplicati possibili.

### 6. Riproduzione dei bug e test

Per ogni P0/P1/P2 prepara una riproduzione con precondizioni, file input,
azioni precise, stato atteso/osservato, file da confrontare, funzioni
attraversate e ipotesi ordinate per probabilità, ognuna con prova concreta.

Proponi una matrice di regressione che includa almeno:

- CAT esterno spin-free con 3 QN;
- CAT con 4 QN, e CAT con 5/6 QN se parser supportati;
- transizioni uguali nei primi 3 QN ma diverse nei QN aggiuntivi;
- save/reopen `assignments.txt` senza perdita di QN/frequenze/intensità;
- riassegnazione e deduplicazione;
- CAT esterno prima/dopo Pred&Fit;
- modifica terza riga `.par`, inclusi valori diversi di `N`;
- Calculate, Fit, undo, restore e più specie;
- fitting intensità con e senza Pred&Fit attivo.

Ogni test deve avere un oracolo: cosa deve restare identico e cosa deve
cambiare.

## Deliverable obbligatorio

Consegna un documento tecnico, non una risposta breve, nell'ordine:

1. **Executive summary** — massimo una pagina; cause dimostrate, non sospetti.
2. **Mappa repository** — moduli, file e responsabilità.
3. **Mappa stato** — tabella writer/reader/proprietà/durata.
4. **Mappa flussi** — diagrammi testuali degli scenari richiesti.
5. **Atlante tecnico completo** — diagrammi disegnati e descrizioni testuali
   di UI, layout, eventi, call graph, stato, persistenza, rendering, errori e
   tutti gli altri sottosistemi, anche se non collegati ai bug segnalati.
6. **Specifica formati** — CAT, assignments, LIN, PAR/VAR, INT e sessione.
7. **Identità delle transizioni e NQN**.
8. **Bug report dettagliati** — causa, file, funzione, linee, condizioni e
   prova riproducibile.
9. **Dipendenze indirette / blast radius** — regressioni possibili per bug.
10. **Architettura target** — confini e interfacce tra assignment da CAT,
   Pred&Fit e fitting intensità.
11. **Piano ordinato** — patch minime/reversibili prima, refactor dopo, con
    test e criteri di accettazione.
12. **Domande bloccanti** — soltanto scelte scientifiche/UX non deducibili dal
    codice e materialmente rilevanti.

Ogni affermazione deve citare file, funzione e linea/intervallo di linee.
Distingui sempre tra fatto verificato nel codice, comportamento riprodotto,
inferenza tecnica e proposta progettuale.

## Regole di esecuzione

- Non modificare codice, file di produzione, cataloghi o sessioni durante
  l'audit, salvo fixture temporanei esplicitamente isolati.
- Non correggere per deduzione: prima mappa e riproduci.
- Non assumere che il modello Pred&Fit attivo descriva un CAT esterno.
- Non assumere che tutti i cataloghi usino tre QN per stato.
- Non usare “N” senza specificare: campo della terza riga `.par`, numero di
  stati, NQN/QNFMT o altro parametro.
- Prima di proporre un refactor, elenca ogni chiamante e persistenza che dipende
  dall'API o stato da cambiare.
