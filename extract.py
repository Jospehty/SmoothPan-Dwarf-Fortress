import json
import re
import os

log_path = r'C:\Users\Joseph\.gemini\antigravity\brain\04d6d7e2-aacf-46ca-85a2-7425860b0eff\.system_generated\logs\transcript.jsonl'
with open(log_path, 'r', encoding='utf-8') as f:
    for line in f:
        data = json.loads(line)
        if 'output' in data.get('content', ''):
            output = data['content']
            if 'Showing lines 1 to 157' in output and 'README.md' in output:
                with open('recovered_readme.txt', 'w', encoding='utf-8') as out:
                    out.write(output)
            if 'Showing lines 1 to 310' in output and 'ARCHITECTURE.md' in output:
                with open('recovered_architecture.txt', 'w', encoding='utf-8') as out:
                    out.write(output)
