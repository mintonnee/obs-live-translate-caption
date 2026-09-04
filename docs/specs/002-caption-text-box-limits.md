# obs-live-translate-caption 자막 텍스트 박스 제한 스펙

작성일: 2026-09-05
대상: `001-caption-translation-pipeline.md` §4.5 자막 구성기(`CaptionComposer`)의 표시 창 규칙과 §4.1 설정 `caption_max_segments`를 "줄 단위 박스 제한"으로 확장
연관 스펙: `README.md`, `001-caption-translation-pipeline.md`

이 문서는 `001`에서 세그먼트 수로만 제한하던 자막 표시 창을 **줄 폭과 줄 수로 제한하는 텍스트
박스**로 바꾸는 범위를 다룬다. 번역된 문장과 원문 interim 전사를 설정한 폭에서 줄바꿈하고, 표시
줄 수를 넘는 부분을 잘라내어 OBS 텍스트 소스가 씬 밖으로 넘치지 않게 한다. 텍스트 소스의
폰트·위치·배경 같은 스타일과 CEA-608 스트림 캡션은 다루지 않는다.

## 1. 목표와 비목표

검증 문장:

```text
자막 폭을 60, 줄 수를 2로 설정하고 긴 문장을 연속으로 말하면, 자막 텍스트 소스에는 항상
2줄 이하, 각 줄은 설정 폭 이하의 텍스트만 표시되고, 텍스트 소스의 박스가 커지거나
씬 밖으로 밀려나지 않는다.
```

### 목표

| 영역 | 목표 |
|---|---|
| 줄바꿈 | 번역 문장을 설정 폭에서 줄바꿈한다. 공백이 있는 언어는 단어 경계에서, 한국어·일본어·중국어처럼 공백이 드문 문장은 글자 경계에서 나눈다. 코드 포인트 중간에서는 절대 나누지 않는다. |
| 폭 계산 | 폭은 "표시 단위"로 센다. 동아시아 전각 문자(한글, 한자, 가나, 전각 기호)는 2, 그 외는 1이다. 한글 20자와 라틴 40자가 같은 폭이 되어 혼합 문장에서도 박스 폭이 일정하다. |
| 줄 창 | 표시 창은 세그먼트가 아니라 **줄** 단위로 유지한다. 새 문장이 들어오면 가장 오래된 줄부터 밀려나고, 표시 줄 수는 항상 설정값 이하다. |
| 잘라내기 | 한 문장이 혼자서 설정 줄 수를 넘기면 앞쪽 줄만 남기고 마지막 줄 끝에 `…`를 붙인다. 잘린 사실을 로그로 남긴다. |
| 원문 표시 | 원문 텍스트 소스(interim/final)에도 같은 폭·줄 수를 적용하되, 말하는 중 길어지는 interim은 **뒤쪽** 줄을 남긴다(가장 최근 말이 보이도록). |
| 프롬프트 | 번역 요청의 system instruction에 "가능하면 N자 이내"라는 길이 힌트를 넣어 잘라내기가 드물게 일어나게 한다. N = 줄 수 × 줄 폭. |
| 설정/원격 제어 | 새 키 `caption_max_lines`, `caption_max_chars_per_line`는 UI와 `SetSourceFilterSettings` 양쪽에서 재시작 없이 반영된다. 기존 키 `caption_max_segments`는 새 키가 없을 때 줄 수로 읽어 호환한다. |
| 테스트 | 폭 계산, 줄바꿈, 줄 창, 잘라내기, 프롬프트 힌트는 libobs 없는 Catch2 테스트로 검증한다. |

### 비목표

