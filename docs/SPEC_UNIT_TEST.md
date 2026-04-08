# Spec: Unit Testing for simple_http

## Objective
Implement a suite of unit tests for the `simple_http` library to ensure the correctness of utility functions and core HTTP handling logic. This improves code reliability and prevents regressions.

## Tech Stack
- **C++23**
- **Catch2 v3.13.0** (with `with_main` configuration)
- **xmake** (build system)

## Commands
- **Build tests:** `xmake build unit_tests`
- **Run tests:** `xmake run unit_tests`
- **Rebuild and run:** `xmake build unit_tests && xmake run unit_tests`

## Project Structure
- `test/unit_tests.cpp` → Main unit test file containing all Catch2 test cases.
- `xmake.lua` → Includes the `unit_tests` target and Catch2 dependency.

## Code Style
- Use Catch2 `TEST_CASE` and `SECTION` macros.
- Use `CHECK` for non-fatal assertions and `REQUIRE` for fatal ones.
- Follow existing project naming conventions (e.g., camelCase for functions in `simple_http` namespace).

```cpp
TEST_CASE("Example Test", "[tags]") {
    SECTION("Sub-section") {
        CHECK(true == true);
    }
}
```

## Testing Strategy
- **Utility Functions:**
    - `base64UrlEncode` / `base64UrlDecode`: Verify correctness with normal strings, empty strings, and special characters.
    - `toLower`: Verify case conversion logic.
    - `isHttp2`: Verify detection of HTTP/2 preface.
    - `LogLevel`: Verify `toString` conversion.
- **Core Logic:**
    - `HttpRequestReader`:
        - URI parsing (path and query separation).
        - Header management (case-insensitive retrieval using lowercase keys).
- **Constants:**
    - Verify `mime` type strings and server versions.

## Boundaries
- **Always:** Use lowercase header names when calling `reader.setHeader()` to ensure compatibility with `getHeader()`.
- **Always:** Run tests before proposing changes to core utility functions.
- **Ask first:** Before adding new testing dependencies or changing the framework.
- **Never:** Modify the core library (`include/simple_http.h`) just to satisfy test cases without explicit approval.

## Success Criteria
- All tests pass in the `unit_tests` target.
- Tests are reproducible via `xmake`.
- 100% pass rate for the current set of 8 test cases (43 assertions).
