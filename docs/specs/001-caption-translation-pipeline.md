# obs-live-translate-caption 자막 번역 파이프라인 스펙

작성일: 2026-09-04
대상: `README.md` "Non-goals (v1)"에서 제외했던 **captions/subtitles**를 v2 기능 슬라이스로 분리
연관 스펙: `README.md`, `docs/superpowers/specs/2026-06-19-single-session-guard-design.md`

이 문서는 `README.md`가 정의한 speech-to-speech 플러그인에서 **자막 번역 출력 모드**를 분리해
다룬다. 마이크 음성을 `gemini-3.5-transcribe-live`로 전사(STT)하고, 확정된 문장 단위로
`gemini-3.1-flash-lite`에 번역을 요청한 뒤, 결과를 OBS 씬의 텍스트 소스에 자막 스타일로
표시한다. 기존 speech-to-speech 경로(`TranslationSession`, `gemini-3.5-live-translate-preview`)의
동작 변경은 다루지 않는다.

## 1. 목표와 비목표

검증 문장:

```text
필터의 출력 모드를 "Captions"로 두고 자막 텍스트 소스를 지정한 뒤 마이크에 말하면,
말을 멈춘 지 수 초 안에 그 텍스트 소스에 대상 언어로 번역된 자막이 나타나고,
설정한 유지 시간이 지나면 자막이 지워진다.
```

### 목표

| 영역 | 목표 |
|---|---|
| 모드 | 기존 필터에 출력 모드(`speech` / `captions`)를 추가한다. 기본값은 `speech`라서 기존 씬 컬렉션은 동작이 바뀌지 않는다. |
| STT | `captions` 모드에서 마이크 PCM을 Live API `gemini-3.5-transcribe-live`로 스트리밍하고 interim/final 전사를 받는다. |
| 번역 | final 전사 세그먼트마다 `gemini-3.1-flash-lite` generateContent를 호출해 대상 언어 텍스트를 얻는다. 직전 세그먼트를 문맥으로 함께 보낸다. |
| 표시 | 번역 결과를 사용자가 지정한 OBS 텍스트 소스(`text_gdiplus` / `text_ft2_source`)에 쓴다. 최근 N개 세그먼트를 유지하고, 침묵이 이어지면 유지 시간 후 비운다. |
| 원문 표시(선택) | 두 번째 텍스트 소스를 지정하면 원문 interim 전사를 실시간으로 쓰고 final로 교체한다. 이중 자막 구성이 가능하다. |
| 순서/복원력 | 번역 응답 순서가 뒤바뀌어도 표시 순서는 전사 확정 순서를 따른다. 세션 끊김은 backoff 재연결, 번역 실패는 세그먼트 단위 드롭으로 격리한다. |
| 원격 제어 | 새 설정 키는 기존 방식대로 `SetSourceFilterSettings`로 바꿀 수 있다. |
| 테스트 | 프로토콜 빌더/파서, 순서 정렬기, 자막 창(window) 로직은 libobs 없는 `unit-tests` 타깃에서 Catch2로 검증한다. |

### 비목표

| 항목 | 제외 이유 |
|---|---|
| 배치 모델 `gemini-3.5-transcribe` 사용 | Interactions API + File API 업로드 전용이라 실시간 스트리밍이 불가능하다. 공식 문서가 실시간에는 `gemini-3.5-transcribe-live`를 지정한다(§3). |
| 자막과 번역 음성 동시 출력 | 하나의 필터가 두 세션(두 WebSocket)을 동시에 유지해야 한다. 단일 세션 설계(`single-session-guard` 스펙)를 유지하기 위해 모드는 배타적으로 둔다. v2 이후 재검토. |
| 스트리밍 출력 CEA-608 캡션(`obs_output_output_caption_text2`) | 텍스트 소스 표시가 먼저다. 플랫폼별 인코더/출력 의존이 커서 별도 스펙으로 분리한다. |
| 파일(txt/srt) 기록 | 외부 오버레이 연동 요구가 확인되지 않았다. 별도 스펙. |
| 원문 언어 수동 지정 | `README.md` v1 결정(자동 감지 유지). transcribe-live도 `languageCodes: []`로 자동 감지한다. |
| 화자 분리, 단어 타임스탬프 | 자막 표시에 불필요하고 custom vocabulary와 호환되지 않는다(§3). |
| API 키 암호화 저장 | `README.md` v1 결정 유지. |
| 텍스트 소스 생성/스타일링 | 사용자가 만든 텍스트 소스의 `text` 필드만 갱신한다. 폰트/위치는 OBS UI 소관. |

