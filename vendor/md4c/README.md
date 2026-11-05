# MD4C

Source: https://github.com/mity/md4c/tree/release-0.5.2
Version: 0.5.2 (release-0.5.2).
License: MIT; see LICENSE.md and the notices in each source file.

Vendored files: src/md4c.c, src/md4c.h, src/entity.c, src/entity.h,
and LICENSE.md from that release.

Local change: initialize label_contents_line_index to zero in
md_is_link_reference_definition() to avoid GCC's -Wmaybe-uninitialized warning
when the parser is compiled with optimization. No other upstream changes.

The parser and entity table compile directly into mdv; no shared library
or package download is needed.
