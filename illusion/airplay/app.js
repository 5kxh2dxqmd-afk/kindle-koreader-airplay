(function () {
    /* v2: on-screen debug strip + several start triggers + XHR watchdog.
     * Status stays on the local receiver; the picture is a file next to this
     * WAF (current.bmp). */
    var API = "http://127.0.0.1:7080";
    var DEBUG = false;   /* true = show status strip at bottom of the screen */
    var splash, viewer, errorPage, statusText, statusDot, frame, frameNote, dbgEl;
    var connected = false, failures = 0, lastFrameId = -1, started = false;
    var FLASH_EVERY = 40;   /* auto full-screen flash after this many new frames; 0 = off. Tap the screen to flash manually. */
    var flashEl, framesSinceFlash = 0;
    var MODE_NAMES = ["AUTO", "INVERTED", "NORMAL"];   /* tap the top bar to cycle; matches server invert_mode */
    var modeIdx = 0;
    var polls = 0, lastInfo = "booting", jsErrors = "", imgInfo = "img:-";

    function byId(id) { return document.getElementById(id); }

    function paint() {
        if (!DEBUG) return;
        if (!dbgEl && document.body) {
            dbgEl = document.createElement("div");
            dbgEl.style.cssText = "position:fixed;left:0;bottom:0;z-index:99;width:100%;" +
                "padding:2px 6px;font:11px monospace;background:#fff;color:#000;border-top:1px solid #000;";
            document.body.appendChild(dbgEl);
        }
        if (dbgEl) {
            dbgEl.textContent = "v3 js:ok polls:" + polls + " fail:" + failures + " | " + lastInfo + " | " + imgInfo +
                (jsErrors ? " | JSERR " + jsErrors : "");
        }
    }
    function dbg(msg) { lastInfo = msg; paint(); }

    window.onerror = function (m, u, l) { jsErrors = m + "@" + l; paint(); return false; };

    function request(path, done) {
        var xhr = new XMLHttpRequest();
        var finished = false;
        function finish(err) { if (finished) return; finished = true; done(err, xhr); }
        try { xhr.open("GET", API + path, true); }
        catch (e) { finish(new Error("open threw " + e)); return; }
        xhr.onreadystatechange = function () {
            if (xhr.readyState === 4) {
                if (xhr.status >= 200 && xhr.status < 300) finish(null);
                else finish(new Error("HTTP " + xhr.status));
            }
        };
        xhr.onerror = function () { finish(new Error("xhr.onerror")); };
        try { xhr.timeout = 4000; xhr.ontimeout = function () { finish(new Error("timeout")); }; } catch (t) {}
        window.setTimeout(function () {
            if (!finished) {
                var rs = xhr.readyState;
                try { xhr.abort(); } catch (a) {}
                finish(new Error("no reply in 5s rs=" + rs));
            }
        }, 5000);
        try { xhr.send(null); } catch (e2) { finish(new Error("send threw " + e2)); }
    }

    /* Black -> white flash across the whole screen: forces the e-ink panel to clear ghosting. */
    function flash() {
        if (!flashEl) {
            flashEl = document.createElement("div");
            flashEl.style.cssText = "position:fixed;left:0;top:0;width:100%;height:100%;z-index:50;display:none;";
            document.body.appendChild(flashEl);
        }
        flashEl.style.background = "#000";
        flashEl.style.display = "block";
        window.setTimeout(function () {
            flashEl.style.background = "#fff";
            window.setTimeout(function () { flashEl.style.display = "none"; }, 250);
        }, 250);
    }

    function showSplash(message) {
        splash.className = "screen";
        viewer.className = "screen hidden";
        errorPage.className = "screen hidden";
        statusText.innerHTML = message || "Ready to connect";
        statusDot.style.background = connected ? "#111" : "#777";
    }

    function showViewer() {
        splash.className = "screen hidden";
        viewer.className = "screen";
        errorPage.className = "screen hidden";
    }

    function update() {
        polls++;
        var tw = window.innerWidth || document.documentElement.clientWidth || 0;
        var th = (window.innerHeight || document.documentElement.clientHeight || 0) - 62;   /* top bar 42 + bottom 20 */
        request("/api/status?t=" + new Date().getTime() + "&w=" + tw + "&h=" + th + "&inv=" + modeIdx, function (err, xhr) {
            if (err) {
                failures++;
                dbg("ERR " + err.message);
                if (failures > 3) {
                    splash.className = "screen hidden";
                    viewer.className = "screen hidden";
                    errorPage.className = "screen";
                }
                window.setTimeout(update, 1500);
                return;
            }

            failures = 0;
            var data;
            try { data = JSON.parse(xhr.responseText); }
            catch (parseError) { dbg("bad JSON: " + xhr.responseText); window.setTimeout(update, 1500); return; }

            dbg("HTTP ok conn=" + data.connected + " frame=" + data.hasFrame + " id=" + data.frameId);
            if (data.connected) {
                connected = true;
                showViewer();
                if (data.hasFrame) {
                    frameNote.innerHTML = "";
                    if (data.frameId !== lastFrameId) {
                        lastFrameId = data.frameId;
                        frame.src = "current.bmp?t=" + data.frameId;
                        if (FLASH_EVERY && ++framesSinceFlash >= FLASH_EVERY) { framesSinceFlash = 0; flash(); }
                    }
                } else {
                    frameNote.innerHTML = "Waiting for the first picture…";
                }
            } else {
                connected = false;
                showSplash("Ready to connect");
                frame.removeAttribute("src");
                lastFrameId = -1;
            }
            window.setTimeout(update, 1400);
        });
    }

    function start() {
        if (started) return;
        splash = byId("splash"); viewer = byId("viewer"); errorPage = byId("error");
        statusText = byId("status-text"); statusDot = byId("status-dot");
        frame = byId("airplay-frame"); frameNote = byId("frame-note");
        if (!splash || !frame) { return; }
        started = true;
        viewer.onclick = function () { framesSinceFlash = 0; flash(); };
        var bar = viewer.getElementsByTagName("div")[0];   /* .viewer-bar */
        var modeLabel = byId("mode-label");
        if (bar) bar.onclick = function (e) {
            e = e || window.event;
            if (e.stopPropagation) e.stopPropagation(); else e.cancelBubble = true;
            modeIdx = (modeIdx + 1) % 3;
            if (modeLabel) modeLabel.innerHTML = "CONNECTED &middot; " + MODE_NAMES[modeIdx];
            framesSinceFlash = 0;
            flash();
        };
        frame.onload = function () { imgInfo = "img ok " + frame.naturalWidth + "x" + frame.naturalHeight; paint(); };
        frame.onerror = function () {
            if (lastFrameId >= 0 && frame.src.indexOf("current.bmp") >= 0) {
                imgInfo = "img ERR file, trying http"; paint();
                frame.src = API + "/api/frame.bmp?t=" + lastFrameId;
                return;
            }
            imgInfo = "img ERR http"; paint();
            frameNote.innerHTML = "Waiting for picture…";
        };
        paint();
        update();
    }

    if (document.addEventListener) {
        document.addEventListener("DOMContentLoaded", start, false);
        window.addEventListener("load", start, false);
    }
    window.setTimeout(start, 800);
    window.setTimeout(start, 2500);
}());
