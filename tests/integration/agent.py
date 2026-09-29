"""A small tool-using agent written against the official OpenAI Python SDK.

It knows nothing about Khonsu: it just reads OPENAI_BASE_URL like any agent.
Env knobs used by the tests:
  AGENT_QUESTION  change the user prompt (to provoke a divergence)
  AGENT_PARALLEL  also fire N concurrent requests (exercises concurrent matching)
"""

import json
import os
from concurrent.futures import ThreadPoolExecutor

from openai import OpenAI

client = OpenAI(max_retries=0)
MODEL = "mock-1"
QUESTION = os.environ.get("AGENT_QUESTION", "What is the weather in the city of the moon god?")

TOOLS = [{"type": "function", "function": {
    "name": "get_weather", "description": "Weather for a city",
    "parameters": {"type": "object", "properties": {"city": {"type": "string"}}, "required": ["city"]}}}]


def get_weather(city: str) -> str:  # a deterministic local tool
    # AGENT_TOOL_NOTE simulates a tool whose output changed since the recording.
    return f"{city}: {20 + len(city)}°C, clear skies{os.environ.get('AGENT_TOOL_NOTE', '')}"


messages = [{"role": "system", "content": "You are a helpful agent."}, {"role": "user", "content": QUESTION}]

# Step 1: the model asks for a tool.
r1 = client.chat.completions.create(model=MODEL, messages=messages, tools=TOOLS, temperature=0.8)
call = r1.choices[0].message.tool_calls[0]
args = json.loads(call.function.arguments)
print(f"step1 tool_call {call.function.name}({args['city']})")
messages.append(r1.choices[0].message.model_dump(exclude_none=True))
messages.append({"role": "tool", "tool_call_id": call.id, "content": get_weather(args["city"])})

# Step 2: streamed final answer.
stream = client.chat.completions.create(model=MODEL, messages=messages, stream=True,
                                        stream_options={"include_usage": True})
answer = "".join((c.choices[0].delta.content or "") for c in stream if c.choices)
print(f"step2 answer {answer}")

# Step 3 (optional): concurrent side requests.
n = int(os.environ.get("AGENT_PARALLEL", "0"))
if n:
    def side(i):
        r = client.chat.completions.create(model=MODEL, messages=[{"role": "user", "content": f"side task {i}"}])
        return f"side{i} {r.choices[0].message.content}"
    with ThreadPoolExecutor(max_workers=n) as pool:
        for line in pool.map(side, range(n)):
            print(line)
