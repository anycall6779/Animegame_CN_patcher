# benghuai.com 최신 APK 다운로드 구조 분석 보고서

작성일: 2026-07-03  
대상 페이지: https://www.benghuai.com/download/

## 결론

`https://www.benghuai.com/download/` 페이지의 APK 링크는 HTML에 직접 박혀 있지 않고, 자바스크립트가 아래 두 경로를 사용해 채운다.

1. 공식/국服 최신 APK 단일 다운로드:
   `https://www.benghuai.com/download/latest`

2. 서버별 APK 목록 JSON:
   `https://www.benghuai.com/download/config`

매 버전마다 페이지를 수동으로 뜯을 필요 없이 `download/config`를 조회하면 서버별 최신 APK URL을 얻을 수 있다. 국服 원본만 필요하면 `download/latest`의 302 리다이렉트 최종 URL을 따라가면 된다.

## 페이지 동작 구조

다운로드 페이지 HTML에는 Android 버튼의 `href`가 비어 있다.

```html
<a class="android" href=""></a>
<a class="client-download-android" href="" target="_blank"></a>
```

이후 로드되는 JS가 링크를 채운다.

주요 스크립트:

- `https://static-event.benghuai.com/new_mihoyo_homepage/js/common.js?v=228`
- `https://static-event.benghuai.com/new_mihoyo_homepage/js/downloadcenter.js?v=228`
- `https://static-event.benghuai.com/new_mihoyo_homepage/config/config.js?v=228`

`common.js`에는 상단 Android 버튼용 링크가 있다.

```js
var android_download="https://www.benghuai.com/download/latest";
$(".download-center .android").attr("href",android_download);
```

`downloadcenter.js`는 서버별 목록을 다음 API에서 가져온다.

```js
$.ajax({
    type:"GET",
    url:base_url+"download/config",
    dataType:"json",
    async:false,
    success:function(result) {
        var client_list = result;
        ...
        client_list[i]["android_download_url"]
        client_list[i]["android_part_download_url"]
    }
});
```

실제 `base_url+"download/config"`는 현재 페이지 기준으로 다음 URL과 같다.

```text
https://www.benghuai.com/download/config
```

## 현재 확인된 최신 서버별 APK 목록

2026-07-03 조회 기준:

| id | name | 표시명 | 업데이트 | APK URL |
|---:|---|---|---|---|
| 1 | gf | 国服 v13.2 | 6月17日 | `https://static.benghuai.com/Download/v13_2/Original.StripResource_13.2.8_341_kZm.apk` |
| 2 | Beta | 先遣服 v13.2 | 6月5日 | `https://static.benghuai.com/Download/v13_2/Beta.StripResource_13.2.103_41.apk` |
| 3 | bilibili | B站专服 v13.2 | 6月17日 | `https://pkg.biligame.com/games/bhxy2_v13.2.8_bili_647106.apk` |
| 4 | dangle | 当乐mix v13.2 | 6月17日 | `https://static.benghuai.com/Download/v13_2/Mix_13.2.54_341.apk` |
| 5 | hun | 混服mix v13.2 | 6月17日 | `https://static.benghuai.com/Download/v13_2/Mix_13.2.54_341.apk` |
| 9 | 翁德兰 | 翁德兰mix v13.2 | 6月17日 | `https://static.benghuai.com/Download/v13_2/Mix_13.2.54_341.apk` |

참고:

- `dangle`, `hun`, `翁德兰`은 현재 같은 Mix APK를 가리킨다.
- `android_part_download_url`은 현재 모두 빈 값이다.
- iOS 링크는 국服 항목에만 있다.

## `/download/latest` 동작

`https://www.benghuai.com/download/latest`는 APK 파일을 직접 들고 있는 URL이 아니라 302 리다이렉트 엔드포인트다.

2026-07-03 기준 Location:

```text
https://static.benghuai.com/Download/v13_2/Original.StripResource_13.2.8_341_kZm.apk
```

HEAD 응답에서 확인된 주요 메타데이터:

```text
Content-Type: application/vnd.android.package-archive
Content-Length: 261083990
Last-Modified: Fri, 12 Jun 2026 02:29:33 GMT
ETag: "BB18202CFD9EC7D2A74274C0FFFAFCF4-25"
```

따라서 국服 원본 APK만 자동으로 받으려면 `download/latest`를 따라가면 된다.

