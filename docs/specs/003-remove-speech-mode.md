# obs-live-translate-caption speech 모드 제거 스펙

작성일: 2026-09-05
대상: `001-caption-translation-pipeline.md` §1 목표 "모드"(`output_mode = speech | captions`)와 §4.2 모드 전환을 폐지하고, 플러그인을 자막 전용으로 축소
연관 스펙: `README.md`, `001-caption-translation-pipeline.md`, `002-caption-text-box-limits.md`

이 문서는 `001`이 기존 speech-to-speech 경로와 공존시키기 위해 도입한 출력 모드 분기를 걷어내고,
음성 번역 파이프라인(`TranslationSession`, *Gemini Translated Audio* 소스, 오디오 출력 페이싱)을
코드베이스에서 제거하는 범위를 다룬다. 자막 파이프라인 자체의 동작 변경, 필터 소스 id·표시 이름
변경, 로케일 파일 추가는 다루지 않는다.

## 1. 목표와 비목표

검증 문장:

```text
플러그인을 설치하고 마이크에 필터를 붙이면 속성창에 자막 설정만 보이고, 소스 추가 목록에
"Gemini Translated Audio"가 없으며, 음성 모드로 저장된 옛 씬 컬렉션을 열어도 필터가 자막 모드로
그대로 동작한다.
```

### 목표

| 영역 | 목표 |
|---|---|
| 코드 제거 | `translation-session.*`, `source.cpp`, `audio-pacing.*`, `output-delay.*`, `owner-guard.*`, `live-protocol.*`와 각 테스트를 삭제한다. 빌드 목록과 테스트 목록에서도 제거한다. |
| 공용 타입 이관 | `ConnStatus`는 `caption-session.hpp`로, `build_realtime_input_message`는 `caption-protocol.*`로 옮긴다(테스트 포함). `base64`, `ring-buffer`, `backoff`, `audio-convert`, `languages.hpp`는 자막 경로가 쓰므로 남는다. |
| 필터 단순화 | `output_mode`, `echo_target`, `playback_delay` 설정과 모드 전환 로직을 제거한다. 필터는 항상 자막 세션을 구동한다. first-wins 단일 필터 규칙과 AuthError 처리는 유지한다. |
| 모듈 등록 | `plugin-main.cpp`는 필터만 등록한다. 모듈 설명 문구에서 speech-to-speech를 뺀다. |
| 호환 | 옛 씬 컬렉션의 `output_mode=speech` 설정은 무시되고 자막 모드로 동작한다. 남아 있는 *Gemini Translated Audio* 소스는 OBS 표준 동작대로 "누락된 소스"로 표시된다. 필터 id `gemini_live_translate_filter`와 표시 이름은 바꾸지 않는다(원격 제어 `filterName` 호환). |
| 문서 | README에서 음성 모드 설명·설정·스크린샷을 걷어내고 출처 표기만 남긴다. `docs/superpowers/*`(음성 세션 소유권 설계 문서)를 삭제한다. |

### 비목표

| 항목 | 제외 이유 |
|---|---|
| 필터 소스 id·표시 이름 변경(`gemini_live_translate_filter`, "Gemini Live Translate") | 씬 컬렉션과 원격 제어 요청이 이 값을 참조한다. 이름은 자막 모드에도 어색하지 않아 바꿀 이득이 작다. |
| 옛 *Gemini Translated Audio* 소스의 자동 정리 | 플러그인이 사용자의 씬을 수정하는 것은 `001` 비목표(텍스트 소스의 `text`만 갱신)와 같은 이유로 하지 않는다. OBS가 누락 소스로 표시하고 사용자가 지운다. |
| 로케일(`data/locale`) 추가 | 로그의 `Failed to load 'en-US' text` 경고는 무해하며 이 스펙과 무관한 별도 작업이다. |
| 자막·음성 동시 출력 | `001` 비목표였고, 음성 경로가 사라지면서 완전히 범위 밖이 된다. |
| `languages.hpp` 목록 재검토 | 목록은 live-translate 모델 기준으로 만든 것이지만 LLM 번역은 이 언어 전부를 처리한다. 항목 추가·삭제는 별도 작업. |
| 원본(`weisunglee/obs-live-translate`)과의 동기화 유지 | 이미 독립 리포지토리이며 공통 코드가 줄어드는 것을 감수한다. |

## 2. 성공 기준

