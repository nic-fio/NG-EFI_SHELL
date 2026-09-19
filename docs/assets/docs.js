/* NESH documentation: table of contents, search, index, code blocks, diagrams.
 * Plain JavaScript, no dependencies (Mermaid is loaded separately when present). */
(function () {
  "use strict";

  var main = document.querySelector("main");
  if (!main) return;

  /* ---- helpers ---- */

  function el(tag, attrs, text) {
    var e = document.createElement(tag);
    if (attrs) for (var k in attrs) e.setAttribute(k, attrs[k]);
    if (text != null) e.textContent = text;
    return e;
  }
  function slug(s) {
    return s.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "") || "section";
  }
  function headingText(h) {
    var c = h.cloneNode(true);
    c.querySelectorAll(".secnum, .anchor, .badge").forEach(function (x) { x.remove(); });
    return c.textContent.trim();
  }

  /* ---- ids, numbering, anchors ---- */

  var used = {};
  document.querySelectorAll("[id]").forEach(function (e) { used[e.id] = true; });
  function uniqueId(base) {
    var id = base, n = 2;
    while (used[id]) id = base + "-" + n++;
    used[id] = true;
    return id;
  }

  var heads = Array.prototype.slice.call(main.querySelectorAll("h2, h3"))
    .filter(function (h) { return !h.closest(".cmd, .no-toc, .titlepage"); });
  var c2 = 0, c3 = 0;
  heads.forEach(function (h) {
    if (!h.id) h.id = uniqueId(slug(h.textContent));
    var numbered = !h.closest(".unnumbered") && !h.classList.contains("unnumbered");
    if (h.tagName === "H2") {
      if (numbered) { c2++; c3 = 0; }
      h.dataset.num = numbered ? String(c2) : "";
    } else {
      if (numbered && c2) c3++;
      h.dataset.num = numbered && c2 ? c2 + "." + c3 : "";
    }
    if (h.dataset.num) h.insertBefore(el("span", { "class": "secnum" }, h.dataset.num), h.firstChild);
  });
  main.querySelectorAll("h2, h3, h4").forEach(function (h) {
    if (!h.id) h.id = uniqueId(slug(h.textContent));
    var a = el("a", { "class": "anchor", href: "#" + h.id, "aria-label": "Link to this section" }, "#");
    h.appendChild(a);
  });

  /* ---- sidebar table of contents ---- */

  var tocBox = document.querySelector(".toc");
  var tocLinks = [];
  if (tocBox) {
    var top = el("ol"), curLi = null, sub = null;
    heads.forEach(function (h) {
      var a = el("a", { href: "#" + h.id });
      if (h.dataset.num) a.appendChild(el("span", { "class": "num" }, h.dataset.num));
      a.appendChild(document.createTextNode(headingText(h)));
      var li = el("li");
      li.appendChild(a);
      tocLinks.push({ a: a, h: h, li: li });
      if (h.tagName === "H2") {
        top.appendChild(li);
        curLi = li;
        sub = null;
      } else if (curLi) {
        if (!sub) { sub = el("ol"); curLi.appendChild(sub); }
        sub.appendChild(li);
      }
    });
    tocBox.appendChild(top);
  }

  /* printable contents at the top of the document */
  var tocPrint = document.getElementById("contents-list");
  if (tocPrint) {
    var ol = el("ol"), cur = null, subo = null;
    heads.forEach(function (h) {
      if (!h.dataset.num) return;
      var li = el("li");
      li.appendChild(el("a", { href: "#" + h.id }, headingText(h)));
      if (h.tagName === "H2") { ol.appendChild(li); cur = li; subo = null; }
      else if (cur) { if (!subo) { subo = el("ol"); cur.appendChild(subo); } subo.appendChild(li); }
    });
    tocPrint.appendChild(ol);
  }

  /* highlight the current section while scrolling */
  function updateCurrent() {
    var pos = window.scrollY + 90, current = null;
    for (var i = 0; i < tocLinks.length; i++) {
      if (tocLinks[i].h.offsetTop <= pos) current = tocLinks[i];
      else break;
    }
    tocLinks.forEach(function (t) {
      t.a.classList.remove("current");
      t.li.classList.remove("open");
    });
    if (current) {
      current.a.classList.add("current");
      var li = current.li;
      while (li && li.tagName === "LI") {
        li.classList.add("open");
        li = li.parentElement && li.parentElement.closest("li");
      }
      var side = document.querySelector(".sidebar");
      if (side && !side.matches(":hover")) {
        var r = current.a.getBoundingClientRect(), sr = side.getBoundingClientRect();
        if (r.top < sr.top + 60 || r.bottom > sr.bottom - 40)
          side.scrollTop += r.top - sr.top - sr.height / 3;
      }
    }
    var bt = document.querySelector(".backtop");
    if (bt) bt.classList.toggle("show", window.scrollY > 800);
  }
  var ticking = false;
  window.addEventListener("scroll", function () {
    if (!ticking) { ticking = true; requestAnimationFrame(function () { ticking = false; updateCurrent(); }); }
  });

  /* ---- code blocks: syntax colors and copy buttons ---- */

  var BASIC_KW = ("and or xor not mod shl shr if then else elseif end for to step next while wend do loop until " +
    "select case is exit continue goto sub function call return local dim redim print input let run append " +
    "cls color locate pause sleep rem").split(" ");
  var C_KW = ("auto break case char const continue default do double else enum extern float for goto if inline " +
    "int long register return short signed sizeof static struct switch typedef union unsigned void volatile while " +
    "bool true false NULL uint8_t uint16_t uint32_t uint64_t int64_t size_t").split(" ");

  function esc(s) {
    return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  }
  function highlightBasic(src) {
    var out = "", re = /('[^\n]*|\bREM\b[^\n]*)|("(?:[^"\n]|"")*")|(&[HhBbOo][0-9A-Fa-f_]+|0[xX][0-9A-Fa-f_]+|\b\d+\b)|([A-Za-z_][A-Za-z0-9_]*\$?)|([\s\S])/gi;
    var m;
    while ((m = re.exec(src))) {
      if (m[1]) out += '<span class="tok-com">' + esc(m[1]) + "</span>";
      else if (m[2]) out += '<span class="tok-str">' + esc(m[2]) + "</span>";
      else if (m[3]) out += '<span class="tok-num">' + esc(m[3]) + "</span>";
      else if (m[4]) {
        var w = m[4];
        if (BASIC_KW.indexOf(w.toLowerCase()) >= 0) out += '<span class="tok-kw">' + esc(w) + "</span>";
        else if (/^[A-Z][A-Z0-9_]*\$?$/.test(w) && w.length > 1 && src.charAt(re.lastIndex) === "(")
          out += '<span class="tok-fn">' + esc(w) + "</span>";
        else out += esc(w);
      } else out += esc(m[5]);
    }
    return out;
  }
  function highlightC(src) {
    var out = "", re = /(\/\*[\s\S]*?\*\/|\/\/[^\n]*)|("(?:[^"\\\n]|\\.)*"|'(?:[^'\\\n]|\\.)*')|(\b0[xX][0-9A-Fa-f]+\b|\b\d+\b)|(#\s*\w+)|([A-Za-z_][A-Za-z0-9_]*)|([\s\S])/g;
    var m;
    while ((m = re.exec(src))) {
      if (m[1]) out += '<span class="tok-com">' + esc(m[1]) + "</span>";
      else if (m[2]) out += '<span class="tok-str">' + esc(m[2]) + "</span>";
      else if (m[3]) out += '<span class="tok-num">' + esc(m[3]) + "</span>";
      else if (m[4]) out += '<span class="tok-kw">' + esc(m[4]) + "</span>";
      else if (m[5]) out += C_KW.indexOf(m[5]) >= 0 ? '<span class="tok-c-kw">' + esc(m[5]) + "</span>" : esc(m[5]);
      else out += esc(m[6]);
    }
    return out;
  }
  /* terminal: lines starting with a prompt ("fs0:\> ", "fs1:\efi> ", "... ") are commands */
  function highlightTerminal(src) {
    return src.split("\n").map(function (line) {
      var m = /^((?:[A-Za-z]+\d*:[^>\n]*|nesh|\.\.\.)> ?|\$ )(.*)$/.exec(line);
      if (m) return '<span class="prompt">' + esc(m[1]) + '</span><span class="cmdline">' + esc(m[2]) + "</span>";
      if (/^\S+: .*(error|not found|cannot|invalid|no such)/i.test(line)) return '<span class="err">' + esc(line) + "</span>";
      return esc(line);
    }).join("\n");
  }

  main.querySelectorAll("pre.code, pre.terminal").forEach(function (pre) {
    var src = pre.textContent.replace(/^\n/, "").replace(/\s+$/, "");
    var lang = pre.getAttribute("data-lang") || (pre.classList.contains("terminal") ? "terminal" : "");
    if (lang === "basic") pre.innerHTML = highlightBasic(src);
    else if (lang === "c") pre.innerHTML = highlightC(src);
    else if (lang === "terminal") pre.innerHTML = highlightTerminal(src);
    else pre.textContent = src;
    var wrap = el("div", { "class": "codewrap" + (pre.classList.contains("terminal") ? " terminal-wrap" : "") });
    pre.parentNode.insertBefore(wrap, pre);
    var label = pre.getAttribute("data-title") ||
      { basic: "NESH BASIC", c: "C", terminal: "Console", shell: "Shell", make: "Make", text: "" }[lang];
    if (label) wrap.appendChild(el("span", { "class": "label" }, label));
    wrap.appendChild(pre);
    var btn = el("button", { "class": "btn copy", type: "button" }, "Copy");
    btn.addEventListener("click", function () {
      var text = src;
      if (lang === "terminal") /* copy only the commands */
        text = src.split("\n").map(function (l) {
          var m = /^(?:[A-Za-z]+\d*:[^>\n]*|nesh)> ?(.*)$/.exec(l);
          return m ? m[1] : null;
        }).filter(function (l) { return l !== null; }).join("\n") || src;
      function done() { btn.textContent = "Copied"; setTimeout(function () { btn.textContent = "Copy"; }, 1400); }
      if (navigator.clipboard && window.isSecureContext) navigator.clipboard.writeText(text).then(done, function () {});
      else {
        var ta = el("textarea");
        ta.value = text;
        document.body.appendChild(ta);
        ta.select();
        try { document.execCommand("copy"); done(); } catch (e) {}
        ta.remove();
      }
    });
    wrap.appendChild(btn);
  });

  /* tables: horizontal scrolling wrapper */
  main.querySelectorAll("table").forEach(function (t) {
    if (t.parentElement.classList.contains("table-wrap")) return;
    var w = el("div", { "class": "table-wrap" });
    t.parentNode.insertBefore(w, t);
    w.appendChild(t);
  });

  /* ---- search and index data ---- */

  var entries = [];
  heads.forEach(function (h) {
    entries.push({ text: headingText(h), kind: h.tagName === "H2" ? "chapter" : "section", href: "#" + h.id, num: h.dataset.num });
  });
  main.querySelectorAll("article.cmd").forEach(function (a) {
    entries.push({ text: a.getAttribute("data-index") || a.id.replace(/^cmd-/, ""), kind: "command", href: "#" + a.id });
  });
  /* "UCASE$(s$) · LCASE$(s$)": one entry per name */
  main.querySelectorAll("dl.defs dt[id]").forEach(function (dt) {
    var names = dt.getAttribute("data-index") ? dt.getAttribute("data-index").split(";")
      : dt.textContent.split("\u00b7").map(function (p) { return (/^\s*([A-Za-z_][A-Za-z0-9_]*\$?)/.exec(p) || [])[1]; });
    names.forEach(function (name) {
      if (name) entries.push({ text: name.trim(), kind: dt.getAttribute("data-kind") || "function", href: "#" + dt.id });
    });
  });
  main.querySelectorAll("[data-index]:not(article):not(dt)").forEach(function (e) {
    if (!e.id) e.id = uniqueId("ix-" + slug(e.getAttribute("data-index").split(";")[0]));
    e.getAttribute("data-index").split(";").forEach(function (term) {
      term = term.trim();
      if (term) entries.push({ text: term, kind: "topic", href: "#" + e.id });
    });
  });

  var box = document.querySelector(".search input"), results = document.querySelector(".search-results");
  var activeIdx = -1, shown = [];
  function where(href) {
    var target = document.getElementById(href.slice(1));
    if (!target) return "";
    for (var i = heads.length - 1; i >= 0; i--)
      if (heads[i].compareDocumentPosition(target) & Node.DOCUMENT_POSITION_FOLLOWING || heads[i] === target)
        return heads[i].dataset.num ? heads[i].dataset.num : "";
    return "";
  }
  function runSearch() {
    var q = box.value.trim().toLowerCase();
    results.innerHTML = "";
    shown = [];
    activeIdx = -1;
    if (!q) { results.classList.remove("open"); return; }
    var scored = [];
    entries.forEach(function (e) {
      var t = e.text.toLowerCase(), i = t.indexOf(q);
      if (i < 0) return;
      var score = (t === q ? 0 : i === 0 ? 1 : 2) * 10 + ({ command: 0, function: 1, chapter: 2, section: 3, topic: 4 }[e.kind] || 5);
      scored.push({ e: e, s: score });
    });
    scored.sort(function (a, b) { return a.s - b.s || a.e.text.localeCompare(b.e.text); });
    var seen = {};
    scored.forEach(function (x) {
      var key = x.e.text + x.e.href;
      if (seen[key] || shown.length >= 40) return;
      seen[key] = true;
      var a = el("a", { href: x.e.href });
      a.appendChild(el("span", { "class": "kind" }, x.e.kind));
      a.appendChild(document.createTextNode(x.e.text));
      a.addEventListener("click", function () { closeSearch(); closeNav(); });
      results.appendChild(a);
      shown.push(a);
    });
    if (!shown.length) results.appendChild(el("div", { "class": "empty" }, "No results"));
    results.classList.add("open");
  }
  function closeSearch() { if (results) results.classList.remove("open"); }
  if (box && results) {
    box.addEventListener("input", runSearch);
    box.addEventListener("keydown", function (ev) {
      if (ev.key === "ArrowDown" || ev.key === "ArrowUp") {
        ev.preventDefault();
        if (!shown.length) return;
        activeIdx = (activeIdx + (ev.key === "ArrowDown" ? 1 : -1) + shown.length) % shown.length;
        shown.forEach(function (a, i) { a.classList.toggle("active", i === activeIdx); });
        shown[activeIdx].scrollIntoView({ block: "nearest" });
      } else if (ev.key === "Enter") {
        var a = shown[activeIdx >= 0 ? activeIdx : 0];
        if (a) { location.hash = a.getAttribute("href"); closeSearch(); closeNav(); box.blur(); }
      } else if (ev.key === "Escape") {
        box.value = "";
        closeSearch();
        box.blur();
      }
    });
    document.addEventListener("click", function (ev) { if (!ev.target.closest(".search")) closeSearch(); });
    document.addEventListener("keydown", function (ev) {
      if (ev.key === "/" && document.activeElement !== box && !/input|textarea/i.test(document.activeElement.tagName)) {
        ev.preventDefault();
        openNavIfSmall();
        box.focus();
      }
    });
  }

  /* ---- alphabetical index ---- */

  var indexBox = document.getElementById("index-list");
  if (indexBox) {
    var groups = {};
    entries.forEach(function (e) {
      if (e.kind === "chapter") return;
      var t = e.text.replace(/^[^A-Za-z0-9]+/, "");
      var letter = (t.charAt(0) || "#").toUpperCase();
      if (!/[A-Z]/.test(letter)) letter = "#";
      (groups[letter] = groups[letter] || []).push(e);
    });
    var letters = Object.keys(groups).sort();
    var bar = el("div", { "class": "index-letters" });
    letters.forEach(function (L) { bar.appendChild(el("a", { href: "#index-" + L }, L)); });
    indexBox.appendChild(bar);
    letters.forEach(function (L) {
      var g = el("div", { "class": "index-group" });
      g.appendChild(el("h4", { id: "index-" + L }, L));
      var ul = el("ul");
      var seen = {};
      groups[L].sort(function (a, b) { return a.text.toLowerCase().localeCompare(b.text.toLowerCase()); })
        .forEach(function (e) {
          var key = e.text.toLowerCase() + e.href;
          if (seen[key]) return;
          seen[key] = true;
          var li = el("li");
          var a = el("a", { href: e.href }, e.text);
          if (e.kind === "command" || e.kind === "function" || e.kind === "statement") a.className = "mono";
          li.appendChild(a);
          var w = where(e.href);
          li.appendChild(el("span", { "class": "where" }, "  " + (e.kind === "section" || e.kind === "topic" ? "" : e.kind + " ") + (w ? "§" + w : "")));
          ul.appendChild(li);
        });
      g.appendChild(ul);
      indexBox.appendChild(g);
    });
  }

  /* ---- navigation on small screens, back to top, theme button ---- */

  function closeNav() { document.body.classList.remove("nav-open"); }
  function openNavIfSmall() { if (window.matchMedia("(max-width: 960px)").matches) document.body.classList.add("nav-open"); }
  var menuBtn = document.querySelector(".menu-btn");
  if (menuBtn) menuBtn.addEventListener("click", function () { document.body.classList.toggle("nav-open"); });
  document.addEventListener("click", function (ev) {
    if (document.body.classList.contains("nav-open") && !ev.target.closest(".sidebar, .menu-btn")) closeNav();
  });
  if (tocBox) tocBox.addEventListener("click", function (ev) { if (ev.target.closest("a")) closeNav(); });

  var bt = el("button", { "class": "btn backtop", type: "button", "aria-label": "Back to top" }, "↑ Top");
  bt.addEventListener("click", function () { window.scrollTo({ top: 0, behavior: "smooth" }); });
  document.body.appendChild(bt);

  var printBtn = document.querySelector(".print-btn");
  if (printBtn) printBtn.addEventListener("click", function () { window.print(); });

  /* ---- Mermaid diagrams ---- */

  var diagrams = Array.prototype.slice.call(main.querySelectorAll(".mermaid"));
  diagrams.forEach(function (d) { d.dataset.src = d.textContent; });
  function renderDiagrams() {
    if (!window.mermaid || !diagrams.length) return;
    window.mermaid.initialize({
      startOnLoad: false,
      securityLevel: "strict",
      theme: "base",
      fontFamily: "system-ui, -apple-system, Segoe UI, Roboto, sans-serif",
      themeVariables: {
        background: "#ffffff", primaryColor: "#e8f1fb", primaryTextColor: "#1c2530", primaryBorderColor: "#0a66c2",
        lineColor: "#56636f", secondaryColor: "#eaf6ee", tertiaryColor: "#f6f8fa", noteBkgColor: "#fff5e0",
        noteTextColor: "#1c2530", actorBkg: "#e8f1fb", actorTextColor: "#1c2530", actorBorder: "#0a66c2",
        signalColor: "#1c2530", signalTextColor: "#1c2530", clusterBkg: "#f6f8fa", clusterBorder: "#dde3ea",
        edgeLabelBackground: "#ffffff", nodeTextColor: "#1c2530"
      }
    });
    diagrams.forEach(function (d) {
      d.removeAttribute("data-processed");
      d.innerHTML = "";
      d.textContent = d.dataset.src;
    });
    try { window.mermaid.run({ nodes: diagrams }); } catch (e) { /* diagrams stay as text */ }
  }
  renderDiagrams();

  /* initial state (after the layout has settled) */
  updateCurrent();
  if (location.hash) {
    var t = document.getElementById(location.hash.slice(1));
    if (t) setTimeout(function () { t.scrollIntoView(); }, 0);
  }
})();
