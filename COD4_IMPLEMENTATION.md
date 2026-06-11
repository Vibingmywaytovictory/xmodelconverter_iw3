# CoD4 / MW2 / MW3 export implementation notes

This document describes the changes made to `xmodelconverter` to read the
binary asset files used by **Call of Duty 4 and the Infinity Ward titles that
share its `version 25 / 0x11` family** (CoD4 / MW2 / MW3 / Ghosts, plus BO2
ports that reuse the same on-disk layout) and convert them back to the
`XMODEL_EXPORT v6` and `XANIM_EXPORT v3` text formats for editing.

The program originally handled only **CoD1/2** (`xmodel` version `0x14` / 20 and
`xanim` version `0xe` / 14). Everything below documents the newer formats and
how they were reverse-engineered and validated.

> **Validation.** Every format claim here was checked against official mod-tool
> output (`XMODEL_EXPORT` / `XANIM_EXPORT` files exported by the CoD4 and MW2
> mod tools). Decoded world-space offsets reproduce the ground truth to within
> ~0.06 units, which is the quantization precision stored in the binaries.

---

## 0. Format versions at a glance

| Asset            | CoD1/2 (old)     | CoD4 / IW family (added)                |
|------------------|------------------|-----------------------------------------|
| `xmodel`         | `0x14` (20)      | `0x19` (25)                             |
| `xmodelparts`    | `0x14` (20)      | `0x19` (25)                             |
| `xmodelsurfs`    | `0x14` (20)      | `0x19` (25)                             |
| `xanim`          | `0xe` (14)       | **`0x11` (17)** — *not* 14/15           |
| `.iwi` textures  | `IWi5`           | `IWi6` (DXT1 / DXT3 / DXT5)             |

A key discovery: **CoD4's binary `xanim` is version 17 (`0x11`)**, the same as
MW2/MW3/Ghosts — version 14 is CoD1/2 only. The mod-tool `XANIM_EXPORT` header
literally reads "Call of Duty 4", confirming this.

---

## 1. Top-level model — `model.cpp`

`XModel::read_xmodel_file` accepts version `0x14` or `0x19`. The v25 header is
identical to v20 except for **one extra byte immediately after `maxs`**:

```
v20 header: version(2) flags(1) mins(12) maxs(12)
v25 header: version(2) flags(1) mins(12) maxs(12) extra(1)
```

Everything after (4 LOD slots, collision section, `u16 nummaterials` + material
name list) is unchanged.

### Material de-duplication
The binary stores **one material name per surface**, not per unique material, so
several surfaces share a name. Before writing `NUMMATERIALS` the exporter builds
a deduplicated list plus a per-mesh remap, and `TRI` lines reference the remapped
unique-material index.

---

## 2. Bone file — `parts.cpp`

`XModelParts::read_xmodelparts_file` accepts `0x14` or `0x19`. The only
difference is the per-bone rotation encoding:

```
v20 per-bone: i8 parent(1) + vec3 trans(12) + 4x float quat(16) = 29 bytes
v25 per-bone: i8 parent(1) + vec3 trans(12) + 3x i16 quat(6)    = 19 bytes
```

v25 stores the quaternion as three `int16` components (`x,y,z / 32767`); `w` is
reconstructed from the unit constraint `w = sqrt(1 - x² - y² - z²)`. Bone names
and the per-bone "partclassification" bytes are unchanged.

---

## 3. Geometry file — `surface.cpp`

This was the most involved model change. `read_xmodelsurface_file` accepts
`0x14` or `0x19`. v25 surfaces come in **two kinds with different headers and
vertex layouts**, distinguished by one field.

### Per-surface header (v25)
```
tileMode(1) unk(2) vertCount(2) triCount(2) firstGroup(2) ...
```
- **RIGID** — `firstGroup == vertCount`. Header is **13 bytes**: the remaining
  two shorts are a *default skin bone* and a `0`. Vertices are **60 bytes**,
  stored in **model space**. The whole surface rides one bone during animation,
  so the verts are weighted to that default skin bone (recorded in `weightbone`)
  but positioned through the root.
- **SKINNED** — `firstGroup != vertCount`. The header is **variable length**: a
  list of `(groupVertCount, bonesPerVert)` pairs terminated by a `(0,0)` pair
  (the group counts sum to `vertCount`). e.g. 2 groups → 19-byte header, 3
  groups → 23-byte header. Vertices carry a per-vertex bone index and optional
  blend weights.

The original "vibe" implementation overfit a single rigid model: it used a fixed
13-byte header and read the rigid/skinned flag from the wrong offset, so it never
detected skinned surfaces and **crashed** on skinned models. The `firstGroup ==
vertCount` test is the reliable discriminator.

