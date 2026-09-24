# blaeck support

Before opening an issue:

1. Check the [README](README.md) and the guide for
   [signals](docs/signals.md), [commands](docs/commands.md),
   [configuration](docs/configuration.md) or [networking](docs/network.md).
2. For frame layouts, check the
   [protocol documentation](https://sebajost.github.io/blaeck-protocol/).
3. Open the nearest example under *File > Examples > blaeck*. Start with Basic or
   Commands; WaveformGenerator combines signals, commands, state channels and events.

If those do not answer the question, open a
[new issue](https://github.com/sebaJoSt/blaeck/issues/new).

## Reporting a problem

Include what you expected, what happened, and:

- The blaeck version or commit, board, board-core version and build tool.
- Whether you use Serial or TCP. For TCP, include the networking library and version.
- The smallest sketch that still shows the problem, with credentials removed.
- Full compiler output for build failures.
- The host application and version for communication problems: Loggbok, blaecktcpy
  or your own program.

For missing signals, commands or channels, include the output from this at the end
of `setup()`, after registration:

```cpp
device.printRejections(&Serial);
```

It prints rejection counts and table capacities, not just "table full" errors.
Invalid declarations and insufficient memory can also cause rejections.

For detailed diagnostics, enable a debug stream before registering entries. For
example, with an already-open Serial data port and a separate Serial1 debug port:

```cpp
Serial1.begin(115200);
device.begin(Serial).withDebugStream(&Serial1);
```

Use that on the sketch's existing `begin()` call, not as an additional initialization.
Use a debug port available on your board. On boards with only one Serial port,
`printRejections(&Serial)` at the end of setup is a useful starting point.

For TCP transport failures, also include `transportError()` or output from
`device.printTransportError(&Serial)` after `read()` or `tick()`. A TCP terminal cannot
show an allocation failure that prevented the terminal from connecting.
