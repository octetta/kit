# kit — Minimal C-friendly Preprocessor

A tiny, dependency-free text preprocessor for C-like code.

Designed for:
- clean syntax (no `#ifdef`)
- simple conditional compilation
- loop unrolling
- small footprint (~single C file)

---

## Features

- `@if / @elif / @else / @endif`
- `@for` loops (unrolling)
- `@define` (int + string)
- `@include`
- `@macro`
- `--trace` debug mode
- file + line error reporting

---

## Example

```c
@define N 2

@for i = 0..N
    buffer[@i] = @i * 2;
@endfor
```

output

```
buffer[0] = 0 * 2;
buffer[1] = 1 * 2;
buffer[2] = 2 * 2;
```

## Build

```
make
```

## Usage

```
./kit ENV=1 DEBUG=0 < input.kit > output.c
```

With include paths:

```
./kit -Iincludes ENV=1 < input.kit > output.c
```

Debug mode:

```
out.c: input.kit kit
  ./kit ENV=1 < $< > $@
```

## Makefile Integration

```
out.c: input.kit kit
  ./kit ENV=1 < $< > $@
```

## Philosophy

- No parsing of C
- Line-based directives only
- Predictable, minimal behavior
- Easy to embed and extend

## Limitations
- Not a full macro language
- No complex expressions in loops
- No token-aware parsing

## License
MIT
