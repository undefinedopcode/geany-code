# Geany Plugin Project

## Building

Use the Geany build tool (`mcp__geany__geanycode_build`) for compilation rather than running cmake/make commands directly:

- **build**: `mcp__geany__geanycode_build` with command `build` — runs `cmake --build build`
- **compile**: Use command `compile` for single-file compilation
- **make**: Use command `make` for make-based builds

## User Interaction

Use `mcp__geany__geanycode_ask_user` instead of `AskUserQuestion` when asking the user questions.
