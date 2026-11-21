// M4 界面回归（仅本地验证用）：双工作台导航 + 偏差标柱 + 机械与电控页 + 回放事件记录
// 用法: NODE_PATH=$(npm root -g) node tools/regression_m4.js [liveUrl] [replayUrl] [outPrefix] [sessionHint]
const puppeteer = require("puppeteer");

const LIVE_URL = process.argv[2] || "http://127.0.0.1:5000/";
const REPLAY_URL = process.argv[3] || "http://127.0.0.1:5001/";
const OUT = process.argv[4] || "/tmp/m4ui";
const SESSION_HINT = process.argv[5] || "";
const chromePath = process.env.HOME +
    "/.cache/puppeteer/chrome/linux-157.0.8103.50/chrome-linux64/chrome";
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function apiJSON(url, options) {
    const resp = await fetch(url, options);
    return resp.json();
}

async function waitOffset(page, positive, minAbs, timeoutMs = 9000) {
    const start = Date.now();
    while (Date.now() - start < timeoutMs) {
        const info = await page.evaluate(() => ({
            detect: document.getElementById("detectState").textContent,
            offset: document.getElementById("vOffsetX").textContent,
            value: document.getElementById("devValue").textContent,
        }));
        const off = Number.parseFloat(info.offset);
        if (info.detect.includes("检测到") && Number.isFinite(off) &&
            (positive ? off > minAbs : off < -minAbs)) {
            return { offset: off, devValue: info.value };
        }
        await sleep(150);
    }
    return null;
}

