# Writing comments

Comments in `src` serve two readers: someone writing a sketch and someone changing
the library.

## Public API documentation

An editor shows these comments when a sketch author hovers a call.

- Start with what the call does in one plain sentence. A field says what it holds.
- Add only what the signature does not explain: defaults, limits, ownership, lifetime
  and failure behavior. Include RAM or timing costs only when useful and verified.
- Write for a sketch, not a host implementation. Frame codes and byte layouts belong
  in the [protocol specification](https://sebajost.github.io/blaeck-protocol/).
  Built-in commands such as `<BLAECK.WRITE_DATA>` are fine because users type them.
- Put a warning where the mistake happens, rather than repeating it across the API.
- Keep descriptions consistent with the implementation. Do not describe a warning as
  a compile error or a pointer as a copied value.
- Refer to other methods as calls, such as `writeState(channelName)` or `tick()`.
- Use `device` as the instance name: `Blaeck device;`. Do not give a variable the same
  name as its class; that can interfere with editor completion.

### Format

Use Doxygen `/*!` blocks directly above declarations: `@brief`, explanation where
needed, `@param` and `@return` where useful, then `@note` or `@warning`, and an `@code`
example.

```cpp
  /*!
    @brief   Copies the name of a select command's option at a given position.

    @param   command  The select command's name.
    @param   index    Position in the withOptions() list, starting at 0.
    @param   out      Where the name is copied. Left empty if this returns false.
    @param   outSize  Size of out, including the terminator.
    @return  False if the command is not a select, the index is past the end, or the
             name does not fit. A name is never cut short.

    @code
      char name[12];
      device.getSelectOptionNameAt("SET_WAVE", waveIndex, name, sizeof(name));
    @endcode
  */
```

Keep type-only overload descriptions consistent. Give overloads with different behavior
their own explanation. Check the hover on the overload a sketch actually calls;
a comment above one declaration is not guaranteed to appear on its siblings.

### Examples

Every new public name should have an example showing how a sketch uses it.

- Write complete statements, not a loose `.withRange(...)`.
- Include the relevant declaration or make its prerequisite clear.
- Use the current reference-based API: `device.begin(Serial)` or
  `device.begin(server)`, after starting the supplied stream or server.
- Use `F()` for strings where that overload is supported.
- Keep examples small. Show the feature being described, not a complete application.

`checkdocs.py` checks coverage and extracts examples for the Check API Docs workflow,
which compiles them on Mega. Shared sketch values and handlers belong in
`extras/tests/DocCodeBlocks/preamble.h`; add only the prerequisites an example needs.
Deleted methods are excluded from coverage.

## Implementation comments

Comment only what the code does not explain. Prefer a short explanation of why over
a description of what each statement does.

Investigation history and rejected approaches belong in commit messages, not source
comments. Avoid duplicating protocol tables or values already defined elsewhere.

## Source-only checks

From the repository root:

```powershell
python extras\scripts\checkprototype.py
python extras\scripts\syncnetwork.py --check
python extras\scripts\checkdocs.py src\Blaeck.h -- -Iextras\tests\host
```

These check structure, packaging, synchronized networking tabs and documentation
coverage. The documentation checker requires `libclang` and compatible C++ headers;
see [CONTRIBUTING.md](../CONTRIBUTING.md) for setup and extraction.
None of these commands compiles examples or establishes runtime correctness.
