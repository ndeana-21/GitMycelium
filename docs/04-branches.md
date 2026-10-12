# Дизайн: работа с ветками и графом

Второй приоритет проекта. Цель — предсказуемое, быстрое управление ветками на
репозиториях, где веток тысячи. Улучшения помечены ★.

## Управление ветками

### Создание / checkout / прочее

- Right-click по коммиту в графе → Create branch here (P0).
- Double-click по бейджу ветки → checkout (P0).
- Rename, Delete (с мультивыбором Shift/Ctrl), Pin/Unpin (P0/P1).
- Right-click по ветке → контекстное меню со всеми действиями: Checkout, Merge
  into current, Rebase current onto this, Rebase this onto…, Create worktree,
  Rename, Delete, Push, Set upstream, Copy name/SHA, Compare with current.
- ✅ ★ **Мультивыбор и групповые операции**: в рейле Shift/Ctrl выделяют несколько
  локальных веток; правый клик внутри выделения даёт групповое меню — «Delete N
  branches…», «Set upstream for N…», «Hide/Show selected», «Solo selected», «Copy N
  branch names». Удаление — **одним** входом в oplog (`RepoSession::deleteBranches`),
  поэтому один undo возвращает весь набор; диалог заранее показывает, сколько веток
  несут коммиты, которых нет ни у кого другого (это согласие, а не сюрприз посреди
  работы). Проверяемая ветка не трогается никогда. `Branch ▸ Delete Merged Branches…`
  (и палитра `branch.delete-merged`) удаляет всё, что уже влито в target
  (`Repository::branchMergedInto(name, base)` — «merged» спрашивается у target, а не
  у HEAD); `branch.delete-selected` / `branch.set-upstream-selected` работают с
  выделением, headless — `--ui-branch-group <delete|delete-merged|set-upstream|
  unmerged|hide|solo>`. Тест `test_branch_group` (9, реальные репы + bare-remote).
- ★ Мультивыбор и групповые операции: массовое удаление, массовая установка
  upstream, «удалить все ветки, уже влитые в main» (защита от случайного
  удаления — подтверждение со списком).

### Видимость и шум

- ✅ **Hide/Solo** рефов: скрыть/оставить только выбранные. Реализовано фильтром
  **обхода истории**, а не скрытием метки: скрытая ветка не пушится в revwalk, её
  уникальные коммиты уходят, общая история остаётся (скрывать через
  `git_revwalk_hide` нельзя — он снёс бы и общих предков). Хранится per-repo
  (`model/ref_visibility`), точки входа — контекстное меню ветки и headless
  `--ui-visibility`; скрытые ветки приглушены в дереве, индикатор в статусбаре
  сбрасывает фильтр.
- ✅ **Pin to left** — стабильная ось для main/prod. Один pin даёт два эффекта:
  ветка уходит вверх списка и получает левую дорожку в графе (после HEAD).
- ✅ **фокус на текущей ветке** — режим: текущая ветка, её target (base) и их
  upstream-цепочки; всё остальное прячется. Target резолвится автоматически: default
  branch ремоута (его локальный дубль при наличии), иначе `main`/`master`, иначе HEAD.
  Флаг `smart` хранится per-repo (`RefVisibility`); HEAD и target никогда не фильтуются.
  Точки входа: `Branch ▸ Visibility`, контекстное меню ветки, палитра
  `view.smart-visibility`, headless `--ui-visibility smart|smart-off`.
