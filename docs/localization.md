# Localization

Text is looked up by key, such as `#MainMenu.NewGame`. Translations are
standard GNU gettext `.po` files, so any PO editor (Poedit, Weblate, Crowdin,
Lokalize, ...) works on them. The code lives in `engine/localization.h`.

## Files

All localization files live in the game's content folder, one per language:

```
game/content/localization/
    en.po       source language: the text for every key
    pl.po
    pt_BR.po
```

- The file name is the language code: `pl.po`, `pt_BR.po` (`pt-BR.po` works
  too). Codes are case-insensitive.
- Packaging puts the folder in `content/game.pak` along with the rest of the
  content. The engine finds the files in loose content and in mounted paks.
- Files must be UTF-8. A file declaring another charset in its `Content-Type`
  header is rejected (convert it with `msgconv -t UTF-8`).

## Entry layout

`msgid` is the key and `msgstr` the text in that file's language. Every
language, English included, has its own file with the same keys:

```po
# en.po
msgid "MainMenu.NewGame"
msgstr "New Game"

msgid "Inventory.Apples"
msgid_plural "Inventory.Apples"
msgstr[0] "{0} apple"
msgstr[1] "{0} apples"
```

```po
# pl.po
msgid "MainMenu.NewGame"
msgstr "Nowa gra"

msgid "Inventory.Apples"
msgid_plural "Inventory.Apples"
msgstr[0] "{0} jabłko"
msgstr[1] "{0} jabłka"
msgstr[2] "{0} jabłek"
```