(async () => {
    const browser = await puppeteer.launch({
        executablePath: chromePath,
        headless: true,
        args: ["--no-sandbox", "--disable-gpu", "--disable-dev-shm-usage",
            "--user-data-dir=/tmp/dart-m4-puppeteer-profile", "--window-size=1920,1080"],
    });
    const report = { checks: {}, vision: {}, mechanical: {}, replay: {}, errors: [] };
    const ok = (name, pass, detail) => { report.checks[name] = { pass: !!pass, detail }; };
    const watch = (page) => {
        page.on("console", (msg) => { if (msg.type() === "error") report.errors.push("console: " + msg.text()); });
        page.on("pageerror", (err) => report.errors.push("pageerror: " + err.message));
        page.on("requestfailed", (req) => {
            const t = req.url();
            if (t.includes("/video_feed") || t.includes("/api/telemetry/stream")) return;
            report.errors.push("requestfailed: " + t);
        });
    };

    try {
        // ==================== A. 视觉调试台：偏差标柱 ====================
        const vision = await browser.newPage();
        watch(vision);
        await vision.setViewport({ width: 1920, height: 1080, deviceScaleFactor: 1 });
        await vision.goto(LIVE_URL, { waitUntil: "domcontentloaded", timeout: 30000 });
        await sleep(3500);

        const gauge = await vision.evaluate(() => {
            const canvas = document.getElementById("devCanvas");
            const rect = canvas ? canvas.getBoundingClientRect() : null;
            return {
                exists: !!document.getElementById("devGauge"),
                canvas: rect ? [Math.round(rect.width), Math.round(rect.height)] : null,
                threshold: document.getElementById("devThreshold").textContent,
                turn: document.getElementById("devTurn").textContent,
                value: document.getElementById("devValue").textContent,
                navLinks: Array.from(document.querySelectorAll(".workbench-nav .wb-link")).map((a) => ({
                    text: a.textContent.trim(), href: a.getAttribute("href"),
                })),
            };
        });
        ok("偏差标柱存在且已布局", gauge.exists && gauge.canvas && gauge.canvas[0] > 100 && gauge.canvas[1] > 20,
            JSON.stringify(gauge.canvas));
        ok("阈值来自运行参数", /阈值 ±\d+(\.\d+)? px/.test(gauge.threshold), gauge.threshold);
        ok("工作台导航链接", gauge.navLinks.length === 3 &&
            gauge.navLinks.some((l) => l.text === "视觉调试") &&
            gauge.navLinks.some((l) => l.text === "机械与电控"), JSON.stringify(gauge.navLinks));
        report.vision.initial = gauge;

        // 正偏差截图
        const positive = await waitOffset(vision, true, 120);
        if (positive) {
            await sleep(200);
            await vision.screenshot({ path: OUT + "_vision_gauge_positive.png" });
        }
        const positiveInfo = await vision.evaluate(() => ({
            value: document.getElementById("devValue").textContent,
            turn: document.getElementById("devTurn").textContent,
            valueClass: document.getElementById("devValue").className,
        }));
        ok("正偏差显示与方向", positive && positiveInfo.value.includes("右偏") &&
            positiveInfo.value.includes("+") && positiveInfo.turn.includes("右转"),
            `${positiveInfo.value} / ${positiveInfo.turn}`);

        // 负偏差截图
        const negative = await waitOffset(vision, false, 120);
        if (negative) {
            await sleep(200);
            await vision.screenshot({ path: OUT + "_vision_gauge_negative.png" });
        }
        const negativeInfo = await vision.evaluate(() => ({
            value: document.getElementById("devValue").textContent,
            turn: document.getElementById("devTurn").textContent,
        }));
        ok("负偏差显示与方向", negative && negativeInfo.value.includes("左偏") &&
            negativeInfo.value.includes("-") && negativeInfo.turn.includes("左转"),
            `${negativeInfo.value} / ${negativeInfo.turn}`);
        report.vision.positive = positiveInfo;
        report.vision.negative = negativeInfo;

        // ==================== B. 导航 + 录制不受影响 ====================
        let recorder = await apiJSON(LIVE_URL.replace(/\/$/, "") + "/api/recorder");
        if (!recorder.recording) {
            await apiJSON(LIVE_URL.replace(/\/$/, "") + "/api/recorder", {
                method: "POST", headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ cmd: "start" }),
            });
            await sleep(2500);
            recorder = await apiJSON(LIVE_URL.replace(/\/$/, "") + "/api/recorder");
        }
        const framesBefore = recorder.frames_processed || 0;
        ok("录制已开启（跳转测试前置）", !!recorder.recording, `recording=${recorder.recording} frames=${framesBefore}`);

        await Promise.all([
            vision.waitForNavigation({ waitUntil: "domcontentloaded", timeout: 15000 }),
            vision.click('.workbench-nav a[href="/mechanical"]'),
        ]);
        await sleep(2500);
        ok("跳转到机械与电控页", vision.url().includes("/mechanical"), vision.url());

        const mechanical = await vision.evaluate(() => ({
            navActive: (() => {
                const el = document.querySelector(".workbench-nav .wb-link.active");
                return el ? el.textContent.trim() : null;
            })(),
            tabs: Array.from(document.querySelectorAll(".mech-side .side-tab")).map((b) => b.textContent.trim()),
            statusPaneVisible: !document.getElementById("mechTabStatus").hidden,
            summary: ["sumTarget", "sumDart", "sumStage", "sumComm"].map((id) => ({
                id, text: document.getElementById(id).textContent,
            })),
            statusRows: document.querySelectorAll("#boardStatusFields .mech-row").length,
            paramRows: document.querySelectorAll("#boardParamFields .mech-row").length,
            visualRows: document.querySelectorAll("#visualInputFields .mech-row").length,
            runtimeRows: document.querySelectorAll("#runtimeFields .mech-row").length,
            chartGroups: document.querySelectorAll("#chartGroups label").length,
            chartSelection: document.getElementById("chartSelection").textContent,
            boardStatusTag: document.getElementById("boardStatusTag").textContent,
            overflow: [document.documentElement.scrollWidth - document.documentElement.clientWidth,
                document.documentElement.scrollHeight - document.documentElement.clientHeight],
        }));
        const summaryOk = mechanical.summary.every((s) => (
            s.id === "sumComm"
                ? (s.text.includes("视觉在线") || s.text.includes("视觉链路"))
                : s.text.includes("待接入")
        ));
        ok("机械页摘要为待接入", summaryOk, JSON.stringify(mechanical.summary));
        ok("电控状态字段待接入", mechanical.statusRows >= 4 && mechanical.boardStatusTag.includes("待接入"),
            `${mechanical.statusRows} 行`);
        ok("视觉侧关联真实数据", mechanical.visualRows >= 8, `${mechanical.visualRows} 行`);
        ok("运行状态真实数据", mechanical.runtimeRows >= 5, `${mechanical.runtimeRows} 行`);
        ok("曲线分组可选择（左侧主观测区）", mechanical.chartGroups === 4 && mechanical.chartSelection.includes("已选择"),
            `${mechanical.chartGroups} 组 / ${mechanical.chartSelection}`);
        ok("右侧页签（状态/参数/运行/人工记录）", mechanical.tabs.length === 4 && mechanical.statusPaneVisible,
            JSON.stringify(mechanical.tabs));
        ok("机械页无页面溢出", mechanical.overflow[0] === 0 && mechanical.overflow[1] === 0,
            JSON.stringify(mechanical.overflow));
        await vision.screenshot({ path: OUT + "_mechanical.png" });
        report.mechanical = mechanical;

        // SIM 预览
        await vision.click("#simToggle");
        await sleep(600);
        const sim = await vision.evaluate(() => ({
            badgeVisible: !document.getElementById("simBadge").hidden,
            target: document.getElementById("sumTarget").textContent,
            yaw: document.querySelector("#boardParamFields .mech-row .v").textContent,
            paramTag: document.getElementById("paramStatusTag").textContent,
        }));
        ok("模拟数据明显标识", sim.badgeVisible && sim.target.includes("SIM") && sim.yaw.includes("SIM"),
            `${sim.target} / ${sim.yaw}`);
        await vision.screenshot({ path: OUT + "_mechanical_sim.png" });
        await vision.click("#simToggle");
        await sleep(400);

        // 曲线选择交互
        const firstChartCheckbox = await vision.$("#chartGroups input[type=checkbox]");
        if (firstChartCheckbox) {
            await firstChartCheckbox.click();
            await sleep(300);
        }
        const chartAfter = await vision.evaluate(() => document.getElementById("chartSelection").textContent);
        ok("曲线分组勾选生效", chartAfter.includes("已选择") || chartAfter.includes("未选择"), chartAfter);

        // 右侧页签切换：参数页签（视觉侧关联）
        await vision.click("#mechTabBtnParams");
        await sleep(500);
        const paramsTab = await vision.evaluate(() => ({
            visible: !document.getElementById("mechTabParams").hidden,
            statusHidden: document.getElementById("mechTabStatus").hidden,
            visualRows: document.querySelectorAll("#visualInputFields .mech-row").length,
        }));
        ok("页签切换到参数", paramsTab.visible && paramsTab.statusHidden && paramsTab.visualRows >= 8,
            `视觉侧 ${paramsTab.visualRows} 行`);
        await vision.screenshot({ path: OUT + "_mechanical_params.png" });

        // 运行页签
        await vision.click("#mechTabBtnRuntime");
        await sleep(500);
        const runtimeTab = await vision.evaluate(() => ({
            visible: !document.getElementById("mechTabRuntime").hidden,
            rows: document.querySelectorAll("#runtimeFields .mech-row").length,
        }));
        ok("页签切换到运行", runtimeTab.visible && runtimeTab.rows >= 5, `${runtimeTab.rows} 行`);

        // 人工记录页签
        await vision.click("#mechTabBtnNotes");
        await sleep(500);
        const notesTab = await vision.evaluate(() => !document.getElementById("mechTabNotes").hidden);
        ok("页签切换到人工记录", notesTab);
        await vision.click("#noteInput");
        await vision.type("#noteInput", "M4 回归测试：Yaw 机构复测备注");
        await vision.click("#noteBtn");
        await sleep(1500);
        const note = await vision.evaluate(() => ({
            msg: document.getElementById("noteMsg").textContent,
            count: document.getElementById("noteTag").textContent,
        }));
        ok("人工调试记录写入会话", note.msg.includes("已写入"), `${note.msg} / ${note.count}`);
        report.mechanical.note = note;

        // 回到视觉页，录制应继续（帧数增长）
        await Promise.all([
            vision.waitForNavigation({ waitUntil: "domcontentloaded", timeout: 15000 }),
            vision.click('.workbench-nav a[href="/"]'),
        ]);
        await sleep(3500);
        const recorderAfter = await apiJSON(LIVE_URL.replace(/\/$/, "") + "/api/recorder");
        const framesAfter = recorderAfter.frames_processed || 0;
        ok("页面跳转不影响录像", !!recorderAfter.recording && framesAfter > framesBefore,
            `${framesBefore} -> ${framesAfter}（recording=${recorderAfter.recording}）`);
        report.recorder = { framesBefore, framesAfter, recording: recorderAfter.recording };

        // ==================== C. 回放页事件记录 ====================
        const replay = await browser.newPage();
        watch(replay);
        await replay.setViewport({ width: 1920, height: 1080, deviceScaleFactor: 1 });
        await replay.goto(REPLAY_URL, { waitUntil: "domcontentloaded", timeout: 30000 });
        await sleep(3000);
        if (SESSION_HINT) {
            await replay.evaluate((hint) => {
                const select = document.getElementById("sessionSelect");
                const opt = Array.from(select.options).find((o) => o.textContent.includes(hint));
                if (opt) {
                    select.value = opt.value;
                    select.dispatchEvent(new Event("change", { bubbles: true }));
                }
            }, SESSION_HINT);
            await sleep(2500);
        }
        await replay.click("#tabBtnEvents");
        await sleep(800);
        const replayEvents = await replay.evaluate(() => ({
            total: document.querySelectorAll("#eventList .event-item").length,
            manual: document.querySelectorAll("#eventList .event-item.manual").length,
            manualText: (() => {
                const el = document.querySelector("#eventList .event-item.manual");
                return el ? el.textContent : null;
            })(),
            meta: document.getElementById("eventMeta").textContent,
            mechLinkVisible: !document.getElementById("mechLink").hidden,
            mechLinkHref: document.getElementById("mechLink").getAttribute("href"),
        }));
        ok("回放事件记录包含人工记录", replayEvents.manual >= 1 &&
            (replayEvents.manualText || "").includes("人工记录"), replayEvents.manualText);
        ok("回放页工作台入口", replayEvents.mechLinkVisible &&
            (replayEvents.mechLinkHref || "").includes("/mechanical"), replayEvents.mechLinkHref);
        await replay.screenshot({ path: OUT + "_replay_events.png" });
        report.replay = replayEvents;

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
    console.error("M4 ui test failed:", err);
    process.exit(1);
});
