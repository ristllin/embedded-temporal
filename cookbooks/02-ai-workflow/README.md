# Cookbook 02 — Chaining and branching

**What you'll learn:** in [chapter 01](../01-hello-world/) a workflow ran one
activity and finished. Here it runs several, and picks between them: one
activity's output feeds a conditional, the conditional chooses one of two
branches, and each branch calls a real Mistral model. This is where the spec
starts doing orchestration — routing data from step to step.

The workflow classifies a piece of text, then replies to it differently
depending on the label:

```
ai.classify  →  conditional (label == "positive"?)  →  ai.chat  →  complete
                        true  → warm thank-you
                        false → empathetic apology
```

## The spec

[`spec.json`](spec.json) has three steps:

```json
{
  "name": "cookbook-02-ai-workflow",
  "steps": [
    { "type": "activity", "name": "ai.classify", "id": "classify", "args_from": "/input/classify" },
    {
      "type": "conditional",
      "predicate": { "path": "/results/classify/label", "op": "eq", "value": "positive" },
      "true_steps": [
        { "type": "activity", "name": "ai.chat", "id": "reply", "args_from": "/input/positive" }
      ],
      "false_steps": [
        { "type": "activity", "name": "ai.chat", "id": "reply", "args_from": "/input/negative" }
      ]
    },
    { "type": "complete", "result_from": "/results" }
  ]
}
```

Step by step:

| Step | What it does |
|---|---|
| `ai.classify` | Runs the `ai.classify` activity with the argument at `/input/classify`. Its result binds under `id: "classify"`, so it becomes `/results/classify`. |
| `conditional` | Reads `/results/classify/label` and compares it to `"positive"`. Equal → walk `true_steps`; anything else → walk `false_steps`. |
| `ai.chat` (either branch) | Runs `ai.chat` with a branch-specific argument (`/input/positive` or `/input/negative`). **Both branches bind under the same `id: "reply"`** — only one runs, so `/results/reply` holds whichever branch was taken. |
| `complete` | Returns the whole `/results` map, so the output carries both `classify` and `reply`. |

Sharing one `id` across both branches is deliberate and legal: the grammar
allows it precisely so a later step can read `/results/reply` without caring
which branch produced it. See the grammar reference in
[`core/include/mwf_core/workflow_spec.h`](../../core/include/mwf_core/workflow_spec.h).

## How data flows

