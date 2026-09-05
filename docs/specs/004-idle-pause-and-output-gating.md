# obs-live-translate-caption 유휴 자동 일시정지·출력 연동 스펙

작성일: 2026-09-05
대상: `001-caption-translation-pipeline.md` §4.3 STT 세션의 "침묵도 연속 전송" 결정에 **세션 수명 조건**을 추가. 마이크가 일정 시간 무음이거나 OBS 출력(방송·녹화·가상 카메라)이 꺼져 있으면 WebSocket을 닫아 API 비용을 멈춘다.
연관 스펙: `README.md`, `001-caption-translation-pipeline.md`

이 문서는 `001`이 "API 키가 있고 primary 필터인 동안 항상 연결"로 두었던 STT 세션에 **자동 일시정지**
조건을 더하는 범위를 다룬다. 일시정지 중에도 오디오 링 버퍼는 계속 채워 재개 직후 첫 발화가 잘리지
않게 한다. 발화 중간의 짧은 침묵을 걸러내는 voice gating(커밋 `cbf1a89`에서 제거)은 다시 도입하지
않으며, 연결된 동안의 전송 방식은 바꾸지 않는다.

## 1. 목표와 비목표

검증 문장:

```text
필터를 켜면 상태가 "Paused (waiting for sound)"이고 아무것도 연결하지 않는다. 한마디 하면 2초 안에
"Connected"가 되어 그 첫 문장이 자막으로 나온다. 이후 마이크가 5분(기본값) 동안 조용하면 상태가
"Paused (idle)"로 바뀌고 OBS 로그에 "caption session paused: idle"과 "caption websocket closed"가
남으며, 다시 말을 시작하면 2초 안에 "Connected"로 돌아온다.
```

### 목표

| 영역 | 목표 |
|---|---|
| 유휴 일시정지 | 16 kHz PCM 청크의 RMS가 임계값(dBFS) 미만인 상태가 `idle_timeout_seconds`(기본 300) 동안 이어지면 WebSocket을 닫는다. 워커 스레드, 설정, 자막 구성기는 유지한다. |
| 시작 대기 | 세션이 시작될 때(필터 생성, 키 입력, stop 뒤 재시작) 유휴 일시정지가 켜져 있으면 첫 소리를 들을 때까지 연결하지 않는다. 상태는 `Paused (waiting for sound)`다. 재연결(9분 회전, 끊김, 키·언어 변경)과 출력 켜짐에 의한 재개에는 적용하지 않는다. |
| 재개 | 일시정지 중 임계값 이상인 청크가 한 번이라도 들어오면 즉시 재연결한다. 링 버퍼는 일시정지 중에도 계속 채우고, 재개 시 직전 1초(pre-roll)와 연결 대기 중 쌓인 오디오를 open 직후 한꺼번에 보낸다. |
| 출력 연동 | `only_while_output_active`(기본 꺼짐)가 켜지면 방송·녹화·가상 카메라 중 하나라도 활성일 때만 연결한다. 상태는 `obs-frontend-api` 이벤트로 받는다. |
| 표시 유지 | 일시정지 중에도 세션 틱은 돌아 자막·원문 소스의 hold 타임아웃이 정상적으로 소스를 비운다. |
| 상태·로그 | `ConnStatus::Paused`와 사유("idle" / "output inactive")를 속성창 상태와 로그에 보여준다. 일시정지 중 재연결 시도·backoff 로그가 반복되지 않는다. |
| 설정/원격 제어 | 새 키 `idle_timeout_seconds`, `idle_threshold_dbfs`, `only_while_output_active`는 UI와 `SetSourceFilterSettings` 양쪽에서 재시작 없이 반영된다. 키가 없는 기존 씬 컬렉션은 기본값으로 동작한다. |
| 테스트 | 유휴 판정, dBFS→RMS 변환, 일시정지 사유 결정, 링 버퍼 꼬리 유지는 libobs 없는 Catch2 테스트로 검증한다. |

### 비목표

