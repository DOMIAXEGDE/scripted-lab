/*---DOC---
{
  "object": "demo.echo_cpp",
  "language": "cpp",
  "summary": "Echoes a JSON object with an added timestamp.",
  "entry": "stdio-json",
  "main": "main",
  "timeout_ms": 2000
}
---END---*/
#include <bits/stdc++.h>
using namespace std;
int main(){
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    string in((istreambuf_iterator<char>(cin)), istreambuf_iterator<char>());
    // very tiny json injection: append a field if it's an object
    // (functionality-first, not a full json lib)
    if (!in.empty() && in.find('{')!=string::npos){
        if (in.find('}')!=string::npos){
            // insert before last }
            auto pos = in.rfind('}');
            string extra = string(",\"echoed_by\":\"cpp\",\"ts\":") + to_string(time(nullptr)) + "}";
            in.replace(pos, 1, extra);
        }
    }
    cout<<in;
    return 0;
}
