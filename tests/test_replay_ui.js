const puppeteer = require('puppeteer');
(async () => {
 const browser = await puppeteer.launch({headless:true,executablePath:process.env.CHROME_PATH || `${process.env.HOME}/.cache/puppeteer/chrome/linux-157.0.8103.50/chrome-linux64/chrome`,args:['--no-sandbox','--disable-setuid-sandbox']});
 const page = await browser.newPage();
 let running = 0, peak = 0, total = 0;
 try {
  await page.setRequestInterception(true);
  page.on('request', r => {
    if (r.url().includes('/api/frame?')) { running++;total++;peak=Math.max(peak,running);setTimeout(()=>r.continue().catch(()=>{}),120); }
    else r.continue().catch(()=>{});
  });
  for(const event of ['requestfinished','requestfailed']) page.on(event,r=>{if(r.url().includes('/api/frame?')) running--;});
  await page.goto(process.argv[2],{waitUntil:'domcontentloaded'});
  await page.waitForFunction(() => state.videoIndex.length > 100 && document.querySelector('#videoImage').complete && document.querySelector('#videoImage').naturalWidth > 0, {timeout:20000});
  await page.evaluate(async()=>{const e=document.querySelector('#timeline');for(let i=0;i<24;i++){e.value=String(Math.floor((i%2 ? .12 : .92)*Number(e.max)));e.dispatchEvent(new Event('input'));await new Promise(r=>setTimeout(r,8));}e.value=String(Math.floor(.78*Number(e.max)));e.dispatchEvent(new Event('input'));});
  await page.waitForFunction(()=>{const el=document.querySelector('#videoImage');return !state.frameLoading && Number(new URL(el.src).searchParams.get('index'))===state.curVideoIndex && el.complete && el.naturalWidth>0;},{timeout:15000});
  console.log('PASS 快速拖动落在最后目标帧');
  await page.click('#playBtn');
  const first=await page.$eval('#videoImage',x=>x.src);
  await new Promise(r=>setTimeout(r,1500));
  const second=await page.$eval('#videoImage',x=>x.src);
  if(first===second) throw new Error('playback image stalled');
  await page.click('#playBtn');
  await page.waitForFunction(()=>!state.frameLoading,{timeout:15000});
  if(peak > 2) throw new Error('too many concurrent frame requests: '+peak);
  console.log('PASS 延迟网络下播放持续更新，图片请求 peak='+peak+' total='+total);
 } finally { await browser.close(); }
})().catch(e=>{console.error(e);process.exitCode=1});
