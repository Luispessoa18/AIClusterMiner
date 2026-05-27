#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <vector>
#include <string>
#include <csignal>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

#include "tokenizer.hpp"
#include "app.hpp"
#include "json.hpp"
#include "api-types.hpp"
#include "nn/nn-network.hpp"

typedef unsigned int pos_t;

using json = nlohmann::json;

// ─── Points persistence ────────────────────────────────────────────────────

static void loadPoints(const char *path, WorkerRuntimeInfo *workers, NnUint n) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size <= 0) { fclose(f); return; }
    std::string buf(size, '\0');
    fread(&buf[0], 1, size, f);
    fclose(f);
    try {
        auto j = json::parse(buf);
        auto &w = j["workers"];
        for (NnUint i = 0; i < n; i++) {
            std::string key(workers[i].hostname);
            if (w.contains(key)) {
                workers[i].points = w[key]["points"].get<uint64_t>();
                workers[i].tokensGenerated = w[key]["tokensGenerated"].get<uint64_t>();
            }
        }
    } catch (...) {}
}

static void savePoints(const char *path, const WorkerRuntimeInfo *workers, NnUint n) {
    json j;
    for (NnUint i = 0; i < n; i++) {
        j["workers"][workers[i].hostname]["points"] = workers[i].points;
        j["workers"][workers[i].hostname]["tokensGenerated"] = workers[i].tokensGenerated;
    }
    FILE *f = fopen(path, "w");
    if (!f) return;
    std::string s = j.dump(2);
    fwrite(s.c_str(), 1, s.size(), f);
    fclose(f);
}

// ─── Embedded Chat UI ─────────────────────────────────────────────────────