| 항목 | 제외 이유 |
|---|---|
| 발화 사이 짧은 침묵을 걸러내는 voice gating | 커밋 `cbf1a89`에서 스트림 불연속이 모델의 VAD·턴 처리를 흐트러뜨려 제거했다. 이 스펙은 "긴 유휴" 뒤에만 끊고, 연결 중에는 침묵을 그대로 보낸다. |
| OBS 볼륨미터 값 직접 조회 | libobs의 `obs_volmeter`는 별도 객체를 붙여야 하고 필터가 이미 같은 오디오를 받는다. 필터 오디오의 RMS로 판정하며, 볼륨미터와 수치가 정확히 같을 필요는 없다. |
| 노이즈 플로어를 추적하는 자동 임계값 | 긴 무음 동안 플로어 추정치가 발화 쪽으로 밀려 올라가는 문제를 따로 풀어야 한다. 첫 구현은 사용자가 정하는 `idle_threshold_dbfs` 슬라이더 하나로 두고, README에 정하는 법을 적는다(§4.1). |
| 마이크 mute·소스 비활성(씬에 없음) 감지 | mute는 필터 뒤에서 적용되어 필터 오디오에 반영되지 않는다. mute된 마이크는 대개 조용하므로 유휴 타임아웃이 대신 처리한다. |
| 일시정지 중 번역 큐 처리 중단 | 일시정지 시점에 남은 final 세그먼트의 번역은 그대로 끝낸다(문장당 1회 HTTP, 비용 미미). 큐를 버리면 마지막 문장이 사라진다. |
| pre-roll 길이·연결 대기 버퍼 크기의 설정화 | 1초 pre-roll과 기존 5초 링 버퍼로 충분하다. 설정 항목이 늘어날 이유가 없다. |
| 시작 대기의 별도 설정 키 | `idle_timeout_seconds = 0`이 유휴 일시정지와 시작 대기를 함께 끄므로 키 하나로 충분하다. 시작 대기만 따로 끌 이유가 나오면 그때 추가한다. |
| 연결 상태의 WebSocket 노출(`GetSourceFilterSettings`) | `001` 비목표 유지(README "Remote control" 참고). |
| macOS/Linux에서 frontend API 부재 시 대체 감지 | `obs-frontend-api`는 OBS Studio 세 플랫폼 모두에서 제공된다. frontend 없는 호스트(libobs 단독)는 `obs_frontend_*`가 false를 돌려주므로 출력 연동을 켜면 연결하지 않는다는 동작만 문서화한다. |

## 2. 성공 기준

