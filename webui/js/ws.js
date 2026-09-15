const Ws = {
  socket: null,
  handlers: [],
  queue: [],
  binHandlers: [],
  connect() {
    if (!Api.token) return;
    if (this.socket && this.socket.readyState <= WebSocket.OPEN) return;
    const proto = location.protocol === "https:" ? "wss://" : "ws://";
    this.socket = new WebSocket(proto + location.host + "/ws");
    this.socket.binaryType = "arraybuffer";
    this.socket.onopen = () => {
      this.socket.send(JSON.stringify({
        type: "auth",
        token: Api.token
      }));
      for (const m of this.queue) this.sendJson(m);
      this.queue = []
    };
    this.socket.onmessage = ev => {
      if (ev.data instanceof ArrayBuffer) {
        for (const h of this.binHandlers) {
          if (h(ev.data)) return
        }
        return
      }
      let msg;
      try {
        msg = JSON.parse(ev.data)
      } catch (e) {
        return
      }
      for (const h of this.handlers) {
        if (h(msg)) return
      }
      if (msg.type === "agent_online" || msg.type === "agent_offline") {
        refreshAgents()
      }
    };
    this.socket.onclose = () => {
      setTimeout(() => {
        if (Api.token) this.connect()
      }, 3000)
    }
  },
  sendJson(obj) {
    if (this.socket && this.socket.readyState === WebSocket.OPEN) {
      this.socket.send(JSON.stringify(obj))
    } else {
      this.queue.push(obj)
    }
  },
  sendBinary(data) {
    if (this.socket && this.socket.readyState === WebSocket.OPEN) {
      this.socket.send(data)
    }
  },
  onMessage(handler) {
    this.handlers.push(handler)
  },
  onBinary(handler) {
    this.binHandlers.push(handler)
  }
};