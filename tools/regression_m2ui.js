// M2-UI 功能回归脚本（仅本地验证用，不进入交付代码）
// 覆盖：实时视频 / SSE 更新 / 实时曲线 / 录制启停 / 回放播放 / 时间轴拖动 / 帧号定位 / 页签切换 / JS 错误
// 用法: NODE_PATH=$(npm root -g) node tools/regression_m2ui.js [liveUrl] [replayUrl]
const puppeteer = require("puppeteer");

const LIVE_URL = process.argv[2] || "http://127.0.0.1:5000/";
const REPLAY_URL = process.argv[3] || "http://127.0.0.1:5001/";
const chromePath = process.env.HOME +
    "/.cache/puppeteer/chrome/linux-157.0.8103.50/chrome-linux64/chrome";
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function collectErrors(page, errors) {
    page.on("console", (msg) => {
        if (msg.type() === "error") errors.push("console: " + msg.text());
    });
    page.on("pageerror", (err) => errors.push("pageerror: " + err.message));
    page.on("requestfailed", (req) => {
        const target = req.url();
        if (target.includes("/video_feed") || target.includes("/api/telemetry/stream")) return;
        errors.push("requestfailed: " + target);
    });
}

(async () => {
    const browser = await puppeteer.launch({
        executablePath: chromePath,
        headless: true,
        args: [
            "--no-sandbox",
            "--disable-gpu",
            "--disable-dev-shm-usage",
            "--user-data-dir=/tmp/dart-m2ui-regression-profile",
            "--window-size=1920,1080",
        ],
    });
    const report = { checks: {}, live: {}, replay: {}, errors: [] };
    const ok = (name, pass, detail) => {
        report.checks[name] = { pass: !!pass, detail };
    };

    try {
        // ==================== A. 实时监测页 ====================
        const live = await browser.newPage();
        collectErrors(live, report.errors);
        await live.setViewport({ width: 1920, height: 1080, deviceScaleFactor: 1 });
        await live.goto(LIVE_URL, { waitUntil: "domcontentloaded", timeout: 30000 });
        await sleep(3000);

        const readLive = () => live.evaluate(() => ({
            seq: document.getElementById("footSeq").textContent,
            fps: document.getElementById("fpsChip").textContent,
            recordChip: document.getElementById("recordChip").textContent,
            recordBtn: document.getElementById("recordToggleBtn").textContent,
            boSession: document.getElementById("boSession").textContent,
            boRecord: document.getElementById("boRecord").textContent,
            videoComplete: (() => {
                const img = document.getElementById("videoImage");
                return !!(img && img.complete && img.naturalWidth > 0);
            })(),
            videoSize: (() => {
                const img = document.getElementById("videoImage");
                return img ? [img.naturalWidth, img.naturalHeight] : null;
            })(),
            detect: document.getElementById("detectState").textContent,
            measureNote: document.getElementById("measureNote").textContent,
            targetX: document.getElementById("vTargetX").textContent,
        }));
        const liveA = await readLive();
        await sleep(2500);
        const liveB = await readLive();

        ok("实时视频", liveB.videoComplete && liveB.videoSize[0] > 0,
            `画面 ${liveB.videoSize ? liveB.videoSize.join("x") : "n/a"}`);
        ok("SSE 数据更新", liveA.seq !== liveB.seq, `${liveA.seq} -> ${liveB.seq}`);
        ok("识别状态渲染", /检测|未检测/.test(liveB.detect), liveB.detect);

        // 实时曲线页签：切换后 4 个 canvas 必须重新绘制（非 0 尺寸）
        await live.click("#tabBtnCharts");
        await sleep(1500);
        const charts = await live.evaluate(() => Array.from(document.querySelectorAll("#chartList .chart-card canvas")).map((c) => ({
            w: Math.round(c.getBoundingClientRect().width),
            h: Math.round(c.getBoundingClientRect().height),
            bitmapW: c.width, bitmapH: c.height,
        })));
        const chartsDrawn = charts.length === 4 && charts.every((c) => c.w > 100 && c.h > 40 && c.bitmapW > 0);
        ok("实时曲线页签", chartsDrawn, JSON.stringify(charts));
        const pauseWorked = await live.evaluate(async () => {
            const btn = document.getElementById("pauseBtn");
            btn.click();
            const paused = btn.textContent.includes("继续");
            btn.click();
            return paused && btn.textContent.includes("暂停");
        });
        ok("曲线暂停/继续", pauseWorked, "");

        // 录制停止 -> 检查录制文件会话结束；再开始 -> 检查新会话
        const wasRecording = liveB.recordBtn.includes("停止");
        if (wasRecording) {
            await live.click("#recordToggleBtn");
            await sleep(3500);
        }
        const stopped = await readLive();
        ok("录制停止", !stopped.recordBtn.includes("停止") || stopped.recordChip.includes("未录制"),
            `${stopped.recordChip} / ${stopped.boRecord}`);
        await live.click("#recordToggleBtn");
        await sleep(3500);
        const started = await readLive();
        ok("录制开始", started.recordChip.includes("录制中") && started.recordBtn.includes("停止"),
            `${started.recordChip} / ${started.boSession} / ${started.boRecord}`);
        ok("录制会话切换", stopped.boSession !== started.boSession || !wasRecording,
            `${stopped.boSession} -> ${started.boSession}`);

        // 录制错误/丢弃状态必须常显（无错误时隐藏即可）
        const recordFlags = await live.evaluate(() => ({
            errorHidden: document.getElementById("boErrorWrap").hidden,
            droppedHidden: document.getElementById("boDroppedWrap").hidden,
            detailBtn: document.getElementById("recDetailBtn").textContent,
        }));
        ok("录制错误常显机制", recordFlags.errorHidden && typeof recordFlags.detailBtn === "string", JSON.stringify(recordFlags));

        // 顶栏 / 底栏录制详情浮层
        await live.click("#recDetailBtn");
        await sleep(300);
        const popover = await live.evaluate(() => ({
            hidden: document.getElementById("recPopover").hidden,
            dir: document.getElementById("popDir").textContent,
            state: document.getElementById("popState").textContent,
        }));
        ok("录制详情折叠区", !popover.hidden && popover.state.includes("录制中"), JSON.stringify(popover));
        await live.click("#recPopoverClose");

        report.live = { liveA, liveB, stopped, started, popover };

        // ==================== B. 历史回放页 ====================
        const replay = await browser.newPage();
        collectErrors(replay, report.errors);
        await replay.setViewport({ width: 1920, height: 1080, deviceScaleFactor: 1 });
        await replay.goto(REPLAY_URL, { waitUntil: "domcontentloaded", timeout: 30000 });
        await sleep(3000);

        const readReplay = () => replay.evaluate(() => ({
            time: document.getElementById("timeText").textContent,
            frame: document.getElementById("frameText").textContent,
            frameBadge: document.getElementById("frameBadge").textContent,
            frameId: document.getElementById("vFrameId").textContent,
            targetX: document.getElementById("vTargetX").textContent,
            locate: document.getElementById("locateResult").textContent,
            videoComplete: (() => {
                const img = document.getElementById("videoImage");
                return !!(img && img.complete && img.naturalWidth > 0);
            })(),
            duration: document.getElementById("durationText") ? document.getElementById("durationText").textContent : null,
            timelineMax: document.getElementById("timeline").max,
            timelineValue: document.getElementById("timeline").value,
            charts: Array.from(document.querySelectorAll("#chartsGrid .chart-card canvas")).map((c) => ({
                w: Math.round(c.getBoundingClientRect().width),
                h: Math.round(c.getBoundingClientRect().height),
            })),
            timeline: document.getElementById("timeText").textContent,
        }));

        const initial = await readReplay();

        // 播放 3 秒
        await replay.click("#playBtn");
        await sleep(3000);
        const playing = await readReplay();
        await replay.click("#playBtn");
        ok("历史视频播放", playing.videoComplete && playing.frame !== "帧 --", `${playing.time} / ${playing.frame}`);
        ok("播放推进时间轴", playing.time !== initial.time, `${initial.time} -> ${playing.time}`);

        // 时间轴拖动到 50%
        await replay.evaluate(() => {
            const el = document.getElementById("timeline");
            el.value = String(Math.round(Number(el.max) * 0.5));
            el.dispatchEvent(new Event("input", { bubbles: true }));
        });
        await sleep(1200);
        const half = await readReplay();
        const parseTime = (text) => {
            const m = /(\d+):(\d+)\.(\d+)/.exec(text || "");
            return m ? Number(m[1]) * 60 + Number(m[2]) + Number(m[3]) / 1000 : NaN;
        };
        const halfExpect = Number(half.timelineMax) / 2 / 1000;
        ok("时间轴拖动", Math.abs(parseTime(half.time) - halfExpect) < 0.6,
            `${half.time}（期望约 ${halfExpect.toFixed(2)} s）`);

        // 按帧号定位：从 placeholder 解析范围，取中间帧号
        const range = await replay.$eval("#frameIdInput", (el) => el.placeholder);
        const nums = (range.match(/(\d+)\D+(\d+)/) || []).slice(1).map(Number);
        const targetId = nums.length === 2 ? Math.round((nums[0] + nums[1]) / 2) : 100;
        await replay.evaluate((id) => {
            const input = document.getElementById("frameIdInput");
            input.value = String(id);
            document.getElementById("locateBtn").click();
        }, targetId);
        await sleep(1200);
        const located = await readReplay();
        ok("按帧号定位", located.locate.includes(`frame ${targetId}`) && located.frameId === String(targetId),
            `${located.locate} / vFrameId=${located.frameId}`);

        // 不存在的帧号：必须给出范围提示
        await replay.evaluate((id) => {
            const input = document.getElementById("frameIdInput");
            input.value = String(id);
            document.getElementById("locateBtn").click();
        }, 999999);
        await sleep(800);
        const missing = await readReplay();
        ok("非法帧号提示", missing.locate.includes("不存在"), missing.locate);

        // 曲线页签：4 个 canvas 有尺寸；播放头随定位变化
        await replay.click("#tabBtnCharts");
        await sleep(1200);
        const replayCharts = await readReplay();
        ok("回放曲线页签", replayCharts.charts.length === 4 && replayCharts.charts.every((c) => c.w > 100 && c.h > 40),
            JSON.stringify(replayCharts.charts));

        // 页签 B / C 内容渲染
        await replay.click("#tabBtnFrame");
        await sleep(500);
        const frameTab = await replay.evaluate(() => ({
            pane: !document.getElementById("tabFrame").hidden,
            videoIdx: document.getElementById("vVideoIdx").textContent,
            timestamp: document.getElementById("vTimestamp").textContent,
            txNote: document.getElementById("txNote").textContent,
        }));
        ok("当前帧数据页签", frameTab.pane && frameTab.videoIdx !== "--" && frameTab.txNote.includes("实际发送"),
            JSON.stringify(frameTab));

        await replay.click("#tabBtnSession");
        await sleep(500);
        const sessionTab = await replay.evaluate(() => ({
            pane: !document.getElementById("tabSession").hidden,
            session: document.getElementById("iSession").textContent,
            accuracy: document.getElementById("iAccuracy").textContent,
            configRows: document.querySelectorAll("#configParams .kv").length,
            errors: document.getElementById("iErrors").textContent,
        }));
        ok("会话信息页签", sessionTab.pane && sessionTab.configRows >= 10 && sessionTab.accuracy.includes("逐帧"),
            JSON.stringify(sessionTab));

        // 视频全屏
        await replay.click("#fsBtn");
        await sleep(600);
        const fullscreen = await replay.evaluate(() => ({
            label: document.getElementById("fsBtn").textContent,
            faux: document.getElementById("videoPanel").classList.contains("faux-fullscreen"),
            real: !!document.fullscreenElement,
        }));
        ok("视频全屏", fullscreen.label.includes("退出全屏") || fullscreen.faux || fullscreen.real,
            JSON.stringify(fullscreen));
        await replay.keyboard.press("Escape");
        await sleep(400);

        report.replay = { initial, playing, half, located, missing, replayCharts, frameTab, sessionTab, fullscreen };

        // 汇总
        report.summary = {
            total: Object.keys(report.checks).length,
            passed: Object.values(report.checks).filter((c) => c.pass).length,
            failed: Object.entries(report.checks).filter(([, c]) => !c.pass).map(([k]) => k),
            js_errors: report.errors,
        };
        console.log(JSON.stringify(report, null, 2));
    } finally {
        await browser.close();
    }
})().catch((err) => {
    console.error("regression failed:", err);
    process.exit(1);
});