1. 파일 부재: `src/`와 `tests/`에 `translation-session`, `source.cpp`, `audio-pacing`, `output-delay`, `owner-guard`, `live-protocol`로 시작하는 파일이 없다. `docs/superpowers/`가 없다. `screenshots/audio-source.png`가 없다.
2. 참조 부재: `src/`, `tests/`, `CMakeLists.txt`, `tests/CMakeLists.txt`, `README.md`를 `TranslationSession|output_mode|echo_target|playback_delay|gemini_translated_audio_source|Translated Audio|build_setup_message|parse_server_message`로 grep하면 README의 출처·마이그레이션 안내 문장 외에는 결과가 없다.
3. `ConnStatus`가 `src/caption-session.hpp`에 정의되어 있고 `filter.cpp`가 `caption-session.hpp`만 포함한다.
4. `build_realtime_input_message`가 `src/caption-protocol.hpp/.cpp`에 있으며, 기존 `test-live-protocol.cpp`의 realtime input 테스트(MIME `audio/pcm;rate=16000`, base64 페이로드)가 `tests/test-caption-protocol.cpp`로 옮겨져 통과한다.
5. `plugin-main.cpp`의 `obs_module_load`가 `obs_register_source`를 한 번만 호출한다(필터). OBS 로그의 모듈 로드 직후 소스 목록에 *Gemini Translated Audio*가 없고, Sources > **+** 메뉴에도 없다(수동).
6. 필터 속성창: **Output**, **echo** 체크박스, **Playback Delay** 슬라이더가 없고, 자막 설정(텍스트 소스 2개, Caption Lines, Max Characters per Line, Caption Hold, Custom Vocabulary, Translation Model)이 항상 활성화되어 있다(수동).
7. 호환: `output_mode: "speech"`, `echo_target`, `playback_delay`가 저장된 옛 씬 컬렉션을 열면 필터가 자막 세션을 시작하고(`configuring caption session` 로그), 설정을 다시 저장하면 그 세 키는 그대로 남아도 무시된다(수동).
8. 원격 제어: `SetSourceFilterSettings`로 `{"output_mode":"speech"}`를 보내도 오류 없이 무시되고 자막이 계속 나온다(수동).
9. 기존 자막 기능 회귀 없음: `caption-protocol`, `translate-protocol`, `caption-composer`, `caption-wrap`, `base64`, `ring-buffer`, `backoff`, `audio-convert` 테스트가 전부 통과한다. 통합 `unit-tests` 실행 파일에서 삭제 모듈의 테스트만 사라진다.
10. README: "How it works"가 자막 파이프라인만 설명하고, Status 목록에 음성 항목이 없으며, Remote control 표에 `echo_target`/`playback_delay`/`output_mode` 행이 없고, Project layout이 실제 파일 목록과 일치한다. 옛 씬 컬렉션 사용자를 위한 마이그레이션 문단이 있다.
11. 빌드/테스트: `cmake --build --preset windows-x64-vs2026`이 exit 0, 우리 소스 경고 0건, `ctest --test-dir build_x64 -C RelWithDebInfo`가 전부 통과한다.

## 3. 전제 조건

