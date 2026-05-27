# B++ Design

## Identity

B++ is clear.

B++ is a normal general-purpose programming language with human-readable syntax.
It should feel simple like Scratch, structured like Ruby or Lua, and strict
enough to catch confusing mistakes early.

## Design Rules

- Prefer clear commands over clever shorthand.
- Prefer one obvious spelling for each common action.
- Keep blocks visible with `end`.
- Avoid hidden magic.
- Make errors explain B++ code.
- Keep punctuation low unless it clearly improves reading.
- Keep B++ native and self-contained.

## Feature Rules

A feature belongs in B++ when it:

- makes programs easier to read,
- removes a common source of beginner confusion,
- helps users check or understand their code,
- works consistently across Windows and Linux,
- fits the existing command-style syntax.

A feature does not belong in B++ when it:

- adds several ways to do the same thing,
- makes behavior surprising,
- exists only because another language has it,
- requires users to understand another language first,
- makes simple code look more complex.

## Syntax Style

B++ statements should read like actions:

```bpp
set score to 10
add 1 to score
say score
```

Blocks should stay visible:

```bpp
if score > 5:
    say "high"
end
```

Loops should say what they do:

```bpp
repeat 3 times as turn:
    say turn
end

for each name in names:
    say name
end
```

## Error Style

B++ errors should point to the bad line and show the expected shape:

```text
[Line 1] Invalid set syntax.
    --> set score 10
    expected: set name to value
```

The compiler should report mistakes in B++ terms.

## Native Direction

B++ now uses the native C compiler path:

```text
B++ -> C -> native executable
```

Native speed should not come from making B++ harder to read. If a low-level
feature needs unclear syntax, the design is wrong. B++ should stay clear first.

## Current Focus

B++ 4.x should finish the native language experience:

- stable syntax,
- syntax checking,
- better errors,
- examples that show real programs,
- public docs,
- reliable Windows and Linux builds,
- native modules.
