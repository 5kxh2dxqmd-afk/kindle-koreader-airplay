(function () {
    /* v2: on-screen debug strip + several start triggers + XHR watchdog.
     * Status stays on the local receiver; the picture is a file next to this
     * WAF (current.bmp). */
    var API = "http://127.0.0.1:7080";
    var DEBUG = false;   /* true = show status strip at bottom of the screen */
    var splash, viewer, errorPage, statusText, statusDot, frame, frameNote, dbgEl;
    var connected = false, failures = 0, lastFrameId = -1, started = false;
    var POLL_MS = 300;          /* status poll interval while connected */
    var SETTLE_TILES = 40;      /* after this many tile redraws... */
    var SETTLE_MS = 1500;       /* ...and this long without changes, flash once to clear ghosting. Tap the screen to flash manually. */
    var flashEl, dirtyTiles = 0, lastChangeAt = 0;
    var tilesEl, tileImgs = [], tileVer = [], gridKey = "";
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

    /* The picture is a grid of small images (t<N>.bmp); only tiles whose version
     * changed are reloaded, so e-ink redraws just the regions that moved. */
    function buildGrid(d) {
        var key = d.width + "x" + d.height + "/" + d.tw + "x" + d.th + "/" + d.cols + "x" + d.rows;
        if (key === gridKey && tilesEl) return;
        gridKey = key;
        if (!tilesEl) {
            tilesEl = document.createElement("div");
            byId("frame-wrap").appendChild(tilesEl);
            frame.style.display = "none";
        }
        tilesEl.innerHTML = "";
        tilesEl.style.cssText = "position:relative;margin:0 auto;width:" + d.width + "px;height:" + d.height + "px;";
        tileImgs = []; tileVer = [];
        for (var i = 0; i < d.cols * d.rows; i++) {
            var c = i % d.cols, r = Math.floor(i / d.cols);
            var left = c * d.tw, top = r * d.th;
            var w = Math.min(d.tw, d.width - left), h = Math.min(d.th, d.height - top);
            tileVer[i] = -1;
            if (w <= 0 || h <= 0) { tileImgs[i] = null; continue; }
            var im = document.createElement("img");
            im.style.cssText = "position:absolute;left:" + left + "px;top:" + top + "px;width:" + w + "px;height:" + h + "px;border:0;";
            tilesEl.appendChild(im);
            tileImgs[i] = im;
        }
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
                if (data.hasFrame && data.tiles) {
                    frameNote.innerHTML = "";
                    var nowMs = new Date().getTime();
                    if (data.frameId !== lastFrameId) {
                        lastFrameId = data.frameId;
                        buildGrid(data);
                        for (var ti = 0; ti < tileImgs.length; ti++) {
                            if (tileImgs[ti] && tileVer[ti] !== data.tiles[ti]) {
                                tileVer[ti] = data.tiles[ti];
                                tileImgs[ti].src = "t" + ti + ".bmp?v=" + data.tiles[ti];
                                dirtyTiles++;
                            }
                        }
                        lastChangeAt = nowMs;
                    } else if (dirtyTiles >= SETTLE_TILES && nowMs - lastChangeAt > SETTLE_MS) {
                        dirtyTiles = 0;   /* picture settled after a burst of redraws: clear ghosting once */
                        flash();
                    }
                } else {
                    frameNote.innerHTML = "Waiting for the first picture…";
                }
            } else {
                connected = false;
                showSplash("Ready to connect");
                frame.removeAttribute("src");
                lastFrameId = -1;
                gridKey = ""; tileVer = []; dirtyTiles = 0;
                if (tilesEl) tilesEl.innerHTML = "";
            }
            window.setTimeout(update, connected ? POLL_MS : 1400);
        });
    }

    function start() {
        if (started) return;
        splash = byId("splash"); viewer = byId("viewer"); errorPage = byId("error");
        statusText = byId("status-text"); statusDot = byId("status-dot");
        frame = byId("airplay-frame"); frameNote = byId("frame-note");
        if (!splash || !frame) { return; }
        started = true;
        viewer.onclick = function () { dirtyTiles = 0; flash(); };
        var bar = viewer.getElementsByTagName("div")[0];   /* .viewer-bar */
        var modeLabel = byId("mode-label");
        if (bar) bar.onclick = function (e) {
            e = e || window.event;
            if (e.stopPropagation) e.stopPropagation(); else e.cancelBubble = true;
            modeIdx = (modeIdx + 1) % 3;
            if (modeLabel) modeLabel.innerHTML = "CONNECTED &middot; " + MODE_NAMES[modeIdx];
            dirtyTiles = 0;
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
