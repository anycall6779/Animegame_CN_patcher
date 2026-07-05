# SIGBUS 크래시 원인 재확인

## 증상

- 기기: Xiaomi 2407FPN8EG
- Android 빌드: Xiaomi/rothko_global/rothko:16/BP2A.250605.031.A3/OS3.0.5.0.WNNMIXM
- Unity: 2019.4.40f1, il2cpp, arm64-v8a
- 크래시 스레드: UnityMain
- signal: `SIGBUS`, code: `BUS_ADRERR`
- backtrace:

```text
#00 pc 0000000001ad21bc .../lib/arm64/libanimegame_native_localify.so
```

## 확인 결과

해당 오프셋은 `libanimegame_native_localify.so`의 `patch_pointer_cache()` 루프에 해당한다.

문제 지점은 `/proc/self/maps`에서 writable mapping을 찾은 뒤 8바이트씩 포인터 값을 읽는 부분이다.

```asm
1ad31bc: ldr x8, [x27]
1ad31c0: cmp x8, x25
```

소스 기준으로는 다음 동작이다.

```cpp
void **slot = reinterpret_cast<void **>(cursor);
if (*slot != original)
  continue;
```

즉 번역 문장, blob 파싱, APK 패치 실패가 아니라, icall 캐시를 찾기 위해 프로세스 메모리를 스캔하던 중 특정 페이지를 읽다가 SIGBUS가 발생한 것이다.

## 왜 Xiaomi/Android 16에서 터질 수 있는가

현재 코드는 권한 문자열이 `rw`인 mapping이면 대부분 스캔한다.

```cpp
if (perms == nullptr || perms[0] != 'r' || perms[1] != 'w')
  return false;
```

하지만 `rw`로 보이는 mapping이라도 모든 주소가 안정적으로 8바이트 단위 읽기를 허용한다고 단정하면 안 된다. 특히 Android 16, 최신 기기, 특수 파일/드라이버/런타임 mapping에서는 페이지 경계나 일부 구간 접근이 SIGBUS로 이어질 수 있다.

로그의 fault address가 `0x...1000` 형태의 페이지 경계이고, `pc`가 스캔 루프의 `ldr`에 걸린 점이 이 판단과 맞는다.

## 이전 수정이 실패한 이유

이전 수정에서는 `path[0] == '/'`인 파일 기반 mapping을 전부 스캔 제외했다.

그 결과 SIGBUS 가능성은 줄었지만, Unity/il2cpp의 icall 캐시가 들어 있는 실제 mapping까지 제외되어 `UnityEngine.TextAsset.get_text` 후킹이 설치되지 않았다. 그래서 게임은 중국어 그대로 표시됐다.

## 다음 수정 방향

한글이 되던 기존 후킹 경로는 유지해야 한다.

안전한 방향은 다음 중 하나다.

1. 스캔 중인 mapping 라인과 fault address를 먼저 로그로 남겨 어떤 mapping에서 SIGBUS가 나는지 특정한다.
2. 모든 파일 mapping을 제외하지 말고, 실제 SIGBUS를 일으키는 mapping 패턴만 제외한다.
3. SIGBUS/SIGSEGV 보호를 넣더라도 기존 icall 캐시 mapping은 계속 스캔되게 한다.
4. 가장 좋은 방향은 전체 메모리 스캔 대신 il2cpp 내부 icall/cache 구조를 더 좁게 찾아 패치하는 것이다.

현재는 한글 동작을 보존하기 위해 SIGBUS 수정 payload를 롤백한 상태다.
