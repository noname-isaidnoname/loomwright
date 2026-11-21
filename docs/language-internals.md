# Language internals

The user-facing language reference is `loomwright --docs` (`kGuide` and
`api_reference()` in `src/docs.cpp`). This document describes the internal
shape of the language and how to extend it. Keep the two in sync when the
language changes.

## Grammar

A `.loom` file contains exactly one `loom "name" { ... }` block. Top level
accepts only:

- `option name = value`
- `remap key SOURCE to TARGET`
- `gui "title" { ... }`
- `func NAME(params) { ... }`
- `when ... { ... }`

A file needs at least one event block. Blocks use braces, one action per line,
and `#` comments.

### Options

| Option | Values |
| --- | --- |
| `input` | `auto`, `evdev` (currently informational) |
| `output` | `auto`, `ydotool`, `uinput` |
| `compositor` | `auto` (default), `x11`, `wayland` — selects panel backend |
| `panic` | key name or `"none"` |
| `screen` | `auto` or `WIDTHxHEIGHT` |
| `allow_launch` | boolean |

### Events

- `when key NAME goes down/up`
- `when key NAME stays held every DURATION`
- `when mouse BUTTON goes down/up`
- `when mouse BUTTON stays held every DURATION`
- `when timer every DURATION`
- `when app starts`
- `when gui BUTTON clicked`
- `when gui window closes`

Chords use `+` (`CTRL+ALT+T`). The last key is the primary key; the preceding
keys must be physically held when it goes down. `event_key` inside a chord
handler is the primary key.

### Statements

`StmtKind` in `src/ast.hpp`:

`Cursor`, `Mouse`, `Key`, `Consume`, `Type`, `Wait`, `Scroll`, `Log`, `Notify`,
`ClipWrite`, `ClipRead`, `Launch`, `Set`, `If`, `Repeat`, `While`, `For`,
`Break`, `Continue`, `Assert`, `Call`, `Return`, `Send`.

An assignment is `set NAME = EXPR` or `set NAME OP= EXPR` with `OP` one of
`+ - * / %`. The parser records the compound form in `Node::set_op` (`SetOp`).
`Runtime::apply_compound_set()` reads the current value, reuses `apply_binary()`
— the same helper that evaluates expression `Binary` nodes — and writes the
result through `assign_variable()`. A compound assignment to an undefined
variable raises a runtime error.

### Expressions

`Expr::Kind` in `src/ast.hpp`: `Literal`, `Variable`, `Unary`, `Binary`,
`Call`, `List`, `Index`, `Slice`, `Conditional`. Operators are `+ - * / %`,
comparisons, `and`/`or` (also `&&`/`||`), unary `-`/`+`/`not`/`!`, parentheses,
the ternary `COND ? THEN : ELSE`, and the list subscript/slice postfixes. The
ternary is the lowest-precedence operator and right-associative; its `args`
hold `[condition, then, else]`. Both `eval()` and `eval_pure()` evaluate the
condition first and then only the selected arm, so an unselected arm with side
effects or a waiting call never runs.
`propagate_argument_flags()` marks the whole node `may_wait`/not `call_free`
when any of the three children is, which keeps the coroutine and borrow paths
correct.

A subscript `LIST[INDEX]` stays an `Index` node with `[base, index]` in `args`.
When a top-level `:` appears inside the brackets the parser builds a `Slice`
with `[base, start, end]` in `args`; an omitted bound (`LIST[START:]`,
`LIST[:END]`, `LIST[:]`) is stored as a `Literal` holding `none`. A colon that
belongs to a ternary in the first expression is consumed by `parse_expr()`, so
`items[flag ? a : b]` is still an `Index` whose child is a `Conditional`. Both
`eval()` and `eval_pure()` check that the base is a list before evaluating
either bound, evaluate each bound exactly once (a bound may call a function or
wait), then call `apply_slice()`. That helper resolves `none` to a default
(0 for start, list length for end), counts negative bounds from the end, clamps
every bound into `[0, size]` instead of raising "index out of range", returns an
empty list when `start >= end`, and always builds a fresh `ListBox` so the
result is independent of the source list.

## AST and data model

- `Script` — name, path, options, events, remaps, panels, functions.
- `Node` — one statement or event block. Flat fields keep the node
  tuple-style: event fields (`event_kind`, `event_key`, `chord_parts`,
  `edge`, `interval`) and statement fields (`a`, `b`, `c`, `set_op`, `exprs`,
  `has_duration`, `duration`, `body`, `otherwise`, `line`).
- `Expr` — expression tree; `ExprPtr` is a `shared_ptr`.
- `Function` — name, params, body, line.
- `GuiPanel` / `GuiWidget` — persistent panel declarations.
- `Remap` — source and target key/chord.
- `Value` — int/double/bool/string/list/none with `repr`/`str` formatting and
  truthiness. `str()` prints `none`, `true`, `false`, the number, the string
  itself, or a bracketed list; `repr()` additionally quotes and escapes
  strings. Floats use the shortest round-trip text (`1.5`, `1.0`, `1e+16`).
  Falsy values are `none`, `false`, `0`, `0.0`, `""`, and the empty list `[]`;
  everything else is truthy. `ListBox::empty()` means "no list storage", which
  is distinct from an empty list: `Value::of_list({})` has storage with zero
  elements and is falsy. `Value::truthy()` uses `items.values().empty()`.