static const char *CHAT_HTML = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>dllama</title>
<style>
* { box-sizing: border-box; margin: 0; padding: 0; }
body { font-family: system-ui, sans-serif; background: #111; color: #ddd; height: 100vh; display: flex; overflow: hidden; }
#sidebar { width: 260px; background: #1a1a1a; border-right: 1px solid #2a2a2a; display: flex; flex-direction: column; padding: 12px; gap: 8px; overflow-y: auto; flex-shrink: 0; }
#sidebar h2 { font-size: 11px; text-transform: uppercase; letter-spacing: 1px; color: #666; margin-bottom: 2px; }
#speed-badge { background: #0d2d4a; color: #5af; padding: 4px 10px; border-radius: 6px; font-size: 13px; font-weight: 600; display: none; }
.node-card { background: #1e1e1e; border-radius: 8px; padding: 10px 12px; border: 1px solid #2a2a2a; }
.node-card .name { font-size: 13px; font-weight: 600; color: #eee; margin-bottom: 4px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.node-card .meta { font-size: 11px; color: #666; line-height: 1.7; }
.node-card .pts { margin-top: 5px; font-size: 12px; color: #e8a020; font-weight: 600; }
#chat { flex: 1; display: flex; flex-direction: column; overflow: hidden; }
#messages { flex: 1; overflow-y: auto; padding: 20px 24px; display: flex; flex-direction: column; gap: 14px; }
.msg { max-width: 72%; padding: 10px 14px; border-radius: 14px; font-size: 14px; line-height: 1.6; white-space: pre-wrap; word-break: break-word; }
.msg.user { align-self: flex-end; background: #1a3d6e; color: #e8f0ff; border-bottom-right-radius: 4px; }
.msg.assistant { align-self: flex-start; background: #222; color: #ddd; border-bottom-left-radius: 4px; }
.msg.assistant.typing::after { content: "▋"; animation: blink .7s step-end infinite; }
@keyframes blink { 50% { opacity: 0; } }
#input-area { padding: 12px 20px; background: #161616; border-top: 1px solid #2a2a2a; display: flex; gap: 8px; align-items: flex-end; }
#prompt { flex: 1; background: #222; border: 1px solid #333; border-radius: 10px; padding: 10px 14px; color: #eee; font-size: 14px; resize: none; outline: none; font-family: inherit; min-height: 44px; max-height: 160px; line-height: 1.5; }
#prompt:focus { border-color: #3a5a8a; }
#send { background: #1a3d6e; color: #d0e4ff; border: none; border-radius: 10px; padding: 10px 22px; cursor: pointer; font-size: 14px; font-weight: 600; flex-shrink: 0; height: 44px; }
#send:hover:not(:disabled) { background: #234e8c; }
#send:disabled { opacity: 0.4; cursor: default; }
</style>
</head>
<body>
<div id="sidebar">
  <h2>Nodes</h2>
  <div id="speed-badge"></div>
  <div id="nodes-list"><div style="color:#555;font-size:12px;padding:4px 0">Loading...</div></div>
</div>
<div id="chat">
  <div id="messages"></div>
  <div id="input-area">
    <textarea id="prompt" placeholder="Type a message… (Shift+Enter for new line)" rows="1"></textarea>
    <button id="send">Send</button>
  </div>
</div>
<script>
const history = [];
let busy = false;

function esc(t) {
  return t.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}

function renderAll() {
  const el = document.getElementById('messages');
  el.innerHTML = history.map(m =>
    `<div class="msg ${m.role}">${esc(m.content)}</div>`
  ).join('');
  el.scrollTop = el.scrollHeight;
}

function getLastEl() {
  const el = document.getElementById('messages');
  return el.lastElementChild;
}

function appendToLast(text) {
  if (!history.length || history[history.length-1].role !== 'assistant') return;
  history[history.length-1].content += text;
  const el = getLastEl();
  if (el) el.textContent = history[history.length-1].content;
  const box = document.getElementById('messages');
  box.scrollTop = box.scrollHeight;
}

async function send() {
  const ta = document.getElementById('prompt');
  const text = ta.value.trim();
  if (!text || busy) return;
  ta.value = '';
  ta.style.height = 'auto';

  history.push({role:'user', content: text});
  history.push({role:'assistant', content: ''});
  renderAll();
  const last = getLastEl();
  if (last) last.classList.add('typing');

  busy = true;
  document.getElementById('send').disabled = true;

  try {
    const msgs = history.slice(0, -1).map(m => ({role:m.role, content:m.content}));
    const res = await fetch('/v1/chat/completions', {
      method: 'POST',
      headers: {'Content-Type':'application/json'},
      body: JSON.stringify({messages: msgs, stream: true})
    });
    const reader = res.body.getReader();
    const dec = new TextDecoder();
    let buf = '';
    while (true) {
      const {done, value} = await reader.read();
      if (done) break;
      buf += dec.decode(value, {stream:true});
      const lines = buf.split('\n');
      buf = lines.pop();
      for (const line of lines) {
        if (!line.startsWith('data:')) continue;
        const data = line.slice(5).trim();
        if (data === '[DONE]') continue;
        try {
          const j = JSON.parse(data);
          const delta = j?.choices?.[0]?.delta?.content;
          if (delta) appendToLast(delta);
        } catch {}
      }
    }
  } catch(e) {
    appendToLast('\n[error: ' + e.message + ']');
  }

  const el = getLastEl();
  if (el) el.classList.remove('typing');
  busy = false;
  document.getElementById('send').disabled = false;
}

document.getElementById('send').addEventListener('click', send);
document.getElementById('prompt').addEventListener('keydown', e => {
  if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); send(); return; }
  requestAnimationFrame(() => {
    const el = e.target;
    el.style.height = 'auto';
    el.style.height = Math.min(el.scrollHeight, 160) + 'px';
  });
});

async function fetchWorkers() {
  try {
    const d = await (await fetch('/v1/workers')).json();
    const badge = document.getElementById('speed-badge');
    const tps = d.tokensPerSec || 0;
    if (tps > 0) {
      badge.style.display = 'block';
      badge.textContent = tps.toFixed(1) + ' tok/s';
    }
    const list = document.getElementById('nodes-list');
    list.innerHTML = (d.nodes || []).map(n => {
      const gb = (n.totalMemoryMb / 1024).toFixed(1);
      const mhz = n.cpuMhz > 0 ? ` @ ${n.cpuMhz} MHz` : '';
      return `<div class="node-card">
        <div class="name" title="${esc(n.hostname)}">${esc(n.hostname)}</div>
        <div class="meta">${n.cpuCores} cores${mhz}<br>${gb} GB RAM</div>
        <div class="pts">${n.points.toLocaleString()} pts &middot; ${n.tokensGenerated.toLocaleString()} tokens</div>
      </div>`;
    }).join('');
  } catch {}
}

fetchWorkers();
setInterval(fetchWorkers, 5000);
</script>
</body>
</html>
)HTML";

// ─── HTTP layer ───────────────────────────────────────────────────────────

enum class HttpMethod {
    METHOD_GET = 0,
    METHOD_POST = 1,
    METHOD_PUT = 2,
    METHOD_DELETE = 3,
    METHOD_OPTIONS = 4,
    METHOD_UNKNOWN = 5
};

class HttpRequest {
public:
    static HttpRequest read(int serverSocket) {
        HttpRequest req(serverSocket);

        std::vector<char> httpRequest = req.readHttpRequest();
        std::string data = std::string(httpRequest.begin(), httpRequest.end());

        std::istringstream iss(data);
        std::string line;
        std::getline(iss, line);

        std::istringstream lineStream(line);
        std::string methodStr, path;
        lineStream >> methodStr >> path;
        req.method = parseMethod(methodStr);
        req.path = path;

        while (std::getline(iss, line) && line != "\r") {
            size_t pos = line.find(':');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                std::string value = line.substr(pos + 2);
                value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
                    return std::isspace(c) || !std::isprint(c);
                }), value.end());
                req.headers[key] = value;
            }
        }

        std::getline(iss, req.body, '\0');

        if (req.body.size() > 0) {
            req.parsedJson = json::parse(req.body);
        }
        return req;
    }

    static HttpMethod parseMethod(const std::string& method) {
        if (method == "GET") return HttpMethod::METHOD_GET;
        if (method == "POST") return HttpMethod::METHOD_POST;
        if (method == "PUT") return HttpMethod::METHOD_PUT;
        if (method == "DELETE") return HttpMethod::METHOD_DELETE;
        if (method == "OPTIONS") return HttpMethod::METHOD_OPTIONS;
        return HttpMethod::METHOD_UNKNOWN;
    }

