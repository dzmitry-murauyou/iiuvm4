# ЛР №4 — вариант А5 «Скрытый режим фотонаблюдения»

Приложение на C++ (Win32 API + OpenCV) для видеонаблюдения с веб-камеры:
предпросмотр в реальном времени, снимки `snapshot_YYYYMMDD_HHMMSS.jpg`, запись
видео, журнал событий и **скрытый режим** — значок в трее и глобальная горячая
клавиша `Ctrl+Shift+F12`, которая делает снимок без открытия окна.

Проект под **Visual Studio 2026**, весь код — **один файл** `Lab4_Camera/main.cpp`.

---

## 1. Состав проекта

```
lab4_camera/
├── Lab4_Camera.sln            решение (Debug|x64, Release|x64)
├── Lab4_Camera/
│   ├── main.cpp               ВСЁ приложение (журнал, захват, запись, интерфейс)
│   ├── app.rc                 значок + сведения о файле
│   ├── app.ico                значок окна и трея
│   └── Lab4_Camera.vcxproj    проект (toolset v145, C++17, /utf-8)
├── docs/
│   └── ОТЧЁТ_ЛР4_А5.md        пояснительная записка + контрольные вопросы
└── README.md
```

Сборка даёт `bin\x64\Release\Lab4_Camera.exe`.

## 2. Что нужно для сборки

1. **Visual Studio 2026** с рабочей нагрузкой «Разработка классических
   приложений на C++» (компилятор MSVC + Windows SDK).