| 전제 | source of truth / 확인 방법 |
|---|---|
| 음성 전용 파일: `src/translation-session.{hpp,cpp}`, `src/source.cpp`, `src/audio-pacing.{hpp,cpp}`, `src/output-delay.{hpp,cpp}`, `src/owner-guard.{hpp,cpp}`, `src/live-protocol.{hpp,cpp}`; 테스트 `tests/test-audio-pacing.cpp`, `test-output-delay.cpp`, `test-owner-guard.cpp`, `test-live-protocol.cpp`. | `CMakeLists.txt` 42–53행, `tests/CMakeLists.txt` 10–31행 (2026-09-05 확인) |
| 자막 경로가 음성 모듈에서 빌려 쓰는 것은 두 가지뿐이다: `ConnStatus`(`translation-session.hpp`)와 `build_realtime_input_message`(`live-protocol.hpp`, 내부에서 `base64_encode` 사용). | `grep -rn "translation-session.hpp\|live-protocol.hpp" src` → `caption-session.hpp:4`, `caption-session.cpp`, `filter.cpp:6` |
| `filter.cpp`의 음성 의존: `TranslationSession::instance()` 호출 5곳, `FilterData::active`/`echo_target`/`playback_delay_ms`, `output_mode` 분기, `stop_caption_session_for_settings`는 자막 전용이라 유지. | `src/filter.cpp` 153, 254, 310, 331, 367, 459행 |
| `plugin-main.cpp`는 필터와 오디오 소스 두 `obs_source_info`를 등록한다. | `src/plugin-main.cpp` `obs_module_load` |
| `languages.hpp`는 자막 필터의 대상 언어 콤보에 쓰인다. 주석의 live-translate 언급은 문구만 고친다. | `src/filter.cpp` `filter_properties`, `src/languages.hpp` 상단 주석 |
| README의 음성 관련 절: How it works 1–2항과 다이어그램, Status의 echo/playback/single-session/lag 항목, Usage 2항과 `screenshots/audio-source.png`, Remote control 표 3행, Key API facts의 live-translate 항목, Supported languages의 "speech has no script" 문단. | `README.md` 2026-09-05 기준 40–49, 145–162, 197–201, 245–248, 385–395, 420–426행 |
| `docs/superpowers/`는 음성 세션의 입력/출력 소유권(first-wins) 설계 문서이며 필터 쪽 first-wins 규칙은 `filter.cpp` 주석에 이미 기록돼 있다. | `docs/superpowers/specs/2026-06-19-single-session-guard-design.md` |
| OBS는 씬 컬렉션에서 등록되지 않은 소스 id를 만나면 해당 소스를 "누락" 상태로 표시하고 로드는 계속한다. 필터 설정의 알 수 없는 키는 `obs_data`에 그대로 남고 무시된다. | OBS 31.1.1 `libobs/obs-scene.c` 소스 로드 경로, `obs_data` 동작 |
| 빌드·테스트 환경은 `001` §3과 같다. | `CMakePresets.json`, `tests/CMakeLists.txt` |

## 4. 기능 범위

### 4.1 삭제와 이관

- 삭제: §3 첫 행의 파일 전부, `screenshots/audio-source.png`, `docs/superpowers/` 디렉터리.
- 이관:
  - `enum class ConnStatus`를 `caption-session.hpp`로 옮긴다. 값과 순서는 그대로다.
  - `build_realtime_input_message(const uint8_t*, size_t)`를 `caption-protocol.hpp/.cpp`로 옮긴다.
    `caption-protocol.cpp`가 `base64.hpp`를 포함하게 되므로 `tests/CMakeLists.txt`의 `caption-protocol`
    모듈 테스트 타깃에 `base64.cpp`를 추가한다. realtime input 테스트 2개(MIME, base64 페이로드)를
    `test-caption-protocol.cpp`로 옮긴다.
- `CMakeLists.txt`와 `tests/CMakeLists.txt`에서 삭제 파일 항목을 뺀다.

### 4.2 필터 단순화 (`filter.cpp`)

- `FilterData`에서 `echo_target`, `playback_delay_ms`, `active`, `captions_mode`를 제거하고
  `caption_active`만 남긴다(이름은 유지).
- `filter_update`: `output_mode` 분기를 없앤다. primary이고 키가 있으면 sink 설치 후
  `CaptionSession::configure`, 아니면 `stop_caption_session_for_settings`. `echo_target`,
  `playback_delay`, `output_mode`는 읽지 않는다(저장된 값은 무시).
- `filter_audio`: 리샘플 청크를 항상 `CaptionSession::push_input_pcm`으로 보낸다. takeover와
  `needs_start` 재시작 로직은 자막 세션 기준으로 유지한다.
- `filter_properties`: **Output**, `echo_target`, `playback_delay` 항목과 `output_mode_modified`
  콜백, `apply_mode_enabled_state`를 제거한다. 자막 항목은 항상 활성화다.
- `filter_defaults`: 세 키의 기본값 등록을 없앤다.
- `filter_status_text`: 자막 분기만 남긴다.
- `filter_destroy`: 자막 세션 정지와 텍스트 소스 비우기만 남긴다.
- 성공 기준 5–8이 우선이고 내부 구조 정리는 자유다.

### 4.3 모듈 등록 (`plugin-main.cpp`)

`live_translate_source_info` extern과 등록을 제거한다. `obs_module_description`은
"Real-time translated captions via the Gemini API."로 바꾼다. 언로드 시 세션 정지·sink 해제는 유지한다.

### 4.4 문서