private:
    int serverSocket;
public:
    std::string path;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    json parsedJson;
    HttpMethod method;

    HttpRequest(int serverSocket) {
        this->serverSocket = serverSocket;
    }

    std::vector<char> readHttpRequest() {
        std::string httpRequest;
        char buffer[1024 * 64];
        ssize_t bytesRead;

        std::string headerData;
        std::string extraReadPastHeader;
        for (;;) {
            bytesRead = recv(serverSocket, buffer, sizeof(buffer) - 1, 0);
            if (bytesRead <= 0) {
                throw std::runtime_error("Error while reading headers from socket");
            }
            buffer[bytesRead] = '\0';
            headerData.append(buffer);

            const size_t endRnRn = headerData.find("\r\n\r\n");
            if (endRnRn != std::string::npos) {
                if (endRnRn < headerData.size() - 4)
                    extraReadPastHeader = headerData.substr(endRnRn + 4);
                break;
            }
            const size_t endNN = headerData.find("\n\n");
            if (endNN != std::string::npos) {
                if (endNN < headerData.size() - 2)
                    extraReadPastHeader = headerData.substr(endNN + 2);
                break;
            }
        }

        httpRequest.append(headerData);

        std::istringstream headerStream(headerData);
        std::string line;
        ssize_t contentLength = 0;
        while (std::getline(headerStream, line) && line != "\r" && line != "\n") {
            size_t pos = line.find(':');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                std::string value = line.substr(pos + 2);
                if (key == "Content-Length") {
                    try {
                      contentLength = std::stoi(value);
                    } catch (const std::invalid_argument& e) {
                      throw std::runtime_error("Bad Content-Length header - not a number");
                    }
                    break;
                }
            }
        }

        if (contentLength > 0) {
            if (extraReadPastHeader.size() > static_cast<size_t>(contentLength)) {
                throw std::runtime_error("Received more body data than Content-Length header said");
            }
            contentLength -= extraReadPastHeader.size();

            std::vector<char> body(contentLength);
            ssize_t totalRead = 0;
            while (totalRead < contentLength) {
                bytesRead = recv(serverSocket, body.data() + totalRead, contentLength - totalRead, 0);
                if (bytesRead <= 0) {
                    throw std::runtime_error("Error while reading body from socket");
                }
                totalRead += bytesRead;
            }
            if (body.size() > 0) {
              httpRequest.append(body.data(), contentLength);
            }
        }

        return std::vector<char>(httpRequest.begin(), httpRequest.end());
    }

    std::string getMethod() {
        if (method == HttpMethod::METHOD_GET) return "GET";
        if (method == HttpMethod::METHOD_POST) return "POST";
        if (method == HttpMethod::METHOD_PUT) return "PUT";
        if (method == HttpMethod::METHOD_DELETE) return "DELETE";
        if (method == HttpMethod::METHOD_OPTIONS) return "OPTIONS";
        return "UNKNOWN";
    }

    void writeCors() {
        std::ostringstream buffer;
        buffer << "HTTP/1.1 204 No Content\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Access-Control-Allow-Methods: GET, POST, PUT, DELETE\r\n"
            << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
            << "Connection: close\r\n"
            << "\r\n";
        std::string data = buffer.str();
        writeSocket(serverSocket, data.c_str(), data.size());
    }

    void writeNotFound() {
        std::ostringstream buffer;
        buffer << "HTTP/1.1 404 Not Found\r\n"
            << "Connection: close\r\n"
            << "Content-Length: 9\r\n"
            << "\r\n"
            << "Not Found";
        std::string data = buffer.str();
        writeSocket(serverSocket, data.c_str(), data.size());
    }

    void writeJson(std::string json) {
        std::ostringstream buffer;
        buffer << "HTTP/1.1 200 OK\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Content-Type: application/json; charset=utf-8\r\n"
            << "Connection: close\r\n"
            << "Content-Length: " << json.length() << "\r\n\r\n" << json;
        std::string data = buffer.str();
        writeSocket(serverSocket, data.c_str(), data.size());
    }

    void writeHtml(const char *html, size_t len) {
        std::ostringstream hdr;
        hdr << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: text/html; charset=utf-8\r\n"
            << "Connection: close\r\n"
            << "Content-Length: " << len << "\r\n\r\n";
        std::string h = hdr.str();
        writeSocket(serverSocket, h.c_str(), h.size());
        writeSocket(serverSocket, html, len);
    }

    void writeStreamStartChunk() {
        std::ostringstream buffer;
        buffer << "HTTP/1.1 200 OK\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Content-Type: text/event-stream; charset=utf-8\r\n"
            << "Connection: close\r\n"
            << "Transfer-Encoding: chunked\r\n\r\n";
        std::string data = buffer.str();
        writeSocket(serverSocket, data.c_str(), data.size());
    }

    void writeStreamChunk(const std::string data) {
        std::ostringstream buffer;
        buffer << std::hex << data.size() << "\r\n" << data << "\r\n";
        std::string d = buffer.str();
        writeSocket(serverSocket, d.c_str(), d.size());
    }

    void writeStreamEndChunk() {
        const char *endChunk = "0000\r\n\r\n";
        writeSocket(serverSocket, endChunk, strlen(endChunk));
    }
};

