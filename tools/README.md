# Research tools (experimental)

- `levelmod/`: reads and edits level scripts (`COMMANDS.PAK`) with
  [CathodeLib](https://github.com/OpenCAGE/CathodeLib) (MIT, tested at v1.0.0, commit 69ef3a8). Clone it with its
  submodules into `third_party/CathodeLib`, then `dotnet build -c Release` (.NET 8 SDK).
  `find <levelsDir> <text>`, `funcs <COMMANDS.PAK> <FunctionType>`, `comp <COMMANDS.PAK> <composite>`,
  `addpartner <COMMANDS.PAK> <archetype> x y z` (experimental, edits the file: use a copy).
- `re/guids.py`: CathodeLib's ShortGuid hash in Python; builds a guid -> name table.
- `re/atoms.py`: names the ShortGuid "atoms" in a memory dump of AI.exe's data section.
- `re/memfind.c`: read-only pattern / float search in a running process.
- `re/*.java`: Ghidra headless scripts (string xrefs, vtable dump, refs-to, decompile, symbol lookup).
