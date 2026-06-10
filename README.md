# AiFace

Talk to an LLM from your Pebble watch. Press **Select**, speak, and your words
are transcribed on-device, sent through the phone to
[OpenRouter](https://openrouter.ai), and the reply is rendered back on the
watch. Hold **Select** to clear the conversation.

Works on modern Pebble hardware — Pebble 2 / Core 2 Duo (`diorite`), Pebble Time
2 (`emery`), Pebble Round 2 (`gabbro`) — plus `basalt` and `flint`.

## Setup

1. Get an API key at <https://openrouter.ai/keys>.
2. Build and install (see below).
3. Open the app's **Settings** in the Pebble phone app, paste your key, and
   pick a model (defaults to `anthropic/claude-haiku-4.5` — any
   [OpenRouter model id](https://openrouter.ai/models) works).

The key is stored on your phone, never in the code.

## Building & running

```sh
npm install                           # installs pebble-clay (+ platform patch)
pebble build                          # build for all target platforms
pebble install --emulator basalt      # run in the emulator
pebble install --phone <ip>           # install to a paired phone (Developer Connection)
```

In the emulator, dictation lets you *type* the prompt, so the full round trip is
testable without a watch. Use `pebble emu-app-config --emulator basalt` to open
the settings page in your browser.

## How it works

```
src/c/main.c        Watch UI: dictation, scrolling conversation, chunk reassembly
src/pkjs/index.js   Phone side: calls OpenRouter, chunks the reply, keeps context
src/pkjs/config.js  Clay settings page (API key + model)
scripts/patch-clay.js  Build fix so pebble-clay supports flint/gabbro platforms
```

The watch and phone exchange data over AppMessage. Replies are split into
~180-byte chunks (AppMessage has a small inbox) and reassembled on the watch.
The phone side keeps the last 20 messages so follow-up questions have context.

## Documentation

Pebble SDK docs and tutorials: <https://developer.repebble.com>