1. `dbfs_to_rms(0.0)`은 32767±1, `dbfs_to_rms(-45.0)`은 184±1, `dbfs_to_rms(-90.0)`은 1.0±0.1을 돌려준다(단위 테스트).
2. `IdleDetector`(timeout 300 s): 무음 청크를 299.9 s 분량 넣으면 `idle()`이 false, 300.0 s에 도달하면 true다(단위 테스트, 시각은 인자로 주입).
3. `IdleDetector`: 무음 200 s → 신호 청크 1개 → 무음 200 s에서는 false, 이어서 무음 100 s를 더 넣으면 true다. 즉 신호 청크가 타이머를 리셋한다(단위 테스트).
4. `IdleDetector`(timeout 0): 무음을 3600 s 넣어도 false다(단위 테스트).
5. `IdleDetector`: idle 상태에서 신호 청크 1개가 들어오면 그 호출에서 즉시 false가 되고, `reset(now)` 호출도 타이머를 now 기준으로 되돌린다(단위 테스트).
5a. `IdleDetector::start_idle(now)`(timeout 300 s): 직후 `idle()`이 true이고 `awaiting_signal()`이 true다. 무음 청크를 넣어도 true가 유지되고, 신호 청크 1개에 둘 다 false가 된다. timeout 0이면 `start_idle` 뒤에도 false다. `start_idle` 뒤 `configure(0)`을 하면 다음 무음 `feed`가 false를 돌려준다(단위 테스트).
6. `resolve_pause(inputs)`: `only_while_output=false, output_active=false, idle=false` → `None`; `only_while_output=true, output_active=false, idle=false` → `OutputInactive`; `only_while_output=false, idle=true` → `Idle`; `only_while_output=true, output_active=false, idle=true` → `OutputInactive`(출력 사유 우선)(단위 테스트).
7. `ByteRingBuffer::keep_last(n)`: 10,000바이트를 쓴 뒤 `keep_last(4000)`이면 `size()==4000`이고 읽은 내용이 마지막 4,000바이트와 같다. `keep_last`가 현재 크기 이상이면 아무것도 버리지 않는다(단위 테스트).
7a. 시작 대기(수동): 기본 설정으로 필터에 키를 넣으면 연결 없이 상태가 `Paused (waiting for sound)`이고 로그에 `caption session paused: waiting for sound`가 남는다. 한마디 하면 2초 안에 `Connected`가 되고 그 문장이 자막으로 나온다. `idle_timeout_seconds`가 0이면 예전처럼 곧바로 `Connecting`/`Connected`다.
8. 유휴 일시정지(수동): 기준 7a 뒤 마이크를 5분간 조용히 두면 로그에 `[live-translate] caption session paused: idle 300s`와 `caption websocket closed`가 남고 속성창 상태가 `Paused (idle)`이다. 이후 5분 더 두어도 `Connecting`/`reconnect` 로그가 추가되지 않는다.
9. 재개(수동): 기준 8 상태에서 문장을 말하면 2초 안에 상태가 `Connected`가 되고 로그에 `caption session resumed: audio`가 남으며, 그 첫 문장이 자막 소스에 나온다(앞 단어가 잘리지 않는다).
10. 출력 연동(수동): `only_while_output_active`를 켜고 방송·녹화·가상 카메라를 모두 끄면 상태가 `Paused (output inactive)`이고 연결 로그가 없다. 녹화를 시작하면 2초 안에 `Connected`, 녹화를 멈추면 `Paused (output inactive)`로 돌아가고 `caption session paused: output inactive` 로그가 남는다. 가상 카메라만 켜도 같은 동작이다.
11. 일시정지 중 표시 정리(수동): 자막이 화면에 있는 상태에서 녹화를 멈춰(기준 10 설정) 즉시 일시정지시키면, `caption_hold_seconds` 뒤에 자막·원문 소스가 비워진다.
12. 원격 제어(수동): 기준 8 상태에서 `SetSourceFilterSettings`로 `{"idle_timeout_seconds": 0}`을 보내면 2초 안에 `Connected`가 된다. 방송·녹화가 꺼진 상태에서 `{"only_while_output_active": true}`를 보내면 `Paused (output inactive)`가 된다.
13. 호환(수동): 세 키가 없는 기존 씬 컬렉션을 열면 필터가 `Paused (waiting for sound)`로 시작해 첫 발화에 `Connected`가 되고, 속성창에 **Idle Timeout** 300, **Idle Threshold** -45, **Only while output is active** 꺼짐이 보인다.
14. 임계값(수동): **Idle Threshold**를 -20 dBFS로 올리면 보통 말소리도 무음으로 판정되어 5분 뒤 일시정지되고, -90 dBFS로 내리면 마이크 노이즈 플로어만으로도 일시정지되지 않는다.
15. 빌드/테스트: `cmake --build --preset windows-x64-vs2026`이 exit 0, `ctest --test-dir build_x64 -C RelWithDebInfo`가 전부 통과한다.

## 3. 전제 조건