- README: 소개 문단의 두 모드 목록을 자막 설명으로 합치고, How it works를 자막 다이어그램만 남기며,
  Status·Usage·Remote control·Key API facts·Supported languages에서 음성 항목을 뺀다. Install 절에
  "0.1.x 음성 버전에서 올라오는 경우" 문단을 추가한다: 옛 씬의 *Gemini Translated Audio* 소스는
  누락으로 표시되니 지우고, 필터는 설정 변경 없이 자막으로 동작한다. 출처 표기(License 절, 소개의
  "continues" 문장)는 유지한다.
- 스펙 `001` §1 목표 표 "모드" 행과 §4.2에 "003에서 폐지"를 적는다. 인덱스에 003을 추가한다.
- `languages.hpp` 상단 주석을 "translation target languages" 기준으로 고친다.

### 4.5 구현 슬라이스

| 슬라이스 | 산출물 | 소유(수정 가능) 경로 | 수정 금지 경로 | 선행 조건 | 상태 |
|---|---|---|---|---|---|
| S1 코드 제거·이관 | §4.1–4.3 전부, 빌드·테스트 통과 | `CMakeLists.txt`, `tests/CMakeLists.txt`, `src/**`, `tests/**`, `screenshots/audio-source.png`(삭제), `docs/superpowers/**`(삭제) | `README.md`, `docs/specs/**`, `installer/**`, `.github/**` | 없음 | 완료 |
| S2 문서 | §4.4 | `README.md`, `docs/specs/**` | `src/**`, `tests/**` | S1 | 완료 |

S1은 삭제와 이관이 서로 얽혀 있어 하나의 슬라이스로 순차 진행한다. S2는 감독자가 직접 수행한다.
완료 판정: S1 = 기준 1(문서 제외), 2(코드), 3, 4, 9, 11. S2 = 기준 1(문서), 2(README), 10. 기준 5–8은
사용자 수동 검증이다. 완료된 슬라이스는 재작업하지 않는다.

감독 규칙: 에이전트는 소유 경로만 수정하고 종료 전 §5 명령을 자기 빌드 디렉터리에서 실행해 exit
code를 보고한다. 파일 삭제는 `git rm`이 아니라 일반 삭제로 하고 git 상태 변경(commit/stage)은
감독자 또는 사용자만 수행한다. 훅 우회 금지. 실행은 `orchestrate-slices` 스킬을 쓴다.

## 5. 검증 방법

```bash
CM="C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin"

# 기준 1: 파일 부재 (아무 출력도 없어야 한다)
ls src tests 2>/dev/null | grep -E "^(translation-session|source\.cpp|audio-pacing|output-delay|owner-guard|live-protocol|test-(audio-pacing|output-delay|owner-guard|live-protocol))"
ls docs/superpowers screenshots/audio-source.png 2>&1 | grep -v "No such file"

# 기준 2: 참조 부재 (README의 출처/마이그레이션 문장만 남아야 한다)
grep -rnE "TranslationSession|output_mode|echo_target|playback_delay|gemini_translated_audio_source|Translated Audio|build_setup_message|parse_server_message" src tests CMakeLists.txt tests/CMakeLists.txt README.md

# 기준 3, 4: 이관 위치
grep -n "enum class ConnStatus" src/caption-session.hpp
grep -n "build_realtime_input_message" src/caption-protocol.hpp tests/test-caption-protocol.cpp

# 기준 9, 11: 전체 빌드 + 테스트
"$CM/cmake.exe" --build --preset windows-x64-vs2026
"$CM/ctest.exe" --test-dir build_x64 -C RelWithDebInfo --output-on-failure
```

명령으로 검증할 수 없는 항목:

- 기준 5, 6: OBS를 실행해 Sources > **+** 목록과 필터 속성창을 확인한다. 로그에서
  `obs_init_module(obs-live-translate-caption.dll)` 이후 등록 소스 목록을 본다.
- 기준 7: 이 변경 전에 저장한 씬 컬렉션(음성 모드 필터 + *Gemini Translated Audio* 소스)을 열어
  누락 소스 표시와 `configuring caption session` 로그를 확인한다.
- 기준 8: `obs-cli` 또는 WebSocket 클라이언트로 `{"output_mode":"speech"}`를 보내 오류 응답이 없고
  자막이 계속 나오는지 확인한다.
- 기준 10: README를 처음부터 읽어 음성 모드 설명이 남아 있지 않은지, Project layout이 `ls src`와
  일치하는지 대조한다.
