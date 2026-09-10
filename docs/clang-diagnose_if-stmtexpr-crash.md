# clang crashes on a statement expression inside `diagnose_if`

## Summary

`__attribute__((diagnose_if(...)))` segfaults the parser when its condition
contains a GNU statement expression. `enable_if`, which is also late-parsed,
produces a clean diagnostic instead, so this is specific to the `diagnose_if`
path rather than to late attribute parsing in general.

## Reproducer

```c
void f(int n) __attribute__((diagnose_if(({n>0;}), "m", "warning")));
```

```
clang -fsyntax-only -std=c99 min.c
```

## Affected

- Homebrew clang 22.1.8 (arm64 macOS)
- clang 24 built from a recent trunk checkout, same host

Crashes in C and in C++. A declaration inside the statement expression is not
required: `({n>0;})` is enough. Removing the statement expression
(`diagnose_if(n>0, ...)`) is fine, and a statement expression in ordinary
function-body code is fine.

## Stack (trunk build, release)

```
1.  min.c:1:44: current parser token 'n'
 #5 clang::Parser::ParseCompoundStatementBody(bool)
 #6 clang::Parser::ParseCompoundStatement(bool)
 #7 clang::Parser::ParseParenExpression(...)
 #8 clang::Parser::ParseCastExpression(...)
 #9 clang::Parser::ParseRHSOfBinaryExpression(...)
#10 clang::Parser::ParseAssignmentExpression(...)
#11 clang::Parser::ParseGNUAttributeArgs(...)
#12 clang::Parser::ParseLexedAttribute(...)
#13 clang::Parser::ParseDeclGroup(...)
```

## Likely cause

`diagnose_if` sets `ParseArgsInFunctionScope`, so its argument is re-lexed
later, in the function's prototype scope, via `ParseLexedAttribute`. That
context has the parameters in scope but no function *body* context.
`ParseCompoundStatementBody` appears to depend on state that only exists while a
real function body is being parsed, and dereferences it unconditionally.

## Contrast

```c
void g(int n) __attribute__((enable_if(({int r=0; r>n;}), "m")));
```

diagnoses rather than crashing, even though `enable_if` is late-parsed too.

## Why it was hit

Binding a name to a function's own return type inside a `diagnose_if` condition
requires a declaration, and the only expression-level way to declare something
in C is a statement expression. Real use, not fuzzing.
