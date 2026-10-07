#!/usr/bin/env python3
"""Tests for the engine's MCP server (src/editor/McpServer.cpp).

    python3 tools/test_mcp.py [-v]

Starts bin/dayfall headless on temporary copies of maps/starter (under xvfb-run when there is no DISPLAY) and
checks the server three ways:

- raw JSON-RPC over stdio: parse errors, unknown methods, batches, protocol negotiation, resources, prompts,
  logging, and cancelling a running tool call;
- raw HTTP (Streamable HTTP): sessions (Mcp-Session-Id, 404 for unknown ones, DELETE), the MCP-Protocol-Version
  header (400), body limit (413), Origin check (403), batches, keep-alive, progress as an event stream;
- the official MCP Python SDK over stdio and HTTP: initialize, list tools / resources / templates / prompts,
  read resources, get a prompt, call tools (images, structuredContent), progress, cancellation.

The SDK part is skipped with a message when the `mcp` package is missing (python3 -m pip install mcp).
Exits non-zero on failure.
"""
import base64
import http.client
import json
import os
import queue
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import warnings

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "bin", "dayfall.exe" if os.name == "nt" else "dayfall")
STARTER = os.path.join(ROOT, "maps", "starter")
LATEST = "2025-11-25"
# six big views: several seconds of rendering on a software GPU, so there is time to cancel
BIG_CAPTURE = {"views": [{"overview": True}, {"player": True}, {"top_down": {}}, {"orbit": {"target": [0, 0], "distance_m": 30}},
                         {"orbit": {"target": [10, 10], "distance_m": 20}}, {"editor": True}],
               "width": 1920, "height": 1080, "format": "png"}

try:
    import anyio
    from mcp import ClientSession, StdioServerParameters, stdio_client
    try:
        from mcp.client.streamable_http import streamable_http_client as http_client   # SDK 2.x
    except ImportError:
        from mcp.client.streamable_http import streamablehttp_client as http_client    # SDK 1.x
    HAVE_SDK = True
except ImportError as e:
    HAVE_SDK = False
    SDK_ERROR = str(e)


def temp_map(tmp):
    """A copy of the starter map: captures are written into it and nothing touches maps/."""
    d = os.path.join(tmp, "starter")
    shutil.copytree(STARTER, d)
    return d


def engine_command(args, log):
    """Command line and environment for the engine. xvfb-run merges stderr into stdout, which carries MCP in stdio
    mode, so under xvfb-run the engine's log goes to the log file through sh."""
    env = dict(os.environ, DAYFALL_LOG=log)
    cmd = [EXE] + args
    if os.name != "nt" and not os.environ.get("DISPLAY") and shutil.which("xvfb-run"):
        cmd = ["xvfb-run", "-a", "sh", "-c", 'exec "$0" "$@" 2>>"$DAYFALL_LOG"'] + cmd
    return cmd, env


def captures_in(map_dir):
    d = os.path.join(map_dir, "captures")
    return set(os.listdir(d)) if os.path.isdir(d) else set()


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def stop(p, logfile=None):
    if p.poll() is None:
        try:
            os.killpg(p.pid, signal.SIGTERM) if os.name != "nt" else p.terminate()
        except ProcessLookupError:
            pass
        try:
            p.wait(timeout=20)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL) if os.name != "nt" else p.kill()
    if p.stdout:
        p.stdout.close()
    if logfile:
        logfile.close()


