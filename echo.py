/*---DOC---
{
  "object": "demo.echo_python",
  "language": "python",
  "summary": "Echoes a JSON object with an added timestamp.",
  "entry": "stdio-json",
  "main": "main",
  "timeout_ms": 2000,
  "inputs": [{"name":"in","type":"json"}],
  "outputs":[{"name":"out","type":"json"}]
}
---END---*/
import sys, json, time

def main():
    data = json.load(sys.stdin)
    data["echoed_by"] = "python"
    data["ts"] = int(time.time())
    json.dump(data, sys.stdout)

if __name__ == "__main__":
    main()
