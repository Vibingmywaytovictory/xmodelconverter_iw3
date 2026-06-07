# CoD4 xmodel_export Implementation Notes

## Overview

This document describes all changes made to `xmodelconverter` to add support for
exporting Call of Duty 4 (Modern Warfare) binary model files (`xmodel`, `xmodelparts`,
`xmodelsurfs`) back to the `XMODEL_EXPORT` v6 text format.

The program previously only handled **CoD2** (version `0x14` / 20).  CoD4 uses
version **`0x19` / 25** across all three file types, with format differences in each.

---

## 1. Version Detection & Dispatch (`main.cpp`)

The entry-point already called `deduce_file_info_from_path()` to split the dropped
path into `basepath` and `filepath` (e.g. `"viewmodel_mw2_acr"`).  The xmodel
file version number is the first two bytes of the binary, so a simple peek suffices
to distinguish CoD2 from CoD4.

An early approach routed version-25 files to a self-contained `convert_cod4_xmodel()`
function (from `cod4_model.cpp`).  After reverse-engineering showed that the vertex
format is identical to CoD2, the cleaner solution was to **extend the existing
readers** in-place and remove the special-case dispatch entirely.  The version-25
path now falls through to the same `read_model()` → surface loop as CoD2.

```
// main.cpp – no special dispatch needed; model/parts/surface readers handle both versions
XModel xm;
if (!read_model(xm, basepath, path, valid_xmodel))
    break;
```

---

## 2. Top-Level xmodel File (`model.cpp`)

### Version check
```cpp
// Before
if (version != 0x14)
    return rd.set_error_message("expected xmodel version 0x14, got %x\n", version);

// After
if (version != 0x14 && version != 0x19)
    return rd.set_error_message("expected xmodel version 0x14 or 0x19, got %x\n", version);
```

### Extra byte after maxs (CoD4-specific)

Hex analysis of a real CoD4 xmodel file confirmed that the on-disk layout is
identical to CoD2 **except for one extra byte immediately after the `maxs` vec3**.
All subsequent data (4 LOD slots, collision section, material list) is unchanged.

```
CoD2 header layout:  version(2) + flags(1) + mins(12) + maxs(12)  = 27 bytes
CoD4 header layout:  version(2) + flags(1) + mins(12) + maxs(12) + extra(1) = 28 bytes
```

```cpp
if (version == 0x19)
    rd.read<u8>(); // CoD4 v25: one extra byte after maxs
```

---

## 3. Bone File (`parts.cpp`)

### Version check
Same pattern — accept `0x14` or `0x19`.

### Quaternion format change

CoD2 stores each bone quaternion as **4 × float** (16 bytes, read via `rd.read_quat()`).
CoD4 stores it as **3 × int16** (6 bytes).  The `w` component is reconstructed by
normalising the unit quaternion constraint (`w = sqrt(1 − x² − y² − z²)`).

```
CoD2 per-bone data:  i8 parent(1) + vec3 trans(12) + float×4 quat(16) = 29 bytes
CoD4 per-bone data:  i8 parent(1) + vec3 trans(12) + i16×3 quat(6)   = 19 bytes
```

Evidence: the xmodelparts file has `numbonesrelative=13`.  Binary data before bone
names spans exactly 247 bytes.  `13 × 19 = 247` ✓ (vs `13 × 29 = 377` which
would overshoot).

```cpp
if (version == 0x19) {
    i16 qx = rd.read<i16>(), qy = rd.read<i16>(), qz = rd.read<i16>();
    rot.x = qx / 32767.0f;
    rot.y = qy / 32767.0f;
    rot.z = qz / 32767.0f;
    float w2 = 1.0f - rot.x*rot.x - rot.y*rot.y - rot.z*rot.z;
    rot.w = (w2 > 0.0f) ? sqrtf(w2) : 0.0f;
    rot = glm::normalize(rot);
} else {
    rot = rd.read_quat();
}
```

The rest of `read_xmodelparts_file` (bone names, partclassification bytes) is
unchanged; the structure and field widths are the same between versions.

---

## 4. Geometry File (`surface.cpp`)

### Version check
Same pattern — accept `0x14` or `0x19`.

### Surface header size

CoD2 uses a **7-byte** per-surface header; CoD4 uses **13 bytes**.  The extra
6 bytes are two unknown `u8` fields before `vertcount` and two unknown `u16` fields
between `tricount` and `boneoffset`.

This was confirmed empirically: a brute-force Python scan tried every combination of
header size (4–20 bytes), vertex size (8–128 bytes, step 4), and field offsets for
`numVerts` / `numTris` across all 19 surfaces in the 632,309-byte surfs file.
The **only** solution that produced a byte-perfect walk was:

```
header = 13 bytes
vertex = 60 bytes
triangle = 6 bytes
numVerts at header offset 3 (u16 LE)
numTris  at header offset 5 (u16 LE)
```