### Vertex layout (both kinds)
```
normal     vec3  (12)   ; negated on read
color      u32   ( 4)
u, v       float ( 8)
binormal   vec3  (12)
tangent    vec3  (12)
[skinned only] numWeights u8 (1) + boneIndex u16 (2)
position   vec3  (12)   ; model-space (rigid) or bone-local (skinned)
[if numWeights > 0] pad u8 (1) + numWeights × (blendIndex u16 + vec3 + weight u16)
```
Triangles are three `u16` indices each (winding reordered `0,2,1`).

### Robustness guards
A desynced/unsupported surface header (see *grenade surfaces* under Known
Limitations) used to produce bogus counts and read past the buffer (access
violation). The reader now bounds-checks the skinned-header walk, the
vert/tri counts (`vertCount*48 + triCount*6 ≤ remaining`), the per-vertex bone
index, and triangle indices, and returns a clean error instead of crashing.

---

## 4. Textures — `iwi.h` + `main.cpp`

### IWi5 and IWi6 (`iwi.h`)
The reader accepts `IWi5` (CoD4) and `IWi6` (MW2/MW3/Ghosts/BO2). For the fields
used (format, usage, dimensions, mip table) the two layouts are identical:

```
"IWi"(3) version(1) format(1) usage(1) width(2) height(2) depth(2) fileSize(4) mipTable...
```