A plural entry repeats the key in `msgid_plural`, because standard PO tools
reject `msgstr[n]` without it. The other files don't contain the English
text. Use the [Localization Editor](#localization-editor) to see languages
side by side.

- Keys: a letter, then letters, digits, `.`, `_` or `-` (`HUD.Ammo_Count`).
  A leading `#` in `msgid` is allowed and ignored.
- Entries with a `msgctxt` are ignored and logged.
- Plurals use the header's `Plural-Forms` expression. Any gettext formula
  works, e.g. Polish's 3 forms.
- `#, fuzzy` and empty `msgstr` entries count as untranslated.
- `#~` obsolete entries are skipped. `#:`, `#.`, `# ...` and `#|` comments are
  ignored.
- Parse errors are logged with the file and line, and that language isn't
  used.

## Lookup and fallback

`Tr("#MainMenu.NewGame")` returns, in order:

1. the text in the active language's file
2. the text in the source language's file (`en.po`)
3. `#MainMenu.NewGame` itself. The missing key is logged once per language.

So a key that's missing or untranslated shows English text, and a key with no
English text is easy to spot on screen.

## Choosing the language

The player's language is a game setting:

```ini
; config/DefaultGameSettings.ini (project default), saved per player
[GameSettings]
Language=auto
```

- `auto` uses the first of the OS preferred languages that has a `.po` file
  (`SDL_GetPreferredLocales`). If none matches, it uses the source language.
- A region falls back to its base language: `pl_PL` uses `pl.po` when there is
  no `pl_PL.po`.
- `-language=pl` on the command line overrides the setting for one run and is
  never saved.

From a settings menu:

```cpp
GameSettings* settings = GetGameSettings();
for (const std::string& code : LocalizationGetAvailableLanguages())   // "en", "de", "pl", ...
    AddLanguageOption(code);
settings->SetLanguage("pl");
settings->ApplySettings(false);   // loads pl.po, reloads UI documents, saves
```

The source language (the fallback) defaults to `en`. To use another one,
call `LocalizationSetSourceLanguage("pl")` before `EngineStart`.

## C++ API

```cpp
#include "localization.h"

Tr("#MainMenu.NewGame");                    // the '#' is optional here
TrPlural("#Inventory.Apples", count);       // msgid_plural / msgstr[n] entry
LocFormat(TrPlural("#Inventory.Apples", count), {std::to_string(count)});
LocFormat(Tr("#Combat.Kill"), {killer, victim});   // "{0} killed {1}"; translators may reorder {n}
LocalizationHasKey("#MainMenu.NewGame");
```

Results are copies, so they stay valid after a language change. Lookups are
main-thread only.

Text you built with `Tr()` and stored somewhere won't change when the language
does. Rebuild it in `EngineApp::onLanguageChanged`, or compare
`LocalizationGetGeneration()` with the value you saw last time.

## UI (RmlUi)

A text node, `<title>` or `<textarea>` whose whole content is a key reference
is replaced by its translation as the document loads. Whitespace around the key
doesn't matter:

```html
<head><title>#MainMenu.Title</title></head>
<button onclick="new_game">#MainMenu.NewGame</button>
<p>
    #HUD.Score
</p>
```

- Other text is left as it is, so `<p>New Game</p>` and `<p>#1 Player</p>`
  aren't touched.
- Data bindings work inside translations. Translators can move them, but must
  not rename them:
  `Score: {{score}}` in `en.po` → `Wynik: {{score}}` in `pl.po`.
- One key per text node. `Press <b>#Input.Start</b>` works because the
  `<b>` contents are their own node. Prefer one key for a whole sentence.
- Attribute values (`placeholder`, `value`, ...) are not translated. Set them
  from C++ with `Tr()`.

When the language changes, the engine reloads every document loaded from a file
(`UiSystem::ReloadDocuments`) and then calls `EngineApp::onLanguageChanged`.
`Rml::ElementDocument*` pointers from before the change are invalid at that
point. Data models live on the context and survive. Documents loaded with
`LoadDocumentFromMemory` aren't reloaded. Rebuild those in the callback.

In debug builds, F7 also re-reads the `.po` files.

## Localization Editor

The Localization tab in Forge (source in `tools/forgeMapEditor/src/modules/localization`)
edits every language of a project at once. Open it with **Window > Localization**, or open
any `.po` file of the project. It works on Forge's current project and reloads when you
switch projects. Scan folders and the source language are under
**Localization > Localization Settings**; **Add Key** is Ctrl+Shift+N.

- **Table:** one row per key, one column per language. Pick the columns in
  **Languages** and drag the headers to reorder them, e.g. English, Polish,
  German and French side by side. The choice is remembered per project.
  Untranslated or missing cells are red and fuzzy ones are yellow. Each header
  shows how much of that language is translated.
- **Editing:** type straight into a cell, or use the **Details** panel. The
  panel shows every visible language for the selected key, including all
  plural forms (labelled with example numbers such as `n = 2, 3, 4…`), the
  fuzzy flag, a note for translators (`#.`), and where the key is used. Rename
  a key there and it changes in every file. Toggling "Plural forms" there
  turns a key into a plural entry in every language. Changing the source
  language's text marks existing translations fuzzy, so they get reviewed.
  Everything can be undone.
- **Filters:** search keys and visible text. "Needs work", "Untranslated" and
  "Fuzzy" look at the visible languages. "Unused in project" lists keys no
  scanned file references. F3 jumps to the next key that needs work.
- **Missing Keys:** on open (and on F5) the editor scans the project folder,
  i.e. where the `.kproject` is, so content and C++ source alike:
  - C/C++ files: `Tr("#Key")`, `TrPlural("#Key", n)` and
    `LocalizationHasKey("#Key")` calls with a string literal key. Set more
    function names in **Localization > Localization Settings**.
  - `.rml`: text nodes and `<title>`s that are exactly `#Key`, the same rule the
    engine uses.
  - Other text files (maps, configs, scripts): `#Some.Key` with at least one
    dot.

  Keys that no `.po` file defines are listed with their file and line. Add
  them to the source language from there.
- **Languages:** **Project > Add Language** creates `<code>.po` with every key
  and the right `Plural-Forms` for common languages.
- **Saving** writes only the files that changed. Comments, references, flags
  and obsolete entries are kept. Each file is parsed with the engine's own
  parser (`engine/po_catalog.cpp`) first, so the editor never writes a file
  the game would reject.

Skipped while scanning: `.git`, `.vs`, `.vscode`, `out`, `build` and `bin`
folders, binary files, `.po`/`.pot` and style sheets (`.rcss`).

## Fonts

The default UI font (LatoLatin) covers Latin scripts only. For Cyrillic, Greek,
CJK and so on, add a font to `FallbackFonts` in `config/DefaultUi.ini`
(see [UI System](ui-system.md#project-configuration)).
