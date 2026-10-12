# Дизайн: разрешение merge-конфликтов

Самая важная часть продукта. Цель — сделать разбор конфликтов быстрым и понятным:
трёхпанельный редактор с базой, автоматический разбор тривиальных случаев и
предсказание конфликтов до операции.

Улучшения сверх типичного merge-редактора помечены ★.

## Статус реализации (обновляется по мере работы)

Реализовано:

- **Движок**: конфликты читаются из индекса через `git_index_conflict_iterator`
  (стадии 1/2/3 = base/ours/theirs). `Repository::conflicts()` отдаёт путь, три oid
  и классификацию (`both modified`, `both added`, `deleted by us/them`, …).
- **Слияние**: `Repository::mergeBranch()` — анализ (up-to-date / fast-forward /
  normal), на конфликтах пишет `MERGE_HEAD` и `MERGE_MSG` (libgit2 этого не делает),
  на чистом результате создаёт merge-коммит с двумя родителями.
- **Разрешение**: `resolveConflict(path, content)` пишет файл в рабочее дерево,
  снимает стадии конфликта и стейджит результат. `commitMerge()` и `abortMerge()`
  завершают или откатывают мердж.
- **UI**: `ConflictView` — отдельная страница главного окна (не диалог):
  список конфликтных файлов, панели Ours | Base | Theirs, Result, Save resolution,
  Abort merge, Complete merge.
- **Разрешение по регионам**: `core/conflict_regions` разбирает diff3-текст
  (из `Repository::conflictMarkedContent`, `git_merge_file` со стилем DIFF3) на
  общий текст и регионы; Result рисует по карточке на регион с чипами
  **Ours / Base / Theirs / Both (ours-first) / Both (theirs-first)**. Невыбранный
  регион сохраняет свои маркеры (ничего не теряется), сохранение со маркерами
  отклоняется. Счётчик «conflict N/M», навигация ⟵/⟶ и **F7/F8**, синхроскролл
  трёх панелей (отключаемый). Ручное редактирование — тумблер «Edit text manually».
  Поиск Ctrl+F по панелям. Кнопки Take ours/base/theirs/both задают сторону всем
  регионам сразу.
- Тесты: `test_merge_workflow` (11), `test_conflict_regions` (8, чистый парсер),
  `test_conflict_view` (9, реальный merge-конфликт + оконный путь), включая полный
  цикл конфликт → разрешение → merge-коммит с двумя родителями и откат.
  Headless `--ui-conflict <regions|take|next|prev|select|text|save|resolve>`.

## Целевой UX редактора

### Раскладка

```
┌───────────────────────────────────────────────────────────────┐
│  [файлы с конфликтами ▾]  конфликт 3/12   ⟵ ⟶   [Save] [Stage] │
├──────────────────┬──────────────────┬─────────────────────────┤
│  OURS (HEAD)     │  BASE (ancestor) │  THEIRS (incoming)      │
│  текущая ветка   │  общий предок ★  │  вливаемое              │
│  ├ read-only     │  ├ read-only     │  ├ read-only            │
├──────────────────┴──────────────────┴─────────────────────────┤
│  RESULT (editable)                                            │
│  встроенные маркеры заменены на блоки, каждый с переключателем │
│  [✓ Ours] [ ] Base [ ] Theirs [ ] Both]  + ручная правка       │
└───────────────────────────────────────────────────────────────┘
```

Ключевое: **есть панель BASE**. Это diff3-представление
(`merge.conflictStyle=diff3`), оно резко уменьшает число ошибок при слиянии —
видно, что изменила каждая сторона относительно предка. ★

### Разрешение по хункам

- Каждый конфликт = карточка в RESULT с набором переключателей
  **Ours / Base / Theirs / Both (ours-first | theirs-first)** и свободным
  редактированием текста блока.
- Прокрутка всех панелей синхронна (можно отключить). ★
- Подсветка в Ours/Theirs: «только эта сторона изменилась» — зелёным, «обе» —
  жёлтым (конфликт). Считаем относительно BASE. ★
- Счётчик «конфликт N/M», переход `F7/F8` или стрелками, список слева/сверху.
- Неразрешённые конфликты нельзя застейджить — Save/Stage подсвечиваются, пока
  есть неразобранные блоки.
- **Слово-диф** ★: слова, которые сторона изменила относительно BASE, подсвечены
  в панелях Ours/Theirs и в блоках Ours/Theirs карточек. `model/word_diff`
  выравнивает строки (строчный LCS), затем диффит слова в парах
  (`emphasisAgainstReference`); `renderHighlightedLine` накладывает диапазоны
  поверх синтаксических цветов. Пустая база (add/add) — кросс-сравнение сторон;
  слишком большой файл (кап таблицы выравнивания) — без подсветки. Зелёно-жёлтая
  классификация «кто изменил строку» (только эта сторона / обе) — ещё нет.
- Поиск `Ctrl+F`, word wrap, синтаксическая подсветка, мини-карта (P2).

### Файловый уровень

- Дерево конфликтующих файлов (из индексных стадий 1/2/3).
- Take Current / Take Incoming по файлу; «разрешить все как Ours/Theirs».
- Для бинарных конфликтов — выбор стороны кнопками, без редактора.

## Движок (как получаем данные)

