// Drives the built page in headless Chrome: node cdp.js <url> <outprefix> <steps-json>
// steps: [{wait:ms},{shot:"name"},{click:[x,y,button]},{eval:"js"},{key:"Escape"}]  (click coords are game coords 320x200)
const { spawn } = require('child_process'); const fs = require('fs');
const CH = process.env.HOME + '/tools/chrome/chrome-headless-shell-linux64/chrome-headless-shell';
const [url, prefix, stepsJson] = process.argv.slice(2); const steps = JSON.parse(stepsJson);
(async () => {
  const proc = spawn(CH, ['--no-sandbox', '--disable-gpu', '--remote-debugging-port=9333', '--window-size=900,640', 'about:blank'], { stdio: 'ignore' });
  await new Promise(r => setTimeout(r, 1500));
  const list = await (await fetch('http://127.0.0.1:9333/json')).json();
  const ws = new WebSocket(list.find(t => t.type === 'page').webSocketDebuggerUrl);
  await new Promise(r => ws.onopen = r);
  let id = 0; const pend = {};
  ws.onmessage = m => { const d = JSON.parse(m.data); if (d.id && pend[d.id]) { pend[d.id](d); delete pend[d.id]; } };
  const send = (method, params = {}) => new Promise(r => { const i = ++id; pend[i] = r; ws.send(JSON.stringify({ id: i, method, params })); });
  await send('Page.enable'); await send('Runtime.enable');
  await send('Page.navigate', { url });
  await new Promise(r => setTimeout(r, 1500));
  for (const s of steps) {
    if (s.wait) await new Promise(r => setTimeout(r, s.wait));
    if (s.shot) { const d = await send('Page.captureScreenshot', { format: 'png' }); fs.writeFileSync(`${prefix}_${s.shot}.png`, Buffer.from(d.result.data, 'base64')); }
    if (s.eval) { const d = await send('Runtime.evaluate', { expression: s.eval, returnByValue: true }); console.log(s.eval, '=>', JSON.stringify(d.result.result.value)); }
    if (s.click) {
      const r = (await send('Runtime.evaluate', { expression: 'JSON.stringify(document.getElementById("screen").getBoundingClientRect())', returnByValue: true })).result.result.value; const b = JSON.parse(r);
      const px = b.x + s.click[0] * b.width / 320, py = b.y + s.click[1] * b.height / 200, btn = s.click[2] === 2 ? 'right' : 'left';
      await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: px, y: py });
      await send('Input.dispatchMouseEvent', { type: 'mousePressed', x: px, y: py, button: btn, clickCount: 1 });
      await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x: px, y: py, button: btn, clickCount: 1 });
    }
    if (s.key) { await send('Input.dispatchKeyEvent', { type: 'keyDown', key: s.key }); await send('Input.dispatchKeyEvent', { type: 'keyUp', key: s.key }); }
  }
  proc.kill(); process.exit(0);
})();