```
CoD2 header: tileMode(1) + vertcount(2) + tricount(2) + boneoffset(2)  = 7 bytes
CoD4 header: tileMode(1) + u8(1) + u8(1) + vertcount(2) + tricount(2)
           + u16(2) + u16(2) + boneoffset(2)                           = 13 bytes
```

```cpp
u8 tilemode = rd.read<u8>();
if (version == 0x19) {
    rd.read<u8>();   // unknown field 1
    rd.read<u8>();   // unknown field 2
}
u16 vertcount = rd.read<u16>();
u16 tricount  = rd.read<u16>();
if (version == 0x19) {
    rd.read<u16>();  // unknown field 3
    rd.read<u16>();  // unknown field 4
}
i16 boneoffset = rd.read<i16>();
```

### Vertex format — identical to CoD2

Once the header is consumed, all vertex and triangle data is byte-for-byte
identical to the existing CoD2 rigid-bone path (60 bytes/vertex):

```
normal    vec3  (12 bytes)
color     u32   ( 4 bytes)
u         float ( 4 bytes)
v         float ( 4 bytes)
binormal  vec3  (12 bytes)
tangent   vec3  (12 bytes)
position  vec3  (12 bytes)   ← in bone-local space
─────────────────────────────
total           60 bytes
```

Triangles are three `u16` indices (6 bytes), winding order identical to CoD2.
All CoD4 ACR surfaces have `boneoffset = 0` (rigid, assigned to `j_gun`), so
no skinning code needed for this model.

---

## 5. Material Deduplication (`model.cpp`)

### Problem

The xmodel binary stores **one material name per surface**, not per unique material.
The ACR has 19 surfaces but only 13 distinct materials — several surfaces share the
same material (`mtl_mw2_acr` appears three times, etc.).  Writing all 19 to
`NUMMATERIALS` produced an invalid export.

### Fix

Before writing faces, build a deduplicated material list and a per-mesh remapping:

```cpp
std::vector<std::string> unique_materials;
std::vector<int> mat_remap(this->materials.size(), 0);
for (int i = 0; i < (int)this->materials.size(); ++i)
{
    auto it = std::find(unique_materials.begin(), unique_materials.end(),
                        this->materials[i]);
    if (it == unique_materials.end()) {
        mat_remap[i] = (int)unique_materials.size();
        unique_materials.push_back(this->materials[i]);
    } else {
        mat_remap[i] = (int)(it - unique_materials.begin());
    }
}
```

`TRI` lines then use `mat_remap[meshindex]` as the material slot, and
`NUMMATERIALS` iterates over `unique_materials`.

---

## 6. Build System (`CMakeLists.txt`)

A root `CMakeLists.txt` was created (none existed):

```cmake
cmake_minimum_required(VERSION 3.20)
project(xmodelconverter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(xmodelconverter
    main.cpp model.cpp parts.cpp surface.cpp animation.cpp cod4_model.cpp)

target_compile_definitions(xmodelconverter PRIVATE GLM_ENABLE_EXPERIMENTAL)

target_include_directories(xmodelconverter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/glm)

if(WIN32)
    target_link_libraries(xmodelconverter PRIVATE comdlg32)
endif()
```

`GLM_ENABLE_EXPERIMENTAL` is required to unlock the `glm/gtx/` headers used
throughout the project.  Built with MSVC 19.44 (Visual Studio 2022), x64,
Release configuration.

---

## 7. Summary of File Changes

| File | Change |
|------|--------|
| `CMakeLists.txt` | **Created** — root build file for MSVC/CMake |
| `main.cpp` | Removed CoD4-specific dispatch; include `cod4_model.h` retained |
| `model.cpp` | Accept version `0x19`; skip 1 extra byte after `maxs`; deduplicate materials |
| `parts.cpp` | Accept version `0x19`; read quat as `3×i16` for CoD4 |
| `surface.cpp` | Accept version `0x19`; read 13-byte surface header for CoD4 |
| `cod4_model.cpp` | Added (generated), path construction fixed; superseded by reader changes |
| `cod4_model.h` | Added (generated) |

---

## 8. Known Limitations

- **Bone Z offset**: The root bone (`j_gun`) exports at `(0, 0, 0)` rather than the
  `(0, 0, 0.784111)` seen in mod-tool exports.  That value is a viewmodel-origin
  offset injected by the mod tools at export time; it is not stored in the binary.
  All child bone offsets are consistent (they're off by the same constant) so the
  relative skeleton is correct.

- **Material texture names**: The `MATERIAL` lines use the material slot name
  (e.g. `mtl_mw2_acr.tga`) as a placeholder.  The actual texture filenames are
  stored in the material binary files (`materials/` folder), not in the xmodel.

- **Both surfs files identical**: `viewmodel_mw2_acr1` and `viewmodel_mw2_acr1 surfs`
  are byte-for-byte duplicates.  Only the primary file (named by the LOD string) is
  read.