| 전제 | source of truth / 확인 방법 |
|---|---|
| 현재 세션 수명: `filter_update`/`filter_audio`가 API 키가 있고 primary이면 `configure()`로 워커를 띄우고, `run()`은 `running_`이 참인 동안 연결·재연결을 반복한다. 유휴 개념이 없다. | `src/filter.cpp` `filter_audio`, `src/caption-session.cpp` `run()` |
| 오디오는 무음 포함 100 ms/3200바이트 청크로 `push_input_pcm()` → `ByteRingBuffer input_`(용량 16000×2×5 = 5초)에 쌓이고, `run()`이 `open`일 때만 꺼내 보낸다. 연결이 닫힌 동안에도 버퍼는 채워지고 5초를 넘는 앞부분은 버려진다. | `src/caption-session.hpp` `input_`, `src/ring-buffer.cpp` `write` |
| RMS 계산·임계값 비교 함수가 이미 있다(`s16le_rms`, `s16le_has_signal`, 선형 RMS 단위). `VoiceGate`는 사용처가 없다. | `src/audio-convert.hpp/.cpp`, `tests/test-audio-convert.cpp` |
| 세션 틱(100 ms)이 `flush_pending_source`, `clear_source_if_idle`, `render_and_publish`를 돌려 hold 타임아웃을 처리한다. 틱은 `run()`의 연결 루프 안에만 있다. | `src/caption-session.cpp` `kTickMs` |
| `obs-frontend-api`는 빌드 환경에 이미 준비되어 있고 아직 링크하지 않는다. 방송·녹화·가상 카메라 상태 조회와 이벤트 콜백을 제공한다. | `.deps/include/obs-frontend-api.h`: `obs_frontend_streaming_active`, `obs_frontend_recording_active`, `obs_frontend_virtualcam_active`, `obs_frontend_add_event_callback`, `OBS_FRONTEND_EVENT_{STREAMING,RECORDING,VIRTUALCAM}_{STARTED,STOPPED}`; `cmake/common/buildspec_common.cmake`(타깃 빌드), `cmake/linux/defaults.cmake`(시스템 탐색) |
| frontend 콜백은 OBS Studio가 모듈을 로드하기 전에 등록되므로 `obs_module_load`에서 `obs_frontend_add_event_callback`을 불러도 된다. frontend가 없으면 `obs_frontend_*_active`는 false를 돌려준다. | `.deps/obs-studio-31.1.1/frontend/api/obs-frontend-api.cpp` `callbacks_valid()` |
| dBFS 기준: 16비트 PCM RMS 32767을 0 dBFS로 둔다(`rms = 32767 × 10^(dB/20)`). 기본 -45 dBFS ≈ RMS 184는 기존 테스트의 "낮은 노이즈"(RMS < 500)보다 낮아 조용한 방의 마이크 노이즈 플로어를 무음으로 본다. | 이 스펙의 결정. 실제 노이즈 플로어는 기준 14로 확인한다. |
| 빌드/테스트 환경과 슬라이스 실행 방식은 `001` §3, §4.8과 같다(모듈별 테스트 실행 파일, 에이전트별 빌드 디렉터리). | `tests/CMakeLists.txt` `add_module_test` |

## 4. 기능 범위

### 4.1 설정 키와 UI

| 키 | 타입 | 기본값 | 의미 |
|---|---|---|---|
| `idle_timeout_seconds` | int | `300` | 이 시간 동안 임계값 이상인 청크가 없으면 일시정지한다. 범위 0–1800. `0`은 유휴 일시정지 끔. |
| `idle_threshold_dbfs` | double | `-45.0` | 청크 RMS가 이 값(dBFS) 이상이면 "신호"다. 범위 -90–-20. |
| `only_while_output_active` | bool | `false` | 켜지면 방송·녹화·가상 카메라 중 하나라도 활성일 때만 연결한다. |

UI(**Translation Model** 아래, **Gemini API Key** 위):

- **Idle Timeout (seconds, 0 = never)** int 슬라이더 0–1800, step 10.
- **Idle Threshold (dBFS)** float 슬라이더 -90–-20, step 1.
- **Only run while streaming, recording or virtual camera is active** 체크박스.

`filter_defaults`가 세 키의 기본값을 등록하고, `filter_update`가 clamp 후 `CaptionConfig`에 넣어
`configure()`로 넘긴다. 세 키는 재연결 없이 적용된다(`configure()`의 `reconnect` 판정에 포함하지
않는다). 속성창 상태 텍스트(`filter_status_text`)는 기존 순서 그대로 세션 `status_text()`를 쓴다.

