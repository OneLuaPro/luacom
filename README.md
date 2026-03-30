# LuaCOM (OneLuaPro Edition)

This repository is a **consolidated super-fork** of the original [davidm/luacom](https://github.com). It merges the most critical advancements, bug fixes, and modernizations from across the entire GitHub fork ecosystem into a single, definitive codebase compatible with modern Lua environments.

## Consolidation Overview

This version was built by systematically analyzing the fork tree and merging the most active and advanced branches. The goal was to unify fragmented improvements that existed in isolation for years.

### Integrated Forks & Contributions
The following "ahead" branches have been merged into this master:

*   **[Eunsolfs/luacom53](https://github.com) (+67 commits):** Major Lua 5.3/5.4 support, 64-bit integer handling, and modern type conversion.
*   **[fiendish/luacom](https://github.com) (+42 commits):** Critical stability patches, memory leak fixes (especially in SAFEARRAY handling), and refactoring using smart pointers.
*   **[moteus/luacom](https://github.com) (+35 commits):** The modern architectural foundation for Lua 5.2/5.3 compatibility and Appveyor CI integration.
*   **[udbg/luacom](https://github.com) (+1 commit):** Added modern **xmake** build system support.
*   **[shere-avintec/luacom](https://github.com) (+1 commit):** CI/CD environment optimizations.
*   **[JoshuaTiffany/luacom](https://github.com) (+1 commit):** Metadata and build-info updates.

## Key Features & Improvements

-   **Broad Compatibility:** Support for Lua 5.1, 5.2, 5.3, and 5.4.
-   **64-Bit Support:** Proper handling of 64-bit integers and modern Windows architectures.
-   **Memory Safety:** Fixed several severe memory leaks in SAFEARRAY decoding and Connection Points.
-   **Smart Pointers:** Internal refactoring to use `tCOMPtr` (Smart Pointers) for more robust COM reference counting.
-   **Modern Build Systems:**
    -   **xmake:** Native support via `xmake.lua`.
    -   **CMake/LuaRocks:** Updated rockspecs for modern deployment.
-   **New Methods:** Added `luacom.ReleaseComObject(obj)` for manual reference control when dealing with non-standard COM servers.
-   **Unicode/Codepage:** Improved UTF-8 and ANSI codepage handling.

## Fork Lineage
As identified by an automated fork-tree analysis, the fragmented state of the project prior to consolidation is illustrated below. Many isolated improvements, which had never been combined, were found to be contained within these forks.
```txt
davidm/luacom
|-- JoshuaTiffany/luacom (+1)
|-- udbg/luacom (+1)
|__ moteus/luacom (+35)
    |-- cybercode3/luacom (+35)
    |-- littleboss01/luacom (+35)
    |-- fiendish/luacom (+42)
    |-- chenlia2013/luacom (+30)
    |-- Eunsolfs/luacom53 (+67)
    |-- Socol111/luacom (+30)
    |__ shere-avintec/luacom (+31)
```

Through the systematic resolution of conflicts and the merging of these branches, this unified 'Super-Fork' was created. The consolidated logic employed to build this definitive repository is represented in the graph below:

``` txt
davidm/luacom (Original)
|-- JoshuaTiffany/luacom (+1)   [MERGED]
|-- udbg/luacom (+1)            [MERGED]
|__ moteus/luacom (+35)         [MERGED]
    |-- fiendish/luacom (+42)   [MERGED]
    |-- Eunsolfs/luacom53 (+67) [MERGED]
    |-- shere-avintec (+31)     [MERGED]
    |-- (others: cybercode3, littleboss01, Socol111, chenlia2013) [UP-TO-DATE]
```

## License
This project follows the original LuaCOM license (MIT/X11). See the [COPYRIGHT](https://github.com/OneLuaPro/luacom/blob/master/COPYRIGHT) file for details.
