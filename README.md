# Gemini Live Translate for OBS

[![Latest release](https://img.shields.io/github/v/release/plan12be/obs-live-translate-caption?sort=semver)](https://github.com/plan12be/obs-live-translate-caption/releases)
[![Release build](https://github.com/plan12be/obs-live-translate-caption/actions/workflows/release.yaml/badge.svg)](https://github.com/plan12be/obs-live-translate-caption/actions/workflows/release.yaml)
[![License: GPL v2](https://img.shields.io/badge/license-GPLv2-blue.svg)](LICENSE)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20macOS%20%7C%20Linux-lightgrey)

A native OBS Studio plugin (**Windows · macOS · Linux**) that translates a
microphone in real time with Google **Gemini**, in one of two output modes:

- **Translated captions** — speech-to-text with `gemini-3.5-transcribe-live`,
  per-sentence translation with `gemini-3.1-flash-lite`, written into an OBS
  text source you style yourself (subtitle overlay). This is the mode this
  repository was created for.
- **Translated speech** — speech-to-speech with the Gemini Live API
  (`gemini-3.5-live-translate-preview`), played back as a separate OBS audio
  source you can route to its own track.

This repository continues
[weisunglee/obs-live-translate](https://github.com/weisunglee/obs-live-translate)
(the original speech-to-speech plugin) as a standalone project and adds the
captions pipeline; the original project's history and GPLv2 license are kept.

> This plugin was built with significant assistance from AI (Claude). It is an
> independent project and is not affiliated with or endorsed by the OBS Project
> or Google. Bug reports and contributions are welcome.

## How it works

The plugin registers two OBS sources:

1. **Gemini Live Translate** *(audio filter)* — add it to your microphone source.
   It resamples the mic to 16 kHz mono 16-bit PCM, chunks it (100 ms /
   3200-byte chunks), and streams it **continuously** to Gemini over a TLS
   WebSocket — including the silence during pauses, which the model relies on to
   detect when an utterance ends and emit its translation promptly. Configure
   your **API key**, **target language**, **echo** option, and optional
   **playback delay** in its properties.

2. **Gemini Translated Audio** *(audio source)* — add it to your scene on its own
   audio track. It receives the 24 kHz translated PCM from Gemini and pushes it
   into the OBS mixer as it arrives.

```
mic ─▶ [Gemini Live Translate filter]
          resample 16 kHz mono → chunk → WebSocket ─▶ Gemini Live API
                                                          │ 24 kHz PCM
                                                          ▼
       [Gemini Translated Audio source] ◀── ring buffer ◀┘
          event-driven push loop → obs_source_output_audio → OBS mixer
```

A single shared `TranslationSession` owns the WebSocket connection (with
reconnect/backoff) and the input/output audio buffers. The output path pushes
every received PCM chunk to OBS with contiguous, duration-spaced timestamps,
paced so the scheduling lead stays bounded (~600 ms, enough to ride out the
model's phrase-boundary delivery jitter) — the OBS mixer is the clock.

### Captions mode

The filter's **Output** setting can be switched from *Translated speech* to
*Translated captions*. In that mode the same mic stream goes to
`gemini-3.5-transcribe-live` (Live API speech-to-text, SMART mode) instead;
every finalized sentence is translated with `gemini-3.1-flash-lite`
(generateContent, the previous three sentences as
context) and written into an OBS **text source** you pick — so the subtitles
use whatever font, outline and position you set on that source. A second,
optional text source can show the source-language transcript live (interim
text while you speak, replaced by the final sentence).

```
mic ─▶ [Gemini Live Translate filter, Output = Translated captions]
          resample 16 kHz mono → chunk → WebSocket ─▶ gemini-3.5-transcribe-live
                                                          │ interim / final text
                                                          ▼
       [Source Transcript text source] ◀── (optional) ◀──┤
                                                          │ final sentence
                                                          ▼
                                       generateContent ─▶ gemini-3.1-flash-lite
                                                          │ translation
                                                          ▼
       [Caption text source] ◀── CaptionComposer (in-order, N lines, hold timer)
```

Translations are shown in the order the sentences were spoken even when the
model answers out of order; a sentence whose translation fails is skipped
(logged) without blocking the next one. The caption source keeps the last
**Caption Lines** sentences and is cleared **Caption Hold** seconds after the
last one. The two modes are exclusive: in captions mode the *Gemini Translated
Audio* source stays silent.

## Getting a Gemini API key

The plugin needs a Google **Gemini API key**:

1. Sign in to **Google AI Studio** at <https://aistudio.google.com/apikey> with
   a Google account.
2. New users: AI Studio creates a default Google Cloud project and key
   automatically once you accept the Terms of Service. Otherwise click
   **Create API key**.
3. Copy the key and paste it into the **Gemini API Key** field of the *Gemini
   Live Translate* filter.

Notes:

- The key is stored **in plaintext** in your scene-collection file — don't share
  that file.
- Usage is billed per Google's pricing: `gemini-3.5-live-translate-preview`
  in speech mode, `gemini-3.5-transcribe-live` plus `gemini-3.1-flash-lite` in
  captions mode. Check current quotas and pricing in AI Studio
  ([pricing](https://ai.google.dev/gemini-api/docs/pricing)).

## Status

Builds for **Windows, macOS and Linux**. Prebuilt packages with captions mode
are published on this repository's
[Releases](https://github.com/plan12be/obs-live-translate-caption/releases) page
once a version tag is pushed; until then, build from source (see below). The
speech-only releases of the original project remain at
[weisunglee/obs-live-translate](https://github.com/weisunglee/obs-live-translate/releases).
Windows is the tested platform; see the note under *Install*. Current behavior:

- ✅ Mic → Gemini streaming (continuous, including pause silence), translated
  audio played back via the OBS mixer.
- ✅ **Captions mode** (v2): speech-to-text with `gemini-3.5-transcribe-live`,
  per-sentence translation with `gemini-3.1-flash-lite`, written into an OBS
  text source (plus an optional source-transcript text source). In-order
  display, per-sentence failure isolation, hold-to-clear, automatic reconnect
  before the Live API's 10-minute session cap. See
  [`docs/specs/001-caption-translation-pipeline.md`](docs/specs/001-caption-translation-pipeline.md).
- ✅ Reconnect with exponential backoff; live API-key / target-language changes.
- ✅ Event-driven push output with a bounded scheduling lead (~600 ms) to ride
  out the model's phrase-boundary delivery jitter.
- ✅ Optional **echo** toggle (default on): output speech even when it is already
  in the target language. With it off, input already in the target language
  stays silent.
- ✅ Optional **playback delay** (0-30 seconds): hold the translated audio stream
  so every translated phrase plays later by the configured amount.
- ✅ Sentence endings play in full — streaming the mic continuously (silence
  included) lets the model detect when an utterance ends and emit it promptly,
  instead of holding it until the next one starts.
- ✅ Single-session by design. If a source has two *Gemini Live Translate*
  filters, only the first runs; the extra is disabled with a warning in its
  properties (removing the first lets the other take over). Likewise only one
  *Gemini Translated Audio* source plays; a second is muted with a warning.
  Note: the plugin runs a single translation stream, so adding the **filter to
  two different sources** is unsupported (both would feed the one session) —
  keep it on a single source.
- ⚠️ The translated audio lags your speech by a few seconds — the model's
  translation latency plus a fixed ~600 ms smoothing buffer (which doesn't
  accumulate; measured backlog stays <50 ms). Tip: when **recording**, pause a
  beat after your last sentence before hitting stop so the trailing translation
  is captured. **Live streams** aren't affected.

## Install (prebuilt)

Download the package for your platform from the
[Releases](https://github.com/plan12be/obs-live-translate-caption/releases) page
(or build it yourself, see *Build from source*), then install it **with OBS
closed**:

- **Windows** — run `…-windows-x64-installer.exe` (it detects your OBS install
  automatically), or extract `…-windows-x64.zip` into your OBS Studio directory
  (e.g. `C:\Program Files\obs-studio\`). Unsigned, so SmartScreen may warn.
- **macOS** — unzip `…-macos-universal.zip` and copy `obs-live-translate-caption.plugin`
  into `~/Library/Application Support/obs-studio/plugins/`. Unsigned / not
  notarized; if Gatekeeper blocks it, run
  `xattr -dr com.apple.quarantine ~/Library/Application\ Support/obs-studio/plugins/obs-live-translate-caption.plugin`.
- **Linux** — extract `…-linux-x86_64.tar.gz` into `~/.config/obs-studio/plugins/`
  (the `.so` should end up at
  `~/.config/obs-studio/plugins/obs-live-translate-caption/bin/64bit/obs-live-translate-caption.so`).

> The macOS and Linux packages are produced by CI but **not yet verified on those
> platforms** — feedback is welcome. Windows is the tested platform.

## Usage

1. Add the **Gemini Live Translate** filter to your microphone source
   (right-click the mic → *Filters* → **+** → *Gemini Live Translate*). Paste your
   API key, pick a target language, leave **echo** on, and optionally set
   **Playback Delay (seconds)** from 0 to 30. Once it connects the status reads
   *Connected*.

   ![Gemini Live Translate filter properties](screenshots/micro-filters.png)

2. Add a **Gemini Translated Audio** source to your scene (Sources → **+** →
   *Gemini Translated Audio*). This plays the translated voice; route it to its
   own track in *Advanced Audio Properties* to keep it separate from your mic.

   ![Gemini Translated Audio source](screenshots/audio-source.png)

3. **Captions instead of speech (optional).** Add a *Text (GDI+)* source (macOS /
   Linux: *Text (FreeType 2)*) to your scene and style it as you like. In the
   filter set **Output** to *Translated captions*, pick that source under
   **Caption Text Source**, and optionally a second text source under **Source
   Transcript Text Source** to show what was recognized. **Caption Lines** (1–4)
   and **Caption Hold (seconds)** (1–30) control how many sentences stay on
   screen and for how long; **Custom Vocabulary** takes comma-separated names or
   terms to bias recognition. The *Gemini Translated Audio* source is not needed
   in this mode. Until a caption source is chosen the status reads *Set a
   caption text source to show captions*; a misspelled name shows *Caption text
   source "…" not found*.

## Remote control (OBS WebSocket)

The filter's settings are plain OBS source settings, so you can change the
**target language**, **echo** option, and **playback delay** live from any OBS WebSocket v5 client
(scripts, [`obs-cli`](https://github.com/muesli/obs-cli), Advanced Scene
Switcher, your own app) using the `SetSourceFilterSettings` request — no extra
plugin support needed:

```json
{ "requestType": "SetSourceFilterSettings",
  "requestData": {
    "sourceName": "Mic/Aux",
    "filterName": "Gemini Live Translate",
    "filterSettings": { "target_lang": "ja" } } }
```

| setting | type | meaning |
|---|---|---|
| `target_lang` | string | BCP-47 code from [`src/languages.hpp`](src/languages.hpp) (e.g. `en`, `zh`, `ja`, `pt-BR`) — the value, not the display name |
| `echo_target` | bool | output speech even when the input is already in the target language |
| `playback_delay` | number | seconds to delay the translated audio stream, clamped to 0-30 |
| `api_key` | string | Gemini API key (rarely sent remotely; clearing it stops the session) |
| `output_mode` | string | `speech` (default) or `captions`; switching stops one session and starts the other |
| `caption_text_source` | string | name of the text source that receives translated captions (captions mode) |
| `caption_source_text_source` | string | optional text source for the source-language transcript; empty disables it |
| `caption_max_segments` | int | sentences kept on screen, clamped to 1-4 |
| `caption_hold_seconds` | number | seconds after the last sentence before the caption source is cleared, clamped to 1-30 |
| `caption_custom_vocabulary` | string | comma-separated phrases passed to the transcriber as custom vocabulary |

Notes:

- `sourceName` / `filterName` must match your OBS names exactly (`filterName`
  defaults to *Gemini Live Translate*).
- Keep the request's default `overlay: true` (merge). With `overlay: false` OBS
  first resets the filter to defaults — and since `api_key` has no default, that
  **clears the key and stops translation**.
- Changing `target_lang` reconnects the session with the new language, so expect
  a brief gap.
- This is **set-only**. The runtime connection status (Connecting / Connected /
  API-key error) is not exposed over WebSocket — `GetSourceFilterSettings`
  returns the stored settings, not the live status.

## Build from source

Requires CMake ≥ 3.28 and a C++17 toolchain. Dependencies (nlohmann/json,
IXWebSocket + mbedTLS) are fetched by CMake; libobs/obs-deps come from
`buildspec.json` on Windows/macOS and from the system on Linux. The
`-DCMAKE_COMPILE_WARNING_AS_ERROR=OFF` flag keeps third-party warnings from
failing the build.

**Windows** (Visual Studio 2022) — produces `build_x64\RelWithDebInfo\obs-live-translate-caption.dll`:

```powershell
cmake --preset windows-x64 -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF
cmake --build --preset windows-x64
```

With Visual Studio 2026 (CMake ≥ 4.1, e.g. the copy bundled with VS) use the
`windows-x64-vs2026` preset instead; it targets the newest installed Windows
SDK and shares the same `build_x64` output directory. On Windows the
Visual Studio generator is required — libobs' own CMake scripts reject Ninja
— so in CLion enable the preset-based profile rather than the default Ninja
"Debug" profile.

`cmake` is usually not on the PATH; the copy bundled with Visual Studio works
(the first configure also downloads obs-deps and builds libobs, which takes a
few minutes):

```powershell
$env:PATH = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;$env:PATH"   # VS 2026
# VS 2022: C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin
```

To try a local build, close OBS and copy the DLL (and the `.pdb`, so crashes
show function names in the OBS log) into the plugin directory:

```powershell
Copy-Item build_x64\RelWithDebInfo\obs-live-translate-caption.dll "C:\Program Files\obs-studio\obs-plugins\64bit\"
Copy-Item build_x64\RelWithDebInfo\obs-live-translate-caption.pdb "C:\Program Files\obs-studio\obs-plugins\64bit\"
```

**macOS** (Xcode 16+, universal) — produces `build_macos/RelWithDebInfo/obs-live-translate-caption.plugin`:

```bash
cmake --preset macos -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF
cmake --build --preset macos --target obs-live-translate-caption
```

**Linux** — libobs comes from the obsproject PPA; install build deps first:

```bash
sudo add-apt-repository --yes ppa:obsproject/obs-studio
sudo apt-get update
sudo apt-get install -y obs-studio libsimde-dev libmbedtls-dev cmake ninja-build pkg-config
cmake --preset ubuntu-x86_64 -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF
cmake --build --preset ubuntu-x86_64 --target obs-live-translate-caption   # build_x86_64/obs-live-translate-caption.so
```

(The exact CI steps live in [`.github/workflows/release.yaml`](.github/workflows/release.yaml).)

## Test

Use your platform's build directory (`build_x64` on Windows, `build_macos` on
macOS, `build_x86_64` on Linux). For example, on Windows:

```bash
# Build the libobs-free unit-test binary first
cmake --build --preset windows-x64 --target unit-tests

# Run the full unit-test suite
ctest --test-dir build_x64 --output-on-failure

# Run a single test case by name
ctest --test-dir build_x64 -R "<test case name>" --output-on-failure
```

The `unit-tests` target covers the pure logic (base64, ring buffer, backoff,
audio conversion, audio pacing/timestamper, Gemini protocol parsing, caption
protocol, translate request/response, caption composer) and does **not**
require libobs. The caption modules also build as standalone binaries
(`caption-protocol`, `translate-protocol`, `caption-composer`; ctest names are
prefixed with `<module>/`) so one module can be iterated on without compiling
the rest.

## Project layout

```
src/
  plugin-main.cpp        module entry; registers the filter + source
  filter.cpp             mic filter: resample → chunk → stream (continuous)
  source.cpp             translated-audio source: event-driven push loop
  translation-session.*  shared WebSocket session + audio buffers + reconnect
  live-protocol.*        build/parse Gemini Live API messages
  caption-session.*      captions mode: STT WebSocket + translate workers + sinks
  caption-protocol.*     build/parse gemini-3.5-transcribe-live messages
  translate-protocol.*   Flash-Lite generateContent request/response
  caption-composer.*     in-order caption window + hold timer (pure logic)
  caption-output.*       writes caption text into an OBS text source by name
  audio-pacing.*         OutputTimestamper (contiguous, lead-bounded timestamps)
  audio-convert.*        PCM downmix / conversion / chunking
  ring-buffer.*          bounded byte ring buffer
  backoff.*              exponential reconnect backoff
  base64.*, languages.hpp
tests/                   Catch2 unit tests (one per pure module)
installer/               Inno Setup script for the Windows installer
cmake/, CMakePresets.json, buildspec.json
```

## Tech stack

C++17 · CMake (obs-plugintemplate) · libobs (OBS 31.x) · IXWebSocket (TLS via
mbedTLS) · nlohmann/json · Catch2.

## Key API facts

- Model: `models/gemini-3.5-live-translate-preview` over a TLS WebSocket.
- **Input** to Gemini: 16 kHz, 16-bit PCM, mono, little-endian, 100 ms chunks.
- **Output** from Gemini: 24 kHz, 16-bit PCM, mono.
- `translationConfig` takes `targetLanguageCode` (BCP-47) and
  `echoTargetLanguage`; there is **no source-language parameter** — the model
  auto-detects the spoken language.
- The speech output stream is continuous (the model emits even during silence)
  and does **not** send `turnComplete` / `generationComplete` / `interrupted`
  in this mode. Perceived cut-offs at sentence ends are the model's own
  phrase-boundary cadence, not a plugin bug — the plugin's delivery is gap-free.
- Captions mode: `models/gemini-3.5-transcribe-live` over the same Live API
  endpoint (`responseModalities: ["TEXT"]`, `inputAudioTranscription.mode:
  "SMART"`, `languageCodes: []` = auto-detect); interim text arrives as
  `serverContent.interimInputTranscription`, finalized sentences as
  `serverContent.inputTranscription`. Sessions are capped at 10 minutes, so the
  plugin reconnects proactively at 9. The batch model `gemini-3.5-transcribe`
  is file/Interactions-API only and cannot be used for live captions.
- Translation uses
  `POST …/v1beta/models/gemini-3.1-flash-lite:generateContent` with the
  `x-goog-api-key` header, a single `role: "user"` turn and no sampling
  parameters. No `thinkingConfig` is sent (Flash-Lite already defaults to
  minimal thinking); if it is ever added, the REST shape is the camelCase
  `generationConfig.thinkingConfig.thinkingLevel` — the SDK-style flat
  `thinking_level` is rejected with HTTP 400 "Unknown name".

## Supported languages

The **target language** dropdown offers the languages
`gemini-3.5-live-translate-preview` supports (from the
[official Live Translate docs](https://ai.google.dev/gemini-api/docs/live-api/live-translate)),
each labelled with its native name plus the English name (e.g. `日本語
(Japanese)`) so both native speakers and others can recognize it. Common
languages — English, Chinese, Japanese, Korean, Spanish, French, German,
Portuguese (BR), Italian, Russian, Indonesian, Thai, Vietnamese — are pinned to
the top; the rest follow in English-name alphabetical order. The exact list and
BCP-47 codes live in [`src/languages.hpp`](src/languages.hpp).

Chinese uses the script-less code **`zh`**, not `zh-Hant` / `zh-Hans`. Output is
speech, which has no script, so the script-specific codes sound identical — and
they break same-language echo (the model doesn't treat spoken Mandarin as an
exact match for a script-tagged target), whereas `zh` translates and echoes
correctly.

There is **no source-language selection** — Gemini auto-detects the spoken
language, so you only pick what to translate *into*.

## Non-goals

Multiple simultaneous sessions (e.g. speech **and** captions at once, or two
target languages), encrypted key storage, and explicit source-language
selection are out of scope. Captions mode (v2) is limited to updating an
existing OBS text source: it does not create or style sources, write caption
files (SRT/TXT), or embed CEA-608 captions into the stream output — see the
non-goals table in
[`docs/specs/001-caption-translation-pipeline.md`](docs/specs/001-caption-translation-pipeline.md).

## Contributing

- **Test-driven.** Write the failing Catch2 test first, then the minimal
  implementation. Pure logic belongs behind the libobs-free `unit-tests`
  target; changes that touch libobs (`filter.cpp`, `source.cpp`,
  `caption-output.cpp`, the session classes) are verified by a full plugin
  build and, for captions, by the manual checks in the spec's §5.
- **Do not skip verification.** If a build or test cannot run because a
  prerequisite (libobs, a Windows toolchain) is missing, say so instead of
  claiming the change works.
- **Branches and commits.** Work on a feature branch and keep commits scoped to
  one change; do not add AI co-author trailers.
- **Specs.** Larger features start as a spec under `docs/specs/` (index and
  template in [`docs/specs/README.md`](docs/specs/README.md)); the captions
  pipeline is `001-caption-translation-pipeline.md`. Keep the non-goals above in
  mind when scoping a change.

## License

Licensed under the GNU General Public License v2.0 — see [LICENSE](LICENSE).
This matches OBS Studio's licensing, since the plugin links against libobs.

Copyright (C) 2026 plan12be. Based on
[obs-live-translate](https://github.com/weisunglee/obs-live-translate),
Copyright (C) 2026 Only26k (weisunglee), also GPL-2.0. The speech-to-speech
pipeline, build system and installer originate from that project; the captions
pipeline was added here.
