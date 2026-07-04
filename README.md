# CHINA_PATCH

 native localify 방식으로 패치하는 도구입니다. 이제 payload를 성공 APK에서 복사하지 않고, 소스에서 생성합니다.

## 구성

```text
build_payload.ps1
patch_original_apk.py
run_patch_apk.ps1
loader_java/com/combosdk/openapi/ComboAppProxy.java
native/animegame_native_localify.cpp
build_tools/make_translation_blob_cpp.py
payload/classes5.dex
```

## Payload 빌드

`classes5.dex`는 Android SDK의 `javac + d8`로 생성됩니다.

`libanimegame_native_localify.so`는 Android NDK의 `aarch64-linux-android26-clang++`로 생성됩니다. SDK만 있고 NDK가 없으면 `.so` 빌드는 실패합니다.

```powershell
cd Animegame_CN_patcher
powershell -ExecutionPolicy Bypass -File .\build_payload.ps1
```

NDK 경로를 직접 줄 수도 있습니다.

```powershell
powershell -ExecutionPolicy Bypass -File .\build_payload.ps1 -NdkRoot "C:\Users\USER\AppData\Local\Android\Sdk\ndk\버전"
```

## APK 패치

payload 빌드가 끝난 뒤:

```powershell
powershell -ExecutionPolicy Bypass -File .\run_patch_apk.ps1 .\Original.StripResource_13.2.8_341_kZm.apk -Sign
```

payload 빌드와 APK 패치를 한 번에 하려면:

```powershell
powershell -ExecutionPolicy Bypass -File .\run_patch_apk.ps1 .\Original.StripResource_13.2.8_341_kZm.apk -BuildPayload -Sign
```

bilibili판도 같은 방식으로 패치할 수 있습니다.

```powershell
powershell -ExecutionPolicy Bypass -File .\run_patch_apk.ps1 .\bhxy2_v13.2.8_bili_647106.apk -Out .\out\bilibili_localify_unsigned.apk -SignedOut .\out\bilibili_localify_signed.apk -Sign
```

## 적용 내용

- binary `AndroidManifest.xml`에서 `com.combosdk.openapi.ComboApplication`을 `com.combosdk.openapi.ComboAppProxy`로 교체
- 생성된 payload dex를 원본 APK의 다음 빈 `classesN.dex`로 추가
  - 국服 원본처럼 `classes4.dex`까지 있으면 `classes5.dex`
  - bilibili판처럼 `classes6.dex`까지 있으면 `classes7.dex`
- 생성된 `lib/arm64-v8a/libanimegame_native_localify.so` 추가
- `-Sign` 옵션 사용 시 `debug.keystore`와 `apksigner.jar`로 서명