The engine carries one JSON document as it runs — `input` (the trigger's input)
plus a `results` map that each activity appends to. Every `args_from`,
`result_from`, and predicate `path` is a JSON pointer into it.

This spec reads from three sub-objects of the input, so the trigger input is
structured to match (`run_local.sh` builds it):

```json
{
  "text": "I absolutely love this little device. …",
  "classify": {
    "text": "I absolutely love this little device. …",
    "labels": ["positive", "negative", "neutral"]
  },
  "positive": {
    "system": "You are a warm, concise customer-support agent.",
    "prompt": "A customer left this positive review: \"…\". Write a genuine 2-sentence thank-you …"
  },
  "negative": {
    "system": "You are an empathetic, concise customer-support agent.",
    "prompt": "A customer left this critical review: \"…\". Write a brief, 2-sentence empathetic reply …"
  }
}
```

The branch prompts live in the input, not the spec — the spec just points a
branch at `/input/positive` or `/input/negative`. Walking the document through
a run:

1. **Start.** `{ "input": { …the object above… } }`
2. **After `ai.classify`.** The activity read `/input/classify` and returned a
   label, which binds under its id:
   `results.classify = { "label": "positive" }`
3. **Conditional.** The predicate resolves `/results/classify/label` →
   `"positive"`, compares `eq "positive"` → true, so it walks `true_steps`.
4. **After `ai.chat`.** That branch read `/input/positive`, called the model,
   and bound the reply: `results.reply = { "text": "Thank you so much …" }`
5. **Complete.** Returns `/results`:
   `{ "classify": { "label": "positive" }, "reply": { "text": "…" } }`

The one non-obvious point: the conditional is **binary** — `label == "positive"`
is true or false. The classifier can return `neutral` (it's in the `labels`
list), but `neutral` is not `"positive"`, so it takes the `false_steps`
(empathetic) branch. If you want three distinct replies, you need a second
conditional — see [Make it your own](#make-it-your-own).

## The activities

Both activities are built into the desktop worker
([`transport/example/cookbook_worker_main.cpp`](../../transport/example/cookbook_worker_main.cpp)).
Each is a plain `mwf::Result<mwf::Bytes>(const mwf::Bytes&)` registered by name,
and both call the Mistral chat API (`POST https://api.mistral.ai/v1/chat/completions`,
model `mistral-small-latest`, `temperature: 0`).

**`ai.classify`** — reads `text` and an optional `labels` array from its
argument, tells the model to answer with exactly one lowercase word, reduces the
reply to a single token, and returns `{ "label": "<word>" }`:

```cpp
registry.registerActivity("ai.classify", [apiKey, model](const Bytes& arg) {
  json a = parseArg(arg);
  std::string text = argText(a);                 // reads a["text"]
  // ...builds a "answer with EXACTLY ONE lowercase word, chosen from <labels>" prompt...
  std::string reply = mistralChat(apiKey, model, system, text, err);
  json out; out["label"] = toLabel(reply);        // reduce to one token
  return mwf::Result<Bytes>::success(toBytes(out.dump()));
});
```

**`ai.chat`** — reads `prompt` (and optional `system`) from its argument, sends
them to the model, and returns `{ "text": "<reply>" }`:

```cpp
registry.registerActivity("ai.chat", [apiKey, model](const Bytes& arg) {
  json a = parseArg(arg);
  std::string prompt = argText(a);                // reads a["prompt"]
  std::string reply = mistralChat(apiKey, model, argSystem(a), prompt, err);
  json out; out["text"] = reply;
  return mwf::Result<Bytes>::success(toBytes(out.dump()));
});
```

That's the whole pattern for "an activity that calls an external service": parse
the JSON argument, do the I/O, return JSON bytes. The spec never knows an HTTP
call happened — it just sees `/results/classify` and `/results/reply` appear.

## Run it

```bash
./run_local.sh
```

`run_local.sh` builds `cookbook_worker` (if needed), registers a uniquely-named
workflow, and starts one worker set to service **two** completions. It then
triggers the workflow twice with two different inputs:

- a **glowing review** — classifies as `positive` → `true_steps` → warm thank-you
- a **critical review** — classifies as `negative` → `false_steps` → empathetic reply

Same spec, same activities, two inputs, two branches. Each run polls to
`COMPLETED` and its output carries the `classify` label and the model's `reply`
text. `MISTRAL_API_KEY` comes from your environment or a gitignored `.env`.

## Make it your own

- **Swap the classifier labels.** Change the `labels` array the trigger passes
  to `ai.classify` (in `run_local.sh`'s `make_input`) — e.g.
  `["bug", "feature-request", "praise"]`. The model is told to choose from
  exactly that set, and the label it returns is what the predicate compares.
- **Change the branch prompts.** The `positive`/`negative` objects in the input
  are just `{system, prompt}` for `ai.chat`. Rewrite them (or the interpolated
  review text) and the replies change — no spec edit needed.
- **Rewrite the predicate.** Point `path` at a different value or use a
  different `op` (`ne`, `lt`, `gt`, `exists`, …). The full predicate grammar is
  in [`workflow_spec.h`](../../core/include/mwf_core/workflow_spec.h).
- **Add a third branch.** Because the conditional is binary, three outcomes take
  two conditionals: nest a second `conditional` inside `false_steps` that tests
  `label == "neutral"`. Give each branch's `ai.chat` its own input sub-object.
- **Change the model.** Both activities use whatever `--model` the worker was
  started with (default `mistral-small-latest`). Add `--model mistral-large-latest`
  to the `--mode worker` line in `run_local.sh`.

## Next

[Chapter 03 — Reproducibility](../03-reproducibility/): the spec is data, not
code, so the same input replays to the same steps and the same result. That's
the Temporal determinism guarantee, made concrete.
