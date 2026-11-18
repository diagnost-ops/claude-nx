# Claude для Nintendo Switch (claude-nx)

Homebrew-приложение (.nro): чат с Claude через Anthropic API. Русский текст, экранная клавиатура, прокрутка стиком и касанием, история чата сохраняется на карте.

## Установка
1. Скопируй `claude-nx.nro` в `sdmc:/switch/`.
2. Запусти через Homebrew Menu (лучше из игры с зажатой R — полный режим).
3. Нужен API-ключ Anthropic (https://console.anthropic.com → API Keys, оплата отдельно от подписки Claude.ai).
   Нажми **Y** в приложении и введи ключ, либо впиши его в `sdmc:/switch/claude/config.txt` (`api_key=sk-ant-...`) — файл создаётся при первом запуске.

## Управление
| Кнопка | Действие |
|---|---|
| A | написать сообщение |
| X | новый чат |
| Y | ввести/сменить API-ключ |
| Крестовина / стики / касание | прокрутка |
| L / R | прокрутка на страницу |
| + | выход |

## Настройки (`config.txt`)
- `model=` — модель (по умолчанию `claude-sonnet-5-5`)
- `max_tokens=` — максимальная длина ответа
- `system=` — системный промпт
- Свой шрифт: положи `font.ttf` в `sdmc:/switch/claude/`.

## Сборка
devkitPro + `switch-sdl2 switch-sdl2_ttf switch-curl`, затем `make`. Или через GitHub Actions (`.github/workflows/build.yml`) — готовый .nro появится в Artifacts.