struct Route {
    std::string path;
    HttpMethod method;
    std::function<void(HttpRequest&)> handler;
};

class Router {
public:
    static void resolve(HttpRequest& request, std::vector<Route>& routes) {
        if (request.method == HttpMethod::METHOD_OPTIONS) {
            request.writeCors();
            return;
        }
        for (const auto& route : routes) {
            if (request.method == route.method && request.path == route.path) {
                route.handler(request);
                return;
            }
        }
        request.writeNotFound();
    }
};

// ─── Chat completion helpers ──────────────────────────────────────────────

void writeChatCompletionChunk(HttpRequest &request, const std::string &delta, const bool stop){
    ChunkChoice choice;
    if (stop) {
        choice.finish_reason = "stop";
    } else {
        choice.delta = ChatMessageDelta("assistant", delta);
        choice.has_delta = true;
    }
    ChatCompletionChunk chunk = ChatCompletionChunk(choice);

    std::ostringstream buffer;
    buffer << "data: " << ((json)chunk).dump() << "\r\n\r\n";
    request.writeStreamChunk(buffer.str());

    if (stop) {
        request.writeStreamChunk("data: [DONE]");
        request.writeStreamEndChunk();
    }
}

static bool tryParseJsonObject(const std::string &text, json &out) {
    json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object())
        return false;
    out = std::move(parsed);
    return true;
}