## 2. 성공 기준

1. `output_mode` 미지정(기존 씬 컬렉션) 또는 `speech`이면 기존 speech-to-speech 동작이 그대로다. 기존 Catch2 테스트 전부 통과하고, 기존 설정 키(`api_key`, `target_lang`, `echo_target`, `playback_delay`)는 그대로 읽힌다.
2. `output_mode=captions`, 유효한 API 키, 존재하는 텍스트 소스 이름(`caption_text_source`)을 설정하고 마이크에 한 문장을 말하면, 말을 멈춘 뒤 그 텍스트 소스에 `target_lang` 언어의 번역 문장이 나타난다.
3. 마지막 발화 후 `caption_hold_seconds`가 지나면 텍스트 소스의 `text`가 빈 문자열이 된다. 그 전에 새 발화가 오면 타이머가 연장된다.
4. 순서 보장: 세그먼트 1, 2, 3의 번역이 3, 1, 2 순으로 도착해도 표시 문자열은 항상 1 → 1+2 → 1+2+3 순으로 갱신된다(단위 테스트).
5. `caption-protocol` 파서: `interimInputTranscription`은 interim, `inputTranscription`은 final로 구분하고, `error`/`setupComplete`/그 외 메시지를 구분한다. setup 메시지에는 `models/gemini-3.5-transcribe-live`, `responseModalities: ["TEXT"]`, `inputAudioTranscription.mode: "SMART"`, `languageCodes: []`가 들어간다(단위 테스트).
6. `translate-protocol` 빌더/파서: 요청 JSON에 `system_instruction`, 마지막 `contents` 항목 `role: "user"`, `generationConfig.maxOutputTokens`가 있고 `thinkingConfig`·`thinking_level`·`temperature`/`topP`/`topK`는 없다. 응답에서 `candidates[0].content.parts[*].text`를 이어 붙여 앞뒤 공백을 제거한 문자열을 반환하고, 후보가 없거나 `error`이면 실패를 반환한다(단위 테스트).
7. 번역 실패(HTTP 5xx/429/타임아웃/파싱 실패)한 세그먼트는 표시에서 건너뛰고 `[live-translate] caption seg=<n> translate failed: <reason>` 로그를 남기며, 다음 세그먼트는 정상 표시된다(단위 테스트 + 수동). HTTP 401/403은 세션을 멈추고 필터 상태 텍스트에 `API key error`를 표시한다.
8. 재연결: WebSocket이 닫히면 `Reconnecting...` 상태 후 backoff(1–30 s)로 재연결한다. 세션 연속 시간이 9분에 도달하면 interim이 비어 있는 시점에 선제 재연결한다. 두 경우 모두 `[live-translate] caption session reconnect: <reason>` 로그로 확인한다.
9. `caption_source_text_source`를 지정하면 말하는 동안 원문 interim 텍스트가 갱신되고, 문장이 확정되면 final 텍스트로 교체된다. 지정하지 않으면 원문 소스는 건드리지 않는다.
10. `SetSourceFilterSettings`로 `output_mode`, `caption_text_source`, `caption_hold_seconds`를 바꾸면 OBS 재시작 없이 반영된다. `speech` → `captions` 전환 시 `TranslationSession`이 멈추고 `CaptionSession`이 시작되며, 반대도 성립한다.
11. 텍스트 소스 이름이 존재하지 않으면 필터 상태 텍스트에 `Caption text source "<name>" not found`가 보이고 로그는 1회만 남긴다(반복 경고 없음). 이름을 고치면 다음 갱신부터 정상 표시된다.
12. 지연 계측: 각 세그먼트마다 `[live-translate] caption seg=<n> stt_final_ms=<a> translate_ms=<b> chars=<c>` 로그가 남는다. 일반 가정용 회선에서 `translate_ms` 중앙값이 1500 ms 이하다(수동 측정, 목표치이며 실패 조건은 아니다).
13. 빌드/테스트: `cmake --build --preset windows-x64-vs2026`(또는 `windows-x64`)이 exit 0, `ctest --test-dir build_x64 -C RelWithDebInfo`가 전부 통과한다.

## 3. 전제 조건