임계값은 고정하지 않고 사용자에게 노출한다. 마이크 노이즈 플로어는 조용한 방의 USB 마이크(-60 dBFS
이하)와 팬 소리가 섞인 노트북 내장 마이크(-40 dBFS 근처) 사이에서 크게 다르고, 고정값이 플로어보다
낮으면 유휴가 영원히 오지 않아 비용이 계속 나가는데도 원인이 보이지 않는다. 슬라이더는 그 실패를
사용자가 진단하고 고칠 수 있게 하는 장치다. 노이즈 플로어를 추적하는 자동 임계값은 긴 무음 동안
추정치가 발화 쪽으로 밀리는 문제를 따로 다뤄야 하므로 첫 구현에서는 넣지 않는다.

README "Remote control" 표에 세 키를, "Usage" 3단계에 세 항목 설명을, "How it works"의
"continuously" 문단 뒤에 일시정지 동작을 한 문단 추가한다. 로드맵 항목도 한 줄 추가한다.
"Usage"의 임계값 설명에는 정하는 방법을 적는다:

- 조용히 있을 때 OBS 오디오 믹서 미터가 머무는 값보다 5–10 dB 높게 잡는다. 기본 -45는 조용한
  방에 맞는 값이며, 미터가 -45 위에 머문다면 임계값을 올려야 5분 뒤 일시정지된다.
- OBS **Noise Gate** 필터를 이 필터보다 **앞에** 두면 무음 구간이 디지털 0이 되어 임계값과 무관하게
  안정적으로 유휴가 잡힌다. 필터 순서는 소스의 *Filters* 창에서 위쪽이 먼저 적용된다.
- 어느 쪽이든 발화가 잘리지 않는지 확인하려면 임계값을 바꾼 뒤 평소 목소리로 말했을 때 상태가
  `Connected`로 유지되는지 본다(기준 14).

### 4.2 유휴 판정과 일시정지 사유 (`pause-policy`, 순수 모듈)

새 모듈 `src/pause-policy.hpp/.cpp`. libobs·IXWebSocket에 의존하지 않는다.

- `double dbfs_to_rms(double dbfs)`: `32767.0 × 10^(dbfs/20)`.
- `class IdleDetector`:
  - `void configure(uint64_t timeout_ms)`: 0이면 항상 비유휴.
  - `bool feed(bool has_signal, uint64_t now_ms)`: 신호면 `last_signal_ms_ = now`이고 false.
    무음이면 `timeout_ms > 0 && now - last_signal_ms_ >= timeout_ms`를 돌려준다.
  - `void reset(uint64_t now_ms)`: 재개·연결·설정 변경 시 타이머를 now로 되돌린다.
  - `void start_idle(uint64_t now_ms)`: "아직 소리를 못 들은" 유휴로 진입한다. timeout이 0이 아니면
    다음 신호 청크까지 무음 `feed`가 true를 돌려준다. 신호 청크나 `reset`이 이 상태를 푼다.
  - `bool idle() const`: 마지막 `feed` 결과. `bool awaiting_signal() const`: `start_idle` 뒤 아직
    신호를 못 봤는지(상태 문구 분기용).
- `enum class PauseReason { None, OutputInactive, Idle }`.
- `struct PauseInputs { bool only_while_output; bool output_active; bool idle; }`,
  `PauseReason resolve_pause(const PauseInputs &)`: 출력 사유가 유휴 사유보다 우선한다(기준 6).

유휴 판정은 성공 기준 1–6이 우선이며 내부 구현은 자유다. 신호 청크 1개로 재개하는 것은 결정
사항이다: 오탐(클릭 소리)의 비용은 연결 한 번이고 타임아웃 뒤 다시 멈추지만, 미탐은 발화 손실이다.

### 4.3 링 버퍼 꼬리 유지 (`ByteRingBuffer`)

`void keep_last(size_t bytes)`를 추가한다: 버퍼 크기가 `bytes`를 넘으면 앞부분을 버려 마지막
`bytes`만 남긴다. 재개 직전 pre-roll 트리밍에 쓴다(§4.4).

