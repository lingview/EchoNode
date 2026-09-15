const TermPanel = {
  sessions: {},
  ro: null,
  idToAgent: {},
  activeAgent: "",
  open(agentId) {
    const sess = this.sessions[agentId];
    if (sess && sess.dead) {
      this._openNew(agentId);
      return
    }
    if (sess) {
      this.activate(agentId);
      if (!sess.sessionId) {
        Ws.sendJson({
          type: "task_req",
          agentId: agentId,
          action: "shell_open",
          payload: {}
        });
        this._armTimeout(agentId)
      }
      return
    }
    this._openNew(agentId)
  },
  _armTimeout(agentId) {
    setTimeout(() => {
      const sess = this.sessions[agentId];
      if (sess && !sess.sessionId && !sess.dead) {
        sess.term.writeln("\r\n[连接超时：请确认 server/agent 正在运行，再点击机器重试]")
      }
    }, 10000)
  },
  _openNew(agentId) {
    const stack = $("terminal-stack");
    $("terminal-hint") && $("terminal-hint").classList.add("hidden");
    const container = document.createElement("div");
    container.className = "terminal-session";
    stack.appendChild(container);
    const term = new window.Terminal({
      cursorBlink: true,
      fontSize: 13
    });
    term.open(container);
    const sess = {
      term, container, sessionId: "", ro: null, dead: false
    };
    this.sessions[agentId] = sess;
    term.onData(data => {
      if (!sess.sessionId || sess.dead) return;
      Ws.sendJson({
        type: "shell_data",
        sessionId: sess.sessionId,
        data: data,
        eof: false
      })
    });
    term.onResize(size => {
      if (sess.sessionId && !sess.dead) {
        Ws.sendJson({
          type: "shell_resize",
          sessionId: sess.sessionId,
          cols: size.cols,
          rows: size.rows
        })
      }
    });
    this.ensureResizeObserver();
    this.activate(agentId);
    Ws.sendJson({
      type: "task_req",
      agentId: agentId,
      action: "shell_open",
      payload: {}
    });
    term.writeln(`--${agentId}--`);
    this._armTimeout(agentId);
    setTimeout(() => this.fitActive(), 200);
    setTimeout(() => this.fitActive(), 600)
  },
  activate(agentId) {
    this.activeAgent = agentId;
    for (const [aid, sess] of Object.entries(this.sessions)) {
      sess.container.classList.toggle("active", aid === agentId)
    }
    this.fitActive();
    setTimeout(() => this.fitActive(), 200);
    if (this.sessions[agentId]) this.sessions[agentId].term.focus();
    document.querySelectorAll("#terminal-tabs .tab").forEach(t => {
      t.classList.toggle("active", t.dataset.agent === agentId)
    });
    const tabs = $("terminal-tabs");
    if (!tabs.querySelector(`[data-agent="${agentId}"]`)) {
      const tab = document.createElement("div");
      tab.className = "tab";
      tab.dataset.agent = agentId;
      tab.textContent = Window_getHostName(agentId);
      tab.onclick = () => this.open(agentId);
      tabs.appendChild(tab)
    }
  },
  fitActive() {
    const sess = this.sessions[this.activeAgent];
    if (!sess || !sess.term) return;
    const stack = $("terminal-stack");
    const probe = document.createElement("span");
    probe.style.cssText = "visibility:hidden;white-space:pre;font-family:consolas, monospace;font-size:13px;position:absolute;";
    probe.textContent = "W".repeat(20);
    document.body.appendChild(probe);
    let cw = 0,
        lh = 0;
    try {
      const dim = sess.term._core._renderService.dimensions.css.cell;
      if (dim && dim.width > 0) {
        cw = dim.width;
        lh = dim.height
      }
    } catch (e) {}
    if (!cw) {
      cw = probe.getBoundingClientRect().width / 20 || 7.2;
      lh = 13 * 1.2
    }
    probe.remove();
    const cols = Math.max(20, Math.floor((stack.clientWidth - 4) / cw));
    const rows = Math.max(10, Math.floor(stack.clientHeight / lh) - 1);
    const dimsChanged = this._lastCw === undefined || Math.abs(cw - this._lastCw) > 0.01;
    this._lastCw = cw;
    if (cols !== sess.term.cols || rows !== sess.term.rows || dimsChanged) {
      sess.term.resize(cols, rows)
    }
  },
  resizeAll() {
    this.fitActive()
  },
  ensureResizeObserver() {
    if (this.ro) return;
    const stack = $("terminal-stack");
    if (!stack) return;
    requestAnimationFrame(() => this.fitActive());
    this.ro = new ResizeObserver(() => this.fitActive());
    this.ro.observe(stack)
  },
  onMessage(msg) {
    if (msg.type !== "task_result" && msg.type !== "shell_data") return false;
    if (msg.type === "task_result") {
      const isUuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
      const isOpenResult = msg.action === "shell_open" || (!msg.action && msg.ok && isUuid.test(msg.data || "") && this.sessions[this.activeAgent] && !this.sessions[this.activeAgent].sessionId);
      if (isOpenResult) {
        const agentId = msg.agentId || this.activeAgent;
        const sess = this.sessions[agentId];
        if (!sess) return false;
        if (msg.ok && msg.data) {
          sess.sessionId = msg.data;
          this.idToAgent[msg.data] = agentId;
          sess.term.writeln("shell 已就绪");
          this.fitActive();
          const size = {
            cols: sess.term.cols,
            rows: sess.term.rows
          };
          Ws.sendJson({
            type: "shell_resize",
            sessionId: sess.sessionId,
            cols: size.cols,
            rows: size.rows
          })
        } else {
          sess.term.writeln(`打开失败:${msg.error||"未知错误"}`)
        }
        return true
      }
      return false
    }
    const agentId = this.idToAgent[msg.sessionId];
    if (!agentId) return false;
    const sess = this.sessions[agentId];
    if (!sess) return false;
    sess.term.write(msg.data);
    if (msg.eof) {
      sess.term.writeln("\r\n-- 会话已结束 --");
      sess.dead = true
    }
    return true
  },
  noop() {},
};

function Window_getHostName(agentId) {
  return (window._agentHosts && window._agentHosts[agentId]) || agentId.slice(0, 8)
}