| 전제 | source of truth / 확인 방법 |
|---|---|
| 실시간 STT 모델 id는 `gemini-3.5-transcribe-live`, Live API(WebSocket) `BidiGenerateContent` 엔드포인트. 입력은 16 kHz 16-bit mono PCM, `audio/pcm;rate=16000`, 100 ms 청크. 서버 메시지는 `serverContent.interimInputTranscription.text`(interim)와 `serverContent.inputTranscription.text`(final). 세션 최대 10분. | <https://ai.google.dev/gemini-api/docs/live-api/live-transcribe> (2026-09-04 확인). 기존 `filter.cpp`의 16 kHz/3200-byte 청크 파이프라인을 그대로 재사용한다. |
| setup 옵션: `inputAudioTranscription.languageCodes`(빈 배열 = 자동 감지), `customVocabulary`(최대 1000개), `mode: "SMART"`(불필요어 제거, 구두점/대문자 정규화). | 같은 문서. 자막에는 SMART를 기본으로 쓴다. custom vocabulary는 설정 키로만 노출하고 기본은 빈 배열. |
| 배치 모델 `gemini-3.5-transcribe`는 Interactions API + File API 전용이며 실시간 스트리밍 불가. | <https://ai.google.dev/gemini-api/docs/transcribe>, <https://ai.google.dev/gemini-api/docs/models/gemini-3.5-transcribe> |
| 번역 모델 id는 `gemini-3.1-flash-lite`(고정 id. 2026-09-04 기준 문서에 `gemini-3.5-flash-lite`, `gemini-3.1-flash-lite`, `gemini-2.5-flash-lite`가 나열됨. 고정 id는 Google 릴리스와 무관하게 번역 동작이 안정적이다). generateContent REST: `POST https://generativelanguage.googleapis.com/v1beta/models/gemini-3.1-flash-lite:generateContent`, 헤더 `x-goog-api-key`. 출력은 텍스트 전용. 3.1 Flash-Lite의 `thinkingLevel` 기본값이 `minimal`이므로 `thinkingConfig`는 보내지 않는다(일부 Flash-Lite 버전은 이 필드를 HTTP 400으로 거부한다). | <https://ai.google.dev/gemini-api/docs/models/gemini-3.5-flash-lite> |
| Gemini 3.5 계열은 REST generateContent에서 `generationConfig.thinkingConfig.thinkingLevel`(`minimal`/`low`/`medium`/`high`, camelCase 중첩. SDK 표기 `thinking_level`은 REST에서 HTTP 400 "Unknown name")을 지원하고, `temperature`/`top_p`/`top_k`는 요청에서 제거하라고 안내한다. 마지막 `contents` 항목은 `role: "user"`여야 한다. | <https://ai.google.dev/gemini-api/docs/whats-new-gemini-3.5> |
| HTTPS 클라이언트는 이미 링크된 IXWebSocket의 `ix::HttpClient`를 쓴다(`USE_TLS=ON`, mbedTLS). 새 의존성을 추가하지 않는다. | `CMakeLists.txt`의 `USE_TLS`/`USE_MBED_TLS`, `build_x64/_deps/ixwebsocket-src/ixwebsocket/IXHttpClient.h` |
| 텍스트 소스 갱신 API: `obs_get_source_by_name` → `obs_data_set_string(settings, "text", …)` → `obs_source_update` → `obs_source_release`. 텍스트 소스의 unversioned id는 Windows `text_gdiplus`(version 3), macOS/Linux `text_ft2_source`(version 2)이며 `obs_source_get_id()`는 `text_gdiplus_v3`처럼 버전 접미사가 붙은 값을 돌려주므로 판별에는 `obs_source_get_unversioned_id()`를 쓴다. | `.deps/obs-studio-31.1.1/libobs/obs.h:1053`, `.deps/obs-studio-31.1.1/plugins/obs-text/gdiplus/obs-text.cpp:1083`, `plugins/text-freetype2/text-freetype2.c:71` |
| 단일 세션 규칙(필터 first-wins, 소스 first-wins)은 유지된다. `captions` 모드에서 *Gemini Translated Audio* 소스는 무음이다. | `docs/superpowers/specs/2026-06-19-single-session-guard-design.md`, `src/filter.cpp` `is_primary_filter` |
| 빌드 환경: CMake ≥ 3.28, VS 2022(`windows-x64`) 또는 VS 2026(`windows-x64-vs2026`), Catch2 v3.5.2 `unit-tests` 타깃. | `CMakePresets.json`, `tests/CMakeLists.txt` |
| 대상 언어 목록은 `src/languages.hpp`를 재사용한다. 번역 프롬프트에는 BCP-47 코드와 영어 이름을 함께 넣는다. | `src/languages.hpp` |

