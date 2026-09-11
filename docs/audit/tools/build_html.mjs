// Builds docs/audit/audit.html: one self-contained, navigable page with every
// Mermaid diagram pre-rendered to inline SVG (headless local Chrome), so the
// file works offline and needs no JavaScript library at view time.
import fs from 'fs';
import path from 'path';
import { marked } from 'marked';
import puppeteer from 'puppeteer-core';

const DOCS = process.argv[2];                     // docs/audit
const OUT = path.join(DOCS, 'audit.html');
const CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const MERMAID = path.join(path.dirname(new URL(import.meta.url).pathname), 'node_modules/mermaid/dist/mermaid.min.js');

const DOCS_LIST = [
  { key: 'piano', file: 'PIANO-FIX.md', short: 'Piano di correzione' },
  { key: 'readme', file: 'README.md', short: 'Report' },
  { key: 'a1', file: 'A1-funzioni.md', short: 'A1 Funzioni' },
  { key: 'a2', file: 'A2-campi.md', short: 'A2 Campi' },
  { key: 'a3', file: 'A3-ui-layout-eventi.md', short: 'A3 UI ed eventi' },
  { key: 'a4', file: 'A4-riproduzioni.md', short: 'A4 Riproduzioni' },
];
const keyOfFile = Object.fromEntries(DOCS_LIST.map(d => [d.file, d.key]));

function slug(text) {
  return text.replace(/<[^>]+>/g, '').trim().toLowerCase().replace(/[^\p{L}\p{N}_\- ]/gu, '').replace(/ /g, '-');
}
function escapeHtml(s) { return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;'); }

// ---------- 1. collect the Mermaid blocks and render them in Chrome ----------
const diagrams = [];
for (const d of DOCS_LIST) {
  const md = fs.readFileSync(path.join(DOCS, d.file), 'utf8');
  for (const m of md.matchAll(/```mermaid\n([\s\S]*?)```/g)) diagrams.push(m[1]);
}
const browser = await puppeteer.launch({ executablePath: CHROME, headless: 'new', args: ['--no-sandbox'] });
const page = await browser.newPage();
await page.setContent('<!doctype html><html><body></body></html>');
await page.addScriptTag({ content: fs.readFileSync(MERMAID, 'utf8') });
const svgs = await page.evaluate(async (list) => {
  window.mermaid.initialize({ startOnLoad: false, theme: 'default', securityLevel: 'loose',
    flowchart: { htmlLabels: true, useMaxWidth: true }, fontFamily: 'Helvetica, Arial, sans-serif' });
  const out = [];
  for (let i = 0; i < list.length; i++) {
    try { const { svg } = await window.mermaid.render('mmd' + i, list[i]); out.push(svg); }
    catch (e) { out.push('<pre class="mmd-error">' + String(e.message || e) + '</pre>'); }
  }
  return out;
}, diagrams);
await browser.close();

// ---------- 2. Markdown → HTML, one section per document ----------
let diagramIndex = 0;
const toc = [];
function convert(d) {
  let md = fs.readFileSync(path.join(DOCS, d.file), 'utf8');
  const headingCount = {};
  const renderer = new marked.Renderer();
  renderer.heading = (text, level, raw) => {
    let s = slug(raw); const n = headingCount[s] || 0; headingCount[s] = n + 1; if (n) s += '-' + n;
    const id = `${d.key}-${s}`;
    if (level <= 3) toc.push({ doc: d.key, level, id, text: text.replace(/<[^>]+>/g, '') });
    return `<h${level} id="${id}"><a class="hlink" href="#${id}">#</a>${text}</h${level}>\n`;
  };
  renderer.code = (code, lang) => {
    if (lang === 'mermaid') {
      const svg = svgs[diagramIndex++];
      return `<figure class="diagram"><div class="diagram-inner">${svg}</div>` +
             `<figcaption><button class="zoom" type="button">ingrandisci</button></figcaption></figure>\n`;
    }
    return `<pre class="code"><code>${escapeHtml(code)}</code></pre>\n`;
  };
  renderer.table = (header, body) =>
    `<div class="table-wrap"><table><thead>${header}</thead><tbody>${body}</tbody></table></div>\n`;
  renderer.link = (href, title, text) => {
    let h = href || '';
    if (/^https?:/.test(h)) return `<a href="${h}" target="_blank" rel="noopener">${text}</a>`;
    const [p, frag] = h.split('#');
    if (!p && frag) h = `#${d.key}-${frag}`;
    else if (keyOfFile[p]) h = frag ? `#${keyOfFile[p]}-${frag}` : `#${keyOfFile[p]}-top`;
    else if (/^\.\.\/\.\.\//.test(p)) { const line = frag ? frag : ''; h = p + (line ? '#' + line : ''); return `<a class="src" href="${h}" title="apre il sorgente">${text}</a>`; }
    return `<a href="${h}">${text}</a>`;
  };
  md = md.replace(/<a id="([^"]+)"><\/a>/g, (_, id) => `<a id="${d.key}-${id}"></a>`);
  let html = marked.parse(md, { renderer, gfm: true, mangle: false, headerIds: false });
  html = html.replace(/\[(FATTO|RIPR|INF|PROP)([^\]]*)\]/g, (m, t, rest) => `<span class="tag tag-${t.toLowerCase()}">${t}${rest}</span>`);
  return `<section class="doc" id="${d.key}-top" data-doc="${d.key}">\n${html}\n</section>\n`;
}
const sections = DOCS_LIST.map(convert).join('\n');

