# How to Run AIClusterMiner on 💻 Linux, MacOS or Windows

This article describes how to run Distributed Llama on 4 devices, but you can also run it on 1, 2, 4, 8... devices. Please adjust the commands and topology according to your configuration.

````
[🔀 SWITCH OR ROUTER]
      | | | |
      | | | |_______ 🔸 device1 (ROOT)     10.0.0.1
      | | |_________ 🔹 device2 (WORKER 1) 10.0.0.2:9999
      | |___________ 🔹 device3 (WORKER 2) 10.0.0.3:9999
      |_____________ 🔹 device4 (WORKER 3) 10.0.0.4:9999
````

1. Install Git and C++ compiler on **🔸🔹 ALL** devices:

  * Linux: 
    ```
    sudo apt install git build-essential
    ```
  * MacOS
    ```
    brew install git
    ```
  * Windows

    Install Git and Mingw (via [Chocolatey](https://chocolatey.org/install)):
    ```powershell
    choco install git mingw
    ```

2. Connect **🔸🔹 ALL** devices to your **🔀 SWITCH OR ROUTER** via Ethernet cable. If you're using only two devices, it's better to connect them directly without a switch.

3. Clone this repository and compile AIClusterMiner on **🔸🔹 ALL** devices:

```sh
git clone https://github.com/Luispessoa18/AIClusterMiner.git
cd AIClusterMiner
make dllama
make dllama-api
```

4. Download the model to the **🔸 ROOT** device using the `launch.py` script.

```sh
python3 launch.py # Prints a list of available models

python3 launch.py llama3_2_3b_instruct_q40 # Downloads the model to the root device
```

5. Set up workers on all **🔹 WORKER** devices.

There are two approaches: using the helper script (`worker_launch.py`) or running `dllama` directly.

---

### Option A — Using `worker_launch.py` (recommended)

`worker_launch.py` downloads the model, pre-caches your weight slice locally, and starts the worker automatically.

**A1 — With local weight pre-cache (fastest start after first run)**

Run once on each worker. Replace `--node-index` with this worker's number (1, 2, 3…) and `--num-nodes` with the total number of machines (root + all workers).

```sh
python3 worker_launch.py llama3_2_3b_instruct_q40 \
  --node-index 1 \
  --num-nodes 2 \
  --server 10.0.0.1 \
  --port 9999
```

The script will:
1. Download the model to `models/llama3_2_3b_instruct_q40/` (skips if already present)
2. Run `prepare-worker` to extract this node's weight slice into a local cache
3. Start the worker — the root will detect the cache and skip weight transfer entirely

On the second run the model is already downloaded and the cache already exists, so startup is instant.

**A2 — Without pre-cache (auto-discovers root, receives weights from server)**

```sh
python3 worker_launch.py llama3_2_3b_instruct_q40 --server 10.0.0.1
```

---

### Option B — Running `dllama` directly

**B1 — Pre-cache weight slice manually (run once per worker before first connect)**

This extracts the worker's slice from a locally downloaded model so it never needs to receive weights over the network.

```sh
# First, download the model on the worker device:
python3 launch.py llama3_2_3b_instruct_q40 -skip-run -skip-script

# Then generate the local weight cache for this node:
./dllama prepare-worker \
  --model models/llama3_2_3b_instruct_q40/dllama_model_llama3_2_3b_instruct_q40.m \
  --cache-dir models/llama3_2_3b_instruct_q40 \
  --node-index 1 \
  --num-nodes 2 \
  --nthreads 4
```

Replace `--node-index` with this worker's index (1-based) and `--num-nodes` with total nodes (root + all workers). Run `prepare-worker` once per worker; re-run only if the model or cluster size changes.

**B2 — Start the worker (after prepare-worker, or to receive weights from root)**

Static mode (root will list this worker's IP explicitly):

```sh
./dllama worker \
  --port 9999 \
  --nthreads 4 \
  --cache-dir models/llama3_2_3b_instruct_q40
```

Discovery mode (worker self-registers with root at boot):

```sh
./dllama worker \
  --port 9999 \
  --nthreads 4 \
  --server 10.0.0.1:9990 \
  --cache-dir models/llama3_2_3b_instruct_q40
```

With `--server`, the worker self-registers with the root node. Use the root's `--worker-port` value here when the API server uses a separate worker registration port. With `--cache-dir`, if the local cache is present (created by `prepare-worker`) the root skips weight transfer entirely; without a pre-existing cache, weights are transferred on first connect and saved for subsequent runs.

---

6. Run the inference to test if everything works fine on the **🔸 ROOT** device:

```sh
./dllama inference \
  --prompt "Hello world" \
  --steps 32 \
  --model models/llama3_2_3b_instruct_q40/dllama_model_llama3_2_3b_instruct_q40.m \
  --tokenizer models/llama3_2_3b_instruct_q40/dllama_tokenizer_llama3_2_3b_instruct_q40.t \
  --buffer-float-type q80 \
  --nthreads 4 \
  --max-seq-len 4096 \
  --workers 10.0.0.2:9999 10.0.0.3:9999 10.0.0.4:9999
```

7. To run the API server, start it on the **🔸 ROOT** device.

**Option A — Static workers** (explicit IP list):

```sh
./dllama-api \
  --port 9990 \
  --model models/llama3_2_3b_instruct_q40/dllama_model_llama3_2_3b_instruct_q40.m \
  --tokenizer models/llama3_2_3b_instruct_q40/dllama_tokenizer_llama3_2_3b_instruct_q40.t \
  --buffer-float-type q80 \
  --nthreads 4 \
  --max-seq-len 4096 \
  --workers 10.0.0.2:9999 10.0.0.3:9999 10.0.0.4:9999
```

**Option B — Dynamic discovery** (workers self-register, no IPs needed):

```sh
./dllama-api \
  --port 9990 \
  --worker-port 9991 \
  --model models/llama3_2_3b_instruct_q40/dllama_model_llama3_2_3b_instruct_q40.m \
  --tokenizer models/llama3_2_3b_instruct_q40/dllama_tokenizer_llama3_2_3b_instruct_q40.t \
  --buffer-float-type q80 \
  --nthreads 4 \
  --max-seq-len 4096 \
  --min-workers 3 \
  --points-file /tmp/dllama_points.json
```

The server waits until the specified number of workers connect, then starts inference. `--port` serves the web UI/API and `--worker-port` receives worker discovery registrations. If a worker disconnects it automatically waits for reconnection and reshards.

Now you can connect to the API server:

```
http://10.0.0.1:9990/v1/models
```

8. When the API server is running, you can open the web chat in your browser, open [llama-ui.js.org](https://llama-ui.js.org/), go to the settings and set the base URL to: `http://10.0.0.1:9990`. Press the "save" button and start chatting!
