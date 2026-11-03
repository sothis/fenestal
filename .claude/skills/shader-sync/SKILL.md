---
name: shader-sync
description: Verify a push-constant, vertex-instance, or UBO struct still agrees across the GLSL shader, the C struct in src/core/app.h, and the _Static_assert that pins its layout. Use after editing any of those three, or when a size/offset assert fires.
---

Three things describe every GPU-visible struct in this project, and all three
have to move together:

1. the GLSL declaration in `shaders/*` — or `testbed/shaders/*` for
   the example's own scene (push constant block, `layout(location=)`
   instance inputs, or UBO block),
2. the C struct in `src/core/app.h` or `src/renderer/vulkan/vk_internal.h` —
   or in `testbed/` for the scene's,
3. the `_Static_assert` under it pinning `sizeof` and the `offsetof`s.

The assert exists because a silent mismatch does not crash — it draws garbage,
or draws correctly on the machine it was written on. Never "fix" a firing assert
by editing its expected number to match the new `sizeof`; that removes the only
check there is. Change the number only once you have confirmed the shader agrees.

## What to check

Work from `src/core/app.h` for the instance structs and
`src/renderer/vulkan/vk_internal.h` for the push-constant structs — grep
`_Static_assert` in both for the full list — and
`testbed/scene_vulkan.c` for the scene's, which live with the example that
owns them. The pinned structs and their shaders are:

| C struct | Shader | Pinned |
|---|---|---|
| `ScenePush` (in each `testbed/scene_*`: Vulkan, GL, D3D12, Metal) | `testbed/shaders/scene.frag`, `star.vert` | `sizeof == 16`, `time` at 8, `srgb` at 12 |
| `TextPush` | `shaders/text.*` | `sizeof == 16`, `srgb` at 8, `texel_u` at 12 |
| `OverlayPush` | `shaders/*.vert` | `sizeof == 8` |
| `RectInstance` | `shaders/rect.vert` | `sizeof == 32`, `color` at 16 |
| `ShapeInstance` | `shaders/shape.vert` | `sizeof == 48`, `p2` at 16, `color` at 32 |
| `GlyphInstance` | `shaders/text.vert` | `sizeof == 48`, `uv` at 16, `color` at 32 |
| `FnlDrawState` | — | `sizeof == 380`; not a GPU struct, but pinned for the same reason |

`FnlDrawState` is the one that is not about the GPU at all. It is compared field
by field in `fnl_draw_state_eq()` rather than with `memcmp`, because its padding
is never written — so the assert is a tripwire: adding a field changes the size,
fails the build, and forces the comparison to be updated with it. A field added
to `FnlDrawState` and forgotten in `fnl_draw_state_eq()` is a frame that never
re-records when that field changes. It carries the prefix because `<windows.h>`
declares a `DrawState` of its own.

For each struct the user touched:

- **Field order and type match** the shader block, member for member.
- **std140 / std430 alignment holds** if it is a UBO: `vec3` aligns to 16, an
  array's stride is rounded up, and a `float` after a `vec3` slots into the pad
  rather than starting a new one. A C struct that "looks the same" is the usual
  way this breaks.
- **Vertex instance attributes**: every `layout(location = N)` in the `.vert` has
  a matching `VkVertexInputAttributeDescription` with the right `format` and
  `offset`, and the binding's `stride` equals the C `sizeof`.
- **Push constant ranges**: the `VkPushConstantRange` size and `stageFlags`
  cover the block as the shader declares it, in every pipeline that uses it.
- **The assert's numbers** reflect the new layout, and the message still names
  the right shader.
- **The translations still hold** (DESIGN *One shader source*): a library
  push block is still named `Push`, it holds nothing whose std140 offset
  differs from its std430 one (a `vec3`, an array, a struct), since GL reads
  it as a std140 uniform block, and every stage of one program that declares
  it declares it whole. The build links each GL program from its translated
  stages (`gl_program()` in `CMakeLists.txt`); a new library shader pairing
  wants a line there.

Also check whether the struct is declared or written in more than one place. A
push block used by two stages is declared whole in each of them, and a struct
the example owns is asserted in the example rather than in `src/core/app.h` —
`ScenePush` in `testbed/scene_vulkan.c`, `scene_gl.c`, `scene_d3d12.c` and
`scene_metal.m` against `testbed/shaders/scene.frag` is the standing case.
Every declaration of one block has to move together.

Report what agrees and what does not. Do not build to check unless asked;
`_Static_assert` is a compile-time check the user will hit on their next build.
