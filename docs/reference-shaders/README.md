# Reference shaders (pre-migration, GL-runtime GLSL — not Vulkan-compatible)

These are leftover shaders from the OpenGL renderer, written for runtime
compilation via the old `Render::Shader` class (deleted during the NVRHI
migration — see `docs/nvrhi-migration-todo.md`). They don't compile as-is
under Vulkan/SPIR-V (loose non-block uniforms, no explicit `layout(location)`
on every input/output), and nothing in the engine references them anymore.

Moved here (out of `assets/shaders/`, which is now glob-scanned and
build-time-compiled to SPIR-V for every `.vert`/`.frag`/`.comp` it contains)
rather than deleted, since they're worth referencing when these features come
back on the NVRHI/Vulkan side:

- `mesh.vert` / `mesh.frag` — the pre-migration PBR mesh shader (Cook-Torrance
  BRDF, full normal/metallic/roughness texture maps, TBN-matrix normal
  mapping). `cube_min.vert/frag` (current, `assets/shaders/`) already reuses
  this BRDF math for clustered lighting, but with fixed `roughness`/`metallic`
  constants instead of material maps — this is the reference for reintroducing
  real per-material PBR texture maps.
- `fxaa.frag` / `screen.vert` — the MSAA+FXAA post-process pass
  (`Application::RebuildPostProcess`, deleted outright during the migration —
  see `docs/nvrhi-migration-todo.md` section 3's still-open post-process
  decision). Reference for an NVRHI render-target + FXAA pass if that comes
  back instead of staying dropped.