## 자동 다운로드 설계안

### 1. 국服 최신 APK만 받을 때

PowerShell:

```powershell
$url = "https://www.benghuai.com/download/latest"
$out = "Original.latest.apk"
Invoke-WebRequest -Uri $url -OutFile $out
```

이 방식은 302 리다이렉트를 자동으로 따라가며 최종 APK를 저장한다.

### 2. 서버별 APK를 모두 받을 때

PowerShell:

```powershell
$config = Invoke-RestMethod -Uri "https://www.benghuai.com/download/config"
New-Item -ItemType Directory -Force "downloads" | Out-Null

foreach ($item in $config) {
    if (-not $item.android_download_url) { continue }

    $safeName = ($item.name -replace '[\\/:*?"<>|]', '_')
    $fileName = Split-Path ([Uri]$item.android_download_url).AbsolutePath -Leaf
    $out = Join-Path "downloads" "$safeName-$fileName"

    Write-Host "Downloading $($item.title) -> $out"
    Invoke-WebRequest -Uri $item.android_download_url -OutFile $out
}
```

### 3. 패치 파이프라인과 연결할 때

현재 `CHINA_PATCH` 기준으로는 국服 원본만 필요하므로 다음 흐름이 가장 단순하다.

```powershell
Invoke-WebRequest `
  -Uri "https://www.benghuai.com/download/latest" `
  -OutFile "CHINA_PATCH\Original.latest.apk"

powershell -ExecutionPolicy Bypass `
  -File CHINA_PATCH\run_patch_apk.ps1 `
  CHINA_PATCH\Original.latest.apk `
  -Sign
```

서버별로 패치해야 한다면 `download/config`에서 `name == "gf"`, `"bilibili"`, `"Beta"`처럼 골라서 입력 APK로 넘기면 된다.

## 권장 파일명 규칙

자동 저장 시 원본 파일명을 보존하는 것이 좋다.

예:

```text
downloads/gf-Original.StripResource_13.2.8_341_kZm.apk
downloads/Beta-Beta.StripResource_13.2.103_41.apk
downloads/bilibili-bhxy2_v13.2.8_bili_647106.apk
downloads/dangle-Mix_13.2.54_341.apk
```

이유:

- 파일명 안에 실제 상세 버전과 빌드 번호가 들어 있다.
- `title`은 `v13.2`까지만 표시되어 세부 빌드 추적에는 부족하다.
- Mix 계열처럼 여러 서버가 같은 APK를 공유할 수 있어 `name-파일명.apk` 형태가 구분에 유리하다.

## 주의점

1. `download/config`의 `title`은 표시용이다. 실제 버전 추적은 APK 파일명 또는 APK Manifest의 `versionName/versionCode`를 기준으로 해야 한다.

2. `/download/latest`는 국服 원본만 가리킨다. Bilibili, Beta, Mix까지 자동화하려면 반드시 `/download/config`를 써야 한다.

3. CDN URL은 바뀔 수 있으므로 하드코딩하지 말고 매번 API를 조회해야 한다.

4. 다운로드 전 `HEAD` 요청으로 `Content-Type`, `Content-Length`, `ETag`, `Last-Modified`를 저장해두면 나중에 “같은 파일인지” 빠르게 확인할 수 있다.

5. Bilibili APK는 `pkg.biligame.com` 도메인을 사용한다. 네트워크 환경에 따라 공식 CDN보다 다운로드 정책이나 속도가 다를 수 있다.

## 추천 구현 로직

1. `GET https://www.benghuai.com/download/config`
2. JSON 배열을 파싱한다.
3. 필요한 서버를 `name`으로 선택한다.
4. `android_download_url`이 비어 있지 않은 항목만 다운로드한다.
5. 저장 후 SHA256을 기록한다.
6. 국服만 필요하면 `/download/latest`를 사용해도 되지만, 기록용으로는 `/download/config`의 `gf` URL도 함께 저장한다.

## 출처

- 다운로드 페이지: `https://www.benghuai.com/download/`
- 서버별 다운로드 JSON: `https://www.benghuai.com/download/config`
- 페이지 스크립트: `https://static-event.benghuai.com/new_mihoyo_homepage/js/downloadcenter.js?v=228`
- 공통 스크립트: `https://static-event.benghuai.com/new_mihoyo_homepage/js/common.js?v=228`
