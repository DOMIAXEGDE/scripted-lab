/*---DOC---
{
  "object": "demo.echo_java",
  "language": "java",
  "summary": "Echoes a JSON object with an added timestamp.",
  "entry": "stdio-json",
  "main": "Main",
  "timeout_ms": 2000
}
---END---*/
import java.io.*;
public class Main {
    public static void main(String[] args) throws Exception {
        StringBuilder sb = new StringBuilder();
        try (BufferedReader br = new BufferedReader(new InputStreamReader(System.in))) {
            String line;
            while ((line = br.readLine()) != null) sb.append(line);
        }
        String in = sb.toString();
        int r = in.lastIndexOf('}');
        if (r >= 0) {
            long ts = System.currentTimeMillis()/1000;
            String extra = ",\"echoed_by\":\"java\",\"ts\":" + ts + "}";
            in = in.substring(0, r) + extra;
        }
        System.out.print(in);
    }
}
