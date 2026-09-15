const Api = {
  token: localStorage.getItem("echonode_token") || "",

  async request(path, options = {}) {
    const headers = Object.assign(
      { "Content-Type": "application/json" },
      this.token ? { "Authorization": "Bearer " + this.token } : {}
    );
    const resp = await fetch(path, Object.assign({ headers }, options));
    if (resp.status === 401) {
      this.token = "";
      localStorage.removeItem("echonode_token");
      throw new Error("unauthorized");
    }
    return resp;
  },

  async login(user, pass) {
    const resp = await fetch("/api/login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ username: user, password: pass })
    });
    if (!resp.ok) throw new Error("invalid credentials");
    const data = await resp.json();
    this.token = data.token;
    localStorage.setItem("echonode_token", this.token);
  },

  agents() { return this.request("/api/agents").then(r => r.json()); },
  tasks() { return this.request("/api/tasks").then(r => r.json()); }
};
