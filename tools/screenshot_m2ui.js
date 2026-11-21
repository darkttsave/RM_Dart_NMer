// M2-UI 界面截图与交互验证脚本（仅本地验证用，不进入交付代码）
// 用法: NODE_PATH=$(npm root -g) node tools/screenshot_m2ui.js <mode> <url> <out.png> [width] [height]
// mode: live-status | live-charts | live-recording | replay-charts | replay-frame | replay-session
const puppeteer = require("puppeteer");

const [, , mode, url, output, widthArg, heightArg] = process.argv;
if (!mode || !url || !output) {
    console.error("usage: node screenshot_m2ui.js <mode> <url> <out.png> [width] [height]");
    process.exit(2);
}

const width = Number(widthArg || 1920);
const height = Number(heightArg || 1080);
const chromePath = process.env.HOME +
    "/.cache/puppeteer/chrome/linux-157.0.8103.50/chrome-linux64/chrome";
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

(async () => {
    const browser = await puppeteer.launch({
        executablePath: chromePath,
        headless: true,
        args: [
            "--no-sandbox",
            "--disable-gpu",
            "--disable-dev-shm-usage",
            "--user-data-dir=/tmp/dart-m2ui-puppeteer-profile",
            "--window-size=" + width + "," + height,
        ],
    });
    try {
        const errors = [];
        const page = await browser.newPage();
        page.on("console", (msg) => {
            if (msg.type() === "error") errors.push("console: " + msg.text());
        });
        page.on("pageerror", (err) => errors.push("pageerror: " + err.message));
        page.on("requestfailed", (req) => {
            const target = req.url();
            // MJPEG 视频流与 SSE 长连接在截图结束时被主动关闭，属正常
            if (target.includes("/video_feed") || target.includes("/api/telemetry/stream")) return;
            errors.push("requestfailed: " + target);
        });

        await page.setViewport({ width, height, deviceScaleFactor: 1 });
        await page.goto(url, { waitUntil: "domcontentloaded", timeout: 30000 });

        const isLive = mode.startsWith("live");
        if (isLive) {
            await sleep(9000);
            if (mode === "live-charts") {
                await page.click("#tabBtnCharts");
                await sleep(2500);
            }
            if (mode === "live-recording") {
                const label = await page.$eval("#recordToggleBtn", (el) => el.textContent.trim());
                if (label.includes("开始")) {
                    await page.click("#recordToggleBtn");
                    await sleep(3500);
                }
            }
        } else {
            await sleep(2500);
            await page.click("#playBtn");
            await sleep(4000);
            await page.click("#playBtn");
            await sleep(400);
            if (mode === "replay-frame") {
                await page.click("#tabBtnFrame");
                await sleep(600);
            } else if (mode === "replay-session") {
                await page.click("#tabBtnSession");
                await sleep(600);
            }
        }

        await page.screenshot({ path: output, fullPage: false });

        const metrics = await page.evaluate((mode) => {
            const text = (id) => {
                const el = document.getElementById(id);
                return el ? el.textContent.trim() : null;
            };
            const rect = (el) => (el ? {
                x: Math.round(el.getBoundingClientRect().x),
                y: Math.round(el.getBoundingClientRect().y),
                w: Math.round(el.getBoundingClientRect().width),
                h: Math.round(el.getBoundingClientRect().height),
            } : null);
            const elRect = (sel) => rect(document.querySelector(sel));
            const canvases = Array.from(document.querySelectorAll(".chart-card canvas")).map((c) => ({
                width: c.width, height: c.height,
                cssW: Math.round(c.getBoundingClientRect().width),
                cssH: Math.round(c.getBoundingClientRect().height),
            }));
            const pane = document.querySelector(".tab-pane:not([hidden])");
            const grid = document.getElementById("chartsGrid");
            const docEl = document.documentElement;
            return {
                viewport: { w: window.innerWidth, h: window.innerHeight },
                doc: {
                    scrollW: docEl.scrollWidth, clientW: docEl.clientWidth,
                    scrollH: docEl.scrollHeight, clientH: docEl.clientHeight,
                    overflowX: docEl.scrollWidth - docEl.clientWidth,
                    overflowY: docEl.scrollHeight - docEl.clientHeight,
                    bodyScrollW: document.body.scrollWidth,
                    bodyScrollH: document.body.scrollHeight,
                },
                layout: {
                    topbar: elRect(".topbar"),
                    videoCard: elRect(".video-card"),
                    videoWrap: elRect(".video-wrap"),
                    side: elRect(".side"),
                    pantabs: elRect(".tab-panels"),
                    bottombar: elRect(".bottombar"),
                    transport: elRect(".transport-bar"),
                },
                paneScroll: pane ? {
                    id: pane.id,
                    scrollH: pane.scrollHeight,
                    clientH: pane.clientHeight,
                    overflow: pane.scrollHeight - pane.clientHeight,
                } : null,
                gridScroll: (grid && !grid.hidden) ? {
                    scrollH: grid.scrollHeight,
                    clientH: grid.clientHeight,
                    overflow: grid.scrollHeight - grid.clientHeight,
                } : null,
                canvases,
                texts: {
                    link: text("linkText"),
                    recordChip: text("recordChip"),
                    fpsChip: text("fpsChip"),
                    txChip: text("txChip"),
                    boSession: text("boSession"),
                    boRecord: text("boRecord"),
                    detectState: text("detectState"),
                    targetX: text("vTargetX"),
                    measureNote: text("measureNote"),
                    frameText: text("frameText"),
                    timeText: text("timeText"),
                    detectFrame: text("detectState"),
                    iSession: text("iSession"),
                    iAccuracy: text("iAccuracy"),
                    configTag: text("configTag"),
                    statusChip: text("statusChip"),
                },
                recording: (() => {
                    const el = document.getElementById("recordToggleBtn");
                    return el ? el.textContent.trim() : null;
                })(),
                videoLoaded: (() => {
                    const img = document.getElementById("videoImage");
                    return img ? { complete: img.complete, naturalW: img.naturalWidth, naturalH: img.naturalHeight } : null;
                })(),
                activeTab: pane ? pane.id : null,
                chartCardCount: document.querySelectorAll(".chart-card").length,
            };
        }, mode);

        console.log(JSON.stringify({ mode, output, errors, metrics }, null, 2));
    } finally {
        await browser.close();
    }
})().catch((err) => {
    console.error("screenshot failed:", err);
    process.exit(1);
});