static bool tryFindJsonFragmentAtEnd(const std::string &text, json &out) {
    if (tryParseJsonObject(text, out))
        return true;

    bool inString = false;
    int depth = 0;
    size_t end = std::string::npos;

    for (size_t i = text.size(); i-- > 0;) {
        char c = text[i];
        if (c == '"') {
            size_t backslashes = 0;
            for (size_t j = i; j > 0 && text[j - 1] == '\\'; j--)
                backslashes++;
            if ((backslashes % 2) == 0)
                inString = !inString;
            continue;
        }
        if (inString)
            continue;
        if (c == '}') {
            if (end == std::string::npos)
                end = i;
            depth++;
            continue;
        }
        if (c == '{') {
            if (depth == 0)
                continue;
            depth--;
            if (depth == 0 && end != std::string::npos) {
                std::string candidate = text.substr(i, end - i + 1);
                if (tryParseJsonObject(candidate, out))
                    return true;
                end = std::string::npos;
            }
        }
    }
    return false;
}

static std::string tryBuildToolsSystemPrompt(const std::vector<Tool> &tools, const ToolChoice &choice) {
    json toolJson = tools;
    std::string prompt = "You have access to the following tools:\n";
    prompt += toolJson.dump();
    prompt += "\n\nWhen you decide to call a tool, respond with a JSON object in this format:\n";
    prompt += "{\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"tool_name\",\"arguments\":\"{...}\"}}]}";
    if (choice.kind == TOOL_CHOICE_NONE)
        prompt += "\nDo not call any tools.";
    else if (choice.kind == TOOL_CHOICE_REQUIRED)
        prompt += "\nYou must call a tool.";
    else if (choice.kind == TOOL_CHOICE_NAMED)
        prompt += "\nYou must call the tool named: " + choice.tool_name + ".";
    return prompt;
}

class NaiveCacheItem {
public:
    pos_t endPos;
    ChatMessage message;
    NaiveCacheItem(pos_t endPos, ChatMessage message) {
        this->endPos = endPos;
        this->message = message;
    }
};

class NaiveCache {
private:
    std::vector<NaiveCacheItem> cache;
public:
    void push(NaiveCacheItem item) {
        cache.push_back(item);
    }

    void clear() {
        cache.clear();
    }

    bool resolveDeltaPrompt(std::vector<ChatMessage>& messages, pos_t& startPos) {
        size_t cacheSize = cache.size();
        if (cacheSize == 0)
            return false;
        if (messages.size() > cacheSize) {
            size_t i = 0;
            while (i < cacheSize) {
                if (
                    cache[i].message.role != messages[i].role ||
                    cache[i].message.content != messages[i].content
                ) break;
                i++;
            }
            if (i == cacheSize) {
                startPos = cache[i - 1].endPos;
                messages.erase(messages.begin(), messages.begin() + i);
                printf("🐤 Found naive cache for %zu messages, pos=%d\n", i, startPos);
                return true;
            }
        }
        cache.clear();
        return false;
    }
};

// ─── ApiServer ────────────────────────────────────────────────────────────

class ApiServer {
private:
    AppInferenceContext *context;
    EosDetector *eosDetector;
    ChatTemplateGenerator *templateGenerator;
    NaiveCache naiveCache;
    double *lastTokensPerSec;

public:
    ApiServer(AppInferenceContext *context, EosDetector *eosDetector, ChatTemplateGenerator *templateGenerator, double *lastTokensPerSec) {
        this->context = context;
        this->eosDetector = eosDetector;
        this->templateGenerator = templateGenerator;
        this->lastTokensPerSec = lastTokensPerSec;
    }