// ---------- 3. page ----------
const tocHtml = DOCS_LIST.map(d => {
  const items = toc.filter(t => t.doc === d.key && t.level >= 2)
    .map(t => `<li class="l${t.level}"><a href="#${t.id}">${t.text}</a></li>`).join('');
  return `<details ${d.key === 'piano' || d.key === 'readme' ? 'open' : ''}><summary><a href="#${d.key}-top">${d.short}</a></summary><ul>${items}</ul></details>`;
}).join('\n');

const css = `
:root{--bg:#fbfbfa;--fg:#1d1f23;--muted:#5d6470;--line:#e2e4e8;--side:#f2f3f5;--accent:#1f5fbf;--code:#f4f5f7;
--fatto:#1f7a4a;--ripr:#1f5fbf;--inf:#9a6a00;--prop:#7a3ea8}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.55 -apple-system,BlinkMacSystemFont,"Helvetica Neue",Arial,sans-serif}
#side{position:fixed;top:0;left:0;bottom:0;width:310px;overflow:auto;background:var(--side);border-right:1px solid var(--line);padding:14px 12px 40px}
#side h1{font-size:15px;margin:0 0 10px}
#side input{width:100%;padding:6px 8px;border:1px solid var(--line);border-radius:6px;margin-bottom:10px;font:inherit;font-size:13px}
#side details{margin:4px 0}
#side summary{cursor:pointer;font-weight:600;font-size:13.5px;padding:3px 0}
#side summary a{color:var(--fg);text-decoration:none}
#side ul{list-style:none;margin:2px 0 8px;padding:0}
#side li{margin:1px 0}
#side li a{display:block;color:var(--muted);text-decoration:none;font-size:12.5px;padding:2px 6px;border-radius:4px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#side li.l3 a{padding-left:18px;font-size:12px}
#side li a:hover,#side li a.active{background:#e3e7ee;color:var(--fg)}
main{margin-left:310px;padding:24px 40px 120px;max-width:1500px}
section.doc{border-bottom:3px solid var(--line);padding-bottom:40px;margin-bottom:40px}
h1,h2,h3,h4{line-height:1.25;scroll-margin-top:12px}
h1{font-size:26px}h2{font-size:21px;margin-top:36px;border-bottom:1px solid var(--line);padding-bottom:4px}
h3{font-size:17px;margin-top:26px}h4{font-size:15px}
.hlink{color:var(--line);text-decoration:none;margin-right:6px;font-weight:400}
h1:hover .hlink,h2:hover .hlink,h3:hover .hlink{color:var(--muted)}
a{color:var(--accent)}a.src{font-family:ui-monospace,Menlo,monospace;font-size:.92em}
code{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:.88em;background:var(--code);padding:1px 4px;border-radius:4px}
pre.code{background:var(--code);border:1px solid var(--line);border-radius:6px;padding:10px 12px;overflow:auto;font-size:12.5px;line-height:1.45}
pre.code code{background:none;padding:0}
.table-wrap{overflow:auto;max-width:100%;margin:10px 0;border:1px solid var(--line);border-radius:6px;background:#fff}
.table-wrap.tall{max-height:70vh}
table{border-collapse:collapse;font-size:12.8px;min-width:100%}
th,td{border-bottom:1px solid var(--line);border-right:1px solid var(--line);padding:5px 8px;vertical-align:top;text-align:left}
th{background:#f0f2f5;position:sticky;top:0;z-index:1}
tr:hover td{background:#f7f9fc}
.filter{margin:14px 0 4px;display:flex;gap:8px;align-items:center;font-size:13px;color:var(--muted)}
.filter input{flex:1;max-width:420px;padding:5px 8px;border:1px solid var(--line);border-radius:6px;font:inherit}
figure.diagram{margin:14px 0;border:1px solid var(--line);border-radius:8px;background:#fff;padding:10px}
.diagram-inner{overflow:auto}
.diagram-inner svg{max-width:100%;height:auto}
figure.diagram.big{position:fixed;inset:10px;z-index:50;margin:0;overflow:auto;box-shadow:0 10px 40px rgba(0,0,0,.25)}
figure.diagram.big .diagram-inner svg{max-width:none;min-width:1400px}
figcaption{text-align:right}
button.zoom{font:inherit;font-size:12px;border:1px solid var(--line);background:var(--side);border-radius:5px;padding:2px 8px;cursor:pointer}
.tag{display:inline-block;font-size:11px;font-weight:700;letter-spacing:.02em;border-radius:4px;padding:0 5px;color:#fff;vertical-align:1px}
.tag-fatto{background:var(--fatto)}.tag-ripr{background:var(--ripr)}.tag-inf{background:var(--inf)}.tag-prop{background:var(--prop)}
.mmd-error{color:#b00020}
#top{position:fixed;right:18px;bottom:18px;background:var(--fg);color:#fff;border-radius:18px;padding:6px 12px;text-decoration:none;font-size:13px;opacity:.75}
@media (max-width:900px){#side{position:static;width:auto;height:auto;border-right:0;border-bottom:1px solid var(--line)}main{margin-left:0;padding:16px}}
`;