### 1. Обнаружение

- Источник истины — индекс со стадиями:
  - stage 1 = base, stage 2 = ours, stage 3 = theirs.
- Читаем через **libgit2** `git_index_conflict_iterator` (C API) — отдаёт тройки
  ancestor/ours/theirs на каждый конфликт; либо `git ls-files -u -z` подпроцессом
  как фолбэк.

### 2. Построение трёх версий и блоков

Вариант A (рекомендуемый): **libgit2 `git_merge_file`** или `git merge-file`
на тройке (base, ours, theirs) — получаем слитый результат и список конфликтных
блоков с границами. Это то, что нужно для карточек-переключателей.

Вариант B: если libgit2 не тянем — вызываем `git merge-file -p --diff3` и парсим
маркеры `<<<<<<<`, `|||||||`, `=======`, `>>>>>>>`. Проще, но разбор аккуратнее.

Конфиг `merge.conflictStyle` уважаем (merge | diff3 | zdiff3).

### 3. Разрешение

- Пользователь собирает финальный текст блока → пишем в рабочее дерево →
  `git add <file>` (стейдж).
- Все конфликты разобраны → `git merge --continue` / `git rebase --continue` /
  `git cherry-pick --continue` (в зависимости от операции). Состояние операции
  знаем из наличия `MERGE_HEAD`, `.git/rebase-merge`, `.git/sequencer`.

### 4. Аборт и откат

- `git merge --abort` / `git rebase --abort` / `git cherry-pick --abort`.
- ★ Снапшот рабочего дерева перед входом в редактор (stash-подобный или копия
  в oplog) — чтобы можно было вернуться, даже если abort недоступен.

### 5. rerere ★ (сделано)

- ✅ `RerereOps` (CLI, libgit2 rerere не умеет): `enabled/autoupdate` — `git config --bool
  --get`; `statusPaths()/remainingPaths()` — `git rerere status`/`remaining` (пусто, если
  rerere выключен); `apply()` — `git rerere`; `forget(path)` — `git rerere forget <path>`.
- ✅ Тумблеры `Branch ▸ Remember conflict resolutions (rerere)` и `…and stage them
  automatically` (`rerere.enabled`/`rerere.autoupdate`, per-repo; единственный источник
  истины — git-конфиг, тумблеры перечитывают его при открытии репозитория). Палитра
  `branch.rerere`/`branch.rerere-autoupdate`.
- ✅ В `ConflictView`: если git применил записанное разрешение в рабочее дерево, а индекс
  всё ещё конфликтен (`status`/`remaining` пусты, маркеров на диске нет) — кнопка
  **«Stage recorded resolution»**; если конфликт только отслеживается — «Reuse recorded
  resolution»; `Forget recorded resolution` в контекстном меню файла (переопределение).
  Headless `--ui-rerere <enable|disable|autoupdate-on|autoupdate-off|status|apply|forget> [path]`.

## E5: предсказание конфликтов ДО операции ★ (сделано)

Полностью локально, без сервера и учётной записи.

- ✅ Движок: `Repository::previewMerge` зовёт `git merge-tree --write-tree --name-only
  --no-messages <ours> <theirs>` (git ≥ 2.38). Вывод: первая строка — id дерева,
  дальше конфликтные пути; при отсутствии настоящего слияния stdout пуст и
  `MergePreview::available = false`. Работает **не трогая рабочее дерево и индекс**.
  `--messages` не используем: строки локализованы; `-z` (NUL) не используем —
  `runGit` читает строки как C-строки и обрезает на NUL.
- ✅ Перед Merge/Rebase (тумблер `Branch ▸ Check for conflicts before merging`,
  per-repo, дефолт выкл.) показываем диалог: «конфликтуют N файлов: список →
  продолжить/отмена». Отмена не трогает рабочее дерево: предсказание идёт **до**
  checkout/merge. Для rebase — один `merge-tree` между `onto` и веткой.
  Headless `--ui-merge-preview <ours> <theirs>`, `--ui-preflight <enable|disable>`.
- Для rebase — предварительный `git merge-tree` между base и каждой вершиной
  диапазона (или через временный worktree в фоне).
- Опция «всегда проверять перед merge» в настройках, per-repo.
- Бонус: кэшируем результат по (base, ours, theirs) — при повторном merge-попытке
  не пересчитываем.

## Интеграция с операциями

| Операция | Обнаружение | Продолжить | Отменить |
|:--|:--|:--|:--|
| merge | `MERGE_HEAD`, стадии индекса | `merge --continue` | `merge --abort` |
| rebase | `.git/rebase-merge` | `rebase --continue` | `rebase --abort` |
| cherry-pick | `.git/sequencer` | `cherry-pick --continue` | `cherry-pick --abort` |
| revert | `.git/sequencer` | `revert --continue` | `revert --abort` |
| stash pop | стадии индекса после pop | `add` + продолжить | `stash` вернуть |

## Что НЕ делаем

- Не пишем свой трёхсторонний merge-алгоритм с нуля на старте — используем
  git/libgit2. Своя реализация возможна позже для семантического merge (F10).
- Не генерируем патчи для бинарников.
- Meld/WinMerge не поддерживаем как встроенные внешние тулзы (совместимость через
  `git config` остаётся).