# ===================================================================================== raw stdio
class StdioEngine:
    """The engine as a stdio MCP server, spoken to line by line."""

    def __init__(self, tmp):
        self.map = temp_map(tmp)
        self.log = os.path.join(tmp, "stdio.log")
        cmd, env = engine_command([self.map, "--headless", "--mcp", "stdio"], self.log)
        self.logfile = open(self.log, "a")
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.logfile, text=True,
                                  bufsize=1, env=env, start_new_session=True)
        self.q = queue.Queue()
        self.backlog = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            try:
                self.q.put(json.loads(line))
            except ValueError:
                self.q.put({"_not_json": line})

    def send(self, msg):
        self.p.stdin.write((msg if isinstance(msg, str) else json.dumps(msg)) + "\n")
        self.p.stdin.flush()

    def wait_for(self, pred, timeout=120):
        """The first message (seen or new) matching pred; the others stay in the backlog."""
        for i, m in enumerate(self.backlog):
            if pred(m):
                return self.backlog.pop(i)
        end = time.time() + timeout
        while True:
            try:
                m = self.q.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                raise AssertionError("no matching message within %d s" % timeout)
            if pred(m):
                return m
            self.backlog.append(m)

    def request(self, method, params=None, rid=None, timeout=120):
        rid = rid if rid is not None else int(time.time() * 1e6)
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self.send(msg)
        return self.wait_for(lambda m: m.get("id") == rid and ("result" in m or "error" in m), timeout)

    def drain(self):
        while True:
            try:
                self.backlog.append(self.q.get_nowait())
            except queue.Empty:
                return

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=30)
        except Exception:
            pass
        stop(self.p, self.logfile)


def init_params(version=LATEST):
    return {"protocolVersion": version, "capabilities": {}, "clientInfo": {"name": "test_mcp", "version": "1"}}


