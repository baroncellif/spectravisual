import { JSDOM } from 'jsdom';
import fs from 'fs';
const dom = new JSDOM('<!doctype html><html><body></body></html>', { pretendToBeVisual: true });
globalThis.window = dom.window; globalThis.document = dom.window.document;
globalThis.DOMParser = dom.window.DOMParser; globalThis.Element = dom.window.Element;
globalThis.HTMLElement = dom.window.HTMLElement;
const { default: mermaid } = await import('mermaid');
mermaid.initialize({ startOnLoad: false });
let bad = 0, total = 0;
for (const f of process.argv.slice(2)) {
  const text = fs.readFileSync(f, 'utf8');
  const re = /```mermaid\n([\s\S]*?)```/g; let m;
  while ((m = re.exec(text))) {
    total++;
    const line = text.slice(0, m.index).split('\n').length;
    try { await mermaid.parse(m[1]); console.log(`ok   ${f.split('/').pop()}:${line} ${m[1].split('\n')[0]}`); }
    catch (e) { bad++; console.log(`FAIL ${f.split('/').pop()}:${line}: ${String(e.message || e).slice(0, 600)}`); }
  }
}
console.log(`${total} blocks, ${bad} failed`);