### 4.4 세션 일시정지 (`CaptionSession`)

`CaptionConfig`에 `idle_timeout_seconds`, `idle_threshold_dbfs`, `only_while_output_active`를
추가하고, `ConnStatus`에 `Paused`를 추가한다. `status_text()`는 `Paused (idle)` /
`Paused (output inactive)`를 돌려준다.

- 신호 판정: `push_input_pcm()`이 청크마다 `s16le_has_signal(pcm, len, dbfs_to_rms(threshold))`를
  계산해 `IdleDetector::feed()`에 넣고 결과를 `idle_` 원자값에 둔다. 오디오 스레드에서 3,200바이트
  RMS 계산은 무시할 수 있는 비용이다. 링 버퍼 쓰기는 일시정지 여부와 관계없이 항상 한다.
- 출력 상태: `void set_output_active(bool)`가 `output_active_` 원자값을 갱신한다(§4.5가 호출).
- 사유 결정: `run()`의 연결 루프와 대기 루프가 틱마다 `resolve_pause({cfg.only_while_output_active,
  output_active_, idle_})`를 평가한다.
  - 연결 중 사유가 `None`이 아니면 `ws.stop()`으로 닫고 `paused: <reason>` 로그, 상태 `Paused`,
    `connected_at = 0`. backoff는 리셋한다(실패가 아니다).
  - 대기 루프: `running_ && !config_changed_ && resolve_pause(...) != None`인 동안 100 ms 간격으로
    틱(`flush_pending_source`, `clear_source_if_idle`, `render_and_publish`)만 돌린다. 연결·재연결
    시도와 backoff 로그는 없다.
  - 대기 루프를 벗어나면 `resumed: <audio|output>` 로그를 남기고, `input_.keep_last(kResumePrerollBytes)`
    (1초 = 32,000바이트)로 오래된 무음을 버린 뒤 새 연결로 넘어간다. 연결 대기 중 쌓인 오디오는
    open 직후 기존 전송 루프가 청크 단위로 한꺼번에 보낸다(5초 용량을 넘긴 부분은 기존대로 버린다).
  - 시작 대기: `configure()`가 워커를 새로 띄우는 시점에 `IdleDetector::start_idle(now)`를 불러
    `idle_`을 true로 둔다. `run()`의 게이트가 이를 `Idle` 사유로 잡아 연결 없이 대기하고, 첫 신호
    청크가 기존 재개 경로(pre-roll 포함)로 연결한다. 상태 문구와 로그는 detector의
    `awaiting_signal()`이 참이면 `waiting for sound`, 아니면 `idle`이다. 재연결·출력 재개 경로는
    `start_idle`을 부르지 않으므로 즉시 연결한다. 타임아웃이 0이면 `start_idle`이 아무것도 하지
    않아 예전처럼 즉시 연결한다.
  - 재개·연결 open 시 `IdleDetector::reset(now)`. 출력이 켜져 재개될 때 마이크가 조용하면 타임아웃
    뒤 다시 유휴로 멈추는 것이 의도된 동작이다.
- 설정 변경: `configure()`는 `idle_timeout_seconds`, `idle_threshold_dbfs`를 detector에 즉시
  반영한다. 타임아웃을 0으로 바꾸면 다음 `feed`에서 비유휴가 되어 재개된다(기준 12).
  `only_while_output_active`는 다음 틱의 `resolve_pause`에서 반영된다.
- `stop()`은 detector를 `reset(now)`하고 `idle_`을 false로 되돌린다: `stop()`이 링 버퍼를 비우므로
  남은 유휴 플래그는 다음 세션이 청크를 보기도 전에 일시정지시키는 오탐이 된다(구현 중 결정).
  `output_active_`는 플러그인 전역 사실이라 건드리지 않는다. 상태는 기존처럼 `Idle`로 되돌린다. `filter_audio`의 `needs_start` 판정은 `is_running()` 기준이므로
  일시정지 중에도 `running_`이 참이라 재시작 루프가 돌지 않는다.
