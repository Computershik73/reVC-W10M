# reVC for Windows 10 Mobile

[reVC](https://github.com/mrxenginner/reVC), the reverse-engineered source of Grand Theft Auto:
Vice City, as a UWP app for **Windows 10 Mobile** phones (32-bit ARM). It builds on the
[Android fork](https://github.com/codepdbh/revc-android-port-evolved), whose fixes for a game
played on a phone (touch, auto-aim, the radar out of the stick's way) it shares.

No game data is included or fetched. The app asks for the player's own copy of the PC game and
copies it into itself; installing and playing, for players: [`app/INSTALL.txt`](app/INSTALL.txt)
(English and Russian), which goes out next to the package.

## Status

**The game runs on two phones, and is playable on one of them** (the other gets 17 to 27
frames a second). `tools/build.py` produces the
signed `builds/reVC_<version>_ARM.appx`.

**The package now draws with Direct3D 11 itself. That runs on both: an HP Elite x3 at 29 to
30 frames a second of the game's 30 (1280x720), a Lumia 650 at 24 to 29 driving through the
city at 1113x626, which is what it is drawn at there if left alone (25 to 30 at 960x540; at
1280x720 17 to 25 by the traffic, and 25 to 27 in a quiet street)**
(*The Direct3D 11 renderer* below). Everything else measured in this
section is of the renderer before it: librw's OpenGL ES renderer through ANGLE, which
`tools/build.py --renderer angle` still builds.

| | HP Elite x3 | Lumia 650 |
|---|---|---|
| Windows 10 Mobile | 15254.603 | 15063.297 |
| GPU, what ANGLE gives on it | Qualcomm's of a Snapdragon 820: OpenGL ES 3.0 | Adreno 304, below Direct3D level 10_1: OpenGL ES 2.0 |
| Panel, render target | 2560x1440, 1280x720 by default | 1280x720, 768x432 by default |
| Taking the game in (1417 files, 1.5 GB) | works | works, 14 minutes; five files over 62 MB only by the app reading them itself |
| From the start to the first picture | 6 s | 75 s the first time (the shader programs are compiled), 15 to 19 s afterwards |
| The menu | 59 frames a second | 37 at 1280x720 (60 at 640x360) |
| In the game | 30, the game's own limit (9 to 15 at 2560x1440) | 26 to 27 standing in a street, 17 to 22 driving, at 768x432 (it was 4 to 7 at 1280x720); see *The slow phone* |
| Memory | | 217 MB of the 390 MB the phone allows an app |

On both: a new game from the menu, the first start's conversion of the textures, the opening
cutscene, on foot and driving, by the log and by frames the game saved (`REVC_FRAMESHOT`); a
player's twenty minutes on the first. The picture is right on both.

### The slow phone

As the game first ran on the Lumia, its frames were the processor's, not the GPU's: the
picture's size made no difference in the game, and the game's own logic is 5 ms of a frame.
The time went into the drawing calls, three to nine hundred of them a frame, at 130 to 180
microseconds each. `REVC_BENCH=1` (below) draws one small quad four hundred times and changes
one thing between the draws; on the Lumia 650, in microseconds a draw on the game's thread,
in two runs of two builds (where both measured the same thing the numbers do not repeat, and
neither is to be read closer than to a factor of two):

| Between the draws | First run | Second run |
|---|---|---|
| nothing | 28 in the first pass; 5 to 7 in later ones (`glDrawArrays`; `glDrawElements` with only the depth mask toggled) | 6 to 14 |
| the same vertex input described again | 37 | |
| new vertices (`glBufferData`) | 49 | 95 to 163 |
| the other texture | 69 | 70 |
| a `mat4` for the vertex shader | 73 | 33 |
| blending on or off | 75 | 43 |
| a `vec4` for the fragment shader | 79 | 24 |
| the other vertex array object | 89 | 106 |
| both uniforms | 96 | 34 |
| the other vertex and index buffers, described | 135 | 56 |
| culling the other way | | 52 |
| the other blend function | | 59 |
| the depth test or the depth mask | | 63 to 74 |
| the other texture filter | | 85 |
| **the other program** | | **347** |

A draw is nearly free; every change of state before it is 25 to 110 microseconds in ANGLE and
the phone's Direct3D driver, and a change of program is 350. An atomic of the game changes
three of the cheaper ones (its matrix, its texture, its buffers), and as librw draws, with the
cheapest of four programs that fits each mesh, the game changed programs 62 to 75 times a
frame (`REVC_PERF=1` counts them). What follows from that, and has been done:

- **Vertex array objects** (librw had the code and did not use it; OpenGL ES 2.0 has them as
  an extension, which ANGLE offers): an atomic's vertex input is bound, not described.
- **Linked programs are kept** in `LocalState\shaders` (ANGLE's program binaries, named by a
  hash of the sources and of the driver's version). The first start still compiles them for
  over a minute; from then on the first picture comes after 15 seconds instead of 75.
- **No copy of the picture where nothing needs one.** The game copied the frame into a
  texture every frame for the rain drops on the screen, drops or not (13 ms at 640x360), and
  again for the colour filter (50 ms at 1280x720: ANGLE at this level does it slowly). The
  first now happens only while there are drops; for the second, a phone with OpenGL ES 2.0
  gets the game's plain colour filter once, as a default (the display menu has the others).
- **Immediate mode** (the HUD, the effects) gives the buffer what it draws instead of
  orphaning 360 KB for every draw.
- On OpenGL ES 2.0 only, as defaults for a slow phone: the PlayStation 2's double-pass alpha
  test off and the draw distance at its shortest (both once: the display menu has them, and
  what is chosen there stays), vehicles in full detail up to 30 m instead of 70
  (`REVC_FULL_DETAIL=1`).
- **One program for a pipeline**, on OpenGL ES 2.0 (`rw::gl3::fewerPrograms`;
  `REVC_PROGRAMS=all` keeps the four). librw has four programs for each pipeline, with or
  without the alpha test, with or without the lights that have a position, and they draw the
  same picture where either fits; the one that fits every mesh is now used for all of them. A
  vehicle's meshes without an environment map go through the environment map's program as
  well, with the map counting for nothing, so that a car's body is one program and not two in
  turn. 62 to 75 changes of program a frame became 11 to 14. (Later, of the default
  pipeline's four, the two with the alpha test, by whether the atomic has lights: that is
  the same for all the meshes of an atomic, so the changes are no more, and what has no
  lights, every building, is spared eight rounds of their arithmetic for each vertex. A few
  per cent at 768x432. `REVC_PROGRAMS=few` is the one program again.)
- **432 lines by default on a GPU of Direct3D level 9** (626 with the Direct3D 11 renderer;
  the shell asks Direct3D, before the game has a context; the RES button still chooses
  otherwise). With the processor's share cut
  down, the pixels count again. Standing where a new game begins: 12 to 13 frames a second at
  1280x720, 14.5 at 960x540, 17.5 at 768x432, and no more than that at 640x360.

- **OpenGL ES on a thread of its own** (`glthread/`; an OpenGL ES 2.0 context gets it after
  its first frame, `REVC_GL_THREAD=0` or `1` says otherwise). With all of the above the calls
  were still two thirds of the game thread's frame. librw and the game call OpenGL through
  glad's table of pointers; a function of `glthread.cpp` now stands in the place of each, and
  writes the call into a queue that a second thread, which has the context, reads: the game's
  work and the driver's are on two of the phone's four cores, and a frame takes the longer of
  the two, not their sum. The game is never more than a frame ahead of the picture. Calls
  with an answer wait for the drawing thread; the game makes none in a frame it only draws
  (the log counts them). 16 frames a second became 19.5.
- **Immediate mode draws from memory** (the HUD, the effects: 45 to 85 draws a frame), on
  OpenGL ES 2.0, where the default vertex array may point into the application's memory. A
  buffer filled anew for every draw was the dearest draw there is here; from memory it is a
  fifth of that (the second table below). With the drawing thread such a draw is one command
  with its vertices and indices copied behind it. 19.5 became 21 to 22.
- **Geometries share buffers** (OpenGL ES 2.0 with vertex array objects;
  `LIBRW_NO_SHARED_BUFFERS=1`): those of one vertex format lie side by side in a vertex buffer
  of 65536 vertices, an index buffer and one vertex array object, and each is drawn from its
  part. 60 to 170 changes of vertex array a frame became 18 to 60, and the draw calls 7 to 10
  per cent shorter: less than the 100 microseconds a change seemed to cost, most of which
  turned out to be the uniforms that change with every atomic. A geometry whose vertices
  change (water) has buffers of its own: a change to a part of a buffer has ANGLE hand all of
  it to Direct3D again, 2 to 5 ms for such a page.
- **Atlases** (OpenGL ES 2.0 with DXT textures; `LIBRW_NO_ATLASES=1`). A building is a few
  meshes that differ in nothing but their texture, each a draw with a change of texture
  before it. When a texture dictionary has been read (`CTxdStore`), those of its textures
  that are DXT, of one format, wrapped both ways and no bigger than 512 are also put
  together into a bigger one, whole blocks as they are, each with its edge blocks repeated
  around it for the filter (`makeAtlases` in librw's `gl3raster.cpp`). Geometry of the game's
  world pipeline is instanced with one more attribute for each vertex, where its texture is
  in the atlas; the fragment shader wraps the coordinates inside that part; and
  `defaultRenderCB` draws the meshes that follow one another and have the same material
  colour as one. The game's models are triangle strips, so the indices are laid out with the
  joining triangles of no area between the meshes. A count beforehand said three draws of
  the world in five could go, and no model has a vertex in two textures; in the game 293
  draws a frame became 160 and 319 changes of texture 170. Standing: 24.4 frames a second
  became 27, at 768x432 and at 640x360 alike.

How a draw of new vertices may be made, by `REVC_BENCH=1` on the Lumia 650, microseconds a
draw (four vertices, six indices):

| | |
|---|---|
| as librw did: a buffer for the vertices and one for the indices, each given its data whole, the input described | 172 |
| `glBufferSubData` at the start of buffers made once | 208 |
| `glBufferSubData` further along each time, the indices counting from there, the input as it was | 118 |
| from memory, no buffers | 36 |
| all the draws' vertices and indices handed over once, each draw its part | 24 |

The first table's small figures did not repeat in a third run (the other texture 16, the
other vertex array 28); another program and a buffer filled for a draw cost the same every
time. What a draw call takes in the game itself, on the game's thread, by what was changed
before it (`REVC_PERF=1` with `REVC_GL_THREAD=0`, before the buffers were shared): nothing of
these 25 to 41 microseconds; a texture 56 to 97; the vertex array, which is to say the first
mesh of an atomic, with its matrix and lights, 117 to 174; both 161 to 260; the program too
425 to 770.

Tried, measured and taken out again, for no gain: drawing the visible entities sorted by
model; vertex array objects for immediate mode; holding the swap back to the display's rate
when a frame is late (the swap itself got slower). Not done, by measurement: drawing in the
order of the textures. A frame binds a texture 300 to 320 times and 205 to 230 of them are
different ones (`REVC_PERF=1` counts both), so a third of the binds at most could go, at 25
to 55 microseconds each, for a list of draws put off and sorted.

**What a frame waits for: the processor or the GPU.** With the calls on a thread of their
own the time that thread works is the frame's, and it can be read against the size of the
picture (`REVC_RESOLUTION`; the same scene of some 290 draws, before the two items below):

| Render target | The drawing thread, ms a frame |
|---|---|
| 320x180 | 39.2 |
| 640x360 | 40.2 |
| 768x432 | 45.3 |
| 1280x720 | 91.5 |

Up to 360 lines it is the processor's calls, 39 to 40 ms whatever the picture's size; above
that the GPU, at about 78 nanoseconds a pixel on top of some 19 ms. Three things were tried
on the GPU's side at 1280x720, where the time is all its own: the lights' arithmetic only
where there are lights, 0 to 3 per cent (kept: see *One program for a pipeline*); no alpha
test in any fragment shader (`REVC_PROGRAMS=noat`, a measurement and not a picture), 11 per
cent, not used so far, because telling by the mesh where the test is needed is what the
changes of program came from; and the textures half as wide and half as high
(`LIBRW_HALF_TEXTURES=1`: the player's own PC textures are drawn as they are, DXT
compressed), 3 to 5 per cent there and 7 to 9 at 768x432, which is not worth a blurred
picture.

The Elite x3 has the build before the last six items (1.0.1115.140; they are for OpenGL ES
2.0, and it has 3.0, where vertex array objects and program binaries are the standard's own):
a new game by the same script, the picture right. At the 2560x1440 its player had chosen it
draws 370 to 940 draws a frame at 75 to 100 microseconds each, 8 to 19 frames a second; at
1280x720 it had been at the game's limit of 30. The last build has not been run on it.

Standing where a new game begins, on the Lumia: 4.6 to 6.5 frames a second at 1280x720 as it
first ran; 11 to 13 after the first five items; 16 at 768x432 with one program for a
pipeline; 24.4 with the drawing thread, immediate mode from memory, shared buffers and the
lights' arithmetic only where there are lights; 26 to 27 with atlases, 160 draws a frame
and 55 immediate ones, for which the drawing thread works 36.5 ms and the game's own 17.
Facing a wall it is the game's own limit of 30.

A run then drove itself by keys held by the clock (`REVC_KEYS=13@3,13@6,s@8,70@52,87@60-65.5,68@61.4-63.2,65@66.5-68.2,87@67.5-72,83@73-75.5`:
into the car, out of the alley and right, left at the lights, and to a stop; with the game
quicker or slower than it was then, that ends in a wall, and `d@12` is what drives now), and what it
does is seen afterwards in a frame for every second (`REVC_FRAMESHOT=1k`, `phone.py frames`).
Along it: 22 frames a second pulling away, 17 through the junction while the next streets
come in from the disc (two frames in three over 50 ms there), 25 to 27 parked at its end;
without atlases 19, 16 and 19 to 22. An earlier player's drive through traffic, at 450 to
650 draws a frame without atlases, was 9 to 11.

The drawing thread is what a frame waits for, and it is not waiting for the GPU at this
size: the swap is 2.5 ms of its frame. The same calls made on the game's own thread take
28 ms where the drawing thread takes 36.5: two threads at work get in each other's way
(the caches, most likely), which is the price of having them side by side. What the thread
does is no longer one thing: a draw for every atlas an atomic's meshes are in, a matrix for
every atomic, fifteen changes of program a frame at a third of a millisecond each (the
world, vehicles, people and the immediate draws take turns), the immediate draws at some
200 microseconds each, and while driving what comes in from the disc.

What a slow stretch of the drive is made of was looked for next, and it is not the disc
(`REVC_PERF=1` has the lines). Of the drawing thread's 47 ms a frame while driving, 216
draws from buffers are 24.5, 68 draws from memory 12.6, the textures handed over 1.2, the
buffers filled 2.0 and the swap 2.3: a junction is slow because there is more of it to
draw. Which part of the game draws them, geometry and immediate: the roads 12 to 14, the
world 55 to 113, the sorted list of what has alpha, which is where vehicles and people are
drawn too, 84 to 120; the sky 7 immediate, the effects 26 to 48 (coronas 11 to 19, rubbish
and skid marks 4 to 19, particles 1 to 12, shadows 5 to 6), the HUD 14 to 18, of which the
radar 10 to 13. By pipeline, with the calls on the game's own thread: the world from
atlases 92 to 166 draws at 80 to 95 microseconds, vehicles 45 to 110 at 85 to 90 (a car in
full detail is some forty draws), skinned people 3 to 4 at 300 to 400.

Tried on the two threads, since the same calls are a quarter slower on the drawing thread:
looking at the queue for a while before sleeping (nothing: the thread runs out of calls
less than once a frame); keeping each thread on a core of its own (`SetThreadAffinityMask`,
which kernel32 has on the phone; the scheduler does move the drawing thread between three of
the four cores, and holding it still made no difference); the drawing thread's priority
above normal (kept: no more frames, fewer of over 50 ms). The calls of one OpenGL context
cannot be shared out between more threads than the one; the game's own thread already
waits half of every frame.

What has been checked, and on what:

| | Checked | Not checked |
|---|---|---|
| The package | on both phones: it installs through the Device Portal, loads and starts | other builds of Windows 10 Mobile, other phones |
| The shell | the files screen and the copy of the game, on both; a player's 23 minutes with the on-screen pad (Elite x3); by snapshots of its XAML (`REVC_SNAPSHOT`, below), the pad as laid out for a vehicle, on foot and in a menu, with PlayStation's and Xbox's names | the radio buttons and the PS/XBOX button under a finger; a copy that is cut short and goes on at the next start; a real gamepad; a keyboard |
| The game | on both phones, by the log and by saved frames: the menu, a new game, the cutscene, on foot, driving; the game's own gamepad menu. On the build PC, `tools/test_game.py`: the start up to the first file of the game, at OpenGL ES 3.0 and 2.0 | anything past the first minutes; saving and loading |
| Graphics | on both phones: the frames above, at OpenGL ES 3.0 and at 2.0. On the build PC, `tools/test_render.py`: every shader program librw and the game make compiles and links at Direct3D feature levels 11, 10_0, 9_3 and 9_1; a skinned triangle lands where its bone puts it; atomics and an immediate triangle are drawn and read back, with shared buffers filled, emptied and filled again; two textures are put into an atlas and two meshes drawn from it as one, as a list of triangles and as strips; each level a second time with OpenGL on the drawing thread | Direct3D level 9_1 with the game's data |
| Sound | on the build PC (x64), `tools/test_audio.py`: OpenAL Soft with the port's XAudio2 backend opens, mixes at the rate of the clock, stops, closes and opens again. On both phones the game opens its audio with one warning from OpenAL Soft, that there is nothing to record with | whether it is heard on a phone: nobody has said |

Two things the first runs taught, both below: the app's first crash was not where the log
showed it (*When the app just ends*), and Windows 10 Mobile 15063 does not copy a large file
for an app that asks it to (*Taking the game in*).

## The Direct3D 11 renderer

[`rwd3d11/`](rwd3d11) is a renderer for librw that calls Direct3D 11 where librw's own calls
OpenGL ES. With it there is no OpenGL ES, no ANGLE and no translation of calls between the
game and Direct3D, and no ANGLE in the package. It is what `tools/build.py` builds now.

**It runs on both phones**, an HP Elite x3 and the Lumia 650, the slow one it was written
for. On the build PC `tools/test_game.py --renderer d3d11` runs the game on a Direct3D device
held to feature level 9_3 (a desktop GPU keeping that level's rules: not a phone's driver).

On the Lumia 650 (Windows 10 Mobile 15063.297, Direct3D level 9_3, 768x432, package
1.0.1116.1820), by the log and by frames of a run that drives itself (`REVC_KEYS=13@3,13@6,s@8,d@12`,
below):

- The picture is right in the menus, a new game, the opening cutscene and two minutes of
  driving. The menu is up 5 seconds after the start: nothing is compiled.
- **24 to 29 frames a second driving, 27 to 29 standing in a street.** The OpenGL ES renderer
  with its drawing thread had 17 to 22 and 26 to 27 (other drives, along other streets).
- A frame's drawing while driving, 250 to 280 draws and 70 immediate ones: state and uniforms
  4 ms, the draw calls 8 ms, the immediate draws 3 ms, on the game's own thread; the OpenGL
  ES renderer's drawing thread worked 47 ms on 216 draws and 68 immediate ones. A draw call is
  20 microseconds with nothing but uniforms changed before it, 30 after another texture, 110
  to 120 after another program. Showing the frame is 1 to 11 ms more, by how much the GPU has
  left to do.
- `REVC_BENCH=1` there: a draw with nothing changed 4 microseconds, a vec4 changed 9, a mat4
  14 to 32, another texture 6 to 13, other shaders 52 to 57 (through ANGLE: 350). New vertices
  into a buffer that is discarded for each draw: 180 to 270; written further along the
  buffer: 6 to 8. A deferred
  context exists: recording a draw costs 4 microseconds and making the recorded calls as much
  as calling directly.

What the first runs there found, and the Elite x3 had not shown:

- **A part of a buffer written with `UpdateSubresource` took 12 to 20 ms**, whatever its
  size: every geometry's vertices and indices as they went into a page, 22 to 28 ms of every
  frame while driving, when the world's models come and go (`REVC_PERF=1` has a line of the
  renderer's own for this now: instancing, constant buffers, textures, the frame's showing).
  The pages are dynamic buffers now, written where nothing is drawn from (`Map` with
  `NO_OVERWRITE`): 0.1 to 0.4 ms a frame. What a geometry leaves in a page is the next one's
  only two frames later, since a draw of the frame before may still wait to read it.
- **Waiting for the vertical blank cost a slow frame up to a sixtieth of a second**: 18
  frames a second driving with the wait, 24 without. A frame that took over 12 ms from its
  first camera to its end is shown at once now; the quicker ones wait as the game asks (the
  menus, a fast phone, where the wait keeps the frames in step with the screen). On a level-9
  GPU the swap chain has three buffers, not two: 27 to 29 frames a second standing, not 25
  to 26. `REVC_D3D_INTERVAL` and `REVC_D3D_BUFFERS` say otherwise.

On the Elite x3 (Windows 10 Mobile 15254.603, package 1.0.1116.1550, 1280x720), at the level
the renderer asks for there, 11_0, and held to 9_3 (`REVC_ANGLE_FL=9_3`), by the log and by
the frames the game saved every second:

- The picture is right in the menus, a new game, the opening cutscene, on foot and driving.
  (The frames are what the renderer drew, read back from Direct3D; somebody played on the
  phone with the on-screen pad during these runs, so it was on the screen too.)
- **29.4 to 29.8 frames a second of the game's 30**, on foot and driving, at both levels.
  The same phone with package 1.0.1116.750 the day before (OpenGL ES 3.0 through ANGLE, no
  drawing thread, no atlases, no shared buffers: those are only for OpenGL ES 2.0 there): 23
  to 28, with 13 to 33 ms of a frame in `RenderScene`, which is 4 ms now.
- A frame's drawing, about 310 draws while driving (`REVC_PERF=1`): state and uniforms 1.2 ms,
  the draw calls of geometry 1.6 ms, 56 immediate draws 0.8 ms. A draw call takes 4
  microseconds where nothing was changed before it, 6 after another texture, 10 to 17 after
  another program.
- `REVC_BENCH=1` there, level 11_0 and 9_3 alike: a draw with nothing changed 1 microsecond,
  a vec4 or a mat4 changed 3 to 4, another texture 3 to 5, other shaders 9 to 10. New vertices
  into a buffer that is discarded for each draw: 134; written further along the buffer: 2,
  which is what immediate mode does. A deferred context exists (the driver has no command
  lists of its own, so Direct3D keeps them): at level 11_0 recording a draw costs 7 to 25
  microseconds and making the recorded calls 9 to 12, more than calling directly; at 9_3
  recording costs 1 and making the calls 1 to 5.

Why: on the slow phone a frame is its calls, and each of them went through ANGLE, which checks
and translates it, before it reached the phone's driver. How much of a call's cost is ANGLE's
and how much the driver's is what `REVC_BENCH=1` now measures on the phone at hand: the same
small quad as before, drawn through Direct3D 11 directly as well
([`bench/d3dbench.cpp`](bench/d3dbench.cpp)), one thing changed between the draws at a time. On
the build PC, where only the proportion says anything: uniforms changed before a draw cost 4
to 16 microseconds through ANGLE and 3 to 4 directly, other vertex buffers 10 and 0.5, another
program 3.7 and 2.

How it is made:

- **Its interface is that of librw's OpenGL renderer** (`rw::gl3`, `RW_GL3`), because the
  game's own pipelines and effects are written against that (`src/extras/custompipes_gl.cpp`,
  `postfx.cpp`, `screendroplets.cpp`) and are compiled as they are; `RW_D3D11` says what is
  behind it. [`rwd3d11/rwd3d11gl.h`](rwd3d11/rwd3d11gl.h) has the few OpenGL names that code
  uses as plain values, and `glUniform3fv` and `glUniform4fv` for the uniforms it sets itself.
  Of librw's `gl/` only what calls no OpenGL is built (the material effects' pipeline is one).
- **The shaders are HLSL, compiled when the package is built** (`rwd3d11/shaders`,
  [`tools/make_shaders.py`](tools/make_shaders.py), the Windows SDK's `fxc`; for feature level
  9_3, which every level above takes too). Nothing is compiled on the phone, where ANGLE took
  over a minute at the first start. Callers still ask for a program by its GLSL sources: the
  tool's list says which HLSL stands for which, and the renderer finds it by a hash of what it
  is handed. A shader's uniforms lie in constant buffers grouped by how often they change; the
  tool reads their places out of the compiler's listing, and a buffer is handed over again
  only when one of its uniforms changed. A vertex shader and a pixel shader are each one object
  however many programs are made of them. The lights are a real loop (`[loop]`): unrolled,
  eight lights of three kinds are more than the 256 instructions a vertex shader has at this
  level. That loop is drawn with only while a point or a spot light is there, which the game
  never has: its lights are directional ones, a shader for those alone is laid out light by
  light, and the renderer puts it in the loop's place (`_dirLight` for `_fullLight`,
  `REVC_D3D_LIGHTS=all` does not).
- **Textures are kept as the OpenGL renderer keeps them**, rows bottom-up and sampled at
  (u, 1-v), the PC game's DXT blocks as they are. A texture that is drawn into is drawn into
  upside down, and the picture is turned over when it is copied into one (the colour filters,
  the drops on the screen), so that there is one rule for all of them.
- **What was done for the slow phone is kept**: the atlases, geometries sharing buffers (a
  geometry's part of a page is written by itself, into a dynamic buffer where nothing is drawn
  from), one program for a pipeline. Vertex array
  objects and kept program binaries have nothing to stand for. Immediate mode writes its
  vertices along one buffer, each draw after the last, and a draw is its part of it.
- **Every vertex has a colour and texture coordinates**, black and nought where its geometry
  has none: Direct3D has no value for an input the vertices lack. What has no normals is drawn
  by the shaders that read none.
- **A drawing thread on a GPU of Direct3D level 9** ([`rwd3d11/thread.cpp`](rwd3d11/thread.cpp);
  `REVC_D3D_THREAD=0` or `1` says otherwise). The game's thread makes Direct3D's calls on a
  deferred context, which only keeps them; at the frame's end the list goes to the drawing
  thread, which makes the calls on the device's own context and shows the frame while the
  game's thread is on the next (it is never more than one frame ahead). What a deferred
  context cannot do goes another way: a geometry's part of a shared buffer is written by the
  drawing thread before the frame's calls, and reading anything back (a frame for
  `REVC_FRAMESHOT`, a texture's pixels) first waits for everything drawn so far. A list
  begins with nothing bound, so the renderer forgets what it has bound with every one it
  hands over. On the Elite x3 there is none: its drawing is under 4 ms of a frame, and
  keeping a call costs more there than making it. (`glthread/` carries OpenGL's calls, and
  is the other build's.)
- **What the thread is worth on the Lumia 650, and what holds that phone back now.** With it
  the game's own thread has 17 to 23 ms of work in a frame while driving, where it had 28
  to 30 and the frame's showing on top (a draw costs it 1.5 microseconds to keep, the states
  and uniforms 2 to 4 ms a frame), and the
  drawing thread makes a frame's calls in 10 to 16 ms. The frames a second are the same as
  without it at 768x432, 24 to 28 driving, because there the frame waits for the GPU: showing
  it takes the drawing thread 17 to 26 ms more, which is the GPU finishing the picture, and
  30 to 40 ms a frame is what the GPU needs at that size. With fewer pixels it does not
  wait (the same drive, by itself each time, `REVC_RESOLUTION`):

  | Render target | With the drawing thread | Without |
  |---|---|---|
  | 768x432 (the default on such a GPU) | 24.7 to 28.3 frames a second driving, the frame shown in 17 to 26 ms | 24.0 to 27.6 |
  | 704x396 | 26.5 to 29.5, shown in 2 to 21 ms | |
  | 640x360 | 28.5 to 30.0, shown in 1 to 10 ms | 27.9 to 29.7 |
  | 960x540 | 18 to 20 in the city's streets (27 to 28 where there is little to draw), shown in 31 to 36 ms | |
  | 1280x720 | 14 to 19 in the city's streets (20 to 21 where there is little to draw), shown in 39 to 48 ms | |

  In the city's streets a frame takes the GPU some 38 ms at 768x432, 51 at 960x540 and 66 at
  1280x720: about 22 ms that do not depend on the size and 0.047 ms for every thousand
  pixels. 25 frames a second at 1280x720 would need that frame in 40 ms, which neither
  half the pixels' cost nor half the rest gives by itself.

  Tried on the GPU's side at 768x432: the picture cleared before a frame, which the game
  does not do (its sky covers it), and the depth buffer given up after it
  (`ID3D11DeviceContext1::DiscardView`), so that a GPU drawing in tiles neither fetches the
  one nor writes out the other: nothing measurable, kept all the same (`REVC_D3D_KEEP=1`
  is without). A depth buffer of 16 bits and no stencil (`REVC_D3D_DEPTH16=1`): the frame
  shown in 6 to 20 ms, 2 to 3 frames a second more, at the price of the depth's precision in
  the distance; a measurement, not what the package does. (At 1280x720 it gives 23 frames
  a second for 20 to 21 where there is little to draw, and nothing in the city's streets.)
- **What the Lumia 650's GPU pays for, and what was done about it** (packages 1.0.1117.220
  and, for what came after it, 1.0.1117.314 and .340).
  The table above says that a frame waits for the GPU; it does not say for what. Three
  measurements do ([`rwd3d11/measure.cpp`](rwd3d11/measure.cpp),
  [`bench/d3dbench.cpp`](bench/d3dbench.cpp)):

  *What a frame asks for* (`REVC_D3D_PIXELS=1` and `2` on the build PC, the same drive): 250
  to 340 draws and 65 immediate ones, 60 to 75 thousand triangles, and 4.4 to 6.6 pictures'
  worth of pixels, of which 3.4 to 4.5 are not hidden: the sky and what is behind the world
  1.9 to 2.4, the world and its roads 1.4 to 2.1, what fades in 0.6 to 1.2, the effects 0.7
  to 1.0 (the lamps' light on the ground alone 0.5 to 0.7 in the evening).

  *What a layer of pixels costs* (`REVC_BENCH=fill`, 1280x720): one colour 2.0 ms, a texture
  and the fog 2.75, with the alpha test 3.1, from an atlas 5.3; hidden behind what is in the
  depth buffer 0.55, but 3.1 with the alpha test, which keeps the GPU from giving a hidden
  pixel up early. A texture seen at 16 of its texels to a pixel: 16.6 ms, and 2.7 if it has
  its smaller copies. Clearing the target and nothing else: 4.1 ms (1.7 at 768x432).

  *What a draw costs after its call has returned* (`REVC_BENCH=1`, the second number of each
  line): 19 microseconds with nothing changed, 25 after another texture, **87 after other
  constants for the vertex shader** (whichever way they are handed over: a buffer written,
  another buffer bound, another part of one buffer), 157 after other shaders. And 32 if the
  same matrix comes with the vertices, from a second buffer read once for the draw.

  And with no pixels at all (the scene drawn into a tenth of the picture's width and height,
  `REVC_D3D_VIEWPORT=10`) the city's frame still took 26 to 44 ms: at 768x432 it was not the
  pixels the frame waited for, but its draws, two in three of which had another object's
  matrix or another material in the vertex shader's constants.

  So where an object is, the light it stands in and its material go with the vertices now,
  on a GPU of level 9 (`INSTANCE` in
  [`rwd3d11/shaders/default_vs.hlsl`](rwd3d11/shaders/default_vs.hlsl) and
  `matfx_env_vs.hlsl`, `render.cpp`; `REVC_D3D_INSTANCE=0` is without): six vectors for a
  draw, written one after another into a buffer of their own, which the draw reads as an
  instance of one. First for everything the default pipeline draws, then for the vehicles
  too (the shader that reads them so stands in for a shader whenever geometry is drawn,
  `shader.cpp`). The vertex shaders' constant buffers change about 20 times a frame where
  they changed 290 times, and the same frame without its pixels takes 23 to 30 ms.

  Then the pixels, which at 1280x720 were what a frame's showing waited for (38 to 46 ms):

  - **What may be seen through is drawn after what may not** (`render.cpp`,
    `REVC_D3D_LATER=0` is without). The meshes that can fail the alpha test are kept, with
    everything they are drawn with, until the geometry before them is done (the next thing
    that is not geometry, or the camera's end); all the others are drawn at once by shaders
    without the test, and their hidden pixels are given up early. Choosing the shader mesh
    by mesh instead (`REVC_PROGRAMS=all`) changed shaders 40 to 126 times a frame and was
    slower than not choosing: a change of shaders costs 170 microseconds in a frame.
    With the PlayStation 2's alpha test as well since 1.0.1117.702 (a kept mesh is drawn in
    that test's two passes). That package did it on every GPU; above level 9 it was not
    shown to be worth anything (the Elite x3, below), and from 1.0.1117.731 it is a level-9
    GPU's alone again.
  - **The sky is drawn after the houses** (`core/main.cpp`, `immed.cpp`; `REVC_SKY_FIRST=1`
    is the game's order): the clouds, the moon, the stars and the ground's colour under the
    horizon, at the far end of the depth buffer and tested against it, by a shader that
    does the alpha test's work without throwing pixels away. The sky's own colours are
    drawn first as ever. Seen through a car's glass there are no clouds. The sky is drawn in
    the state the frame begins with (`DefinedState`), not the one the world's pass leaves:
    with that pass's fog still on, every cloud was a square of the fog's colour (packages
    1.0.1117.314 to .449 had them; seen on a phone before it was seen in the test's small
    frames, which is what `REVC_FRAMESHOT_FULL=1` is for now).
  - Each of the two takes 8 to 10 ms off a frame of the quiet street at 1280x720.

  The same drive:

  | Render target | Before (1.0.1116.1910) | With the draws' data in the vertices (1.0.1117.220) | And the two for the pixels (1.0.1117.314 to .459) |
  |---|---|---|---|
  | 768x432 | 24.7 to 28.3 frames a second, the frame shown in 17 to 26 ms | 29.7 to 29.9 (28.4 while the first street comes in), shown in 1 ms: the GPU is waited for no more | |
  | 960x540 | 18 to 20 in the city's streets, 27 to 28 where there is little to draw | 23.5 to 27.7, and 27.7 to 28.7 | 24.8 to 29.9 (the lowest with 310 to 340 draws in a frame), and 28.3 to 29.8 |
  | 1024x576 | | | 25.1 to 29.6 |
  | 1114x626 | | | 24.1 to 29.0 |
  | 1280x720 | 14 to 19, and 20 to 21 | 15.5 to 19.2, and 22.5 to 22.8 | 17.2 to 24.7 by the traffic (200 to 330 draws), and 25.0 to 26.9 |

  Where the shell is left to choose on such a GPU it is 626 lines now (87 per cent of the
  Lumia's 720, which is also a step of the RES button): the size at which that drive still
  has 24 frames a second and more (two of three runs; the third, through more traffic, 320
  to 350 draws in a frame, had 20 to 23).

  **On an HP Elite x3** (Direct3D level 11_0, a screen of 2560x1440; package 1.0.1117.459,
  the same drive, 28 runs of 80 seconds with nothing else running on the phone, which the
  Device Portal's list of processes says for every run: `w10m/work/elite_series.sh`; the
  PlayStation 2's alpha test off, which earlier runs with `REVC_SPARE=1` had left so in the
  game's settings there):

  - **How warm the phone is decides.** Cold, it has the 30 frames a second at 1920x1080 and at
    2560x1440 as the package is. After ten to fifteen minutes of runs one after another (on
    its charger) it has 22 to 24 at 1920x1080 (20 to 28 by the traffic) and still 30 at
    1280x720, which is what the shell chooses there; 2560x1440 was not run warm as the
    package is (with what a slow phone gets: 20 to 27). 1536x864 had 30 in the runs it was
    given, which came a few minutes after the others: warm, not as warm. Thirteen minutes of
    rest, and 1920x1080 is at 30 again.
  - **What a slow phone gets is worth a tenth there**, at 1920x1080 on the warm phone, by the
    draws in a frame (the car goes a busier way or a quieter one, 300 to 450 draws, and that
    is worth more than any switch: runs compare only window by window,
    `w10m/work/elite_windows.py`):

    | | 300 draws | 450 draws |
    |---|---|---|
    | as the package is | 24.6 | 21.6 |
    | all of it (`REVC_SPARE=1`, `REVC_D3D_INSTANCE=1`, `REVC_D3D_LATER=1`, `REVC_D3D_THREAD=1`, `REVC_D3D_BUFFERS=3`) | 27.3 | 24.4 |
    | all of it, vehicles in full detail to 70 m and the sky first (`REVC_FULL_DETAIL=1`, `REVC_SKY_FIRST=1`) | 26.9 | 24.4 |
    | all of it but the opaque meshes first (`REVC_D3D_LATER=0`) | 24.8 | 22.1 |

    The vehicles' detail and the sky's place, the two that change the picture, do nothing
    there. The last row was read at first as the opaque meshes drawn first being all of it,
    and is not that: a slow phone without them has one shader, with the alpha test, for
    every mesh, and the package as it is on this phone has not (it chooses mesh by mesh), so
    the row says what that one shader costs there. The drawing thread by itself
    (`REVC_D3D_THREAD=1`, one run): 26.3 and 23.0.
  - **The opaque meshes first on that phone, by themselves** (package 1.0.1117.702, which
    had them and the instance stream on every GPU; 1.0.1117.731 has them on level 9 alone
    again, and `REVC_D3D_INSTANCE=1 REVC_D3D_LATER=1` for another): 23.4 frames a second,
    and 23.2 with the
    two switched off (`REVC_D3D_LATER=0 REVC_D3D_INSTANCE=0`), in the one pair of runs at
    1920x1080 that found the phone equally fast. Of the seven runs there were, three found
    it faster than the rest (30, 28.8 and 26.1 where the others had 22 to 23.4: it goes
    between its slow state and its fast one within minutes), and the series was cut short.
    Not shown to be worth anything there. With the PlayStation 2's alpha test on, which is
    what the game has on such a phone unless its menu says otherwise, a frame has a fifth
    more draws (265 where 225, 585 where 485) and one to two frames a second less in
    traffic.
  - **Level 9_3 is worth nothing by itself**: held to it (`REVC_D3D_LEVEL=9_3`), 2560x1440 on
    the warm phone has 20 to 27.5, and 20 to 27 at level 11 with the same treatment.

  The first measurements there were taken while another app of the phone's owner was running
  and were put down to it; they are those of the warm phone. Two things that mislead: the
  GPU's load with the game closed is whatever the compositor makes of the app in front (30
  per cent for an animation, with no app using the processor), and the load during a run is
  a share of the clock the GPU runs at the moment (1280x720 showed 80 per cent where
  1536x864 showed 60). What a frame in traffic takes at 1280x720 is about 22 ms and 0.09 ms
  for each of its 200 to 330 draws.

  Tried, and not what the package does:

  - The textures' smaller copies (`REVC_D3D_MIPS=1`, `raster.cpp`), which an atlas cannot
    have, so that they only count without atlases, where they take 5 to 8 ms off a frame at
    1280x720 and the 200 more draws put as much on. In an atlas they would need the level
    chosen by the slopes of the coordinates before they are wrapped (`SampleGrad`), which
    that GPU does in 7.6 ms a picture's worth where it samples from an atlas in 5.2.
  - Atlases for the meshes that do not repeat their texture only, with the coordinates moved
    into the atlas when the geometry is instanced and no wrapping in the pixel shader
    (`REVC_ATLAS=fit`, `pipe.cpp`): a fifth of the meshes that an atlas joins are of that
    kind, the draws are nearly those of no atlases at all.
  - A depth buffer of 16 bits (`REVC_D3D_DEPTH16=1`): 4 to 5 ms off a frame at 1280x720 (27
    to 28 frames a second in the quiet street, 18 to 21 in traffic), with the precision it
    costs in the distance and without the stencil the game has a use for.
- The window and the input are still GLFW's API (`glfw/`, built without EGL: `REVC_D3D11`).
  The renderer makes a swap chain of the render target's size and the shell puts it into its
  panel, stretched over it (`host_set_swap_chain`).

Checked on the build PC, at feature level 9_3 and at the PC's own, against the OpenGL
renderer's frames of the same scripted runs (`REVC_KEYS`, `REVC_FRAMESHOT`): the menus, a new
game, the opening cutscene, on foot, the self-driving run of 110 seconds, the game's plain and
its full colour filter, both vehicle pipelines. The pictures are the same. What came after
that (the lights' shaders, what a draw has of its own with its vertices, what is seen through
drawn later, the sky after the houses) was looked at in frames of the self-driving run, on
the PC at both levels and on the Lumia 650 (there in frames of 320x180), not compared pixel
by pixel; an HP Elite x3 has not run it. Not checked anywhere: rain on the screen, a
panel that changes size, a device that is lost.

## How it fits together

| Layer | Desktop reVC | Here |
|---|---|---|
| The game | `src/` | the same sources, built with MSVC for ARM32; `GTA_W10M` marks the few places that differ |
| RenderWare | librw (`vendor/librw`), OpenGL 3 or ES | librw with a renderer of its own on **Direct3D 11** ([`rwd3d11/`](rwd3d11), feature level 9_3 and up), presenting into a XAML `SwapChainPanel`. Or, as it was: librw's OpenGL ES renderer on **ANGLE** |
| Window, input | GLFW (`src/skel/glfw/glfw.cpp`) | the same skeleton; GLFW's API is implemented in [`glfw/`](glfw) over the shell (and over EGL, where ANGLE draws) |
| Sound | OpenAL Soft | OpenAL Soft 1.24.3 with an XAudio2 backend ([`openal/`](openal)): its own UWP backend is WASAPI, which faults on 15063 phones when it is opened off the UI thread (butterscotch-w10m) |
| MP3 (radio, cutscenes) | mpg123 | mpg123 1.32.10, its C decoder ([`deps/mpg123`](deps/mpg123)) |
| Controls | keyboard, mouse, gamepad | an on-screen pad in XAML ([`app/TouchPad.h`](app/TouchPad.h)) that the game sees as a gamepad; a real gamepad (Windows.Gaming.Input) takes over; a keyboard works. The game's gamepad menu is on (`GAMEPAD_MENU`): its controller type decides the pictures in the game's prompts, and the pad wears the same names (PlayStation's by default, Xbox's, the Switch's) |
| Files | the game's folder | the game copied into LocalState `game\` through the folder picker ([`app/GameFiles.h`](app/GameFiles.h)), with reVC's own `gamefiles\` from the package over it |

The shell ([`app/App.cpp`](app/App.cpp), C++/WinRT, no XAML markup) owns the window and the UI
thread. The game runs on a thread of its own (`reVC_Main`) and meets the shell only through
[`app/host_api.h`](app/host_api.h): the panel, an event queue, the pad's state, what the player
is doing (a menu, on foot, in a vehicle, a cutscene: the pad shows the controls that fit), a log.

The shell speaks two languages, each line written twice where it is used (`T(english, russian)`,
[`app/pch.h`](app/pch.h)): Russian on a phone whose first language is Russian (Settings > Time &
language > Language), English on every other. Its dialogs' one button is its own for the same
reason. The log's first lines say which it is; `REVC_LANG` shows the other one. The game's
language is the game's own setting.

Who made the port and how to reach them is at the foot of the files screen, and in the game
under Options, "About the Port" (`FEO_ABT`, an entry `src/core/re3.cpp` adds; its text is in
`CText::Get`, no file of the game has it): the game asks the shell (`host_show_about`), which
shows the same lines over the game's picture until Close, the Back key, a key or a button of
the pad.

What a phone decides, and what is done about it:

- **Direct3D feature level.** ANGLE gives OpenGL ES 3.0 from 10_1 up and ES 2.0 below, which
  is most phones (9_3). librw asked for ES 3.1 or 2.0; it now also takes 3.0
  ([`patches/librw`](patches/librw)).
- **256 vertex shader constants at level 9.** A skin's 64 bone matrices alone are that many.
  There the skin shaders get three rows of each matrix instead (`BONES_AS_ROWS`, same patch,
  and `src/extras/shaders/neoRimSkin.vert`).
- **DXT textures.** ANGLE names them with three extensions where librw looked for one; without
  that every compressed texture was expanded to four or eight times its size.
- **Resolution.** The game draws into a swap chain that may be smaller than the panel, and the
  compositor stretches it: 720 lines at most by default (a 1440x2560 panel has four times the
  pixels of that), or 100/87/75/60/50 per cent with the RES button in the menus. Set at the
  start. How fast any of these is on a phone has not been measured.
- **No desktop.** No user32, winmm, shell or registry: the skeleton's calls to them are
  compiled out for `GTA_W10M`, and asserts and errors go to the log and to a dialog of the
  shell instead of a message box.
- **Api-sets.** The import libraries are the 1607 SDK's; a phone's api-set schema is a subset.
  The package carries a forwarder DLL under each api-set name it imports, which the loader
  takes only where the schema does not know the name (as venetium-w10m and gecko-w10m do).

## Layout

```
app/                  the shell: App.cpp, TouchPad.h, GameFiles.h, Gamepad.h, host_api.h,
                      the manifest, the tiles, INSTALL.txt, THIRD-PARTY.txt
rwd3d11/              the Direct3D 11 renderer for librw, and shaders/, its HLSL
bench/                what a draw costs on the device at hand, through Direct3D 11 itself
                      and through ANGLE (REVC_BENCH)
glfw/                 GLFW's API (the part librw and the game use) over the shell, and over
                      ANGLE where that draws
glthread/             the OpenGL ES build's: OpenGL ES on a thread of its own, glthread.cpp,
                      and calls.inc, which tools/gen_glthread.py makes from librw's glad.h
openal/               the XAudio2 backend for OpenAL Soft
deps/mpg123/          how libmpg123 is built here (its sources are fetched)
patches/              librw (its OpenGL renderer's interface for rwd3d11/; and for the OpenGL
                      ES build: OpenGL ES 3.0, ANGLE's DXT names, bone rows, vertex array
                      objects, kept programs, one program for a pipeline, immediate mode from
                      memory, shared buffers, atlases) and OpenAL Soft (registering the
                      backend), applied by tools/build.py
cmake/                the toolchain file: MSVC for ARM, the app container's link line; and
                      rwd3d11.cmake, librw with the Direct3D 11 renderer
CMakeLists.txt        everything that is built
test/                 what runs on the build PC: openal/, render/, game/
tools/                build.py, test_*.py, phone.py, symbolize.py, make_shaders.py,
                      gen_glthread.py, make_assets.py, env.py (where things are)
work/, builds/        made by the tools, not in the repository
../src/skel/w10m/     the game's side of host_api.h
```

Changes to the game's own sources are few and marked `GTA_W10M` or `GTA_TOUCH_CONTROLS` (a diff
of `src` against the Android fork's `miami` branch shows them): the GLFW skeleton, asserts and
errors, the MP3 shortcut resolver, the streaming's sector size, the rim-light skin shader, the
three places the Android fork changed for touch, the gamepad menu with PlayStation as its
default type, the radio taking a step from the pad's two radio buttons (a gamepad has one, and
only forward), the American text standing in for lines a language's file lacks (reVC's own
menu entries are not in its Russian file and read "FET_AGS missing"), the debug timers around
the parts of the scene, read out for the log, and what a slow phone is spared (the copy of the
picture for rain drops when there are none, the distance to which vehicles are drawn in full).

## Build

On Windows, with:

- **Visual Studio 2022** with the *MSVC v143 ARM build tools* (17.14 is the last release that has
  them) and the x64 ones for the tests. cl.exe and link.exe, not clang: no SEH on ARM32 there.
- **Windows SDK 10.0.22621** (headers, tools: the last with ARM32 libraries) and **10.0.14393**
  (the import libraries).
- **CMake**, **Ninja**, **git**, **Python 3.12** or later.
- Next to this repository, as for the other ports of this family:
  **butterscotch-w10m** with its ANGLE built (`python tools/build_angle.py` there, and
  `--arch x64` for the tests), and **gecko-w10m**: ANGLE's headers, the UWP C runtime
  (`vcruntime140.dll`, `msvcp140.dll`) from its packaging stage, and its signing certificate
  (`Gecko.pfx`, Publisher `CN=Computershik`), which the phone must trust.
  `tools/env.py` lists the environment variables that move each of these.

```bash
git submodule update --init vendor/librw
```

```bash
python w10m/tools/build.py
```

That fetches OpenAL Soft (a pinned commit) and mpg123 (a pinned SHA-256), applies the patches
(to `vendor/librw` too: the submodule then shows as modified), builds, stages, checks the
imports, packs and signs `w10m/builds/reVC_<version>_ARM.appx`, with `Computershik.cer` and
`INSTALL.txt` beside it and the linker maps in `builds/maps`. `--list` names the steps;
`build.py build pack` runs only those, `--from build` that one and the rest.
`build.py --renderer angle configure build stage shims imports pack` makes the package that
draws with OpenGL ES through ANGLE instead (`--renderer d3d11` goes back); the choice stays
with the build folder.

The tests, on the build PC:

```bash
python w10m/tools/test_audio.py
```

```bash
python w10m/tools/test_render.py
```

```bash
python w10m/tools/test_game.py --renderer d3d11 --game "D:\Games\GTA Vice City" --seconds 60 --level 9_3
```

`test_render.py` runs at each Direct3D level twice: the second time every shader program must
come from the folder the first run kept them in, and draw the same. It also draws two atomics
and an immediate triangle in turn, from recorded vertex input.

The last one runs the game itself (x64, off-screen, with sound) in a copy of the game that has
reVC's `gamefiles` over it, with the Direct3D 11 renderer or, without `--renderer`, with
OpenGL ES through ANGLE; `--level` holds Direct3D to a feature level. It ends after
`--seconds`. With `REVC_TEST_LOCAL` set to a folder, what a phone does by itself works here
too: `REVC_KEYS` walks the menus and drives, `REVC_FRAMESHOT=2k` leaves a frame every two
seconds in that folder, and two runs' frames can be laid side by side. Without `--game` it
only shows that the engine starts. `test_render.py` is of the OpenGL ES renderer.

## On the phone

Developer mode and the Device Portal; over USB the portal is at `http://127.0.0.1:10080` or
`https://127.0.0.1:10443`.

```bash
python w10m/tools/phone.py install
```

```bash
python w10m/tools/phone.py log
```

```bash
python w10m/tools/phone.py run --seconds 60
```

(Adapted from butterscotch-w10m's and renpy-w10m's.) While files are being copied to the phone
over the same cable the portal mostly does not answer.

With two phones on USB the PC forwards the portal's ports to one of them only, and not always
the same one: `phone.py phones` lists the phones and says whose portal the USB ports are. The
other is named by its Wi-Fi portal, in front of any command, and what is fetched from it goes
to `work/phone-<address>`:

```bash
python w10m/tools/phone.py --portal http://192.168.31.137 run --seconds 60
```

`run` is for a run nobody holds the phone for: it stops the app, starts the game at once, waits
(`--seconds`), and fetches the log and, with `REVC_FRAMESHOT` set, the game's last picture.
`launch`, `stop`, `version`, `ls`, `file NAME`, `rm NAME`, `mem`, `env NAME=value` and `unenv`
are the others. The portal drops connections for half a minute at a time; the tool asks again.

The app's log is `LocalState\revc.log` (`revc.prev.log`: the run before), written while the
game runs. Every line starts with the seconds since the app started; then the shell's lines
(`shell:`), the GLFW layer's (`egl:`, `glfw:`, `perf:` every ten seconds), ANGLE's troubles
(`angle:`), everything the game, librw and OpenAL Soft print (`game:`), failed assertions,
C++ exceptions as they are thrown (`note:`, the first two dozen: most are caught), the app's
memory against the phone's limit for it whenever it has moved by 32 MB (`shell: memory`), and
every ten seconds of play where a frame's time went, by the game's own debug timers:

```
perf: 5.2 fps, swap 1.9 ms a frame, 53 frames over 50 ms (worst 325 ms), target 640x360
perf: ms a frame in the game's parts: CGame::Process 8 DMAudio.Service 1 CnstrRenderList 3 RenderScene 143 Sky 2 Roads 3 World 67 Water 1 WaterTransparent 3 FadingIn 59 RenderEffects 7 BackBuffer 13 RenderMotionBlur 4 Render2dStuff 5
```

(Sky to FadingIn are parts of RenderScene; FadingIn is where vehicles and people are drawn.)
The game names every file it opens, every sound it measures and every texture it converts:
those lines stay out of the log unless `REVC_VERBOSE=1` is set, and the last eight of them are
written with a fault (`game (before that):`). Faults:

```
crash: exception 0xC0000005 at reVC.exe+0x1D8419 (thread 1964, the game's thread), reading address 00000010
crash: r0 ... (the registers)
crash: stack reVC.exe+0x1D6165 reVC.exe+0x169081 OpenAL32.dll+0x3AD23 ...
```

The stack lines are every word of the faulting thread's stack that points into code: the
callers in order, with leftovers of earlier calls among them. `tools/symbolize.py` names them
from the linker maps of the build the log is from (`builds/maps`, kept for every package: reVC,
OpenAL32, libmpg123):

```bash
python w10m/tools/symbolize.py
```

### Taking the game in

The picked folder can only be read through the system's storage broker (`GameFiles.h`). Each
file is copied with `StorageFile.CopyAsync`; on the Lumia 650 that fails, with nothing but
`E_FAIL`, for every file over about 62 MB (`cuts.img`, `sfx.raw`, two radio stations,
`gta3.img`), and the app then reads the file through the broker's stream and writes it itself,
which the log notes (`files: gta3.img: CopyAsync failed ... reading the file instead`). The copy
goes into `game.part`, which becomes `game` when everything is in. What is already there at its
full size is not copied again, the folder is remembered, and a copy that was cut short goes on
by itself when the app is next opened. The screen is kept on meanwhile: a phone suspends an
app it has locked over. The log has the free space, a line of progress every 20 seconds and,
if it stops, the file it stopped at.

One thing not to do again: asking the broker for a folder a page of items at a time
(`GetItemsAsync(start, count)`) has it read the whole folder for every page, 36 seconds each
for Audio's 1300 files on the Lumia.

### When the app just ends

Not everything that ends a process is an exception a handler sees. The C runtime ends it at
once (a fail-fast) when one of its functions is handed something invalid, when `abort()` is
called, or when a C++ exception finds nobody to catch it. On these phones the trace of such
an end is misleading: a moment later the UI thread faults inside `CoreUIComponents.dll`,
writing through a null pointer, with nothing of the app on its stack. That is the phone's
navigation client taking the window away from a process that is already being ended, not the
cause. The shell therefore notes these ends where they happen (`crash: the C runtime was
handed an invalid parameter`, with the stack; `crash: abort() was called`; `crash:
std::terminate`), and an invalid parameter no longer ends the game: the call fails and the
game goes on.

Settings for a test run are empty files in LocalState named `env.NAME=value` (the app cannot
open what the portal uploads, but it sees the names); `phone.py env` makes one and `unenv`
takes it away again, which is not to be forgotten: they stay until removed.

| Setting | Effect |
|---|---|
| `REVC_ANGLE_FL=9_3` | a phone with a level 11 GPU takes the path of one with 9_3 (OpenGL ES 2.0) |
| `REVC_RESOLUTION=50` | the render target for this run, in per cent of the panel (0: as the app chooses itself, 720 lines at most, 626 on a GPU of Direct3D level 9; 432 there in the OpenGL ES build), whatever the RES button says |
| `REVC_VERBOSE=1` | everything the game prints goes into the log |
| `REVC_FRAMESHOT=5` | the game's picture, every five seconds, as `LocalState\frame.bmp` (640 wide at most): `phone.py file frame.bmp`, which makes a PNG of it too. `REVC_FRAMESHOT=1k` keeps every one, a second apart, as `frame_001.bmp` and so on (320 wide), each logged with the time on the clock of `REVC_KEYS`; `phone.py frames` fetches them, takes them off the phone and puts them on one sheet, `frames.png` (with PIL), which is how a scripted drive is seen and corrected. A series begins with none of the one before it and ends by itself at 900 frames (the setting left on a phone that was then played on for hours filled a gigabyte) |
| `REVC_KEYS=13@3,13@6,s@8` | keys pressed by the clock, which starts when the game's menu first comes up: virtual key 13 (Enter) after 3 seconds and again after 6 (a new game), and from 8 on every cutscene is skipped as it starts. `87@45-75` holds a key from 45 to 75: W, the accelerator of the game's PC keys (65 and 68, A and D, steer; 83, S, brakes). `d@12`: from then on the player's car is driven as the traffic's cars are, along the roads and round what is on them, and a player without a car is put behind the wheel of the nearest: **a run that drives itself**, `REVC_KEYS=13@3,13@6,s@8,d@12` (keys held by the clock end in a wall as soon as the game is a little quicker or slower than where they were timed). With the frames, a way into the game from the PC |
| `REVC_PERF=1` | every ten seconds, the draws of a frame and the time in them (`perf: 290 draws a frame: vertex input 1.1 ms, state and uniforms 2.6 ms, the draw calls 41.1 ms; 55 immediate draws, 13.3 ms`; with a drawing thread that is the time to queue them, and its own is in the line `the drawing thread worked 44.2 ms a frame on 1437 calls`), what changed between them (`12 programs, 342 uniforms, 305 textures (208 different ones), ... 73 vertex arrays, 40 blend`), and without a drawing thread what a draw call took by what was changed before it |
| `REVC_GL_THREAD=0` (`1`) | OpenGL ES is called on the game's own thread, as on a phone with OpenGL ES 3.0; `1` gives such a phone the drawing thread. With `REVC_PERF=1` the log then has the draw calls by pipeline and by what was changed before them; with the drawing thread, that thread's time by kind of call |
| `REVC_GL_PRIORITY=0` | the drawing thread at the priority of any other thread, not above |
| `REVC_BENCH=1` | when the menu first comes up: what a draw costs on this phone through Direct3D 11 itself, and each kind of change between two draws (`d3d bench:` lines), which formats its Direct3D has and whether a frame's calls can be recorded on one thread and made on another; in the OpenGL ES build, the same draws through ANGLE first (`bench:` lines; *The slow phone* above has a Lumia 650's) |
| `REVC_D3D_LEVEL=9_3` | the Direct3D 11 renderer asks for no more than this feature level (`REVC_ANGLE_FL` in the OpenGL ES build) |
| `REVC_D3D_INTERVAL=0` (`1`) | every frame is shown at once, or every frame waits for the vertical blank; left alone, the frames that took over 12 ms do not wait (*The Direct3D 11 renderer* above) |
| `REVC_D3D_BUFFERS=2` (to `4`) | the buffers of the Direct3D 11 renderer's swap chain: three on a level-9 GPU and two elsewhere if left alone |
| `REVC_D3D_THREAD=0` (`1`) | Direct3D is called on the game's own thread, as on a GPU above level 9; `1` gives such a GPU the drawing thread. `REVC_D3D_PRIORITY=0`: that thread at the priority of any other, not above. With `REVC_PERF=1` the `perf: Direct3D:` line has what each thread did |
| `REVC_D3D_KEEP=1`, `REVC_D3D_DEPTH16=1` | measurements on the GPU's side: the picture is not cleared before a frame nor the depth buffer given up after it; a depth buffer of 16 bits without stencil |
| `REVC_D3D_INSTANCE=0` (`1`) | what differs between the world's draws (where the object is, its light, its material) is handed over as the vertex shader's constants again, as on a GPU above level 9; `1` gives such a GPU the other way, with the vertices |
| `REVC_D3D_LIGHTS=all` | the vertex shader with every kind of light is drawn with wherever the game asks for it, not the one for directional lights alone |
| `REVC_D3D_LATER=0` (`1`) | every mesh is drawn where the game draws it, as on a GPU above level 9; left alone, a level-9 GPU draws what may be seen through after what may not. `1` with `REVC_D3D_INSTANCE=1` gives a GPU above level 9 that order |
| `REVC_SKY_FIRST=1` | the clouds and the horizon before the world, as the game has them, not after its houses |
| `REVC_SPARE=1` (`0`) | the phone is treated as a slow one, or is not, whatever its Direct3D level: the single alpha pass, one program for a pipeline, vehicles in full detail to 30 m, the sky after the houses. With `REVC_D3D_INSTANCE=1`, `REVC_D3D_LATER=1`, `REVC_D3D_THREAD=1` and `REVC_D3D_BUFFERS=3` a fast phone draws as a slow one does |
| `REVC_KEEP_SETTINGS=1` | a phone that is treated as a slow one for the first time keeps the colour filter, the draw distance and the PlayStation 2's alpha test its game has: for a test on somebody's phone (until 1.0.1117.702 the alpha test was turned off all the same, and saved so) |
| `REVC_FRAMESHOT_FULL=1` | the frames `REVC_FRAMESHOT` saves have every pixel of the picture (a series is 320 pixels wide otherwise) |
| `REVC_ATLAS=fit` | only the meshes that do not repeat their texture are drawn from an atlas, without wrapping in the pixel shader: a trial (*The Direct3D 11 renderer*) |
| `REVC_D3D_MIPS=1` | a texture that comes with one level gets its smaller copies when it is loaded (not those in atlases); costs the frames in which a part of the city comes in |
| `REVC_BENCH=fill` | when the menu first comes up: what a layer of pixels over the whole picture costs this phone's GPU, by what it is drawn with and whether it is hidden, and what a hundred thousand triangles cost, by the vertex shader (`d3d fill:` lines) |
| `REVC_D3D_PIXELS=1` (`2`) | the pixels drawn are counted, for the frame and for each of the game's timed parts (`perf:` lines with `REVC_PERF=1`, in pictures' worth); `2`: every pixel a triangle covers, hidden or not, and a wrong picture. No drawing thread, and every frame waits for its count |
| `REVC_D3D_VIEWPORT=60`, `REVC_D3D_SCISSOR=50`, `REVC_D3D_DELAY=15` | measurements: the picture drawn into so many per cent of its width and height; nothing drawn outside so many per cent of them; the drawing thread kept busy for so many milliseconds before each frame's calls |
| `REVC_FULL_DETAIL=1` | a slow phone keeps vehicles in full detail up to 70 m |
| `REVC_PS2_ALPHA=1` (`0`) | the PlayStation 2's alpha test on, or off, for this run, on any phone, whatever the display menu's setting is; the game saves it so the next time it saves its settings |
| `REVC_NO_SHADER_CACHE=1`, `LIBRW_NO_VERTEX_ARRAYS=1`, `LIBRW_NO_VERTICES_FROM_MEMORY=1`, `LIBRW_NO_SHARED_BUFFERS=1`, `LIBRW_NO_ATLASES=1` | without the kept programs, without vertex array objects, with a buffer filled for every immediate draw, with buffers of its own for every geometry, with a draw and a texture of its own for every mesh: to tell what each is worth |
| `LIBRW_PAGE_VERTICES=16384` | how many vertices a page of shared buffers holds (65536 at most, which is what it is left at) |
| `LIBRW_HALF_TEXTURES=1` | textures of 64 pixels and more each way are made half as wide and half as high as they are read: a measurement of what their size costs (*The slow phone*), with a blurred picture |
| `REVC_PROGRAMS=all` (`few`, `noat`) | a phone with OpenGL ES 2.0 picks the cheapest of a pipeline's four programs for each mesh again; `few`: the one with the lights for every mesh (and on a phone with 3.0, the same); `noat`: nothing is discarded by the alpha test, a measurement of what it costs the GPU and not a picture |
| `REVC_FILES_SCREEN=1` (`end`) | the files screen though the game's files are there, as a phone without them has it; `end`: scrolled to its foot, where the port's author and links are. With the snapshot, a look at it from the PC (the phone's screen has to be on: a snapshot of a locked phone is empty) |
| `REVC_LANG=en` (`ru`) | the shell speaks English, or Russian, whatever the phone's language: a look at the one the phone at hand is not set to |
| `REVC_ABOUT_CLOSE=4` | "About the Port" (the game's Options) closes itself after so many seconds: with `REVC_KEYS` walking to it (`40@3,13@4.5,38@6,38@7.5,13@9.5` from the first menu) a run nobody holds the phone for opens it, closes it and goes on in the menu |
| `REVC_SNAPSHOT=1` | the shell saves its XAML (the files screen, the pad; the game's picture is not XAML and comes out black) as `LocalState\snapshot.png` whenever it changes: `phone.py file snapshot.png` |
| `REVC_PAD_PREVIEW=menu` (`foot`, `vehicle`, `cutscene`; `vehicle-xbox`...) | the pad as it is in that part of the game, without a game, with Xbox's names for `-xbox`: with the snapshot, a look at its layout from the PC |
| `ALSOFT_LOGLEVEL=3` | OpenAL Soft says what it does, into the log |

## Licences

The game's code: `LICENSE.md` at the root of the repository. What else is in the package and
under which terms: [`app/THIRD-PARTY.txt`](app/THIRD-PARTY.txt). OpenAL Soft and mpg123 are
LGPL and are separate DLLs. Not made or endorsed by Rockstar Games or Take-Two Interactive.
