ProjectV on Windows: a test build
=================================

Built by GitHub Actions from the commit named in VERSION.txt, with MSVC, for 64-bit Windows 10 or 11.
It needs a graphics driver with Vulkan 1.1 or newer (every current NVIDIA, AMD and Intel driver has it).
Nothing else needs installing: glfw3.dll and the Visual C++ runtime are beside each program.

Each program has a folder. Open the folder and run its .exe (double-click, or from a terminal inside
the folder: the programs find their shaders and assets relative to where they run).

  hello_voxel          The smallest one. A shape in a window; mouse to look, W A S D to move, R / F up / down,
                       Esc frees the cursor and a click takes it back.
  renderer_gallery     Several renderers over one scene.   renderer_gallery.exe --renderer restir-pt
                                                           renderer_gallery.exe --renderer fast
  voxel_mayhem_demo    A park to break things in (F1 shows the keys).
  terrain_generator    Terrain streams in as you fly; [ and ] move the sun.
  sandbox              A physics sandbox.
  physics              Eleven physics set pieces; 1-9 0 - pick one.

What to check (the full list is docs/plans/windows.md in the repository, "The hand check"):
  1. hello_voxel: a window opens and shows a coloured shape; resize it; look around; close it with the window button.
  2. renderer_gallery, restir-pt and fast: the image looks right (lit, no black or garbage).
  3. voxel_mayhem_demo: play a minute (walk, fire, break the wall).
  4. terrain_generator: terrain appears and streams; the sun keys work.
Anything that fails: the program's console output (run it from a terminal), your GPU and driver version,
and a screenshot if it draws wrongly.

Self-tests (run in a terminal, in the program's folder):
  set "MAYHEM_SELFTEST=1" && voxel_mayhem_demo.exe       plays itself and checks what it did
  set "SANDBOX_SELFTEST=600" && sandbox.exe
  set "DEMOS_SELFTEST=1" && physics.exe
(The quotes matter: without them cmd puts a trailing space in the value.)