    void complete(HttpRequest& request) {
        InferenceParams params = parseRequest(request);

        pos_t startPos = 0;
        std::vector<ChatMessage> deltaPrompt = params.messages;
        naiveCache.resolveDeltaPrompt(deltaPrompt, startPos);

        size_t nInputItems = deltaPrompt.size();
        std::unique_ptr<ChatItem[]> inputItemsPtr(new ChatItem[nInputItems]);
        ChatItem *inputItems = inputItemsPtr.get();
        for (size_t i = 0; i < nInputItems; i++) {
            inputItems[i].role = deltaPrompt[i].role;
            inputItems[i].message = deltaPrompt[i].content;
        }

        GeneratedChat inputPrompt = templateGenerator->generate(nInputItems, inputItems, true);
        printf("🔹\033[34m%s", inputPrompt.content);
        fflush(stdout);

        int nPromptTokens;
        std::unique_ptr<int[]> promptTokensPtr(new int[inputPrompt.length + 2]);
        int *promptTokens = promptTokensPtr.get();
        bool isStart = startPos == 0;
        context->tokenizer->encode((char*)inputPrompt.content, promptTokens, &nPromptTokens, isStart, true);

        pos_t promptEndPos = startPos + nPromptTokens - 1;
        if (promptEndPos > context->header->seqLen)
            promptEndPos = context->header->seqLen;

        pos_t maxPredPos = params.max_tokens > 0 ? (promptEndPos + params.max_tokens) : context->header->seqLen;
        if (maxPredPos > context->header->seqLen)
            maxPredPos = context->header->seqLen;

        for (size_t j = 0; j < deltaPrompt.size(); j++) {
            naiveCache.push(NaiveCacheItem(promptEndPos, deltaPrompt[j]));
        }

        std::string buffer;

        if (params.stream)
            request.writeStreamStartChunk();
        if (inputPrompt.publicPrompt != nullptr) {
            if (params.stream)
                writeChatCompletionChunk(request, inputPrompt.publicPrompt, false);
            buffer += inputPrompt.publicPrompt;
        }

        printf("(%d tokens)\033\n[0m🔸", promptEndPos - startPos);
        fflush(stdout);

        NnUint pos = startPos;
        int token;
        for (NnUint i = 0; ;) {
            long remainingTokens = promptEndPos - pos;
            if (remainingTokens <= 0)
                break;

            NnUint batchSize = remainingTokens < context->args->nBatches
                ? remainingTokens
                : context->args->nBatches;

            context->inference->setBatchSize(batchSize);
            context->inference->setPosition(pos);
            for (NnUint j = 0; j < batchSize; j++)
                context->inference->setToken(j, promptTokens[i + j]);

            context->inference->forward();

            i += batchSize;
            pos += batchSize;
            token = promptTokens[i]; // last prompt token seeds first generation step
        }

        context->inference->setBatchSize(1);
        context->tokenizer->resetDecoder();
        eosDetector->reset();

        auto genStart = std::chrono::steady_clock::now();

        for (; pos < maxPredPos;) {
            context->inference->setPosition(pos);
            context->inference->setToken(0, token);
            context->inference->forward();

            token = context->sampler->sample(context->inference->logitsPipe);

            char *piece = context->tokenizer->decode(token);
            EosDetectorType eosType = eosDetector->append(token, piece);

            if (piece != nullptr) {
                printf("%s", piece);
                fflush(stdout);
            }

            if (eosType == NOT_EOS || eosType == EOS) {
                char *delta = eosDetector->getDelta();
                if (delta != nullptr) {
                    std::string deltaStr(delta);
                    if (params.stream)
                        writeChatCompletionChunk(request, deltaStr, false);
                    buffer += deltaStr;
                }
                eosDetector->reset();
            }
            pos++;
            if (eosType == EOS) break;
        }

        int nCompletionTokens = pos - promptEndPos;

        // Track tokens/s for the dashboard
        double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - genStart).count();
        if (elapsed > 0.0 && nCompletionTokens > 0)
            *lastTokensPerSec = nCompletionTokens / elapsed;

