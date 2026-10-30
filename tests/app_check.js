// Load the PC dashboard (running against the simulator) in Chromium: no JS errors, values fill in,
// AUTO can be enabled through the confirmation dialog, no sideways scroll; saves screenshots.
//   node tests/app_check.js http://127.0.0.1:8766/ build
const { chromium } = require('playwright');
const url = process.argv[2] || 'http://127.0.0.1:8765/';
const out = process.argv[3] || 'build';
(async () => {
  const browser = await chromium.launch();
  const errors = [];
  let ok = true;
  const page = await browser.newPage({ viewport: { width: 1200, height: 1000 }, deviceScaleFactor: 2 });
  page.on('pageerror', e => errors.push(e.message));
  page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
  page.on('dialog', d => d.accept());
  await page.goto(url);
  await page.waitForTimeout(2500);
  // number panels start collapsed; the three per-sensor graphs are visible
  const folds = await page.$$eval('details.fold', d => d.map(x => x.open));
  if (folds.length !== 3 || folds.some(o => o)) { ok = false; errors.push(`folds not all collapsed: ${folds}`); }
  for (const i of [0, 1, 2]) if (!(await page.isVisible('#g' + i))) { ok = false; errors.push(`graph S${i + 1} not visible`); }
  await page.click('#bA');
  await page.waitForTimeout(1500);
  const msg = await page.textContent('#cmdmsg');
  await page.waitForTimeout(25000);
  // calibration panel: candidate from the last 30 s, preview, send; then a $RAW bench test at 45 %
  await page.click('#fold-cal > summary');
  await page.click('#fold-sens > summary');
  await page.click('text=Use last 30 s as DRY');
  const d0 = await page.inputValue('#d0');
  await page.click('text=Send to board ($CAL)');
  await page.waitForTimeout(1500);
  const calMsg = await page.textContent('#cmdmsg');
  await page.click('text=Copy board values');
  await page.fill('#tp', '45'); await page.dispatchEvent('#tp', 'input');
  const r0 = await page.inputValue('#r0');
  await page.click('text=Start test input');
  await page.waitForTimeout(3000);
  const testSt = await page.textContent('#testst');
  console.log(`calibration: DRY candidate S1=${d0}, send -> ${calMsg}; test input S1 raw ${r0} -> ${testSt}`);
  if (!d0) { ok = false; errors.push('no calibration candidate'); }
  if (!/TEST INPUT ACTIVE/.test(testSt)) { ok = false; errors.push('test input did not start'); }
  await page.click('text=End test input');
  await page.waitForTimeout(2000);
  const v = {};
  for (const id of ['conn', 'avg', 'pump', 'mode', 'demand', 'led']) v[id] = (await page.textContent('#' + id)).trim();
  const rows = await page.$$eval('#sens tr', r => r.length);
  const badges = [];
  for (const i of [0, 1, 2]) badges.push((await page.textContent('#st' + i)).trim());
  console.log('per-sensor status:', badges.join(' '), '| soil status:', v.demand);
  if (badges.some(b => !/^(DRY|WET)$/.test(b)) || !/^(DRY|WET)$/.test(v.demand)) { ok = false; errors.push('DRY/WET status missing'); }
  const ev = await page.textContent('#log');
  console.log('after A:', msg, '|', JSON.stringify(v), '| sensor rows', rows);
  if (v.conn !== 'LIVE' || v.avg === '--' || rows !== 3) { ok = false; errors.push('values never filled in'); }
  if (!/auto_enabled/.test(ev) || !/test_input_on/.test(ev)) { ok = false; errors.push('AUTO was not enabled'); }
  if (!/pump_command_on/.test(ev)) { ok = false; errors.push('simulated pump never ran'); }
  await page.screenshot({ path: `${out}/app_desktop.png`, fullPage: true });
  await page.setViewportSize({ width: 390, height: 844 });
  await page.waitForTimeout(1500);
  const overflow = await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
  if (overflow > 0) { ok = false; errors.push(`phone width scrolls sideways by ${overflow}px`); }
  await page.screenshot({ path: `${out}/app_phone.png`, fullPage: true });
  await browser.close();
  if (errors.length) { console.log('ERRORS:\n' + errors.join('\n')); ok = false; }
  console.log(ok ? 'app check passed' : 'app check FAILED');
  process.exit(ok ? 0 : 1);
})();