## Statement dispatch

`Runtime::execute()` (`src/runtime.cpp`) switches on `StmtKind` and handles one
statement. `execute_nodes()` runs a body and propagates `Break`, `Continue` and
`Return` signals. `eval()` is a coroutine because a user-defined function may
wait; it cannot return its result through a plain return value. An expression
that cannot wait is evaluated by `eval_pure()`, and a statement that cannot
wait by `execute_pure()`, so the common path of a script does not allocate a
coroutine frame. The parser marks the expressions and the statements that may
wait. It also marks call-free expression trees, letting binary operators
borrow values without risking a callback invalidating the reference. Variable
nodes resolve `event_key` and `event_edge` at parse time.

The parser resolves every call site to a `Builtin` enumerator once; `Runtime::call_builtin()`
dispatches on the enum. `call_user_function()` binds parameters into a fresh scope, pushes it, runs the
body, and pops it on the way out. A body consisting of one call-free `return`
does not need its own coroutine or a return signal. A longer body can also
return without a signal when its outer block reaches a synchronous `return`;
nested or suspending returns still use the existing unwind path. The outer
user-function await is unchanged. A function can read the script's variables
but its own assignments stay local.

## Builtins

The table in `src/builtins.cpp` is a `constexpr` array sorted by name and is the
single source of truth for names and arity; a `static_assert` guards the sorted
order. `Runtime::call_builtin` implements exactly these ids; anything else is a
user-defined function or an unknown function reported by the checker. Call sites
carry the resolved `Expr::builtin`, so evaluation dispatches on the enum instead of
comparing spellings while a script runs. Current groups:

- Numbers/general: `min`, `max`, `abs`, `round`, `floor`, `ceil`, `sqrt`,
  `pow`, `sign`, `gcd`, `lcm`, `sum`, `avg`, `clamp`, `int`, `float`,
  `is_number`, `str`, `bool`, `len`, `random`, `randint`, `elapsed`.
- Strings: `upper`, `lower`, `trim`, `title`, `reverse`, `starts_with`,
  `ends_with`, `contains`, `replace`, `substr`, `index_of`.
- Lists: `join`, `split`, `append`, `sort`, `reverse`, `first`, `last`, `count`.
- Runtime state: `key_down`, `mouse_held`, `suspend_hotkeys`,
  `hotkeys_suspended`, `set_hotkey_enabled`, `hotkey_enabled`, `screen_width`,
  `screen_height`.
- Windows/dialogs: `active_window_title`, `active_window_class`,
  `active_window_process`, `active_window_pid`, `gui_alert`, `gui_input`,
  `gui_confirm`.
- Panels: `gui_value`, `gui_set_text`, `gui_set_visible`.

## Key names

Key names accept identifiers and numeric tokens, so `when key 1 goes down` and
`key CTRL+1 tap` work. Validation lives in `validate_key_name()`
(`src/keys.cpp`). `canonical_key_name()` upper-cases and resolves aliases;
`canonical_key()` in `src/runtime.cpp` wraps it.

Mouse buttons are `left`, `right`, `middle`, `side`, `extra`
(`find_mouse_button()` in `src/keys.cpp`). Runtime event names carry a `MOUSE_`
prefix (`MOUSE_LEFT`, ...), and `normalize_key_event()` maps `BTN_*` codes to
them, so the left button can never fire a `LEFT` arrow-key binding.

## String decoding

Strings decode `\\`, `\n`, `\r`, `\t`, `\'`, and `\"` in **one pass**. Keep it
single-pass; a chained replace broke `"\\n"`.

## Extension points

### Add a statement

1. Add a `StmtKind` to `src/ast.hpp`.
2. Parse it in `src/parser.cpp`.
3. Handle it in `Runtime::execute()` (`src/runtime.cpp`).
4. Add a checker rule in `src/checker.cpp` when it has static constraints.
5. Document it in `src/docs.cpp` (`kGuide` and `api_reference()`).

### Add a builtin

1. Add the name and arity to the table in `src/builtins.cpp`.
2. Add the `Builtin` enumerator to `src/builtins.hpp` and implement it in `Runtime::call_builtin()`.
3. Give it a row in `src/builtins.cpp` (keep the table sorted by name; the `static_assert` enforces it).
4. Document it in `src/docs.cpp`.

### Add a backend

Implement `warp`, `move`, `button`, `key`, `type_text`, `scroll`, `close`, and
`name` in `OutputBackend` (`src/backends.hpp`), then register it in
`choose_backend()` (`src/backends.cpp`). Read
[implementation-traps.md](implementation-traps.md#backends) first.

## Checker

`src/checker.cpp` validates options against `option_rules()`, key names,
builtin arity, hold-token balance, `break`/`continue`/`return` placement,
remap conflicts, GUI IDs, and warns when `/dev/uinput` is not writable. It runs
before any device is opened. `--check` collects diagnostics and returns 2 on
errors; it never opens input devices or requires permissions.