- **Format → DDS FourCC**: DXT1 → `DXT1`, **DXT3 → `DXT3`**, otherwise `DXT5`.
  (The old code wrote `DXT5` for anything that wasn't DXT1, corrupting DXT3.)
- **Mip extraction**: the full-resolution mip is always the **last block** in the
  file. Its size is derived directly from the dimensions (DXT1 = `w*h/2`,
  DXT3/DXT5 = `w*h`, with 4×4 block rounding) and taken from the tail. This is
  robust for single-mip images (the "no-mipmap" flag makes the mip table
  degenerate — every entry equals the file size — which previously produced a
  0-byte texture).

### Material parsing (`main.cpp`)
A material file contains a texture table of 12-byte entries
`(semanticNamePtr, typeFlags, imageNamePtr)`. The converter matches the semantic
strings `colorMap`, `normalMap`, `specularMap` and converts **all three** maps
(previously only the color map). Empty slots are skipped.

Converted `.dds` files are written into an **`exported/images/`** subfolder
(created on demand) so the `exported/` folder stays tidy.

---

## 5. Animations — `animation.cpp` (the big one)

CoD4/MW `xanim` is version **`0x11` (17)**. `read_xanim_file` accepts `0xe` or
`0x11`.

### Header
```
version(2) numFrames(2) numParts(2) flags(1) [v17: extra(1)] framerate(2) [v17: extra(2)]
```
`flags` bit 0 = looping, bit 1 = delta. If delta, a `tag_origin`
rotation+translation block is read first; if looping, `numFrames` is incremented.

### Flag arrays
Two bitfields (flip, simple) follow, **`numParts>>3` bytes each** in v17 (v14
used `((numParts-1)>>3)+1`). In v17 these flags are advisory only (see
rotations).

### Per-part data — rotations then translations
**Rotations:** `u16 count`; frame indices (omitted if `count ∈ {1, numFrames}`,
else `u8` when `numFrames ≤ 255` else `u16`); then per key:
- **full** — `3× i16` (`/32767`, `w` reconstructed), or
- **2-byte single-axis** — a single `i16` (one axis; `x=y=0`).

The simple-quat flag is **not authoritative** — some single-axis bones (notably
grenade-launcher `j_slider_m320` / `j_grenade_ammo`) use the 2-byte form without
the flag set. So v17 **always probes**: read 2-byte only if reading the full
`3× i16` form would leave a malformed translation block immediately after
(`peek_translation_valid`). This conservative lookahead resolves every grenade
variant with no false positives.

**Translations:** `u16 count`; if `count == 1`, a single `vec3` float; otherwise
frame indices + a flag byte + `vec3 mins` + `vec3 size` (float), then one sample
per key:
- `flag == 1` → `3× u8`, value = `mins + (q/255) · size`
- `flag == 0` → `3× u16`, value = `mins + (q/65535) · size` (higher precision)

### Notify / notetracks
The binary ends with `u8 count` notify entries (`string + u16 frame`, **0-based**).
These are stored and emitted as a real `NOTETRACKS` section on `PART 0`:
```
NOTETRACK 0
NUMKEYS n
FRAME <frame> "<sound name>"
```
Previously they were read and discarded.

### Output (`export_file`)
`XANIM_EXPORT v3`: `NUMPARTS` / `PART` list from the reference skeleton, then
per-frame **world-space** matrices via `util::get_world_transform`. Sparse
keyframes are **interpolated** (slerp for rotation, lerp for translation) so
every output frame is filled, matching the mod tools.

---

## 6. Reference skeleton merge — `main.cpp` + `model.cpp`

A binary xanim animates the whole hand+weapon rig (~82 parts), but the binary
weapon model only carries ~14 weapon bones, and **no binary in the set holds the
hand skeleton**. The hand skeleton exists only as an `XMODEL_EXPORT` text rig.

- `read_xmodel_export_skeleton` (`model.cpp`) parses a hands rig from
  `XMODEL_EXPORT` text: `BONE` hierarchy + world bind matrices, converted to
  parent-local transforms (`local = inverse(parentWorld) · world`, via
  `glm::quat_cast`).
- `read_animation` auto-discovers `<basepath>/hands_skeleton/*.XMODEL_EXPORT` and
  **merges** it with the weapon model: hands bones first, then the weapon bones,
  reparenting the weapon root (`j_gun`) onto `tag_weapon`. If no rig is present it
  falls back to weapon-only output (legacy behaviour).
- Anim part names (lowercase) are matched to rig bone names (mixed case)
  **case-insensitively**.
- **Weapon-bone translations are deltas from the model bind** (added to the bind
  local), whereas hand-bone translations are absolute local. This was found by
  comparing decoded output to ground truth: `tag_flash` rel `j_gun` =
  `bind(28.11) + anim(-4.24) = 23.87`, matching exactly.

Result: ~85 parts emitted (all 82 animated bones + a few static rig bones),
matching ground truth to quantization precision.

---

## 7. Build system — `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.20)
project(xmodelconverteriw3 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
add_executable(xmodelconverteriw3 main.cpp model.cpp parts.cpp surface.cpp animation.cpp)
target_compile_definitions(xmodelconverteriw3 PRIVATE GLM_ENABLE_EXPERIMENTAL)
target_include_directories(xmodelconverteriw3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/glm)
if(WIN32)
    target_link_libraries(xmodelconverteriw3 PRIVATE comdlg32)
endif()
```
Built with MSVC (Visual Studio 2022), x64, Release.

### Usage
Drop the weapon model and asset onto the exe, or pass as arguments:
```
xmodelconverteriw3  xmodel\<weapon>            # model + textures -> exported/
xmodelconverteriw3  xmodel\<weapon>  xanim\<anim>   # model is the reference, anim -> exported/
```
For full-skeleton animation output, place a hands rig at
`hands_skeleton\<viewhands>.XMODEL_EXPORT`; it is merged automatically.

---

## 8. Known limitations

- **Grenade-launcher surfaces** (`viewmodel_m16m203`, m320 variants): the v25
  *surface* layout for the grenade attachment is a further variant that has not
  been reverse-engineered. The **model** export of such weapons fails with a
  clean "unsupported variant" error (no longer a crash). Their **animations**
  export correctly when paired with a working reference model.
- **ADS fire anims** carry a constant global offset (≈4–5 units, *sight
  dependent*: iron sight 4.07, optics 5.0). This is the ADS sight-to-eye
  alignment the engine/mod tools bake into ADS exports from the model's sight
  tag — it is **not stored in the binary** (no bone is animated for it), so the
  converter outputs the raw animation without it.
- **Frame numbering** is 0-based; the original Maya timeline base (e.g. 221) is
  not stored in the binary. Notetracks use the same 0-based frames, so the file
  is internally consistent.
- Output has a few fewer parts than the mod-tool export (e.g. 85 vs 88): the
  extra Maya FK/SDK control bones are baked out of the compiled binary and are
  unrecoverable.

---

## 9. Summary of changes

| File             | Change |
|------------------|--------|
| `model.cpp`      | Accept xmodel v25 (extra byte after maxs); material de-dup; **`read_xmodel_export_skeleton`** (parse a hands rig from `XMODEL_EXPORT` text). |
| `parts.cpp`      | Accept v25; read bone quaternions as `3× i16`. |
| `surface.cpp`    | Accept v25; rigid vs skinned detection; variable skinned header; rigid default-skin-bone; bounds-check guards against bad counts/indices. |
| `iwi.h`          | Accept `IWi6`; emit `DXT3` FourCC; derive mip0 size from dimensions (fixes single-mip). |
| `main.cpp`       | Convert color/normal/specular maps; write `.dds` to `exported/images`; merge hands rig + weapon model as the xanim reference (weapon-bone delta translations). |
| `animation.cpp`  | Parse xanim v17 (header, flag arrays, `3× i16` + 2-byte rotations via lookahead, quantized translations); keyframe interpolation; write notetracks; case-insensitive bone matching. |
| `types.h`        | v25 surface/quat fields; `XAnim` members for notetracks, merged reference, weapon-bone start; skeleton-parser declaration. |
| `util.h`         | `to_lower`, `make_directory`, `find_first_xmodel_export`. |
| `CMakeLists.txt` | Root build file (MSVC/CMake). |
