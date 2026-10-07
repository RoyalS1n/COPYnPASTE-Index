---
name: dayfall-level
description: Build or change DAYFALL levels through the live DAYFALL editor (the dayfall MCP server): terrain, paths, towns, props, foliage, lighting, collectibles, the player character, and testing routes. Use whenever the task is about a map or level, or the dayfall tools are available.
---

# Building levels in the DAYFALL editor

You are working inside the live editor through the `dayfall` MCP tools. The human sees every change in the
editor window as you make it.

## Before changing anything

When the human says "this", "that one" or "here", call `editor_state`: it returns what they clicked in the editor
window and where they are looking. `editor_select {ids, frame: true}` highlights items for them ("this one?").

1. `project_info`: map, rules, coordinates, what exists, the open batch, warnings.
2. `world_get` (a section or ids; objects come with their world box), `catalog` (what you can place),
   `terrain_info` (height grid).
3. Plan in batches. One theme each: terrain, layout, foliage, gameplay, lighting or character.
4. Name the places you will work on with `area_set` (town, square, forest_north); every `area` argument then takes
   the name.

## Each batch

1. `batch_begin {title}`.
2. A few related edits. Prefer one call with many items (`object_add {objects: [...]}`, `terrain_sculpt {ops: [...]}`).
3. `capture`: the views that show what you changed (`orbit` around it, `top_down` for layouts, `player` for
   the player's eye, named `camera`s).
4. Look at the images. Check: floating or buried objects, gaps and overlaps, scale against the 1.8 m
   mannequin, path continuity, lighting. Fix and capture again.
5. `world_check` (with the batch's `area`): floating, buried and duplicate objects, objects on paths, entities
   inside objects, empty scatter rules. Fix every error and warning, or say why it is intended.
6. `world_diff` lists what the batch changed; `batch_end {notes}` with that and what you saw. It saves the map.

Never change terrain, lighting and character in the same batch; the editor refuses and tells you why.

## Testing

- `route_set` then `walk_test` before any art: every stuck / fall / steep / water event is a bug to fix.
- `play_sim` for jumps, ledges and stairs: scripted move / run / jump / turn through the real controller.
- Its captures show the in-game HUD (minimap, collectible counter, messages). `hud_set` changes the HUD (a
  gameplay edit); `capture {"hud": true}` previews it while editing, `"hud": false` hides it.

## Facts

- Metres, Z up, X east, Y north. `yaw_deg` turns counter-clockwise from +X (0 = east, 90 = north).
- `[x, y]` positions sit on the ground and follow terrain edits; `[x, y, z]` is absolute.
- Primitives are parametric (`{"type": "box", "size": [w, d, h], "material": "plaster"}`); their origin is
  the bottom centre (gems: the centre).
- Paths flatten and carve the ground under them; moving or deleting a path restores the ground.
- Building sites: `find_space {size: [w, d], near or area, near_path}` returns level, free spots with the yaw that
  faces the path; level a site with the `terrain_sculpt` op it suggests.
- Relative placement: `object_add` with `place: {on: id}` (on its top), `{next_to: id, side: east, gap_m}` or
  `{relative_to: id, offset: [dx, dy]}` (in its frame) instead of `position`; also in `object_update set`.
- Groups: `object_update {tag, rotate_by_deg, pivot: "center"}` turns them together; `object_duplicate` makes
  rows (offset) and rings (rotate_by_deg about a pivot).
- `undo` reverts edits; `batch_end` saves.

Tool reference: `docs/TOOLS.md`. Map format: `docs/MAP_FORMAT.md`.
