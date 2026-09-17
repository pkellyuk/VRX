import re, html

t = open(r'C:\Users\paulj\dev\VRX\bench\dml_docs.html', encoding='utf-8').read()
t = re.sub(r'<(script|style)[^>]*>.*?</\1>', '', t, flags=re.S)

for kw in ['D3D11', 'GetDevice', 'OrtDmlApi', 'GetExecutionProviderApi', 'interop', 'OrtValue']:
    hits = list(re.finditer(kw, t))
    print(f'===== {kw}: {len(hits)} hits =====')
    seen = set()
    for m in hits[:12]:
        s = max(0, m.start() - 250)
        e = min(len(t), m.end() + 250)
        snippet = re.sub(r'<[^>]+>', ' ', t[s:e])
        snippet = html.unescape(re.sub(r'\s+', ' ', snippet)).strip()
        key = snippet[:80]
        if key in seen:
            continue
        seen.add(key)
        print(f'--- @ {m.start()}: {snippet}\n')