| 항목 | 제외 이유 |
|---|---|
| 폰트 메트릭 기반 픽셀 폭 계산 | 플러그인은 텍스트 소스의 폰트를 알 수 없고(다른 플러그인의 설정), GDI+/FreeType 렌더러가 플랫폼마다 다르다. 표시 단위 근사 + 사용자가 폭을 조정하는 방식이 단순하고 예측 가능하다. |
| 텍스트 소스 생성·스타일 변경, `word_wrap`/`extents` 자동 설정 | `001` 비목표(사용자 소스의 `text`만 갱신) 유지. 대신 README에 권장 소스 설정을 적는다. |
| 시간 기반 페이지 넘김(긴 문장을 여러 화면에 순차 표시) | 문장이 화면에 머무는 시간을 예측할 수 없어 다음 문장과 겹친다. 잘라내기 + 프롬프트 길이 힌트로 대체한다. |
| 일본어 금칙 처리 전체(행두·행말 금칙 표 전부) | 자막 가독성에 영향이 큰 "닫는 구두점이 줄 머리에 오지 않게" 규칙만 넣는다. 나머지는 폭 근사 오차보다 작은 문제다. |
| CEA-608 스트림 캡션 | 라틴 문자 전용이라 한국어 자막 넘침 문제의 해법이 아니다. 별도 스펙. |
| 단어 중간 하이픈 삽입 | 번역문의 의미를 바꿀 수 있다. 폭보다 긴 단어는 폭에서 그대로 자른다. |

## 2. 성공 기준

1. `display_width(text)`: 한글·한자·가나·전각 기호는 코드 포인트당 2, 그 외(ASCII, 라틴 확장, 반각 가나 제외)는 1을 돌려준다. `"한글 ab"` → 7(공백 1 포함), `"한글ab"` → 6, `"日本語テスト"` → 12, `"hello"` → 5(단위 테스트).
2. `wrap_text(text, max_width)`(라틴): `"the quick brown fox jumps over the lazy dog"`, 폭 20 → 모든 줄의 폭이 20 이하이고, 줄은 공백에서만 나뉘며, 줄 앞뒤 공백이 없다(단위 테스트).
3. `wrap_text`(한국어): 공백 없는 한글 30자, 폭 40 → 첫 줄 한글 20자, 둘째 줄 10자. 코드 포인트 중간 절단 없음(UTF-8 바이트 검사)(단위 테스트).
4. `wrap_text`(혼합): `"한국어 subtitle 테스트 문장입니다"`, 폭 20 → 각 줄 폭 20 이하, 공백이 있는 위치에서는 공백을 우선 사용(단위 테스트).
5. `wrap_text`(긴 단어): 폭보다 긴 단어(예: URL 60자, 폭 20)는 폭에서 하드 분할한다(단위 테스트).
6. `wrap_text`(구두점): 줄 폭 경계 직후 문자가 `。、，．,.!?！？」』）)` 중 하나이면 그 문자는 앞 줄에 붙인다(앞 줄이 그 문자의 폭만큼, 즉 1–2 단위 초과하는 것을 허용. 경계당 1글자만 끌어온다)(단위 테스트).
7. 줄 창: `max_lines=2`, 세그먼트 A(1줄) 방출 후 세그먼트 B(2줄) 방출 → 표시는 B의 2줄. 이어 C(1줄) → 표시는 B의 둘째 줄 + C(단위 테스트).
8. 잘라내기: `max_lines=2`에서 4줄로 감기는 세그먼트 → 표시는 첫 2줄이고 둘째 줄 끝이 `…`로 끝나며 둘째 줄 폭이 `max_width` 이하다. `render()`가 잘린 세그먼트 번호를 알려주어(반환 구조 또는 별도 조회) 세션이 `[live-translate] caption seg=<n> truncated lines=<a> kept=<b>` 로그를 남긴다(단위 테스트 + 로그 수동 확인).
9. 원문 interim: `wrap_tail(text, max_width, max_lines)`는 감긴 줄 중 **마지막** `max_lines`줄을 돌려준다(단위 테스트). OBS에서 길게 말하면 원문 소스가 항상 설정 줄 수 이하로 유지되고 최신 말이 아래에 보인다(수동).
10. 프롬프트 힌트: `TranslateRequest.max_chars=80`이면 system instruction에 `80 characters`가 포함되고, `0`이면 `characters` 길이 힌트 문구가 없다(단위 테스트).
11. 설정: UI 슬라이더 **Caption Lines**(1–6, 기본 2)와 **Max Characters per Line**(10–120, 기본 60)이 보이고, `SetSourceFilterSettings`로 `caption_max_lines`, `caption_max_chars_per_line`를 바꾸면 다음 문장부터 반영된다(수동).
12. 호환: `caption_max_lines` 키 없이 `caption_max_segments=3`만 있는 씬 컬렉션을 열면 줄 수 3으로 동작한다. 두 키가 모두 있으면 `caption_max_lines`가 우선한다(수동, 씬 컬렉션 JSON 편집으로 확인).
13. 기존 동작 유지: `001`의 순서 보장·실패 스킵·hold 타임아웃 테스트가 그대로 통과한다. `caption_max_lines=1`, 폭 120이면 한 줄 자막이 된다.
14. 넘침 수동 확인: 1920×1080 씬, GDI+ 텍스트 소스(폰트 48, 워드랩 끔, 커스텀 extents 끔), 폭 60, 줄 수 2로 한국어 문장 20개를 연속 발화해도 소스의 바운딩 박스 너비가 한글 30자 폭(약 1440 px)을 넘지 않고 높이가 2줄을 넘지 않는다.
15. 빌드/테스트: `cmake --build --preset windows-x64-vs2026`이 exit 0, `ctest --test-dir build_x64 -C RelWithDebInfo`가 전부 통과한다.

