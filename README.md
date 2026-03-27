# Pasta Client

Pasta Client is a custom DDNet/TClient-based client project.

This repository is essentially a **pasted/copy-based KRX client base** that has been reworked into a separate project. A lot of functionality is already present, but **not everything is finished**, and some parts are still rough, inconsistent, or buggy.

The current goal of this repo is simple:

- keep a large experimental cheat/client base in one place
- use it as a foundation for further development
- improve stability and behavior over time

## Status

This project is **not polished**.

Things to keep in mind:

- a number of features are unfinished
- some systems are still placeholders or only partially ported
- some features work, but not exactly how they should yet
- bugs, bad edge cases, and broken behavior are expected

So this should be treated as a **base to build on**, not as a clean final product.

## What It Is

Pasta Client currently includes a large amount of experimental functionality around:

- aimbot systems
- avoid/anti-freeze systems
- Fent Bot
- Pilot Bot
- TAS tools
- misc automation
- visuals and HUD features

Some of those systems are already usable, some are still in-progress, and some need major cleanup.

## Important Note

This repo is openly acknowledged as a **copy/paste style KRX-based client base**.

It is not presented as a fully original clean-sheet project.

If you are looking for:

- perfect code quality
- complete feature parity
- bug-free behavior
- production-ready polish

this is not that.

If you are looking for:

- a big client base
- lots of experimental systems already wired in
- a foundation for further cheat/client work

then this repo can still be useful.

## Build

Clone the repository and build it the same way you would build a DDNet/TClient-style client.

On Windows, the usual flow is:

```bat
cmake -S . -B build
cmake --build build --config Release --target game-client
```

The built client binary is typically:

```text
build/DDNet.exe
```

## Current Reality

This project may contain:

- unfinished UI
- half-ported logic
- broken prediction in some features
- unstable behavior in bots
- inconsistent settings behavior

That is expected for now.

## Summary

Pasta Client is a **KRX-style pasted client base** with a lot of experimental features and a lot of unfinished work.

It may be buggy, incomplete, and messy in places, but as a **starting point / base for further cheat development**, it can still be good.
