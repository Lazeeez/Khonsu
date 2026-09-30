"""A customer-support agent: reads the customer's message, looks up the order, checks the
refund policy, issues the refund if the policy allows it, emails the customer, and streams
a final reply. Real function calling against any OpenAI-compatible model.

The tools are wrapped with @khonsu.tool, so under `khonsu replay` the payment and the email
are not repeated: their recorded results come back instead.
"""

import json
import os
from datetime import date

import khonsu
from openai import OpenAI

MODEL = os.environ.get("MODEL", "qwen2.5:3b")
OUTBOX = os.environ.get("OUTBOX", "../outbox.log")  # the "outside world": payments and emails


@khonsu.tool
def lookup_order(order_id: str) -> dict:
    """Read an order from the shop's database (orders.json in the workspace)."""
    orders = json.load(open("orders.json"))
    return orders.get(order_id.strip().lstrip("#"), {"error": f"no order {order_id}"})


@khonsu.tool
def get_refund_policy() -> str:
    return open("refund_policy.md").read()


@khonsu.tool
def issue_refund(order_id: str, amount: float, reason: str) -> dict:
    """Talks to the payment provider. A real side effect: it must never run twice."""
    with open(OUTBOX, "a") as f:
        f.write(f"REFUND {order_id} {amount:.2f} ({reason})\n")
    return {"status": "refunded", "order_id": order_id, "amount": round(float(amount), 2)}


@khonsu.tool
def send_email(to: str, subject: str, body: str) -> str:
    with open(OUTBOX, "a") as f:
        f.write(f"EMAIL {to}: {subject}\n")
    return "sent"


TOOLS = [
    {"type": "function", "function": {"name": "lookup_order", "description": "Look up an order by its id",
     "parameters": {"type": "object", "properties": {"order_id": {"type": "string"}}, "required": ["order_id"]}}},
    {"type": "function", "function": {"name": "get_refund_policy", "description": "Read the shop's refund policy",
     "parameters": {"type": "object", "properties": {}}}},
    {"type": "function", "function": {"name": "issue_refund", "description": "Refund an order through the payment provider",
     "parameters": {"type": "object", "properties": {"order_id": {"type": "string"}, "amount": {"type": "number"},
                                                     "reason": {"type": "string"}},
                    "required": ["order_id", "amount", "reason"]}}},
    {"type": "function", "function": {"name": "send_email", "description": "Email the customer",
     "parameters": {"type": "object", "properties": {"to": {"type": "string"}, "subject": {"type": "string"},
                                                     "body": {"type": "string"}},
                    "required": ["to", "subject", "body"]}}},
]
IMPL = {"lookup_order": lookup_order, "get_refund_policy": get_refund_policy,
        "issue_refund": issue_refund, "send_email": send_email}

client = OpenAI(max_retries=0, timeout=300)
messages = [
    {"role": "system", "content": (
        f"You are the support agent for Moonlight Supply. Today is {date(2026, 9, 30).isoformat()}. "
        "Always look up the order and read the refund policy before deciding. If the policy allows a refund, "
        "issue it and email the customer a confirmation. If it does not, email them explaining why. "
        "Use the tools; do not invent order details.")},
    {"role": "user", "content": open("ticket.txt").read()},
]

for _ in range(8):
    r = client.chat.completions.create(model=MODEL, messages=messages, tools=TOOLS, temperature=0.7)
    msg = r.choices[0].message
    messages.append(msg.model_dump(exclude_none=True))
    if not msg.tool_calls:
        break
    for call in msg.tool_calls:
        try:
            args = json.loads(call.function.arguments or "{}")
        except json.JSONDecodeError:
            args = {}
        fn = IMPL.get(call.function.name)
        try:
            out = fn(**args) if fn else {"error": f"unknown tool {call.function.name}"}
        except (TypeError, ValueError, KeyError) as e:
            out = {"error": f"bad arguments: {e}"}
        messages.append({"role": "tool", "tool_call_id": call.id, "content": json.dumps(out)})
        print(f"tool  {call.function.name}({json.dumps(args)}) -> {json.dumps(out)[:120]}")

# The customer-facing reply, streamed token by token.
print("reply ", end="", flush=True)
stream = client.chat.completions.create(model=MODEL, temperature=0.7, stream=True, messages=messages + [
    {"role": "user", "content": "Write the short reply we show the customer in the chat window."}])
for chunk in stream:
    if chunk.choices and chunk.choices[0].delta.content:
        print(chunk.choices[0].delta.content, end="", flush=True)
print()
