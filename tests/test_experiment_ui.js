// Requires an isolated running backend and a playback session named session_1.
const puppeteer = require('puppeteer');
(async () => {
 const base = process.argv[2];
 const browser = await puppeteer.launch({headless:true, executablePath:process.env.CHROME_PATH || `${process.env.HOME}/.cache/puppeteer/chrome/linux-157.0.8103.50/chrome-linux64/chrome`, args:['--no-sandbox']});
 try {
  const page = await browser.newPage();
  await page.goto(base + '/experiments?session=session_1', {waitUntil:'domcontentloaded'});
  await page.waitForSelector('#recordList .record-item');
  // Open a record already containing this session, then create a different one.
  const record = await page.evaluate(async () => {
   const list = await (await fetch('/api/experiments')).json();
   for (const item of list.records) {
    const detail = await (await fetch('/api/experiments/' + item.id)).json();
    if (detail.record.sessions.includes('session_1')) return detail.record;
   }
   throw Error('test needs a record linked to session_1');
  });
  await page.evaluate(id => {document.querySelectorAll('#recordList .record-item').forEach(b => {if(b.textContent.includes(id)) b.click();});}, record.id);
  await page.waitForFunction(id => document.querySelector('#recordMeta').textContent.includes(id), {}, record.id);
  await page.click('#newRecord');
  if (await page.$eval('#sessions', el => el.textContent.includes('session_1'))) throw Error('session leaked to a new record');
  await page.type('[name=title]', 'UI 导出保护');
  await page.click('#saveDraft');
  await page.waitForFunction(() => document.querySelector('#message').textContent.includes('已保存'));
  await page.type('[name=title]', '修改');
  await page.click('#exportBtn');
  if (!(await page.$eval('#message', e => e.textContent)).includes('先保存')) throw Error('unsaved export not blocked');
  await page.evaluate(() => { uploading = true; });
  await page.click('#newRecord');
  if (!(await page.$eval('#message', e => e.textContent)).includes('正在上传')) throw Error('navigation during upload not blocked');
  await page.evaluate(() => { uploading = false; });
  page.on('dialog', dialog => dialog.accept());
  await page.setRequestInterception(true);
  page.on('request', req => {if (/\/api\/experiments\/EXP-/.test(req.url()) && req.method()==='GET') setTimeout(() => req.continue().catch(()=>{}), 600); else req.continue();});
  await page.click('#recordList .record-item');
  await page.evaluate(() => document.querySelector('#uploadAttachment').click());
  if (!(await page.$eval('#message', e => e.textContent)).includes('操作正在进行')) throw Error('upload allowed while record is loading');
  await page.waitForFunction(() => !busy);
  console.log('PASS 已关联录像不串记录、未保存不导出、上传中禁止切换');
 } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