- 9분 선제 재연결은 연결 중에만 판정한다(`connected_at == 0`이면 건너뛴다, 기존 코드 그대로).
- 번역 워커와 큐는 일시정지와 무관하게 동작한다(비목표 참고).

### 4.5 출력 상태 감지 (`output-state`, libobs·frontend 의존)

새 모듈 `src/output-state.hpp/.cpp`:

- `void output_state_init()`: `obs_frontend_add_event_callback` 등록 후
  `obs_frontend_streaming_active() || obs_frontend_recording_active() || obs_frontend_virtualcam_active()`
  로 초기값을 세션에 넣는다.
- 콜백: `OBS_FRONTEND_EVENT_{STREAMING,RECORDING,VIRTUALCAM}_{STARTED,STOPPED}`와
  `OBS_FRONTEND_EVENT_FINISHED_LOADING`에서 위 OR 값을 다시 계산해 `CaptionSession::set_output_active()`에
  넘기고, 값이 바뀌면 `[live-translate] output active: <true|false>` 로그를 남긴다.
- `void output_state_shutdown()`: `obs_frontend_remove_event_callback`.
- `plugin-main.cpp`의 `obs_module_load`/`obs_module_unload`가 호출한다.

`CMakeLists.txt`에 `find_package(obs-frontend-api REQUIRED)`와 `OBS::obs-frontend-api` 링크를
추가한다. 테스트 실행 파일은 이 모듈을 포함하지 않는다.

### 4.6 로그

| 시점 | 로그 |
|---|---|
| 일시정지 | `[live-translate] caption session paused: idle <timeout>s` / `paused: waiting for sound` / `paused: output inactive` |
| 재개 | `[live-translate] caption session resumed: audio` / `resumed: output` |
| 출력 상태 변화 | `[live-translate] output active: true` / `false` |

일시정지 중에는 `Connecting`, `reconnect` 로그가 나오지 않는다(기준 8).

### 4.7 구현 슬라이스

| 슬라이스 | 산출물 | 소유(수정 가능) 경로 | 수정 금지 경로 | 선행 조건 | 상태 |
|---|---|---|---|---|---|
| S0 스캐폴드 | `pause-policy.hpp` 계약 + 스텁, `ring-buffer.hpp` `keep_last` 선언 + 스텁, `CaptionConfig` 필드·`ConnStatus::Paused`·`set_output_active` 선언 + 스텁, `output-state.hpp` 계약, CMake(frontend-api 링크, 소스·테스트 타깃 `add_module_test(pause-policy)`, `add_module_test(ring-buffer)`) | `CMakeLists.txt`, `tests/CMakeLists.txt`, `src/pause-policy.hpp`, `src/pause-policy.cpp`(스텁), `tests/test-pause-policy.cpp`(스텁), `src/ring-buffer.hpp`, `src/ring-buffer.cpp`(스텁 추가만), `src/caption-session.hpp`, `src/caption-session.cpp`(스텁 추가만), `src/output-state.hpp` | 그 외 | 없음 | 완료 |
| S1 pause-policy | dBFS 변환, `IdleDetector`, `resolve_pause` + 테스트 | `src/pause-policy.cpp`, `tests/test-pause-policy.cpp` | 그 외 전부 | S0 | 완료 |
| S2 ring-buffer | `keep_last` 구현 + 테스트 | `src/ring-buffer.cpp`, `tests/test-ring-buffer.cpp` | 그 외 전부 | S0 | 완료 |
| S3 세션 일시정지 | 신호 판정, 대기 루프, pre-roll, 상태·로그 | `src/caption-session.cpp`, `src/caption-session.hpp`(private 멤버만) | `src/pause-policy.*`, `src/ring-buffer.*`, `src/filter.cpp` | S1, S2 | 완료 |
| S4 필터·출력 상태 | 설정 키/UI/clamp, `output-state.cpp`, `plugin-main` 훅 | `src/filter.cpp`, `src/output-state.cpp`, `src/plugin-main.cpp` | `src/caption-session.*`, `src/pause-policy.*` | S0 | 완료 |
| S5 문서 | README(How it works, Usage, Remote control, 로드맵), 스펙 상태 | `README.md`, `docs/specs/*` | `src/`, `tests/` | S3, S4 | 완료 |