2. **OpenCV 4.x для Windows** — установщик `opencv-4.x.x-windows.exe` с
   [opencv.org/releases](https://opencv.org/releases/) (распаковывается в `C:\opencv`).
3. Переменная окружения `OPENCV_DIR`:

```bat
setx OPENCV_DIR "C:\opencv\build"
```

(после `setx` перезапустите Visual Studio, чтобы она увидела переменную).

## 3. Сборка и запуск

1. Открыть `Lab4_Camera.sln` в Visual Studio 2026.
2. Выбрать конфигурацию **Release | x64** (или Debug | x64).
3. `Ctrl+Shift+B` — сборка, затем `F5` — запуск.

Результат: `bin\x64\Release\Lab4_Camera.exe`, DLL OpenCV копируются туда же
автоматически (Post-Build Event).

### 3.1. Как задать `OpenCvDir` / `OpenCvVersion` (своё свойство)

Это **пользовательские свойства MSBuild**, поэтому в окне
«Свойства проекта» (Project Properties) их не видно — там только встроенные
настройки компилятора и линковщика. Задать их можно четырьмя способами.

**Способ 1 — системная переменная окружения (проще всего, ничего не правим):**

```bat
setx OPENCV_DIR "C:\opencv\build"
setx OPENCV_VERSION "4120"
```

Перезапустить Visual Studio. Проект сам подхватит оба значения: в `.vcxproj`
стоит `$(OPENCV_DIR)` и `$(OPENCV_VERSION)` как первый приоритет.

**Способ 2 — правка самого проекта** (файл `Lab4_Camera\Lab4_Camera.vcxproj`):

```xml
<PropertyGroup Label="OpenCV">
  <OpenCvDir Condition="'$(OPENCV_DIR)' != ''">$(OPENCV_DIR)</OpenCvDir>
  <OpenCvDir Condition="'$(OpenCvDir)' == ''">C:\opencv\build</OpenCvDir>   <!-- ← свой путь -->

  <OpenCvVersion Condition="'$(OPENCV_VERSION)' != ''">$(OPENCV_VERSION)</OpenCvVersion>
  <OpenCvVersion Condition="'$(OpenCvVersion)' == ''">4120</OpenCvVersion>   <!-- ← своя версия -->
</PropertyGroup>
```

В Visual Studio: правый клик по проекту → **Unload Project** → правый клик →
**Edit Project File** (или просто открыть `.vcxproj` в Блокноте/VS Code).

**Способ 3 — отдельный property sheet (не трогает проект, удобно для чужого ПК):**

1. `View → Other Windows → Property Manager` (в VS 2026: `View → Property Manager`);
2. правый клик по конфигурации (**Release | x64**) → **Add New Project Property Sheet…**;
3. назвать, например, `opencv.props`, сохранить в папке проекта;
4. открыть его в редакторе и вписать внутрь `<Project>`:

```xml
<?xml version="1.0" encoding="utf-8"?>
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup Label="UserMacros">
    <OpenCvDir>C:\opencv\build</OpenCvDir>
    <OpenCvVersion>4120</OpenCvVersion>
  </PropertyGroup>
</Project>
```

Property Sheet подключается *после* самого проекта, поэтому его значения
перекрывают значения по умолчанию (условия `Condition` в проекте при этом
остаются в силе — если `OPENCV_DIR` не задана, победит `.props`).

**Способ 4 — из командной строки, без правок вообще:**

```bat
msbuild Lab4_Camera.sln /p:Configuration=Release /p:Platform=x64 ^
        /p:OpenCvDir="C:\opencv\build" /p:OpenCvVersion=4100
```

Свойства из `/p:` имеют наивысший приоритет.

**Проверка, что применилось.** При сборке первым делом печатается строка

```
OpenCV: каталог = C:\opencv\build, версия библиотеки = opencv_world4120.lib
```

и, если такого `.lib` нет в `x64\vc*\lib`, выводится предупреждение с
подсказкой — это быстрее, чем ловить `LNK1104` на этапе линковки.

### Если OpenCV другой версии

В `Lab4_Camera.vcxproj` есть два свойства — поправьте их под свою сборку:

```xml
<OpenCvDir Condition="'$(OpenCvDir)' == ''">C:\opencv\build</OpenCvDir>
<OpenCvVersion Condition="'$(OpenCvVersion)' == ''">4100</OpenCvVersion>
```

`OpenCvVersion` — это версия в имени библиотеки `opencv_world<версия>.lib`:

| Версия OpenCV | `OpenCvVersion` | папка lib |
|---|---|---|
| 4.8 | `480` | `x64\vc16\lib` |
| 4.9 | `490` | `x64\vc16\lib` |
| 4.10 | `4100` | `x64\vc16\lib` |
| 4.11 / 4.12 | `4110` / `4120` | `x64\vc16\lib` |
| 5.0 | `5000` | `x64\vc17\lib` |

Скачать: `opencv-<версия>-windows.exe` со страницы
<https://github.com/opencv/opencv/releases> (или с opencv.org/releases) —
это самораспаковывающийся архив, распаковывать в `C:\opencv`, чтобы получилось
`C:\opencv\build\include` и `C:\opencv\build\x64\vc16\lib`.

> Для этой работы достаточно **OpenCV 4.x** (проверялось на 4.10/4.12).
> OpenCV 5.0 тоже подходит (нужен C++17, у нас он включён), но если не хотите
> разбираться с изменениями API — ставьте 4.12.

Если ставили OpenCV через **vcpkg**, вместо этого проще заменить в проекте
инклюды/библиотеки на vcpkg-интеграцию (`vcpkg integrate install`).

> Проект настроен на toolset `v145` (VS 2026). Если открываете в VS 2022 —
> Properties → General → Platform Toolset → `v143`.

## 4. Соответствие заданию (вариант А5)

| Требование | Реализация в `main.cpp` |
|---|---|
| Главное окно со всеми элементами базового монитора | класс `App`: предпросмотр, панель устройства, панель сохранения, журнал |
| Предпросмотр в реальном времени | поток `Camera::Run` + `StretchDIBits` в `App::Paint` |
| Информация: устройство, разрешение, FPS, формат | блок «Информация» (обновляется раз в секунду) |
| Снимок / запись видео | `TakeSnapshot()`, `StartRecording()` / `StopRecording()` (`Recorder`) |
| Каталог сохранения | поле каталога + `Обзор…` (`SHBrowseForFolderW`), запоминается в реестре |
| Индикатор состояния камеры | цветной кружок в шапке: серый / жёлтый / зелёный / красный |
| Журнал с метками времени + «Сохранить отчёт» | `AddLog()`, `SaveReport()`; параллельно пишется `camera_log.txt` |
| **Кнопка «Свернуть в трей»** | `HideToTray()` + `Shell_NotifyIcon(NIM_ADD)` |
| **Глобальная клавиша `Ctrl+Shift+F12`** | `RegisterHotKey` → `WM_HOTKEY` → `TakeSnapshot(true)` |
| **Снимок в фоне, `snapshot_YYYYMMDD_HHMMSS.jpg`** | `UniquePath(...)` + `cv::imwrite` |
| **Balloon tip о сохранении** | `Balloon()` (`NIF_INFO`) |
| **Восстановление окна по двойному клику** | `OnTray()` → `WM_LBUTTONDBLCLK` → `Restore()` |
| **Журналирование фоновых операций** | уровень `Background` (в журнале выделен синим) |
| Автообновление списка камер | `WM_DEVICECHANGE` → `RefreshDevices()` |

Перечисление камер — через DirectShow (`ICreateDevEnum` + `IPropertyBag`),
индексы совпадают с порядком, который использует OpenCV-бэкенд DirectShow.
Захват кадров идёт в отдельном потоке, GUI только забирает копию последнего
кадра — интерфейс не «замерзает» на чтении.

## 5. Работа с приложением

1. Запуск. При первом обращении Windows спросит доступ к камере
   (**Параметры → Конфиденциальность → Камера**).
2. Выбрать камеру, разрешение, частоту кадров → **«Подключить камеру»**.
3. **«Снимок»** — JPEG в выбранный каталог, **«Начать запись»** — AVI/MJPEG.
4. **«Свернуть в трей»** — окно исчезает, камера продолжает работать.
5. `Ctrl+Shift+F12` из любого приложения — снимок + всплывающее уведомление.
6. Двойной щелчок по значку в трее — окно возвращается;
   правый клик — меню (показать окно / снимок / выход).
7. **«Сохранить отчёт»** — `report_YYYYMMDD_HHMMSS.txt` с полным журналом.

### Файлы результатов

```
<каталог сохранения>\
    snapshot_20261029_183012.jpg   снимки
    video_20261029_183500.avi      видеозаписи (MJPEG)
    camera_log.txt                 журнал (дописывается построчно, UTF-8)
    report_20261029_190000.txt     отчёт
```

Каталог по умолчанию: `%USERPROFILE%\Pictures\CameraLab4`.

## 6. Про оформление

Все цвета и отступы собраны в начале `main.cpp` в блоке `namespace ui` —
под стиль ЛР №1 достаточно поменять несколько констант (`kWindow`, `kHeader`,
`kPanel`, `kBorder`, `kAccent`, шрифты в `CreateFonts()`).

## 7. Возможные проблемы

| Симптом | Что делать |
|---|---|
| `LNK1104: не удаётся открыть файл opencv_world4100.lib` | Не задан `OPENCV_DIR` или неверный `OpenCvVersion` (см. п.3) |
| Камера не найдена | Занята другим приложением / запрещён доступ в «Конфиденциальность → Камера»; нажать «Обновить» |
| `Ctrl+Shift+F12` не срабатывает | Комбинацию заняло другое приложение — в журнале будет предупреждение |
| Видео не открывается | AVI/MJPEG требует чётных размеров кадра — они приводятся автоматически; проверьте права на каталог |
| Кириллица в пути | Путь передаётся в OpenCV как UTF-8, при отказе — в системной ANSI-кодировке |
