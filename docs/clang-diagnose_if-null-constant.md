# `diagnose_if` rejects `p != NULL` in C, and accepts `p != 0`

## Summary

In C, a late-parsed attribute condition that compares a pointer against a null
pointer constant spelled `(void *)0` is rejected as never constant. The same
comparison against `0` is accepted, and the same code in C++ is accepted.

Since `<stddef.h>` defines `NULL` as `((void *)0)`, this rejects the spelling a
C programmer would reach for first:

```c
void f(int *p) __attribute__((diagnose_if(p != NULL, "null", "warning")));
```

```
error: 'diagnose_if' attribute expression never produces a constant expression
note: this conversion is not allowed in a constant expression
```

The note points at the cast, not at the parameter, which is the useful clue: it
is the pointer conversion that is refused, not the dependence on `p`.

## Reproducer

```c
void a(int *p) __attribute__((diagnose_if(p != ((void *)0), "m", "warning"))); /* error */
void b(int *p) __attribute__((diagnose_if(p != (void *)0,   "m", "warning"))); /* error */
void c(int *p) __attribute__((diagnose_if(p != 0,           "m", "warning"))); /* ok */
void d(int *p) __attribute__((diagnose_if(!p,               "m", "warning"))); /* ok */
```

```
clang -fsyntax-only -std=c99 min.c
```

## Affected

- Apple clang 21.0.0 (clang-2100.1.1.101), arm64 macOS
- clang 24.0.0git from a recent trunk checkout, same host

Every `-std=` from c89 to c17 behaves the same.

## Scope: C only, and not specific to `diagnose_if`

`enable_if` rejects the identical condition, so this is the C
constant-expression path rather than the `diagnose_if` path. (That is the
opposite of the statement-expression crash in
[`clang-diagnose_if-stmtexpr-crash.md`](clang-diagnose_if-stmtexpr-crash.md),
which `enable_if` handles cleanly.)

| | `p != (void *)0` | `p != 0` |
|---|---|---|
| C, `diagnose_if` | error | ok |
| C, `enable_if` | error | ok |
| C++, `diagnose_if` | ok | ok |
| C++, `diagnose_if(p != nullptr)` | ok | — |

C++ accepts it because a cast to a pointer type is allowed in its constant
evaluator; the C evaluator refuses the conversion outright, before it ever gets
to ask whether the result would fold once `p` is known.

## Why it matters here

`c_contracts.h` lowers every precondition to `diagnose_if`, so this is what a
project sees the moment it writes the most ordinary precondition there is. It
cost a real annotated decoder a build error on the first attempt:

```c
size_t decode(unsigned char *op, ...)
    pre (op != NULL)   /* rejected; had to become  pre (op != 0)  */
```

Nothing in the header can work around it: the condition reaches `diagnose_if`
as the author wrote it, which is the whole point of quoting the clause. The
workaround is to spell the null pointer `0`, and it has to be documented rather
than fixed here.