        // Accumulate points for all nodes
        if (context->workers != nullptr && context->nWorkerInfos > 0) {
            uint64_t totalTokens = (uint64_t)(nPromptTokens + nCompletionTokens);
            for (NnUint i = 0; i < context->nWorkerInfos; i++) {
                context->workers[i].points += totalTokens;
                context->workers[i].tokensGenerated += totalTokens;
            }
            if (context->pointsFile != nullptr)
                savePoints(context->pointsFile, context->workers, context->nWorkerInfos);
        }

        ChatMessage reply("assistant", buffer);
        if (pos == context->header->seqLen) {
            naiveCache.clear();
        } else {
            naiveCache.push(NaiveCacheItem(pos, reply));
        }

        if (params.stream) {
            writeChatCompletionChunk(request, "", true);
        } else {
            Choice choice(reply);

            if (!params.tools.empty()) {
                std::vector<ToolCall> parsedToolCalls;
                json tempJson;
                if (tryFindJsonFragmentAtEnd(buffer, tempJson) &&
                    tryParseToolCallsFromJson(tempJson, parsedToolCalls)) {
                    choice.message.tool_calls = parsedToolCalls;
                    choice.finish_reason = "tool_calls";
                }
            }

            ChatUsage usage(nPromptTokens, nCompletionTokens, nPromptTokens + nCompletionTokens);
            ChatCompletion completion(choice, usage);
            std::string chatJson = ((json)completion).dump();
            request.writeJson(chatJson);
        }
        printf("🔶\n");
        fflush(stdout);
    }

private:
    InferenceParams parseRequest(HttpRequest& request) {
        InferenceParams params = parseInferenceParams(
            request.parsedJson,
            context->args->temperature,
            context->args->topp,
            context->args->seed);
        context->sampler->setSeed(params.seed);
        if (!params.tools.empty()) {
            std::string prompt = tryBuildToolsSystemPrompt(params.tools, params.tool_choice);
            ChatMessage toolMessage("system", prompt);
            params.messages.insert(params.messages.begin(), toolMessage);
        }
        return params;
    }
};

// ─── Route handlers ───────────────────────────────────────────────────────

void handleCompletionsRequest(HttpRequest& request, ApiServer *api) {
    api->complete(request);
}

void handleModelsRequest(HttpRequest& request, const char* modelPath) {
    std::string path(modelPath);
    size_t pos = path.find_last_of("/\\");
    std::string modelName = (pos == std::string::npos) ? path : path.substr(pos + 1);

    Model model(modelName);
    ModelList list(model);
    std::string response = ((json)list).dump();
    request.writeJson(response);
}

void handleWorkersRequest(HttpRequest& request, AppInferenceContext *context, double tokensPerSec) {
    std::ostringstream j;
    j << "{\"tokensPerSec\":" << tokensPerSec << ",\"nodes\":[";
    for (NnUint i = 0; i < context->nWorkerInfos; i++) {
        if (i > 0) j << ",";
        const WorkerRuntimeInfo &w = context->workers[i];
        j << "{"
          << "\"nodeIndex\":" << w.nodeIndex << ","
          << "\"hostname\":\"" << w.hostname << "\","
          << "\"cpuCores\":" << w.cpuCores << ","
          << "\"cpuMhz\":" << w.cpuMhz << ","
          << "\"totalMemoryMb\":" << w.totalMemoryMb << ","
          << "\"points\":" << w.points << ","
          << "\"tokensGenerated\":" << w.tokensGenerated
          << "}";
    }
    j << "]}";
    request.writeJson(j.str());
}

// ─── Main server callback ─────────────────────────────────────────────────