const js = `
(function(){
  var q=document.getElementById('tocq');
  q.addEventListener('input',function(){var v=q.value.toLowerCase();
    document.querySelectorAll('#side li').forEach(function(li){li.style.display=li.textContent.toLowerCase().indexOf(v)>=0?'':'none';});
    if(v) document.querySelectorAll('#side details').forEach(function(d){d.open=true;});});
  document.querySelectorAll('.table-wrap').forEach(function(w){
    var rows=w.querySelectorAll('tbody tr'); if(rows.length<25) return;
    w.classList.add('tall');
    var f=document.createElement('div'); f.className='filter';
    f.innerHTML='<span>Filtra '+rows.length+' righe</span><input type="search" placeholder="testo da cercare nelle righe">';
    w.parentNode.insertBefore(f,w);
    f.querySelector('input').addEventListener('input',function(e){var v=e.target.value.toLowerCase();
      rows.forEach(function(r){r.style.display=r.textContent.toLowerCase().indexOf(v)>=0?'':'none';});});
  });
  document.querySelectorAll('td:first-child').forEach(function(td){
    if(/^([A-Z]{1,2}-\\d{1,3}|D\\d{1,2}|\\d\\.\\d{1,2})$/.test(td.textContent.trim())) td.style.whiteSpace='nowrap';});
  document.querySelectorAll('button.zoom').forEach(function(b){b.addEventListener('click',function(){
    var fig=b.closest('figure'); fig.classList.toggle('big'); b.textContent=fig.classList.contains('big')?'chiudi':'ingrandisci';});});
  document.addEventListener('keydown',function(e){if(e.key==='Escape')document.querySelectorAll('figure.big').forEach(function(f){f.classList.remove('big');f.querySelector('button').textContent='ingrandisci';});});
  var links=[].slice.call(document.querySelectorAll('#side li a'));
  var byId={}; links.forEach(function(a){byId[a.getAttribute('href').slice(1)]=a;});
  var obs=new IntersectionObserver(function(es){es.forEach(function(en){if(en.isIntersecting){var a=byId[en.target.id]; if(a){links.forEach(function(x){x.classList.remove('active');}); a.classList.add('active');}}});},{rootMargin:'0px 0px -80% 0px'});
  document.querySelectorAll('h2[id],h3[id]').forEach(function(h){obs.observe(h);});
})();
`;

const html = `<!doctype html>
<html lang="it"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Audit forense SpectraVisual</title><style>${css}</style></head>
<body>
<nav id="side"><h1>Audit SpectraVisual</h1>
<input id="tocq" type="search" placeholder="Cerca nell'indice (es. B-12, NQN, restore)">
${tocHtml}
</nav>
<main>
${sections}
</main>
<a id="top" href="#${DOCS_LIST[0].key}-top">↑ inizio</a>
<script>${js}</script>
</body></html>`;
fs.writeFileSync(OUT, html);
console.log(`wrote ${OUT}: ${(html.length / 1024).toFixed(0)} KB, ${diagrams.length} diagrams, ${svgs.filter(s => s.includes('mmd-error')).length} render errors`);