## 4. 기능 범위

### 4.1 설정 키와 UI

필터(`gemini_live_translate_filter`)에 다음 키를 추가한다. 기존 키는 그대로 둔다.

| 키 | 타입 | 기본값 | 의미 |
|---|---|---|---|
| `output_mode` | string | `speech` | `speech` = 기존 speech-to-speech, `captions` = 이 스펙의 자막 파이프라인 |
| `caption_text_source` | string | `""` | 번역 자막을 쓸 텍스트 소스 이름. 빈 값이면 자막을 쓰지 않는다(상태 텍스트로 안내). |
| `caption_source_text_source` | string | `""` | 원문(interim/final) 전사를 쓸 텍스트 소스 이름. 선택. |
| `caption_max_segments` | int | `2` | 자막 소스에 유지할 최근 세그먼트 수. 범위 1–4. 줄바꿈(`\n`)으로 이어 붙인다. 줄 폭·줄 수 기반 박스 제한과 `caption_max_lines`로의 대체는 `002-caption-text-box-limits` 참조. |
| `caption_hold_seconds` | double | `4.0` | 마지막 표시 갱신 후 자막을 비우기까지의 시간. 범위 1–30. |
| `caption_custom_vocabulary` | string | `""` | 쉼표로 구분한 편향 어휘. 빈 값이면 setup에서 생략한다. |

UI: `output_mode`는 콤보(`Translated speech` / `Translated captions`). 텍스트 소스 두 항목은
`obs_enum_sources`로 unversioned id가 `text_gdiplus`/`text_ft2_source`인 소스 이름을 나열하는 콤보로 채우되,
편집 가능한 콤보(`OBS_COMBO_TYPE_EDITABLE`)로 두어 아직 없는 이름도 미리 적을 수 있게 한다.
`captions` 모드에서는 `echo_target`, `playback_delay`를 비활성화(`obs_property_set_enabled`)한다.
`README.md` "Remote control" 표에 새 키를 추가한다.

### 4.2 모드 전환과 세션 소유

`filter_update`는 `output_mode`에 따라 `TranslationSession` 또는 새 `CaptionSession` 중 하나만
구동한다. 전환 시 반대편 세션에 `stop()`을 호출한다. primary 판정(`is_primary_filter`)과
"primary가 아니면 pass-through" 규칙은 두 모드에 공통이다. `filter_audio`는 primary이고
`captions` 모드일 때 리샘플된 3200-byte 청크를 `CaptionSession::push_input_pcm`으로 보낸다.
성공 기준 1, 10이 우선이며 내부 분기 구조는 자유다.

### 4.3 STT 세션 (`CaptionSession`)

`TranslationSession`과 같은 싱글턴/워커 스레드 구조를 따르되 별도 클래스로 둔다(오디오 출력
버퍼, 인터럽트, 지연 버퍼가 없다).

- 연결: `ws.setUrl(BidiGenerateContent?key=…)` → open 시 `build_caption_setup_message(...)` 전송.
- 입력: 필터에서 받은 청크를 `realtimeInput.audio`(기존 `build_realtime_input_message` 재사용)로
  연속 전송한다. 침묵도 보낸다(기존 결정, `filter.cpp` 주석).
- 수신: `parse_caption_server_message`가 `Interim{text}` / `Final{text}` / `Error` / `Other`를
  돌려준다. Final은 단조 증가 `seq`를 부여해 번역 큐에 넣고, Interim은 원문 소스 표시로만 쓴다.
- 재연결: close/error 시 `Backoff(1000, 30000)`으로 재시도(기존 `backoff.hpp`). 연결 시각 기준
  9분 경과 시 interim이 비어 있는 첫 시점에 선제 재연결한다(성공 기준 8). 재연결 사이 청크
  손실은 허용한다.
- 상태: `ConnStatus`와 `status_text()`를 같은 형식으로 제공해 필터 속성창이 두 세션 중
  활성 쪽 상태를 보여준다.

### 4.4 번역 요청 (`translate-protocol` + HTTP 워커)

- 요청 빌더 `build_translate_request(target_code, target_name, context[], text)`:
  - `system_instruction`: "실시간 자막 번역기. 입력을 `<target_name> (<target_code>)`로 번역.
    번역문만 출력, 따옴표/설명/원문 금지. 입력이 이미 대상 언어이면 그대로 정리해 출력."
  - `contents`: 단일 `user` 턴. 본문에 직전 최대 3개 원문 세그먼트를 "Context:" 블록으로,
    현재 세그먼트를 "Translate:" 블록으로 넣는다(모델 턴을 마지막에 두지 않기 위한 결정).
  - `generationConfig`: `maxOutputTokens: 256`만. `thinkingConfig`와 샘플링 파라미터 없음(§3).