- ✅ ★ **Фильтры-пресеты видимости** (живые режимы per-repo, не снимки): «мои ветки»
  (по автору tip'а), «активные за 30 дней», «по префиксу» (`feature/`), «без upstream»,
  «впереди/позади upstream». Данные — одним `git for-each-ref` (`git/ref_meta`: автор,
  дата, upstream, ahead/behind); пересчитывается на каждом refresh. `Branch ▸ Visibility`,
  палитра `view.preset.*`, headless `--ui-visibility preset:<id>`.
- ★ **Поиск и палитра веток**: fuzzy-поиск по имени, переход к ветке в графе.

## Граф (движок раскладки)

- Модель — DAG коммитов; lane (колонка) = ветка, merge-коммиты = соединения.
- Инкрементальная укладка: подгружаем историю страницами (например, по 5k
  коммитов), раскладываем в фоне, дорисовываем. UI доступен сразу после первого
  экрана.
- ✅ Ghost branch для каждого коммита — не содержащая, а ближайшая по топологии.
  Бейдж показывается и на текущей строке, не только на hover; **double-click по
  бейджу** = checkout той ветки (тот же сигнал, что и double-click по настоящей
  метке). Когда честного ответа нет — бейджа нет.
- ✅ Hover-подсветка ветки: при наведении на ветку (в графе или в левом дереве)
  яркими остаются её tip и **все предки** в загруженном диапазоне, остальное
  затухает. Обход ограничен сверху (кап), при превышении подсветки нет, а не
  «кусок» её; общая история ярка для обеих веток.
- ★ **Мини-карта графа** — обзор всей истории с навигацией (P2, на монорепах
  indispensable).
- ★ **Лента ветки**: панель «ветка X: ahead N / behind M, список коммитов
  впереди/позади» — быстрый разбор состояния без чтения графа.
- ✅ **Анимации графа** ★: новый коммит/строка — fade-in узла, исчезнувший (drop,
  скрытая ветка, WIP убран) — ghost fade-out на старом месте, чекаут/reset/commit —
  пульс на новом HEAD, операции над ветками — плавный сдвиг узлов по дорожкам (lane-x)
  и по вертикали (снимки строк скользят), новая метка ветки — fade пилюли, долгая
  сетевая операция — бегущая полоса. Движок `graph/graph_animation` (Qt-free: диф
  двух снимков `{oid,row,lane}` + HEAD, фейковые часы, переходы appear/vanish/move/
  pulse/label); вью морфит снимки граф-ячейки поверх строк, делегаты спрашивают
  alpha/«кто владеет строкой». Пагинация и открытие репы не анимируются; в headless
  и `--shot` выключено (детерминизм), тумблер `View ▸ Animate Graph`, headless
  `--ui-graph-animation <commit|checkout|branch>` печатает переходы.

## Merge / Rebase из графа

- ✅ Drag-and-drop ветки на ветку → список операций у бейджа с выбором
  **Merge** / **Rebase** / **Squash** / Checkout, с предсказанием конфликтов (см.
  03-merge-conflicts.md, раздел E5).
- ✅ ★ **Pre-flight перед drag-drop**: список операций показывает строку
  `git merge-tree` — «No conflicts predicted» или «Conflicts in N file(s): …» —
  ещё до нажатия. Строка появляется и в меню-фолбэке (drop из дерева рефов).
  Модальный pre-flight при этом не повторяется: вопрос уже задан в списке.
- Rebase диапазона: Shift-select коммитов → right-click по целевой →
  «Rebase N commits onto …». Rebase на конкретный коммит — из его контекстного меню.
- ✅ **Squash-merge** — `git merge --squash` через бинарь: изменения стейджатся,
  коммит делает читатель вручную. Точки входа: список у бейджа,
  **Branch ▸ Squash Merge…**, палитра `branch.squash-merge`. Конфликты — на
  странице конфликтов; завершение — обычный однородительский коммит. Тумблер в
  Preferences > Merge Behavior — ещё нет, поведение всегда такое.
- После мержа — при конфликтах автоматически открывается merge-редактор.

## Worktrees

- ✅ Создание из контекстного меню ветки/секции; каждый worktree — своё рабочее дерево и
  индекс, общая история. Реализовано через бинарь (`WorktreeOps`: list/add/remove/
  lock/unlock/prune), секция **«Worktrees»** в Left Panel (текущая помечена), контекстное
  меню строки (Open / Lock / Remove / Copy path), double-click = Open, диалог
  **New Worktree…**, `Branch ▸ New Worktree… / Prune Worktrees`, палитра
  `worktree.new`/`worktree.prune`/`worktree.open:<name>`; headless `--ui-worktree`.
- ✅ Open (в текущем табе) — пока **не** в новом табе: мультитабы (A2) ещё не сделаны,
  поэтому открытие перенаправляет всё окно; заголовок помечается `[worktree: …]`.
- ✅ Lock/Unlock; Locked/Prunable видны в списке. Remove + delete branch — отдельным
  шагом (сейчас Remove сохраняет ветку; удалить её — `Delete Branch`).
- ✅ ★ В графе — отдельный WIP-узел на каждый worktree с незакоммиченными изменениями
  (B4b-2). `RepoSession::worktreeWip()` отдаёт по узлу на грязный worktree (текущий первым):
  текущий читается через libgit2, связанный — `git status --porcelain` (второй хендл на ту же
  репу запрещён). `CommitModel::setWorktreeWip` держит их блоком в начале `m_rows`; у каждого
  родитель — HEAD своего worktree, id `//wip` (текущий) / `//wip:<path>`, подпись
  `// WIP · <worktree> · N staged, M unstaged`. `isWipRow`/`WipRole` — по всем узлам; бросок
  ветки, `selectedCommitIds` и контекстное меню их отвергают; выбор чужого узла называет его
  worktree, «Stage All» доступен только для своего. Тест `test_wip_node`
  (`eachDirtyWorktreeGetsItsOwnNode`) + `test_worktrees` (`worktreeWipFollowsEachCheckoutsChanges`).
- ✅ Наследование настроек из исходной репы: pins, скрытые/solo ветки, smart/пресеты,
  layout колонок, auto-fetch и pre-flight ключуются по **общему git-dir**
  (`git_repository_commondir`), поэтому worktree видит те же настройки, что и main.
  Старые записи (ключ — путь checkout) переносятся один раз при открытии, чтобы апгрейд
  не выглядел сбросом. Свёрнутые папки не персистятся вовсе (expansion — состояние
  сессии), так что наследовать нечего.
- ⬜ ★ Индикатор «ветка worktree merged into target» (по локальному графу, без
  облачных PR) — сигнал «worktree можно чистить».

## Submodules

- ✅ Панель **Submodules** в Left Panel: путь, URL, состояние (`in sync` / `out of sync` /
  `not initialized` / `conflict`); вложенные показываются полным путём (`lib/inner`).
  Реализовано через бинарь (`SubmoduleOps`: list/add/update/sync/deinit; рекурсия —
  `--recursive`), `SubmoduleEntry` в `core/types.h`, роли path/url/state, глиф `Submodule`,
  иконка `Icon::Submodule`.
- ✅ Add (URL + путь + ветка), Initialize / Update / Update to remote / Sync URL /
  Deinitialize, Open = `openRepository` вложенной репы; `Branch ▸ Submodules ▸ Add… /
  Initialize All / Update All / Sync All`; палитра `submodule.*`; headless `--ui-submodule`.
  Операции идут через `guarded`, **не oplog** — вложенный checkout и `.git/config` вне
  снимка указателей.
- ✅ `out of sync` — checked-out коммит не совпал с gitlink в индексе родителя;
  закоммитить указатель (`git add <path>`) возвращает `in sync`.
- ✅ gitlink/`.gitmodules` в Commit Panel (B5b): libgit2's status не сообщает смену gitlink,
  поэтому `Repository::status()` считает указатели явно — staged (index ↔ HEAD-дерево через
  `git_diff_tree_to_index`) и unstaged/conflict (`SubmoduleOps::list`, т.е. `git submodule
  status`); без `.gitmodules` бинарь не запускается. `FileChange` получил `submodule`,
  `recordedOid`, `checkedOutOid`; `CommitFilesModel` рисует глиф `Submodule` и tooltip
  `recorded → checked-out`. Стадирование gitlink-пути уходит в `git add <path>`, discard —
  в `git submodule update -- <path>`; `.gitmodules` остаётся обычным файлом. Тесты
  `test_submodules` (13).
- ✅ Auto-update сабмодулей (B-3): per-repo тумблеры `Branch ▸ Submodules ▸ Update after
  fetch/checkout/merge` и `…tracking the remote branch` (off по умолчанию; `QSettings`
  по общему git-dir). После успешных fetch/pull, checkout, завершённого merge/rebase
  `maybeAutoUpdateSubmodules()` запускает `git submodule update --init --recursive
  [--remote]` на воркере (`RepoSession::requestAutoUpdateSubmodules` →
  `submodulesUpdated`), с busy-индикатором графа; провал — в статус-бар (не модал).
  Палитра `submodule.autoupdate`/`submodule.autoupdate-remote`; headless
  `--ui-submodule autoupdate <enable|disable|remote-on|remote-off|status|run>`. Тест
  `test_submodules` (+1: хук возвращает checkout к записанному указателю).

## Синхронизация веток

- Ahead/behind индикаторы в Left Panel и у бейджей в графе.
- Fetch All, Fetch per-remote; Pull с режимом (ff-if-possible / ff-only / rebase).
- Push с правильной обработкой непривязанной ветки (`push --set-upstream`).
- ★ **Force-with-lease** только с визуальным сравнением remote-состояния и
  подтверждением (P2).

## Модель данных (набросок)

```cpp
struct Branch {
    std::string name;      // refs/heads/feature/x
    std::string shortName; // feature/x
    Oid         target;    // вершина
    std::optional<Upstream> upstream;
    uint32_t    ahead = 0, behind = 0;
    bool        hidden = false, soloed = false, pinnedLeft = false;
};

struct GraphLayout {
    std::vector<Lane>   lanes;   // порядок колонок
    std::vector<NodePos> nodes;  // позиция коммита (lane,row) — страницами
    std::vector<Edge>   edges;   // соединения, merge-дуги
};
```

Раскладка кэшируется и перестраивается инкрементально при появлении новых
коммитов (fetch) — пересчитываем только «грязный» хвост.

## Производительность веток

- Тысячи рефов: читаем через `git for-each-ref --format` одним вызовом (не по
  одной ветке), парсим пакетом.
- Раскладка графа — офф-UI-поток; UI рисует то, что уже посчитано.
- Фокус на текущей ветке по умолчанию **включён** на больших репах — ★ с автоподсказкой «репа большая, включаю
  фокусный режим».
