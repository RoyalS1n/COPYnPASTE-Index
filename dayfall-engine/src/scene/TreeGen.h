#pragma once
// Procedural trees, a mesh type like the primitives: {"type": "tree", "species": "oak|birch|pine|bush|dead", "height",
// "crown_radius", "trunk_radius", "seed", "leaves", "detail", "bark_material", "leaf_material"}.
// Broadleaf crowns grow by space colonisation (Runions, Lane & Prusinkiewicz 2007, "Modeling Trees with a Space
// Colonization Algorithm"): attraction points fill the crown, branch tips grow toward the points near them and the
// points they reach die. Branch thickness follows the pipe model (r^n = sum of the children's r^n). Pines are built
// as a straight leader with whorls of drooping branches. Origin: the base of the trunk.
#include "scene/MeshAsset.h"
#include <nlohmann/json.hpp>

namespace df {
struct TreeShape {
    float height = 10, crownRadius = 4, trunkRadius = 0.3f, crownBase = 3;
};
// The resolved size of a tree spec (species defaults filled in); throws df::Error on bad parameters.
TreeShape treeShape(const nlohmann::json& spec);
MeshAsset makeTree(const nlohmann::json& spec);
nlohmann::json treeCatalog();
}  // namespace df
