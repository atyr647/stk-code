#!/bin/sh
# Equivalence test for the STK_LOW_MEMORY collision mesh (CompactTriangleMesh
# + exact-reserve BVH build): builds the same triangles through stock
# btTriangleMesh and CompactTriangleMesh and requires a byte-identical
# serialized BVH, identical raycast hits and identical AABB queries.
# Usage: run.sh <host build dir with lib/bullet/*.a>
set -e
B=$(cd "$(dirname "$0")/../../.." && pwd)
BL=${1:?host build dir}/lib/bullet
OUT=${TMPDIR:-/tmp}/compact_mesh_test
g++ -O2 -DSTK_LOW_MEMORY -DBULLET_SMALL_MEMORY -I$B/src -I$B/lib/bullet/src \
    -I$B/lib/graphics_engine/include -I$B/lib/irrlicht/include -I$B/lib/glm \
    "$(dirname "$0")/compact_mesh_test.cpp" $B/src/physics/triangle_mesh.cpp \
    $B/lib/bullet/src/BulletCollision/CollisionShapes/btOptimizedBvh.cpp \
    -o $OUT -Wl,--unresolved-symbols=ignore-all \
    $BL/libbulletdynamics.a $BL/libbulletcollision.a $BL/libbulletmath.a
$OUT