class RawStdio(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="dayfall_mcp_")
        cls.e = StdioEngine(cls.tmp)
        r = cls.e.request("initialize", init_params(), timeout=180)
        assert r["result"]["protocolVersion"] == LATEST, r
        cls.e.send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    @classmethod
    def tearDownClass(cls):
        cls.e.close()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_capabilities(self):
        r = self.e.request("initialize", init_params())["result"]
        for c in ("tools", "resources", "prompts", "logging"):
            self.assertIn(c, r["capabilities"])
        self.assertEqual(r["serverInfo"]["name"], "dayfall")
        self.assertIn("dayfall://docs/agent-workflow", r["instructions"])

    def test_protocol_negotiation(self):
        try:
            for want, got in (("2024-11-05", "2024-11-05"), ("2025-03-26", "2025-03-26"), ("2025-06-18", "2025-06-18"),
                              ("1999-01-01", LATEST), (LATEST, LATEST)):
                self.assertEqual(self.e.request("initialize", init_params(want))["result"]["protocolVersion"], got)
            self.e.request("initialize", init_params("2025-03-26"))
            r = self.e.request("tools/call", {"name": "project_info"})["result"]
            self.assertNotIn("structuredContent", r)   # added in 2025-06-18
            self.assertEqual(json.loads(r["content"][0]["text"])["map"]["name"], "Starter")
        finally:
            self.e.request("initialize", init_params())
        r = self.e.request("tools/call", {"name": "project_info"})["result"]
        self.assertEqual(r["structuredContent"]["map"]["name"], "Starter")

    def test_parse_error(self):
        self.e.send("{not json")
        m = self.e.wait_for(lambda m: m.get("error", {}).get("code") == -32700)
        self.assertIsNone(m["id"])

    def test_invalid_and_unknown(self):
        self.e.send({"jsonrpc": "2.0", "id": "bad", "params": {}})
        self.assertEqual(self.e.wait_for(lambda m: m.get("id") == "bad")["error"]["code"], -32600)
        self.assertEqual(self.e.request("no/such_method")["error"]["code"], -32601)
        self.assertEqual(self.e.request("tools/call", {"name": "no_such_tool"})["error"]["code"], -32602)
        self.assertEqual(self.e.request("prompts/get", {"name": "build_level"})["error"]["code"], -32602)   # needs description
        self.assertEqual(self.e.request("prompts/get", {"name": "no_such_prompt"})["error"]["code"], -32602)
        for uri in ("dayfall://nothing", "dayfall://docs/nope", "dayfall://map/section/not_a_section",
                    "dayfall://captures/../map.json", "dayfall://captures/missing.jpg"):
            err = self.e.request("resources/read", {"uri": uri})["error"]
            self.assertEqual(err["code"], -32002, uri)
            self.assertEqual(err["data"]["uri"], uri)

    def test_batch(self):
        self.e.send([{"jsonrpc": "2.0", "id": "b1", "method": "ping"}, {"jsonrpc": "2.0", "method": "notifications/progress"},
                     {"jsonrpc": "2.0", "id": "b2", "method": "tools/list"}])
        m = self.e.wait_for(lambda m: isinstance(m, list))
        self.assertEqual([x["id"] for x in m], ["b1", "b2"])
        self.assertTrue(all(t.get("title") for t in m[1]["result"]["tools"]))

    def test_resources(self):
        uris = [r["uri"] for r in self.e.request("resources/list")["result"]["resources"]]
        for u in ("dayfall://docs/agent-workflow", "dayfall://docs/map-format", "dayfall://docs/tools", "dayfall://map/document"):
            self.assertIn(u, uris)
        t = self.e.request("resources/templates/list")["result"]["resourceTemplates"]
        self.assertIn("dayfall://map/section/{section}", [x["uriTemplate"] for x in t])
        c = self.e.request("resources/read", {"uri": "dayfall://docs/tools"})["result"]["contents"][0]
        self.assertEqual(c["mimeType"], "text/markdown")
        self.assertIn("## walk_test", c["text"])
        routes = json.loads(self.e.request("resources/read", {"uri": "dayfall://map/section/routes"})["result"]["contents"][0]["text"])
        self.assertIn("loop", routes)
        hud = self.e.request("resources/read", {"uri": "dayfall://map/section/hud"})["result"]["contents"][0]
        self.assertEqual(json.loads(hud["text"]), None)   # a known section the starter map does not have

    def test_logging(self):
        self.assertEqual(self.e.request("logging/setLevel", {"level": "loud"})["error"]["code"], -32602)
        self.assertEqual(self.e.request("logging/setLevel", {"level": "info"})["result"], {})
        try:
            self.e.request("tools/call", {"name": "stats"})
            m = self.e.wait_for(lambda m: m.get("method") == "notifications/message" and "tool stats" in str(m["params"]["data"]), 30)
            self.assertEqual(m["params"]["level"], "info")
        finally:
            self.e.request("logging/setLevel", {"level": "warning"})

    def test_progress_and_cancel(self):
        before = captures_in(self.e.map)
        rid = "cap-cancel"
        self.e.send({"jsonrpc": "2.0", "id": rid, "method": "tools/call",
                     "params": {"name": "capture", "arguments": BIG_CAPTURE, "_meta": {"progressToken": "tok"}}})
        p = self.e.wait_for(lambda m: m.get("method") == "notifications/progress")
        self.assertEqual(p["params"]["progressToken"], "tok")
        self.assertEqual(p["params"]["total"], 6)
        self.e.send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": rid, "reason": "test"}})
        self.assertEqual(self.e.request("ping")["result"], {})   # the transport keeps answering while the tool runs
        self.e.request("tools/call", {"name": "stats"})          # runs on the main thread once the capture stopped
        time.sleep(0.5)
        self.e.drain()
        self.assertFalse([m for m in self.e.backlog if m.get("id") == rid], "a cancelled request gets no response")
        late = [m for m in self.e.backlog if m.get("method") == "notifications/progress" and m["params"]["progress"] > 1]
        self.assertFalse(late, "progress after the cancellation")
        written = captures_in(self.e.map) - before
        self.assertLess(len(written), 6, "the capture did not stop: %s" % sorted(written))


# ===================================================================================== raw http
class HttpEngine:
    def __init__(self, tmp):
        self.map = temp_map(tmp)
        self.log = os.path.join(tmp, "http.log")
        self.port = free_port()
        cmd, env = engine_command([self.map, "--headless", "--mcp", "http:%d" % self.port], self.log)
        self.logfile = open(self.log, "a")
        self.p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=self.logfile, stderr=subprocess.STDOUT, env=env,
                                  start_new_session=True)
        end = time.time() + 180
        while True:
            c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
            try:
                c.request("GET", "/")
                if c.getresponse().status == 200:
                    return
            except OSError:
                pass
            finally:
                c.close()
            if time.time() > end or self.p.poll() is not None:
                raise RuntimeError("the engine did not start an HTTP server; see " + self.log)
            time.sleep(0.25)

    def close(self):
        stop(self.p, self.logfile)

    @property
    def url(self):
        return "http://127.0.0.1:%d/mcp" % self.port