- 응답 파서 `parse_translate_response(json) -> optional<string>`: 성공 기준 6.
- HTTP 워커: `ix::HttpClient`로 POST, 타임아웃 5 s, 동시 요청 최대 3개. 결과는 `seq`와 함께
  `CaptionComposer::on_translated(seq, text)` 또는 `on_failed(seq, reason)`으로 넘긴다.
- 실패 정책: 401/403 → `AuthError`로 세션 정지(기존 speech 모드와 동일). 그 외 → 세그먼트
  드롭 + 로그, 세션 유지(성공 기준 7).

### 4.5 자막 구성기 (`CaptionComposer`, 순수 로직)

표시 창을 세그먼트 수가 아니라 줄 폭·줄 수로 제한하는 확장은 `002-caption-text-box-limits`가 다룬다.

시계를 주입받는(`now_ms` 인자) 순수 클래스로 두어 단위 테스트한다.

- `push_final(seq, source_text)`: 대기 큐에 등록.
- `on_translated(seq, text)` / `on_failed(seq, reason)`: 결과 기록. 큐 앞쪽부터 결과가 갖춰진
  세그먼트를 순서대로 방출(drop된 것은 건너뜀). 성공 기준 4, 7.
- `render(now_ms) -> optional<string>`: 최근 `max_segments`개 방출 세그먼트를 `\n`으로 이어
  붙인 문자열. 마지막 방출 시각 + `hold_ms`가 지나면 창을 비우고(이미 방출된 텍스트는 다시 보이지 않는다) 빈 문자열을 한 번 반환하며 이후는
  `nullopt`(변화 없음). 성공 기준 3.
- 선두 세그먼트의 번역이 5 s(타임아웃)를 넘겨도 오지 않으면 HTTP 워커가 `on_failed`를
  보내므로 구성기 자체에는 타이머가 없다.

### 4.6 텍스트 소스 출력 (`caption-output`, libobs 의존)

- `CaptionOutput::write(source_name, text)`: 이름으로 소스를 찾아 `text`만 갱신한다. 이름이
  비어 있으면 no-op. 소스가 없으면 이름당 1회 경고 로그 + 상태 텍스트 반영(성공 기준 11).
- 워커 스레드에서 호출한다. `obs_source_update`는 호출 스레드에서 실행되지만 텍스트 소스
  플러그인이 자체 락으로 보호하므로 허용한다. 갱신 빈도는 interim의 경우 100 ms당 최대 1회로
  제한한다(코얼레싱).
- `captions` 모드를 벗어나거나 필터가 파괴되면 두 텍스트 소스를 빈 문자열로 비운다.

### 4.7 로그와 계측

세그먼트 확정 시각(`stt_final_ms`: 마지막 interim 이후 final까지), 번역 왕복(`translate_ms`),
문자 수를 성공 기준 12 형식으로 `LOG_INFO`에 남긴다. 원문/번역 본문은 `LOG_DEBUG`에만 남긴다.

### 4.8 구현 슬라이스

| 슬라이스 | 산출물 | 소유(수정 가능) 경로 | 수정 금지 경로 | 선행 조건 | 상태 |
|---|---|---|---|---|---|
| S0 스캐폴드 | 신규 파일 스텁 생성, 빌드 등록 | `CMakeLists.txt`, `tests/CMakeLists.txt`, 아래 S1–S5 파일의 빈 스텁 생성(이후 소유권은 각 슬라이스로 이전) | `src/*.cpp` 기존 파일 | 없음 | 완료 |
| S1 caption-protocol | setup 빌더, 서버 메시지 파서 + 테스트 | `src/caption-protocol.hpp`, `src/caption-protocol.cpp`, `tests/test-caption-protocol.cpp` | 그 외 전부 | S0 | 완료 |
| S2 translate-protocol | 요청 빌더, 응답 파서 + 테스트 | `src/translate-protocol.hpp`, `src/translate-protocol.cpp`, `tests/test-translate-protocol.cpp` | 그 외 전부 | S0 | 완료 |
| S3 caption-composer | 순서 정렬/창/유지 시간 로직 + 테스트 | `src/caption-composer.hpp`, `src/caption-composer.cpp`, `tests/test-caption-composer.cpp` | 그 외 전부 | S0 | 완료 |
| S4 caption-session | WebSocket 워커, HTTP 워커, 재연결, 계측 로그 | `src/caption-session.hpp`, `src/caption-session.cpp` | `src/translation-session.*`, `src/filter.cpp` | S1, S2, S3 | 완료 |
| S5 OBS 통합 | 설정 키/UI, 모드 전환, 텍스트 소스 출력 | `src/filter.cpp`, `src/caption-output.hpp`, `src/caption-output.cpp`, `src/plugin-main.cpp` | `src/caption-session.*`, `src/source.cpp` | S4 | 완료 |
| S6 문서 | README 갱신, 이 스펙 상태 갱신 | `README.md`, `docs/specs/*` | `src/`, `tests/` | S5 | 완료 |

