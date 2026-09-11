# Strumenti della documentazione di audit

Servono a verificare e rigenerare la documentazione dopo ogni passo di
[PIANO-FIX.md](../PIANO-FIX.md). Richiedono Node.js (≥ 18), Python 3 e Google
Chrome installato in `/Applications/Google Chrome.app` (usato in modalità
headless per disegnare i diagrammi).

Una volta sola:

```sh
cd docs/audit/tools && npm install
```

Dopo ogni modifica dei documenti (dalla radice del repository):

```sh
python3 docs/audit/tools/check_links.py                 # link a file, righe e ancore dei bug
(cd docs/audit/tools && node validate.mjs ../README.md ../A1-funzioni.md ../A2-campi.md \
    ../A3-ui-layout-eventi.md ../A4-riproduzioni.md ../PIANO-FIX.md)   # sintassi Mermaid
(cd docs/audit/tools && node build_html.mjs ..)          # rigenera docs/audit/audit.html
```

`build_html.mjs` produce un unico file autonomo: diagrammi Mermaid già
convertiti in SVG, indice laterale con ricerca, filtri sulle tabelle grandi.
L'HTML resta locale: non va pubblicato.

Le sezioni generate delle appendici si rigenerano con
`python3 docs/audit/repro/ast_index.py . <cartella temporanea>`: il contenuto di
`functions.md` sostituisce tutto ciò che segue il titolo *A1.3* in
`A1-funzioni.md`, quello di `fields.md` tutto ciò che segue *A2.3* in
`A2-campi.md`. Le sezioni scritte a mano (A1.1, A1.2, A2.1, A2.2) si aggiornano
a mano.