S1, S2, S4는 S0 뒤에 병렬 가능하다. S3은 S1의 `pause-policy` 계약과 S2의 `keep_last`를 소비하므로
둘 뒤에 시작한다. S5는 S3·S4 뒤 순차다. S0의 스텁은 컴파일만 되면 되고(반환값 `None`/false/no-op),
스텁 파일의 소유권은 S0 완료 후 표의 슬라이스로 넘어간다.

완료 판정: S1 = 기준 1–6. S2 = 기준 7. S3 = 기준 8, 9, 11, 12(세션 부분). S4 = 기준 10, 12(설정
부분), 13, 14. S5 = 기준 15 확인 + 문서. 완료된 슬라이스는 재작업하지 않는다.

감독 규칙: 에이전트는 자신의 소유 경로만 수정하고, 종료 전 §5의 해당 명령을 자기 빌드 디렉터리에서
실행해 exit code를 보고한다. 훅 우회(`--no-verify`, `LEFTHOOK=0`) 금지. git 상태 변경
(commit/stage/branch)은 감독자 또는 사용자만 수행한다. 실행은 `orchestrate-slices` 스킬을 쓴다.

## 5. 검증 방법

```bash
CM="C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin"

# 기준 1–6: pause-policy 모듈 테스트
"$CM/cmake.exe" --build --preset windows-x64-vs2026 --target pause-policy
"$CM/ctest.exe" --test-dir build_x64 -C RelWithDebInfo -R "^pause-policy/" --output-on-failure

# 기준 7: ring-buffer 모듈 테스트
"$CM/cmake.exe" --build --preset windows-x64-vs2026 --target ring-buffer
"$CM/ctest.exe" --test-dir build_x64 -C RelWithDebInfo -R "^ring-buffer/" --output-on-failure

# 기준 15: 전체 빌드(플러그인 DLL, frontend-api 링크 포함) + 전체 테스트
"$CM/cmake.exe" --build --preset windows-x64-vs2026
"$CM/ctest.exe" --test-dir build_x64 -C RelWithDebInfo --output-on-failure
```

명령으로 검증할 수 없는 항목:

- 기준 8: 빌드한 DLL을 OBS에 설치하고 필터를 켠 뒤 마이크를 5분간 조용히 둔다(물리 mute 또는 마이크
  덮기). OBS 로그(`도움말 → 로그 파일 → 현재 로그 보기`)에서 `paused: idle 300s`와
  `caption websocket closed`를 확인하고, 이후 5분 동안 `Connecting`/`reconnect` 줄이 없는지 본다.
- 기준 9: 기준 8 상태에서 "안녕하세요, 테스트입니다"처럼 첫 단어가 분명한 문장을 말하고 자막에
  첫 단어가 포함되는지, 로그에 `resumed: audio`가 있는지 확인한다.
- 기준 10, 11: 체크박스를 켜고 방송·녹화·가상 카메라를 모두 끈 상태에서 상태 텍스트를 확인한 뒤,
  녹화 시작 → `Connected`, 문장 하나 발화 → 자막 표시 → 녹화 중지 → `Paused (output inactive)` →
  hold 시간 뒤 자막이 비는지 본다. 가상 카메라만 켜서 같은 순서를 반복한다.
- 기준 12: `obs-cli`로 `{"idle_timeout_seconds":0}`, `{"only_while_output_active":true}`를 각각
  보내고 상태 변화를 확인한다.
- 기준 13: 이 스펙 이전에 저장한 씬 컬렉션을 열어 상태와 속성창 기본값을 확인한다.
- 기준 14: 임계값을 -20, -90으로 바꿔 각각 5분 뒤 상태를 확인한다.