static void server(AppInferenceContext *context) {
    NnSocket serverSocket(createServerSocket(context->args->host, context->args->port));

    TokenizerChatStops stops(context->tokenizer);
    ChatTemplateGenerator templateGenerator(context->args->chatTemplateType, context->tokenizer->chatTemplate, stops.stops[0]);
    EosDetector eosDetector(stops.nStops, context->tokenizer->eosTokenIds.data(), stops.stops, stops.maxStopLength, stops.maxStopLength);

    double lastTokensPerSec = 0.0;

    if (context->pointsFile != nullptr)
        loadPoints(context->pointsFile, context->workers, context->nWorkerInfos);

    ApiServer api(context, &eosDetector, &templateGenerator, &lastTokensPerSec);

    if (strcmp(context->args->host, "0.0.0.0") == 0 ||
        strcmp(context->args->host, "127.0.0.1") == 0)
        printf("Server URL: http://localhost:%d/\n", context->args->port);

    std::vector<Route> routes = {
        {
            "/",
            HttpMethod::METHOD_GET,
            [](HttpRequest& req) { req.writeHtml(CHAT_HTML, strlen(CHAT_HTML)); }
        },
        {
            "/v1/chat/completions",
            HttpMethod::METHOD_POST,
            std::bind(&handleCompletionsRequest, std::placeholders::_1, &api)
        },
        {
            "/v1/models",
            HttpMethod::METHOD_GET,
            std::bind(&handleModelsRequest, std::placeholders::_1, context->args->modelPath)
        },
        {
            "/v1/workers",
            HttpMethod::METHOD_GET,
            [context, &lastTokensPerSec](HttpRequest& req) {
                handleWorkersRequest(req, context, lastTokensPerSec);
            }
        }
    };

    while (true) {
        try {
            NnSocket clientSocket(acceptSocket(serverSocket.fd));
            HttpRequest request = HttpRequest::read(clientSocket.fd);
            printf("🔷 %s %s\n", request.getMethod().c_str(), request.path.c_str());
            Router::resolve(request, routes);
        } catch (const NnTransferSocketException& e) {
            printf("Socket error: %d %s\n", e.code, e.what());
        } catch (const NnExecutorException &e) {
            throw;
        }
    }
}

// ─── Entry point ──────────────────────────────────────────────────────────

#ifdef _WIN32
    #define EXECUTABLE_NAME "dllama-api.exe"
#else
    #define EXECUTABLE_NAME "dllama-api"
#endif

void usage() {
    fprintf(stderr, "Usage: %s {--model <path>} {--tokenizer <path>} [--host <addr>] [--port <p>]\n", EXECUTABLE_NAME);
    fprintf(stderr, "        [--buffer-float-type {f32|f16|q40|q80}]\n");
    fprintf(stderr, "        [--weights-float-type {f32|f16|q40|q80}]\n");
    fprintf(stderr, "        [--max-seq-len <max>]\n");
    fprintf(stderr, "        [--nthreads <n>]\n");
    fprintf(stderr, "        [--workers <ip:port> ...]\n");
    fprintf(stderr, "        [--min-workers <n>]\n");
    fprintf(stderr, "        [--points-file <path>]\n");
    fprintf(stderr, "        [--temperature <temp>]\n");
    fprintf(stderr, "        [--topp <t>]\n");
    fprintf(stderr, "        [--seed <s>]\n");
    fprintf(stderr, "Example:\n");
    fprintf(stderr, "  sudo nice -n -20 ./dllama-api --port 9990 --nthreads 4 \\\n");
    fprintf(stderr, "    --model dllama_model_llama3_2_3b_instruct_q40.m \\\n");
    fprintf(stderr, "    --tokenizer dllama_tokenizer_llama3_2_3b_instruct_q40.t \\\n");
    fprintf(stderr, "    --buffer-float-type q80 --max-seq-len 8192 \\\n");
    fprintf(stderr, "    --points-file /tmp/dllama_points.json\n");
    fflush(stderr);
}

int main(int argc, char *argv[]) {
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif

    AppCliArgs args = AppCliArgs::parse(argc, argv, false);
    if (args.help) {
        usage();
        return EXIT_SUCCESS;
    }

    initQuants();
    initSockets();

    while (true) {
        try {
            runInferenceApp(&args, server);
        } catch (const NnConnectionSocketException &e) {
            printf("🚨 Connection error: %s\n", e.what());
        } catch (const NnExecutorException &e) {
            printf("🚨 Inference error: %s\n", e.what());
        }

        printf("🔄 Retrying in 3 seconds...\n");
        std::this_thread::sleep_for(std::chrono::seconds(3));
        args.info = false;
    }

    cleanupSockets();
    return EXIT_SUCCESS;
}