class RawHttp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="dayfall_mcp_")
        cls.e = HttpEngine(cls.tmp)
        cls.conn = http.client.HTTPConnection("127.0.0.1", cls.e.port, timeout=120)
        r, body = cls.post({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": init_params()})
        cls.sid = r.getheader("Mcp-Session-Id")
        assert r.status == 200 and cls.sid and body["result"]["protocolVersion"] == LATEST, (r.status, body)
        r, _ = cls.post({"jsonrpc": "2.0", "method": "notifications/initialized"}, sid=cls.sid)
        assert r.status == 202, r.status

    @classmethod
    def tearDownClass(cls):
        cls.conn.close()
        cls.e.close()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    @classmethod
    def post(cls, msg, sid=None, version=None, headers=None, raw=None):
        h = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
        if sid:
            h["Mcp-Session-Id"] = sid
        if version:
            h["MCP-Protocol-Version"] = version
        h.update(headers or {})
        cls.conn.request("POST", "/mcp", body=raw if raw is not None else json.dumps(msg), headers=h)
        r = cls.conn.getresponse()
        data = r.read()
        body = None
        if r.getheader("Content-Type", "").startswith("application/json") and data:
            body = json.loads(data)
        elif r.getheader("Content-Type", "").startswith("text/event-stream"):
            body = [json.loads(line[6:]) for line in data.decode().splitlines() if line.startswith("data: ")]
        return r, body

    def call(self, method, params=None, rid=7, **kw):
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        return self.post(msg, sid=kw.pop("sid", self.sid), version=kw.pop("version", LATEST), **kw)

    def test_session_and_keepalive(self):
        r, body = self.call("tools/list")
        sock = self.conn.sock
        self.assertEqual(r.status, 200)
        self.assertEqual(r.getheader("Connection"), "keep-alive")
        self.assertTrue(all(t["title"] for t in body["result"]["tools"]))
        r, body = self.call("ping")
        self.assertEqual(body["result"], {})
        self.assertIs(self.conn.sock, sock, "the connection was not kept alive")

    def test_unknown_session(self):
        r, body = self.call("ping", sid="0123456789abcdef0123456789abcdef")
        self.assertEqual(r.status, 404)

    def test_protocol_header(self):
        r, body = self.call("ping", version="1999-01-01")
        self.assertEqual(r.status, 400)
        self.assertIn("MCP-Protocol-Version", body["error"]["message"])
        for v in ("2024-11-05", "2025-03-26", "2025-06-18", LATEST):
            self.assertEqual(self.call("ping", version=v)[0].status, 200)

    def test_parse_error(self):
        r, body = self.post(None, sid=self.sid, raw="{oops")
        self.assertEqual(r.status, 400)
        self.assertEqual(body["error"]["code"], -32700)

    def test_body_too_large(self):
        with socket.create_connection(("127.0.0.1", self.e.port), timeout=30) as s:
            s.sendall(b"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: 17000000\r\n\r\n{")
            self.assertIn(b" 413 ", s.recv(4096).split(b"\r\n")[0] + b" ")

    def test_origin(self):
        r, _ = self.call("ping", headers={"Origin": "http://evil.example"})
        self.assertEqual(r.status, 403)
        r, _ = self.call("ping", headers={"Origin": "http://localhost.evil.example"})
        self.assertEqual(r.status, 403)
        r, _ = self.call("ping", headers={"Origin": "http://localhost:5173"})
        self.assertEqual(r.status, 200)

    def test_batch(self):
        r, body = self.post([{"jsonrpc": "2.0", "id": "a", "method": "ping"}, {"jsonrpc": "2.0", "id": "b", "method": "prompts/list"}],
                            sid=self.sid)
        self.assertEqual(r.status, 200)
        self.assertEqual([m["id"] for m in body], ["a", "b"])
        self.assertIn("review_level", [p["name"] for p in body[1]["result"]["prompts"]])

    def test_get_and_delete(self):
        self.conn.request("GET", "/mcp", headers={"Accept": "text/event-stream", "Mcp-Session-Id": self.sid})
        r = self.conn.getresponse()
        r.read()
        self.assertEqual(r.status, 405)
        r, body = self.post({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": init_params("2025-06-18")})
        sid = r.getheader("Mcp-Session-Id")
        self.assertNotEqual(sid, self.sid)
        self.assertEqual(body["result"]["protocolVersion"], "2025-06-18")
        self.conn.request("DELETE", "/mcp", headers={"Mcp-Session-Id": sid})
        r = self.conn.getresponse()
        r.read()
        self.assertEqual(r.status, 200)
        self.assertEqual(self.call("ping", sid=sid)[0].status, 404)

    def test_progress_stream(self):
        args = {"name": "walk_test", "arguments": {"route": "loop", "captures": False}, "_meta": {"progressToken": 42}}
        r, events = self.call("tools/call", args, rid=99)
        self.assertEqual(r.getheader("Content-Type"), "text/event-stream")
        self.assertGreaterEqual(len(events), 2)
        self.assertTrue(all(e["method"] == "notifications/progress" and e["params"]["progressToken"] == 42 for e in events[:-1]))
        self.assertEqual(events[-1]["id"], 99)   # the response ends the stream
        self.assertTrue(events[-1]["result"]["structuredContent"]["passed"])
        r, body = self.call("tools/call", args, rid=100, headers={"Accept": "application/json"})   # no stream wanted: plain JSON
        self.assertEqual(r.getheader("Content-Type"), "application/json")
        self.assertEqual(body["id"], 100)

    def test_no_session(self):
        r, body = self.post({"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "project_info"}})
        self.assertEqual(r.status, 200)
        self.assertNotIn("structuredContent", body["result"])   # no header: protocol 2025-03-26


# ===================================================================================== MCP Python SDK
def sc(result):
    return getattr(result, "structured_content", None) or getattr(result, "structuredContent", None)


def is_error(result):
    return getattr(result, "is_error", None) or getattr(result, "isError", False)


def error_code(exc):
    err = getattr(exc, "error", None)
    return getattr(err, "code", None) if err is not None else getattr(exc, "code", None)


async def exercise(t, session, map_dir, logs=None):
    """The same checks over any transport; t is the TestCase."""
    init = await session.initialize()
    t.assertEqual(getattr(init, "protocol_version", None) or init.protocolVersion, LATEST)
    names = {x.name: x for x in (await session.list_tools()).tools}
    for n in ("project_info", "capture", "walk_test", "play_sim"):
        t.assertIn(n, names)
        t.assertTrue(names[n].title)
    uris = [str(r.uri) for r in (await session.list_resources()).resources]
    t.assertIn("dayfall://docs/agent-workflow", uris)
    t.assertIn("dayfall://map/document", uris)
    templates = (await session.list_resource_templates()).resource_templates if hasattr(session, "list_resource_templates") else []
    t.assertIn("dayfall://map/section/{section}", [str(getattr(x, "uri_template", None) or x.uriTemplate) for x in templates])
    prompts = [p.name for p in (await session.list_prompts()).prompts]
    t.assertEqual(sorted(prompts), ["build_level", "refine_area", "review_level", "test_route"])

    doc = (await session.read_resource("dayfall://docs/agent-workflow")).contents[0]
    t.assertIn("batch_end", doc.text)
    mapdoc = json.loads((await session.read_resource("dayfall://map/document")).contents[0].text)
    t.assertEqual(mapdoc["name"], "Starter")
    t.assertIn("loop", mapdoc["routes"])
    try:
        await session.read_resource("dayfall://map/section/nope")
        t.fail("an unknown resource was read")
    except Exception as e:
        t.assertEqual(error_code(e), -32002)

    p = await session.get_prompt("build_level", {"description": "a small valley town with three collectibles"})
    t.assertEqual(p.messages[0].role, "user")
    t.assertIn("a small valley town", p.messages[0].content.text)
    t.assertIn("walk_test", p.messages[0].content.text)

    if logs is not None:
        with warnings.catch_warnings():   # deprecated after 2025-11-25, which this server speaks
            warnings.simplefilter("ignore")
            await session.set_logging_level("info")
    r = await session.call_tool("project_info", {})
    t.assertFalse(is_error(r))
    t.assertEqual(sc(r)["map"]["name"], "Starter")
    t.assertEqual(json.loads(r.content[0].text)["map"]["name"], "Starter")
    if logs is not None:
        with anyio.fail_after(30):
            while not any("tool project_info" in str(m) for m in logs):
                await anyio.sleep(0.05)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            await session.set_logging_level("warning")

    r = await session.call_tool("capture", {"views": [{"overview": True}, {"player": True}], "width": 320, "height": 180})
    t.assertFalse(is_error(r))
    images = [c for c in r.content if c.type == "image"]
    t.assertEqual(len(images), 2)
    t.assertEqual(getattr(images[0], "mime_type", None) or images[0].mimeType, "image/jpeg")
    t.assertEqual(base64.b64decode(images[0].data)[:2], b"\xff\xd8")
    t.assertEqual([v["label"] for v in sc(r)["views"]], ["overview", "player"])
    caps = [str(x.uri) for x in (await session.list_resources()).resources if str(x.uri).startswith("dayfall://captures/")]
    t.assertTrue(caps, "the new captures are not listed as resources")
    blob = (await session.read_resource(caps[0])).contents[0]
    t.assertEqual(base64.b64decode(blob.blob)[:2], b"\xff\xd8")

    progress = []

    async def on_progress(value, total, message=None):
        progress.append((value, total, message))

    r = await session.call_tool("walk_test", {"route": "loop", "captures": False}, progress_callback=on_progress)
    t.assertFalse(is_error(r))
    t.assertTrue(sc(r)["passed"])
    t.assertTrue(progress, "no progress notification")
    t.assertIn("waypoint", progress[0][2])

    # cancel a long capture once it reports progress; the next call must not wait for all six views
    before = captures_in(map_dir)
    started = anyio.Event()

    async def on_capture_progress(value, total, message=None):
        started.set()

    scope = anyio.CancelScope()

    async def long_call():
        with scope:
            await session.call_tool("capture", BIG_CAPTURE, progress_callback=on_capture_progress)

    async with anyio.create_task_group() as tg:
        tg.start_soon(long_call)
        with anyio.fail_after(120):
            await started.wait()
        await anyio.sleep(0.2)
        scope.cancel()
    t.assertTrue(scope.cancel_called)
    r = await session.call_tool("stats", {})
    t.assertFalse(is_error(r))
    written = captures_in(map_dir) - before
    t.assertLess(len(written), 6, "the cancelled capture rendered every view: %s" % sorted(written))


@unittest.skipUnless(HAVE_SDK, "the MCP Python SDK is not installed (python3 -m pip install mcp)")
class Sdk(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="dayfall_mcp_")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_stdio(self):
        map_dir = temp_map(self.tmp)
        log = os.path.join(self.tmp, "sdk_stdio.log")
        cmd, env = engine_command([map_dir, "--headless", "--mcp", "stdio"], log)
        logs = []

        async def on_log(params):
            logs.append(params.data)

        async def main():
            with anyio.fail_after(600):
                with open(log, "a") as errlog:
                    async with stdio_client(StdioServerParameters(command=cmd[0], args=cmd[1:], env=env), errlog=errlog) as streams:
                        async with ClientSession(streams[0], streams[1], logging_callback=on_log) as session:
                            await exercise(self, session, map_dir, logs)

        anyio.run(main)

    def test_http(self):
        e = HttpEngine(self.tmp)
        try:
            async def main():
                with anyio.fail_after(600):
                    async with http_client(e.url) as streams:
                        async with ClientSession(streams[0], streams[1]) as session:
                            await exercise(self, session, e.map)

            anyio.run(main)
        finally:
            e.close()


if __name__ == "__main__":
    if not os.path.exists(EXE):
        sys.exit("build the engine first: %s is missing" % EXE)
    if not HAVE_SDK:
        print("note: skipping the MCP SDK tests (%s); python3 -m pip install mcp" % SDK_ERROR, file=sys.stderr)
    unittest.main()