## 3. 전제 조건

| 전제 | source of truth / 확인 방법 |
|---|---|
| 현재 창 규칙: `CaptionComposer`는 `emitted_`(세그먼트 deque, 최대 4)에서 최근 `max_segments`개를 `\n`으로 이어 붙인다. 줄바꿈·폭 개념이 없다. | `src/caption-composer.cpp` `build_display()`, `src/caption-composer.hpp` `kMaxSegments` |
| 원문 interim은 `CaptionSession::publish_source_text()`가 100 ms 코얼레싱만 하고 텍스트를 그대로 `source_sink_`에 넘긴다. | `src/caption-session.cpp` `publish_source_text`, `flush_pending_source` |
| 설정 키와 UI는 `filter.cpp`가 소유한다. `caption_max_segments`(1–4, 라벨 "Caption Lines")는 사실상 세그먼트 수다. | `src/filter.cpp` `kMinCaptionSegments`, `filter_defaults`, `filter_properties` |
| 번역 프롬프트는 `build_translate_request()`의 system instruction 문자열이며 `TranslateRequest`에 길이 힌트 필드가 없다. | `src/translate-protocol.hpp/.cpp` |
| OBS 텍스트 소스는 `text` 설정에 `\n`을 그대로 줄바꿈으로 렌더링한다. `word_wrap`이 켜져 있으면 소스가 추가로 줄을 감고, `extents`가 켜져 있으면 지정 크기로 잘린다. | `.deps/obs-studio-31.1.1/plugins/obs-text/gdiplus/obs-text.cpp`(`word_wrap`, `extents`, `extents_cx/cy`), `plugins/text-freetype2` |
| 표시 단위 판정 범위(전각=2): U+1100–115F, U+2E80–303E, U+3041–33FF, U+3400–4DBF, U+4E00–9FFF, U+A000–A4CF, U+AC00–D7A3, U+F900–FAFF, U+FE30–FE4F, U+FF00–FF60, U+FFE0–FFE6, U+20000–2FFFD, U+30000–3FFFD. 그 외 1. 결합 문자(U+0300–036F)는 0. | Unicode EastAsianWidth의 W/F 범위를 단순화한 것(<https://www.unicode.org/reports/tr11/>). 이모지 등 나머지는 1로 근사한다. |
| 소스는 UTF-8(`/utf-8` 컴파일)이며 코드 포인트 경계는 UTF-8 선행 바이트로 판정한다. 별도 ICU/유니코드 라이브러리를 추가하지 않는다. | `cmake/windows/compilerconfig.cmake`, `CMakeLists.txt` 의존성 목록 |
| 빌드/테스트 환경과 슬라이스 실행 방식은 `001` §3, §4.8과 같다(모듈별 테스트 실행 파일, 에이전트별 빌드 디렉터리). | `tests/CMakeLists.txt` `add_module_test` |

## 4. 기능 범위

### 4.1 설정 키와 UI

| 키 | 타입 | 기본값 | 의미 |
|---|---|---|---|
| `caption_max_lines` | int | `2` | 자막 소스에 표시할 최대 줄 수. 범위 1–6. |
| `caption_max_chars_per_line` | int | `60` | 줄 폭(표시 단위). 범위 10–120. 전각 문자는 2로 센다. 기본값 60은 1920×1080 씬에 폰트 48(한글 약 48 px)로 한 줄 한글 30자·라틴 60자, 약 1440–1560 px가 되어 좌우 여백이 남는 값이다. |
| `caption_max_segments` | int | (유지) | 레거시. `caption_max_lines`가 저장되어 있지 않을 때만 줄 수로 읽는다. UI에는 더 이상 노출하지 않는다. |

UI: **Caption Lines** 슬라이더(1–6)는 `caption_max_lines`에 바인딩하고, **Max Characters per
Line (CJK count as 2)** 슬라이더(10–120)를 그 아래에 둔다. 둘 다 `captions` 모드에서만 활성화한다.
`filter_defaults`는 `caption_max_lines=2`, `caption_max_chars_per_line=60`을 등록한다.
`filter_update`는 `obs_data_has_user_value(settings, "caption_max_lines")`가 거짓이고
`caption_max_segments`에 사용자 값이 있으면 그 값을 줄 수로 쓴다(성공 기준 12).

README "Remote control" 표에 두 키를 추가하고, `caption_max_segments`를 레거시로 표기한다.
README "Usage"의 자막 단계에 권장 텍스트 소스 설정을 적는다: 워드랩 끔, 커스텀 extents 끔(플러그인이
줄을 감으므로 이중 줄바꿈을 피한다), 가로 정렬 가운데.

### 4.2 폭 계산과 줄바꿈 (`caption-wrap`, 순수 모듈)

새 모듈 `src/caption-wrap.hpp/.cpp`:

- `int display_width(std::string_view utf8)`: §3의 범위 표로 코드 포인트당 0/1/2를 합산한다.
- `std::vector<std::string> wrap_text(std::string_view utf8, int max_width)`: 결과 각 줄의 폭이
  `max_width` 이하가 되도록 나눈다. 규칙은 우선순위 순으로:
  1. 입력의 기존 `\n`은 강제 줄바꿈이다.
  2. 폭을 넘기기 직전의 마지막 공백(U+0020, U+3000)에서 나눈다. 공백은 줄 양끝에서 제거한다.
  3. 공백이 없으면 폭 경계의 코드 포인트 사이에서 나눈다(한국어·일본어·중국어 및 긴 라틴 단어).
  4. 경계 직후 문자가 닫는 구두점(성공 기준 6 목록)이면 앞 줄에 붙인다.
  5. `max_width < 1`이면 1로 취급한다. 빈 입력은 빈 벡터.
- `std::vector<std::string> wrap_tail(std::string_view utf8, int max_width, int max_lines)`:
  `wrap_text` 결과의 마지막 `max_lines`줄.
- `std::string truncate_with_ellipsis(const std::string &line, int max_width)`: 줄 끝에 `…`(U+2026,
  폭 1)를 붙이되 결과 폭이 `max_width` 이하가 되도록 뒤에서 코드 포인트를 제거한다.

폭 판정과 줄바꿈은 성공 기준 1–6이 우선이며, 내부 자료구조와 알고리즘은 자유다.

### 4.3 줄 단위 창과 잘라내기 (`CaptionComposer`)

`CaptionComposerConfig`를 `{max_lines(1–6), max_width(10–120), hold_ms}`로 바꾼다.
`max_segments`는 제거한다(호환은 §4.1의 설정 읽기에서 처리한다).

- 방출(`render`)에서 번역 세그먼트를 `wrap_text(text, max_width)`로 줄 목록으로 바꾼다. 줄 수가
  `max_lines`를 넘으면 앞 `max_lines`줄만 남기고 마지막 줄을 `truncate_with_ellipsis`로 처리한
  뒤, 잘린 세그먼트 번호와 줄 수를 기록한다.
- 창은 줄 deque다. 방출 줄을 뒤에 붙이고 `max_lines`를 넘는 앞쪽 줄을 버린다. 표시 문자열은 창의
  줄을 `\n`으로 이은 것이다.
- hold 타임아웃, 순서 보장, 실패 스킵, `context()`, `clear()`는 `001` §4.5 그대로다.
- 잘림 통지: `render()`의 반환을 `struct RenderResult { std::optional<std::string> display; std::vector<Truncation> truncated; }`
  로 확장하거나, `take_truncations()` 조회 메서드를 추가한다. 어느 쪽이든 세션이 성공 기준 8의 로그를
  남길 수 있으면 된다.
- 설정 변경(`set_config`)으로 `max_width`가 바뀌면 창의 기존 줄은 다시 감지 않는다(다음 세그먼트부터
  적용). `max_lines`가 줄어들면 앞쪽 줄을 즉시 버린다.

### 4.4 원문 표시 (`CaptionSession`)

`publish_source_text()`와 `flush_pending_source()`가 sink에 넘기기 직전에
`wrap_tail(text, max_width, max_lines)`를 `\n`으로 이어 붙인 문자열로 바꾼다. interim과 final 모두
같은 규칙이다. 폭·줄 수는 `CaptionConfig`에 추가한 `max_lines`, `max_width`를 쓴다.

세션은 `render_and_publish()`에서 잘림 통지를 읽어 성공 기준 8 형식으로 `LOG_INFO`를 남긴다.

### 4.5 프롬프트 길이 힌트 (`translate-protocol`)

`TranslateRequest`에 `int max_chars = 0`을 추가한다. `max_chars > 0`이면 system instruction 끝에
`Keep the translation within about <N> characters when possible; prefer shorter wording over dropping meaning.`
를 붙인다. 세션은 `N = max_lines × max_width`를 넣는다. 이 힌트는 권고일 뿐이고 잘라내기가 최종
안전장치다.

### 4.6 구현 슬라이스

| 슬라이스 | 산출물 | 소유(수정 가능) 경로 | 수정 금지 경로 | 선행 조건 | 상태 |
|---|---|---|---|---|---|
| S0 스캐폴드 | `caption-wrap.hpp` 계약, `CaptionComposerConfig`/`TranslateRequest` 필드 추가, CMake·테스트 타깃 등록, 스텁 | `CMakeLists.txt`, `tests/CMakeLists.txt`, `src/caption-wrap.hpp`, `src/caption-wrap.cpp`(스텁), `tests/test-caption-wrap.cpp`(스텁), `src/caption-composer.hpp`(config 필드), `src/translate-protocol.hpp`(필드) | 그 외 | 없음 | 완료 |
| S1 caption-wrap | 폭 계산, 줄바꿈, 꼬리 줄, 말줄임 + 테스트 | `src/caption-wrap.cpp`, `tests/test-caption-wrap.cpp` | 그 외 전부 | S0 | 완료 |
| S2 translate 힌트 | `max_chars` 힌트 문구 + 테스트 | `src/translate-protocol.cpp`, `tests/test-translate-protocol.cpp` | 그 외 전부 | S0 | 완료 |
| S3 composer 줄 창 | 줄 deque 창, 잘라내기, 잘림 통지 + 테스트 갱신 | `src/caption-composer.hpp`(private/반환 타입), `src/caption-composer.cpp`, `tests/test-caption-composer.cpp` | 그 외 전부 | S1 | 완료 |
| S4 세션·필터 통합 | 설정 키/UI/호환 읽기, 원문 wrap, 잘림 로그, 프롬프트 N 전달 | `src/filter.cpp`, `src/caption-session.hpp`, `src/caption-session.cpp` | `src/caption-wrap.*`, `src/caption-composer.*`, `src/translate-protocol.*` | S1, S2, S3 | 완료 |
| S5 문서 | README(설정 표, 권장 소스 설정), 스펙 상태 | `README.md`, `docs/specs/*` | `src/`, `tests/` | S4 | 완료 |

S1과 S2는 S0 뒤에 병렬 가능하다. S3은 S1의 `caption-wrap` 계약을 소비하므로 S1 뒤에 시작한다.
S4 → S5는 순차다. `tests/CMakeLists.txt`에 `add_module_test(caption-wrap …)`를 추가하고
`caption-composer` 모듈 테스트 타깃에 `caption-wrap.cpp`를 포함시키는 것은 S0이 한다.

완료 판정: S1 = 기준 1–6, 9(함수 부분). S2 = 기준 10. S3 = 기준 7, 8(단위 부분), 13.
S4 = 기준 8(로그), 9(표시), 11, 12. S5 = 기준 14, 15 확인 + 문서. 완료된 슬라이스는 재작업하지
않는다.

감독 규칙: 에이전트는 자신의 소유 경로만 수정하고, 종료 전 §5의 해당 명령을 자기 빌드 디렉터리에서
실행해 exit code를 보고한다. 훅 우회(`--no-verify`, `LEFTHOOK=0`) 금지. git 상태 변경
(commit/stage/branch)은 감독자 또는 사용자만 수행한다. 실행은 `orchestrate-slices` 스킬을 쓴다.

## 5. 검증 방법

```bash
CM="C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin"

# 기준 1–6, 9(함수): caption-wrap 모듈 테스트
"$CM/cmake.exe" --build --preset windows-x64-vs2026 --target caption-wrap
"$CM/ctest.exe" --test-dir build_x64 -C RelWithDebInfo -R "^caption-wrap/" --output-on-failure

# 기준 7, 8(단위), 13: composer 모듈 테스트 (001 테스트 포함)
"$CM/cmake.exe" --build --preset windows-x64-vs2026 --target caption-composer
"$CM/ctest.exe" --test-dir build_x64 -C RelWithDebInfo -R "^caption-composer/" --output-on-failure

# 기준 10: translate-protocol 모듈 테스트
"$CM/cmake.exe" --build --preset windows-x64-vs2026 --target translate-protocol
"$CM/ctest.exe" --test-dir build_x64 -C RelWithDebInfo -R "^translate-protocol/" --output-on-failure

# 기준 15: 전체 빌드 + 전체 테스트
"$CM/cmake.exe" --build --preset windows-x64-vs2026
"$CM/ctest.exe" --test-dir build_x64 -C RelWithDebInfo --output-on-failure
```

명령으로 검증할 수 없는 항목:

- 기준 8(로그): OBS에서 줄 수 1, 폭 10으로 두고 긴 문장을 말한 뒤 로그에
  `caption seg=<n> truncated lines=<a> kept=1`이 있는지 확인한다.
- 기준 9(표시): 원문 텍스트 소스를 지정하고 10초 이상 끊지 않고 말한다. 소스가 설정 줄 수를 넘지
  않고 마지막 말이 아래 줄에 보이는지 확인한다.
- 기준 11: 필터 속성창에서 두 슬라이더를 바꾸고, `obs-cli`로
  `{"caption_max_lines":1,"caption_max_chars_per_line":20}`를 보낸 뒤 다음 문장이 1줄·폭 20으로
  나오는지 확인한다.
- 기준 12: OBS를 닫고 씬 컬렉션 JSON에서 필터 설정의 `caption_max_lines`를 지우고
  `caption_max_segments: 3`을 남긴 뒤 다시 열어 3줄로 동작하는지, 두 키를 모두 두면
  `caption_max_lines`가 이기는지 확인한다.
- 기준 14: 1920×1080 씬, GDI+ 텍스트 소스(폰트 48, 워드랩 끔, extents 끔), 폭 60, 줄 수 2로
  한국어 문장 20개를 연속 발화하고 소스의 빨간 바운딩 박스가 씬 밖으로 나가지 않는지 확인한다.