S1–S3은 S0 뒤에 병렬 진행 가능하다. S4 → S5 → S6은 순차다.

완료 판정(성공 기준 매핑): S1 = 기준 5. S2 = 기준 6. S3 = 기준 3(순수 부분), 4, 7(순수 부분).
S4 = 기준 7, 8, 12. S5 = 기준 1, 2, 3, 9, 10, 11. S6 = 기준 13 확인 + 문서. 완료된 슬라이스는
재작업하지 않는다.

감독 규칙: 각 에이전트는 자신의 소유 경로만 수정한다. 종료 전 §5의 해당 명령을 실행해
exit code를 보고한다. 훅 우회(`--no-verify`, `LEFTHOOK=0`)를 금지한다. git 상태 변경
(commit/stage/branch)은 감독자 또는 사용자만 수행한다. 이 표를 실제로 실행할 때는
`orchestrate-slices` 스킬을 사용한다.

## 5. 검증 방법

```bash
# VS 2026 환경은 windows-x64-vs2026, CI/VS 2022는 windows-x64 프리셋을 쓴다.
CM="C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin"

# 기준 5, 6, 4, 3, 7(순수 부분), 1(기존 테스트 회귀)
"$CM/cmake.exe" --build --preset windows-x64-vs2026 --target unit-tests
"$CM/ctest.exe" --test-dir build_x64 -C RelWithDebInfo --output-on-failure
"$CM/ctest.exe" --test-dir build_x64 -C RelWithDebInfo -R "caption-protocol|translate-protocol|caption-composer" --output-on-failure

# 기준 13: 플러그인 DLL 포함 전체 빌드
"$CM/cmake.exe" --build --preset windows-x64-vs2026
```

명령으로 검증할 수 없는 항목:

- 기준 2, 3, 9: OBS에 `text_gdiplus` 소스 두 개(`Caption`, `Caption Source`)를 만들고 필터를
  `captions` 모드로 설정한다. 한 문장을 말하고 멈춘 뒤 `Caption`에 번역이 뜨는지, 말하는 동안
  `Caption Source`에 원문 interim이 갱신되는지, 침묵 `caption_hold_seconds` 후 둘 다 비는지 확인한다.
- 기준 7: 방화벽 등으로 `generativelanguage.googleapis.com`의 HTTPS(443)만 잠시 차단하고
  말한다. `translate failed` 로그 후 차단 해제 시 다음 문장이 정상 표시되는지 확인한다.
  잘못된 키를 넣으면 상태 텍스트가 `API key error`로 바뀌는지 확인한다.
- 기준 8: 네트워크를 10초간 끊었다 복구해 `Reconnecting...` → `Connected` 전이와
  `caption session reconnect: closed` 로그를 확인한다. 9분 이상 연속 발화 후
  `caption session reconnect: session age` 로그를 확인한다.
- 기준 10: `obs-cli` 또는 임의 WebSocket v5 클라이언트로 `SetSourceFilterSettings`에
  `{"output_mode":"captions","caption_text_source":"Caption"}`를 보내고 재시작 없이 자막이
  시작되는지, `{"output_mode":"speech"}`로 되돌리면 번역 음성이 다시 나오는지 확인한다.
- 기준 11: `caption_text_source`에 없는 이름을 넣고 속성창 상태 텍스트와 로그(1회)를 확인한
  뒤 올바른 이름으로 고쳐 다음 문장부터 표시되는지 확인한다.
- 기준 12: OBS 로그 파일에서 `caption seg=` 줄 20개 이상의 `translate_ms` 중앙값을 계산한다.
