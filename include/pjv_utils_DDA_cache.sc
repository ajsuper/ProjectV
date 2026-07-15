#define MAX_STACK_SIZE 12
#define RENDER_MODE 3 //1-8
#define VOXEL_TYPEDATA_SLICES 3

// World-unit scale. Voxels are now 1 world unit (voxelScale = 1); the shaders were tuned for
// ~0.0727-unit voxels, so the scene is ~13.76x larger. Multiply world-space DISTANCE constants
// (self-hit ray-origin offsets, ray lengths) by this; leave voxel-relative / step-count values.
#ifndef WORLD_SCALE
#define WORLD_SCALE 13.76
#endif

/* File structure:
 * - Uniforms
 * - Textures
 * - Constants
 * - Structs
 * - Traversal arrays.
 * - LUT's
 * - box/ray intersections
 * - Traversal helper functions
 * - Traversal functions
 */

USAMPLER2D(tree64Data, 13);
USAMPLER2D(voxelTypeData, 14);
USAMPLER2D(headerData, 15);
USAMPLER2D(gridInfo, 11);   // Top-level grid descriptors + scene counts (see gpu_interface.cpp).
USAMPLER2D(cellMap, 12);    // Per-grid cell -> chunk index maps (0xFFFFFFFF = empty).
USAMPLER2D(looseList, 10);  // Compact list of loose chunk handles (header rows) to broadphase.

struct RayQuery {
    bool doTransparency = true;
    uint startLOD = 0;
    uint finishLOD = 0;
    uint distanceToFinishLOD = 0; // Measured in voxels.
    uint maxRaySteps = 100;
};

// User defined return struct from render function.
struct returnStruct {
    vec4 globalIllumination;
    vec4 ambientOcclusion;
    vec4 directIllumination;
    vec4 normal;
    vec4 albedo;
    vec4 motionVector;
};

//Box structure with a position(min of x, y, z) and a size.
struct BoxAABB {
    vec3 position;
    float size;
};

struct Tree64NodeData {
    uint data1;
    uint data2;
    uint data3;
};

struct CombinedNode64 {
    //BoxAABB boundingBox; // Every node has one.
    uint thisNodeZOrderInParent; // Root node won't have one since it has no parent.
    uint dataIndex; // Every node will have one except for the candidate node.
};

struct PushResults {
    bool continueMarching;
    bool successfullyPushed;
};

struct AdvanceResults {
    bool continueMarching;
    bool leaveChild;
};

//Ray structure with an origin, direction, and color.
struct Ray {
    vec3 origin;
    vec3 direction;
};

struct IntersectionResult {
    float distance;    // Distance from ray origin to intersection point
                       // -1.0f means no intersection
                       // 0.0f means ray starts inside box (for entry function)
    vec3 normal;       // Normal vector at intersection point
                       // {0, 0, 0} means no intersection or ray starts inside box
                       // For entry normals: points OUTWARD from box face
                       // For exit normals: points OUTWARD from box face
};

struct TraversalNode {
    BoxAABB boundingBox;
    uint ZOrderInParent; // (0, 0), (1, 0), (0, 1)... Will go up to 16 for branching factor of 4.
    //uint dataIndex;
};

struct chunkHeader { // Not designed to be user interfacable on CPU. Only exists during runtime, mainly on GPU. Only the necessary information for rendering.
    uint chunkID;
    float positionX;
    float positionY;
    float positionZ;
    float scale;
    uint resolution;
    uint geometryStartIndex;
    uint geometryEndIndex;
    uint voxelTypeDataStartIndex;
    uint voxelTypeDataEndIndex;
    uint padding[2];
    vec4 rotation; // World rotation quaternion [x, y, z, w]. Identity = (0,0,0,1).
};

struct GPUChunkHeader { // Not designed to be user interfacable on CPU. Only exists during runtime, mainly on GPU. Only the necessary information for rendering.
    uint chunkID;
    float positionX;
    float positionY;
    float positionZ;
    float scale;
    uint resolution;
    uint geometryStartIndex;
    uint geometryEndIndex;
    uint voxelTypeDataStartIndex;
    uint voxelTypeDataEndIndex;
    uint dataRefID;
    uint padding[1];
};

struct Voxel {
    uint index;
    vec3 color;
    vec3 normal;
};

struct SceneIntersectData {
    uint steps;
    uint headerIndex;
    BoxAABB foundBox;
    // Authoritative hit data straight from the DDA's own arithmetic. Callers must use
    // these instead of re-intersecting foundBox analytically: a fresh slab test on a
    // boundary-exact hit disagrees with the march by ULPs and misclassifies the hit.
    float rayT;  // Distance to the hit along the ray. -1.0 on a miss.
    vec3 normal; // Outward normal of the face the ray entered through. (0,0,0) when the
                 // ray started inside the volume and hit before its first DDA step.
};

static uint nodeStackZOrder[5];
static uint nodeStackDataIndex[5];
static Tree64NodeData dataCache[5];
static uint nodeStackQuantity = 0;

// Per-axis coarse forward masks for the skip optimization. For a 4x4x4 Z-order
// (bits: Z1,Y1,X1,Z0,Y0,X0), each axis has a lower-half mask (axis coord 0-1)
// and an upper-half mask (coord 2-3). Only 6 masks total (2 per axis).
// Index: axis*2 + half   axis: 0=X,1=Y,2=Z   half: 0=LO,1=HI
//
// Bit ordering matches checkZOrderInValidMasks: bit 31 = z-order 0,
// bit 0 = z-order 31 (within each word). z-orders 32-63 go to FWD_MASK_HI
// with the same reversal.
const uint FWD_MASK_LO[6] = {
    0xFF00FF00u,  // X_LO — z-order bit3=0  (X coordinate 0 or 1)
    0x00FF00FFu,  // X_HI — z-order bit3=1  (X coordinate 2 or 3)
    0xFFFF0000u,  // Y_LO — z-order bit4=0  (Y coordinate 0 or 1)
    0x0000FFFFu,  // Y_HI — z-order bit4=1  (Y coordinate 2 or 3)
    0xFFFFFFFFu,  // Z_LO — z-order bit5=0  (Z coordinate 0 or 1)
    0x00000000u   // Z_HI — z-order bit5=1  (Z coordinate 2 or 3)
};

const uint FWD_MASK_HI[6] = {
    0xFF00FF00u,  // X_LO
    0x00FF00FFu,  // X_HI
    0xFFFF0000u,  // Y_LO
    0x0000FFFFu,  // Y_HI
    0x00000000u,  // Z_LO
    0xFFFFFFFFu   // Z_HI
};

float getRayBoxEntryDistance(Ray ray, BoxAABB box) {
    IntersectionResult r;
    if (box.size <= 0) {
        return -1.0;
    }

    vec3 boxMin = box.position;
    vec3 boxMax = box.position + vec3(box.size);

    // Inside check (required to preserve behavior)
    if (all(greaterThanEqual(ray.origin, boxMin)) &&
        all(lessThanEqual(ray.origin, boxMax))) {
        return 0;
    }

    vec3 invDir = 1.0 / ray.direction;

    vec3 t0 = (boxMin - ray.origin) * invDir;
    vec3 t1 = (boxMax - ray.origin) * invDir;

    vec3 tNearVec = min(t0, t1);
    vec3 tFarVec  = max(t0, t1);

    float tNear = max(max(tNearVec.x, tNearVec.y), tNearVec.z);
    float tFar  = min(min(tFarVec.x,  tFarVec.y),  tFarVec.z);

    // Miss test (cannot be removed)
    if (tNear > tFar || tFar < 0.0) {
        return -1;
    }
    return tNear;
}

float getRayBoxEntryDistanceForSureHit(Ray ray, BoxAABB box) {
    vec3 boxMin = box.position;
    vec3 boxMax = box.position + vec3(box.size);

    vec3 invDir = 1.0 / ray.direction;

    vec3 t0 = (boxMin - ray.origin) * invDir;
    vec3 t1 = (boxMax - ray.origin) * invDir;

    vec3 tNearVec = min(t0, t1);

    // Entry distance = max component of tNearVec
    float tNear = max(max(tNearVec.x, tNearVec.y), tNearVec.z);

    // Clamp to zero to preserve behavior for rays starting inside
    return max(tNear, 0.0);
}

IntersectionResult getRayBoxEntry(Ray ray, BoxAABB box) {
    IntersectionResult r;
    if (box.size <= 0) {
        r.distance = -1.0;
        r.normal = vec3(0.0);
        return r;
    }

    vec3 boxMin = box.position;
    vec3 boxMax = box.position + vec3(box.size);

    // Inside check (required to preserve behavior)
    if (all(greaterThanEqual(ray.origin, boxMin)) &&
        all(lessThanEqual(ray.origin, boxMax))) {
        r.distance = 0.0;
        r.normal   = vec3(0.0);
        return r;
    }

    vec3 invDir = 1.0 / ray.direction;

    vec3 t0 = (boxMin - ray.origin) * invDir;
    vec3 t1 = (boxMax - ray.origin) * invDir;

    vec3 tNearVec = min(t0, t1);
    vec3 tFarVec  = max(t0, t1);

    float tNear = max(max(tNearVec.x, tNearVec.y), tNearVec.z);
    float tFar  = min(min(tFarVec.x,  tFarVec.y),  tFarVec.z);

    // Miss test (cannot be removed)
    if (tNear > tFar || tFar < 0.0) {
        r.distance = -1.0;
        r.normal   = vec3(0.0);
        return r;
    }

    // Normal selection (branch-minimal, exact)
    vec3 normal = vec3(0.0);

    
    if (tNear == tNearVec.x)
        normal = vec3(-sign(ray.direction.x), 0.0, 0.0);
    else if (tNear == tNearVec.y)
        normal = vec3(0.0, -sign(ray.direction.y), 0.0);
    else
        normal = vec3(0.0, 0.0, -sign(ray.direction.z));
    

    r.distance = tNear;
    r.normal   = normal;
    return r;
}

IntersectionResult getRayBoxExit(Ray ray, BoxAABB box) {
    IntersectionResult r;
    if (box.size < 0) {
        r.distance = -1.0;
        r.normal = vec3(0.0);
        return r;
    }

    vec3 boxMin = box.position;
    vec3 boxMax = box.position + vec3(box.size);

    vec3 invDir = 1.0 / ray.direction;

    vec3 t0 = (boxMin - ray.origin) * invDir;
    vec3 t1 = (boxMax - ray.origin) * invDir;

    vec3 tNearVec = min(t0, t1);
    vec3 tFarVec  = max(t0, t1);

    float tNear = max(max(tNearVec.x, tNearVec.y), tNearVec.z);
    float tFar  = min(min(tFarVec.x,  tFarVec.y),  tFarVec.z);

    if (tNear > tFar || tFar < 0.0) {
        r.distance = -1.0;
        r.normal   = vec3(0.0);
        return r;
    }

    vec3 normal = vec3(0.0);

    if (tFar == tFarVec.x)
        normal = vec3(sign(ray.direction.x), 0.0, 0.0);
    else if (tFar == tFarVec.y)
        normal = vec3(0.0, sign(ray.direction.y), 0.0);
    else
        normal = vec3(0.0, 0.0, sign(ray.direction.z));

    r.distance = tFar;
    r.normal   = normal;
    return r;
}

IntersectionResult getRayBoxExitForSureHit(Ray ray, BoxAABB box) {
    IntersectionResult r;

    vec3 boxMin = box.position;
    vec3 boxMax = box.position + vec3(box.size);

    vec3 invDir = 1.0 / ray.direction;

    vec3 t0 = (boxMin - ray.origin) * invDir;
    vec3 t1 = (boxMax - ray.origin) * invDir;

    vec3 tFarVec = max(t0, t1);

    float tFar = min(min(tFarVec.x, tFarVec.y), tFarVec.z);
    r.distance = tFar;

    // Normal selection (branch-minimal)
    if (tFar == tFarVec.x)
        r.normal = vec3(sign(ray.direction.x), 0.0, 0.0);
    else if (tFar == tFarVec.y)
        r.normal = vec3(0.0, sign(ray.direction.y), 0.0);
    else
        r.normal = vec3(0.0, 0.0, sign(ray.direction.z));

    return r;
}


float getRayBoxExitDistanceForSureHit(Ray ray, BoxAABB box) {
    vec3 boxMin = box.position;
    vec3 boxMax = box.position + vec3(box.size);
    vec3 invDir = 1.0 / ray.direction;

    vec3 t0 = (boxMin - ray.origin) * invDir;
    vec3 t1 = (boxMax - ray.origin) * invDir;

    // tFar = distance to exit the box
    vec3 tFarVec = max(t0, t1);
    return min(min(tFarVec.x, tFarVec.y), tFarVec.z);
}


uint tree64s(int index) {
    ivec2 texSize = textureSize(tree64Data, 0);
    int pixelIndex = index / 4;
    int x = pixelIndex % texSize.x;
    int y = pixelIndex / texSize.x;
    int colorIndex = index % 4;
    uvec4 pixel = texelFetch(tree64Data, ivec2(x, y), 0);
    return pixel[colorIndex];
}

uint voxelTypeDatas(int index) {
    ivec2 texSize = textureSize(voxelTypeData, 0);
    int pixelIndex = index / 4;
    int x = pixelIndex % texSize.x;
    int y = pixelIndex / texSize.x;
    int colorIndex = index % 4;
    uvec4 pixel = texelFetch(voxelTypeData, ivec2(x, y), 0);
    return pixel[colorIndex];
}

chunkHeader headers(int headerIndex) {
    int index = headerIndex * 4;

    uvec4 pixel0 = texelFetch(headerData, ivec2(index + 0, 0), 0);
    uvec4 pixel1 = texelFetch(headerData, ivec2(index + 1, 0), 0);
    uvec4 pixel2 = texelFetch(headerData, ivec2(index + 2, 0), 0);
    uvec4 pixel3 = texelFetch(headerData, ivec2(index + 3, 0), 0);

    chunkHeader header;
    header.chunkID = pixel0.r;
    header.positionX = uintBitsToFloat(pixel0.g);
    header.positionY = uintBitsToFloat(pixel0.b);
    header.positionZ = uintBitsToFloat(pixel0.a);
    header.scale = uintBitsToFloat(pixel1.r);
    header.resolution = pixel1.g;
    header.geometryStartIndex = pixel1.b;
    header.geometryEndIndex = pixel1.a;
    header.voxelTypeDataStartIndex = pixel2.r;
    header.voxelTypeDataEndIndex = pixel2.g;
    header.padding[0] = pixel2.b;
    header.padding[1] = pixel2.a;
    header.rotation = vec4(uintBitsToFloat(pixel3.r), uintBitsToFloat(pixel3.g),
                           uintBitsToFloat(pixel3.b), uintBitsToFloat(pixel3.a));

    return header;
}

int headersLength() {
    ivec2 texSize = textureSize(headerData, 0);
    return int(texSize.x / 4);
}

// Builds the rotation matrix R (R * v rotates v by q) from a quaternion [x, y, z, w].
// R is orthonormal, so its inverse is transpose(R). Used to bring rays into a chunk's
// local (axis-aligned) voxel frame and to push hit normals/positions back to world.
mat3 rotationFromQuat(vec4 q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float xx = x * x, yy = y * y, zz = z * z;
    float xy = x * y, xz = x * z, yz = y * z;
    float wx = w * x, wy = w * y, wz = w * z;
    // GLSL mat3(vec3,vec3,vec3) takes the three vec3 as COLUMNS (column-major), so these
    // are columns 0,1,2 of R.
    return mat3(
        vec3(1.0 - 2.0 * (yy + zz), 2.0 * (xy + wz),       2.0 * (xz - wy)),
        vec3(2.0 * (xy - wz),       1.0 - 2.0 * (xx + zz), 2.0 * (yz + wx)),
        vec3(2.0 * (xz + wy),       2.0 * (yz - wx),       1.0 - 2.0 * (xx + yy))
    );
}

//Returns a random float.
float randomFloat0to1(vec2 uv, float timeX) {
    vec3 coords = vec3(uv * timeX, timeX);

    coords = fract(coords * vec3(443.8975, 441.4234, 437.195));
    coords += dot(coords, coords.yzx + 19.19);

    float n = dot(coords, vec3(12.9898, 78.233, 45.164));
    return fract(sin(n) * 43758.5453123);
}

// Given a ZOrder index and the grid resolution,
// return the original (x, y, z) that produced it.
uvec3 inverseZOrderIndex(uint ZOrder, uint gridResolution) {
    uint x = 0u;
    uint y = 0u;
    uint z = 0u;

    int levels = int(log2(float(gridResolution)));

    for (int i = 0; i < levels; i++)
    {
        uint bitX = (ZOrder >> (3 * i))     & 1u;
        uint bitY = (ZOrder >> (3 * i + 1)) & 1u;
        uint bitZ = (ZOrder >> (3 * i + 2)) & 1u;

        x |= (bitX << i);
        y |= (bitY << i);
        z |= (bitZ << i);
    }

    return uvec3(x, y, z);
}

//Given an integer x, y, z, and the resolution of the voxel grid, return the ZOrderIndex of that point.
uint calculateZOrderIndex(uint x, uint y, uint z, uint gridResolution){
    uint ZOrder = 0;
    int levels = int(log2(float(gridResolution)));
    for(int i = 0; i < levels; i++){
        uint bitX = (x >> i) & 1u;
        uint bitY = (y >> i) & 1u;
        uint bitZ = (z >> i) & 1u;

        ZOrder |= (bitX << (3 * i)) | (bitY << (3 * i + 1)) | (bitZ << (3 * i + 2));
    }
    return ZOrder;
}

// Hardcoded version of calculateZOrderIndex for gridResolution = 4 (log2(4) = 2 iterations).
uint calculateZOrderIndex4(uint x, uint y, uint z){
    uint ZOrder = 0;
    // i = 0
    ZOrder |= (x & 1u) | ((y & 1u) << 1) | ((z & 1u) << 2);
    // i = 1
    ZOrder |= (((x >> 1) & 1u) << 3) | (((y >> 1) & 1u) << 4) | (((z >> 1) & 1u) << 5);
    return ZOrder;
}

// Near Plane: 0.1
// Far Plane: 100.0

/**
 * Calculate the starting ray direction for the initial ray trace.
 *
 * @param uv - The normalized device coordinates (NDC) of the pixel, ranging from 0 to 1.
 * @param res - The resolution of the screen, where res.x is the width and res.y is the height.
 * @param cameraPosition - The position of the camera in world space.
 * @param cameraDirection - The direction the camera is facing in world space.
 * @param fov - The field of view of the camera in degrees.
 * @return vec3 - The normalized direction of the ray starting from the camera.
 */
//Calculate the starting ray direction for the inital ray trace
vec3 rayStartDirection(vec2 uv, vec2 res, vec3 cameraPosition, vec3 cameraDirection, float fov){
    // NDC in [-1, 1] with the usual top-left-origin UV flip.
    vec2 ndc = vec2(uv.x, 1.0 - uv.y) * 2.0 - 1.0;
    float aspectRatio = res.x / res.y;
    float scale = tan(radians(fov * 0.5)); // vertical-FOV convention

    // Orthonormal camera basis. right/up are normalized so the effective FOV stays
    // correct for ANY look direction (an un-normalized basis shrinks with the pitch
    // angle and warps the FOV). Handedness and the UV flip match the previous version,
    // so the reprojection inverse (worldToUV) stays exact.
    vec3 forward = normalize(cameraDirection);
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right   = normalize(cross(forward, worldUp));
    vec3 up      = normalize(cross(right, forward));

    return normalize(right * (ndc.x * scale * aspectRatio) + up * (ndc.y * scale) + forward);
}

uint calculateZOrderIntoBox(uint branchingFactor, vec3 intersectionOnBox, BoxAABB box) {
    // Assume box.position is the minimum corner of the box
    float xLocal = (intersectionOnBox.x - box.position.x) / box.size;
    float yLocal = (intersectionOnBox.y - box.position.y) / box.size;
    float zLocal = (intersectionOnBox.z - box.position.z) / box.size;

    // Clamp to avoid max-face and numerical precision issues
    xLocal = clamp(xLocal, 0.0f, 0.999999f);
    yLocal = clamp(yLocal, 0.0f, 0.999999f);
    zLocal = clamp(zLocal, 0.0f, 0.999999f);

    uint xZOrderPos = uint(xLocal * branchingFactor);
    uint yZOrderPos = uint(yLocal * branchingFactor);
    uint zZOrderPos = uint(zLocal * branchingFactor);

    uint zOrder = calculateZOrderIndex(xZOrderPos, yZOrderPos, zZOrderPos, 4);
    return zOrder;
}

uint calculateZOrderIntoBoxFast(vec3 pos, BoxAABB box) {
    uvec3 gridPos = uvec3((pos - box.position) / box.size * 4.0);
    
    // Clamp to valid range (0-3 for 4x4x4 grid)
    gridPos = min(gridPos, uvec3(3));
    
    // Interleave bits: XYZXYZ pattern
    uint zOrder = 0;
    zOrder |= (gridPos.x & 1u) << 0;    // X bit 0
    zOrder |= (gridPos.y & 1u) << 1;    // Y bit 0  
    zOrder |= (gridPos.z & 1u) << 2;    // Z bit 0
    zOrder |= ((gridPos.x >> 1) & 1u) << 3; // X bit 1
    zOrder |= ((gridPos.y >> 1) & 1u) << 4; // Y bit 1
    zOrder |= ((gridPos.z >> 1) & 1u) << 5; // Z bit 1
    
    return zOrder;
}

BoxAABB newBoxFromParent(uint branchingFactor, BoxAABB parentBox, uint zOrderInParent) {
    BoxAABB newBox;
    newBox.size = parentBox.size / branchingFactor;

    uvec3 childPositionInParent = inverseZOrderIndex(zOrderInParent, 4);
    newBox.position = parentBox.position + vec3(childPositionInParent) * newBox.size;

    return newBox;
}

BoxAABB parentBoxFromChildAndZOrder(uint branchingFactor, BoxAABB childBox, uint childZOrderInParent) {
    BoxAABB parentBox;

    parentBox.size = childBox.size * branchingFactor;

    uvec3 childPositionInParent =
        inverseZOrderIndex(childZOrderInParent, branchingFactor);

    parentBox.position =
        childBox.position - vec3(childPositionInParent) * childBox.size;

    return parentBox;
}

/*
void emplaceNodeDataIndexies(uint value) {
    nodeDataIndexies[nodeDataIndexiesSize] = value;
    nodeDataIndexiesSize += 1;
}

void emplaceTraversalBoxes(BoxAABB box) {
    traversalBoxes[traversalBoxesSize] = box;
    traversalBoxesSize += 1;
}

void emplaceTraversalZOrder(uint value) {
    traversalZOrder[traversalZOrderSize] = value;
    traversalZOrderSize += 1;
}
*/

Tree64NodeData tree64(uint indexOfNode) {
    //ivec2 texSize = textureSize(tree64Data, 0);
    //int pixelIndex = index / 4;
    //int x = pixelIndex % texSize.x;
    //int y = pixelIndex / texSize.x;
    //int colorIndex = index % 4;
    //uvec4 pixel = texelFetch(tree64Data, ivec2(x, y), 0);
    ivec2 texSize = textureSize(tree64Data, 0);
    int x = indexOfNode % texSize.x;
    int y = indexOfNode / texSize.x;
    uvec4 pixel = texelFetch(tree64Data, ivec2(x, y), 0);
    Tree64NodeData nodeData;
    nodeData.data1 = pixel.x;
    nodeData.data2 = pixel.y;
    nodeData.data3 = pixel.z;
    return nodeData; 
}

// See if the valid bit for this z order is set.
bool checkZOrderInValidMasks(uint mask1, uint mask2, uint zOrder) {
    uint idx = zOrder & 31u;                 // 0..31
    uint bit = 0x80000000u >> idx;           // bit position (31-idx)

    uint sel = uint(zOrder >> 5);            // 0 or 1
    sel = ~sel + 1u;                         // 0 or 0xFFFFFFFF

    uint mask = (mask1 & ~sel) | (mask2 & sel);

    return (mask & bit) != 0u;
}

uint calculateSiblingsBeforeThisZOrder2(uint branchingFactor, uint mask1, uint mask2, uint zOrder) {
    uint siblings = 0;
    for (int i = 0; i < (branchingFactor * branchingFactor * branchingFactor); i++) {
        if (zOrder == i) return siblings;
        if (checkZOrderInValidMasks(mask1, mask2, i)) { // Increase counter if child is valid.
            siblings += 1;
        }
    }
    return siblings;
}

// Hamming algorithm
uint countBits(uint x) {
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0F0F0F0Fu;
    x = x + (x >> 8);
    x = x + (x >> 16);
    return x & 0x3Fu;
}

uint calculateSiblingsBeforeThisZOrder(uint branchingFactor, uint mask1, uint mask2, uint zOrder) {
    uint siblings = 0u;

    if (zOrder < 32u) {

        // No bits before zOrder 0
        if (zOrder == 0u) {
            return 0u;
        }

        // Keep bits strictly before this zOrder
        uint beforeMask = 0xFFFFFFFFu << (32u - zOrder);
        siblings = countBits(mask1 & beforeMask);

    } else {

        // All bits in mask1 are before
        siblings = countBits(mask1);

        uint z2 = zOrder - 32u;

        if (z2 > 0u) {
            uint beforeMask2 = 0xFFFFFFFFu << (32u - z2);
            siblings += countBits(mask2 & beforeMask2);
        }
    }

    return siblings;
}

// ------------------------------------------------------------
// Z-order movement via bit arithmetic (branchless, replaces MOVE_LUT).
// Extracts 2-bit-per-axis coords, adds/subtracts via sign(direction),
// clamps to [0,3], re-interleaves.  Bit-exact to the original LUT.
uint moveZOrder(uint zOrder, vec3 direction)
{
    int xBits = int((zOrder & 1u) | ((zOrder >> 2) & 2u));
    int yBits = int(((zOrder >> 1) & 1u) | ((zOrder >> 3) & 2u));
    int zBits = int(((zOrder >> 2) & 1u) | ((zOrder >> 4) & 2u));

    xBits = clamp(xBits + int(sign(direction.x)), 0, 3);
    yBits = clamp(yBits + int(sign(direction.y)), 0, 3);
    zBits = clamp(zBits + int(sign(direction.z)), 0, 3);

    uint ux = uint(xBits);
    uint uy = uint(yBits);
    uint uz = uint(zBits);

    return (ux & 1u) | ((uy & 1u) << 1) | ((uz & 1u) << 2) |
           ((ux & 2u) << 2) | ((uy & 2u) << 3) | ((uz & 2u) << 4);
}

// Check if any valid child cell exists ahead of the ray within the current 4x4x4 node.
// Uses per-axis coarse half-volume masks derived from the current z-order position.
// When the current cell is already in the forward half of an axis, only cells in that
// same half can possibly be ahead (a 50%-volume prune). Zero-direction axes contribute
// no pruning (identity mask = 0xFFFFFFFF).
bool anyValidAhead(uint mask1, uint mask2, uint zOrder, ivec3 stepDir) {
    uint fwdLo = 0xFFFFFFFFu;
    uint fwdHi = 0xFFFFFFFFu;

    uint xHi = (zOrder >> 3) & 1u;
    if (stepDir.x > 0 && xHi != 0u)        { fwdLo &= FWD_MASK_LO[1]; fwdHi &= FWD_MASK_HI[1]; }
    else if (stepDir.x < 0 && xHi == 0u)   { fwdLo &= FWD_MASK_LO[0]; fwdHi &= FWD_MASK_HI[0]; }

    uint yHi = (zOrder >> 4) & 1u;
    if (stepDir.y > 0 && yHi != 0u)        { fwdLo &= FWD_MASK_LO[3]; fwdHi &= FWD_MASK_HI[3]; }
    else if (stepDir.y < 0 && yHi == 0u)   { fwdLo &= FWD_MASK_LO[2]; fwdHi &= FWD_MASK_HI[2]; }

    uint zHi = (zOrder >> 5) & 1u;
    if (stepDir.z > 0 && zHi != 0u)        { fwdLo &= FWD_MASK_LO[5]; fwdHi &= FWD_MASK_HI[5]; }
    else if (stepDir.z < 0 && zHi == 0u)   { fwdLo &= FWD_MASK_LO[4]; fwdHi &= FWD_MASK_HI[4]; }

    return ((fwdLo & mask1) | (fwdHi & mask2)) != 0u;
}

// Implementations from Nvidia.
int findMSB(int x) {
    int i;
    int mask;
    int res = -1;
    if (x < 0) {
        x = ~x;
    }
    for(i = 0; i < 32; i++) {
        mask = 0x80000000 >> i;
        if (x & mask) {
            res = 31 - i;
            break;
        }
    }
    return res;
}

int findLSB(int x)
{
  int i;
  int mask;
  int res = -1;
  for(i = 0; i < 32; i++) {
    mask = 1 << i;
    if (x & mask) {
      res = i;
      break;
    }
  }
  return res;
}

// Will not work for high res position! Coordinate scale must ALWAYS be 1 node, not 1 voxel...
uint getBoundariesCrossed(ivec3 previousNodeCoordinate, ivec3 proposedNodeCoordinate, uint meaningles) {
      ivec3 diff = previousNodeCoordinate - proposedNodeCoordinate;
      int boundaryPos = 0;
      if      (diff.x != 0) boundaryPos = max(previousNodeCoordinate.x, proposedNodeCoordinate.x);
      else if (diff.y != 0) boundaryPos = max(previousNodeCoordinate.y, proposedNodeCoordinate.y);
      else if (diff.z != 0) boundaryPos = max(previousNodeCoordinate.z, proposedNodeCoordinate.z);
      else return 0u;

      if (boundaryPos == 0) return 0u;
      if ((uint(boundaryPos) & 3u) != 0u) return 0u;

      uint v = uint(boundaryPos);
      uint bits = 0u;
      if ((v & 0xFFFFu) == 0u) { bits += 16u; v >>= 16; }
      if ((v & 0x00FFu) == 0u) { bits += 8u;  v >>= 8;  }
      if ((v & 0x000Fu) == 0u) { bits += 4u;  v >>= 4;  }
      if ((v & 0x0003u) == 0u) { bits += 2u;  v >>= 2;  }
      if ((v & 0x0001u) == 0u) { bits += 1u;             }
      return bits / 2u;
}

// Level = 0: highest resolution of this tree, Level = 1, resolution/4...
uint getZOrderInParentFromThisNodesLevel(ivec3 highResPosition, uint level) {
    ivec3 positionAtLevelRes = highResPosition >> (2 * level);
    ivec3 positionInParent = positionAtLevelRes % 4;
    return calculateZOrderIndex4(positionInParent.x, positionInParent.y, positionInParent.z);
}

// Resolution is the resolution that we want it to find the intersect at. 
// BoxAABB boundingBox is assumed to be positionted correctly in traversal space. (bottom left corner is where it actually lies in voxel coordinates, size is how many voxels it takes up)
ivec3 determineTraversalCoordinatesFromRayAndBox(Ray ray, BoxAABB boundingBox) {
    float t = getRayBoxEntryDistanceForSureHit(ray, boundingBox);

    // Robust, scale-consistent entry: take the hit point and clamp it just inside the
    // box before flooring. Voxels are 1 unit, so the small inset off the max faces is
    // well-scaled and reliably lands in the last cell rather than one past it. This
    // avoids the old fixed-eps ray nudge (wrong at grazing angles) and the
    // equal(p, floor(p)) test (essentially never true in float).
    vec3 p = ray.origin + ray.direction * t;
    vec3 boxMinF = boundingBox.position;
    vec3 boxMaxF = boundingBox.position + vec3(boundingBox.size);
    p = clamp(p, boxMinF, boxMaxF - vec3(1e-4));

    // On an exact grid plane the cell is ambiguous: floor always picks the upper cell,
    // which is only correct when the ray travels up that axis. Pick the cell the ray is
    // actually entering, otherwise a solid upper cell the ray merely touches is
    // reported as a hit (boundary fireflies).
    vec3 fp = floor(p);
    fp -= vec3(equal(p, fp)) * vec3(lessThan(ray.direction, vec3(0.0)));

    ivec3 voxelCoordinate = clamp(ivec3(fp),
                                  ivec3(boxMinF), ivec3(boxMaxF) - ivec3(1));
    return voxelCoordinate;
}

// Resolution is the resolution that we want it to find the intersect at. 
// BoxAABB boundingBox is assumed to be positionted correctly in traversal space. (bottom left corner is where it actually lies in voxel coordinates, size is how many voxels it takes up)
ivec3 determineTraversalCoordinatesFromRayAndBoxAndRayDistance(Ray ray, BoxAABB boundingBox, float rayT) {
    // Same robust entry as determineTraversalCoordinatesFromRayAndBox, but with the ray
    // distance supplied by the caller: clamp the hit point just inside the box, then floor.
    vec3 p = ray.origin + ray.direction * rayT;
    vec3 boxMinF = boundingBox.position;
    vec3 boxMaxF = boundingBox.position + vec3(boundingBox.size);
    p = clamp(p, boxMinF, boxMaxF - vec3(1e-4));

    // Direction-aware floor on exact grid planes (see determineTraversalCoordinatesFromRayAndBox).
    // rayT here is usually a cell-boundary t, so p landing exactly on an interior plane
    // is the common case, not the rare one.
    vec3 fp = floor(p);
    fp -= vec3(equal(p, fp)) * vec3(lessThan(ray.direction, vec3(0.0)));

    ivec3 voxelCoordinate = clamp(ivec3(fp),
                                  ivec3(boxMinF), ivec3(boxMaxF) - ivec3(1));
    return voxelCoordinate;
}


// Rounds our voxel coordinates down.
// Takes in high res voxel coordinates, node size.
// Then rounds voxel coordinates down to the nearest multiple of node size.
ivec3 roundVoxelCoordinatesBasedOnNodeSize(ivec3 highResPosition, uint targetNodeSize) {
    ivec3 nodeSpaceCoordinate = highResPosition >> uint(log2(targetNodeSize));
    return nodeSpaceCoordinate << uint(log2(targetNodeSize)); // Could also be bit shift, targetNodeSize is always power of 4.
}

// LOD selection ---------------------------------------------------------------
// Chooses the coarsest tree level the traversal is allowed to descend to at a
// given distance along the ray. The returned value is in the same units as
// candidateNodeLevel: 0 = finest (1-voxel) nodes, 1 = 4-voxel nodes, 2 = 16-voxel
// nodes, and so on.
//
// Near the ray origin the cap is startLOD. As the ray travels the cap ramps
// linearly, reaching finishLOD once it has covered distanceToFinishLOD voxels,
// and stays at finishLOD for the rest of the ray. This lets distant geometry
// collapse into coarser, cheaper nodes while keeping full detail up close.
//
// The all-zero default (startLOD = finishLOD = distanceToFinishLOD = 0) yields a
// constant cap of 0, i.e. full-resolution traversal — behaviour identical to
// having no LOD at all.
uint computeTargetLOD(float distanceInVoxels, RayQuery rayQuery) {
    // A zero ramp distance means there is nothing to interpolate across: the cap
    // is finishLOD everywhere (an instantaneous jump at the origin).
    if (rayQuery.distanceToFinishLOD == 0u) {
        return rayQuery.finishLOD;
    }

    if (distanceInVoxels >= float(rayQuery.distanceToFinishLOD)) {
        return rayQuery.finishLOD;
    }

    float t = distanceInVoxels / float(rayQuery.distanceToFinishLOD);
    // mix() handles both the usual case (startLOD < finishLOD, detail falling off
    // with distance) and a reversed ramp. uint() floors toward the finer level so
    // we never cap coarser than requested.
    return uint(mix(float(rayQuery.startLOD), float(rayQuery.finishLOD), t));
}

SceneIntersectData marchRayThroughTree64_DDA(Ray ray, RayQuery rayQuery, BoxAABB boundingBox, uint tree64StartIndex, uint tree64EndIndex, uint tree64Resolution) {
    SceneIntersectData returnData;
    returnData.rayT = -1.0;
    returnData.normal = vec3(0.0);
    vec3 invRayDir = 1.0/ray.direction;
    // Normal of the face the ray most recently crossed. Starts as the volume entry face
    // (zero if the ray origin is inside the volume) and is updated on every committed
    // DDA step, so any hit — found while descending or while stepping — reports the
    // face the ray actually came through, with no analytic re-derivation.
    vec3 hitNormal = getRayBoxEntry(ray, boundingBox).normal;
    // If res = 4, treeLevels = 1. In this case just the root node exists.
    // If res = 16, treeLevels = 2. In this case just the root node exists.
    uint treeLevels = log2(tree64Resolution)/2.0; // Levels in the tree. 1 for 4x4x4 volume, 2 for 16x16x16 etc...
    // If res = 4, candidateNodeLevel = 0. In this case, candidate node IS highest resolution node (hence the - 1 to make it 0).
    uint candidateNodeLevel = treeLevels - 1; // Starting one level down into the tree.
    uint shift = 2 * candidateNodeLevel;
    uint stepSize = 1u << (shift);

    ivec3 traversalPosition = ivec3(0); // Always at 1 voxel resolution, even if level/step size isn't.
    traversalPosition = determineTraversalCoordinatesFromRayAndBox(ray, boundingBox);
    // Above is same logic used in push!

    nodeStackZOrder[0] = 0;
    nodeStackDataIndex[0] = tree64StartIndex;
    nodeStackZOrder[1] = getZOrderInParentFromThisNodesLevel(traversalPosition, candidateNodeLevel);
    nodeStackDataIndex[1] = 0;
    nodeStackQuantity = 2;

    //traversalPosition = roundVoxelCoordinatesBasedOnNodeSize(traversalPosition, tree64Resolution/4);
    traversalPosition = (traversalPosition >> shift) << shift;
    ivec3 stepI = ivec3(sign(ray.direction));
    float rayT = getRayBoxEntryDistanceForSureHit(ray, boundingBox);

    uint stepCount = 0;
    bool previouslyPopped = false;
    dataCache[0] = tree64(tree64StartIndex);
    Tree64NodeData data = dataCache[0];
    for (; stepCount < rayQuery.maxRaySteps; stepCount++) {
        //stepCount += 1;
        while (candidateNodeLevel >= 0) {
            //stepCount -= 1;
            if (previouslyPopped) break;
            if (checkZOrderInValidMasks(data.data1, data.data2, nodeStackZOrder[nodeStackQuantity - 1u])) {
                if((data.data3 & 0b1) == 1u) {
                    returnData.foundBox.position = traversalPosition;
                    returnData.foundBox.size = stepSize;
                    returnData.steps = stepCount;
                    returnData.rayT = rayT;
                    returnData.normal = hitNormal;
                    return returnData;
                    // Leaf found! Handle accordingly.
                }
                // LOD cutoff. This candidate is occupied (valid in its parent's
                // mask) but is an interior node we would normally descend into.
                // If it already sits at (or below) the coarsest level allowed for
                // its distance along the ray, stop here and treat the whole node
                // as solid instead of resolving finer geometry inside it. rayT is
                // the distance to this candidate in voxel units, and this is the
                // only place the traversal descends, so capping here is sufficient
                // to guarantee we never resolve finer than the requested LOD.
                if (candidateNodeLevel <= computeTargetLOD(rayT, rayQuery)) {
                    returnData.foundBox.position = traversalPosition;
                    returnData.foundBox.size = stepSize;
                    returnData.steps = stepCount;
                    returnData.rayT = rayT;
                    returnData.normal = hitNormal;
                    return returnData;
                }
                BoxAABB candidateBox;
                candidateBox.position = vec3(traversalPosition);
                candidateBox.size = float(stepSize);
                ivec3 highResPosition = determineTraversalCoordinatesFromRayAndBoxAndRayDistance(ray, candidateBox, rayT);
                uint bottomChildPointer = (data.data3 >> 1) & 0b01111111111111111111111111111111;
                uint parentDataIndex = nodeStackDataIndex[nodeStackQuantity - 2u];
                uint childrenBeforeThisNode = calculateSiblingsBeforeThisZOrder(4, data.data1, data.data2, nodeStackZOrder[nodeStackQuantity - 1u]);
                nodeStackDataIndex[nodeStackQuantity - 1u] = bottomChildPointer + parentDataIndex + childrenBeforeThisNode;
                nodeStackZOrder[nodeStackQuantity] = getZOrderInParentFromThisNodesLevel(highResPosition, candidateNodeLevel - 1); // off by 1 error?
                nodeStackDataIndex[nodeStackQuantity] = 0;
                dataCache[nodeStackQuantity - 1u] = tree64(nodeStackDataIndex[nodeStackQuantity - 1u]);
                nodeStackQuantity += 1;
                candidateNodeLevel -= 1;
                // equates to : pow(4, candidateNodeLevel);
                shift = 2u * candidateNodeLevel;
                stepSize = 1u << (shift);
                //traversalPosition = roundVoxelCoordinatesBasedOnNodeSize(highResPosition, stepSize);
                traversalPosition = (highResPosition >> shift) << shift;


                data = dataCache[nodeStackQuantity - 2u];
            } else {
                break;
            }
        }

        vec3 nextPlanes = mix(vec3(traversalPosition), vec3(traversalPosition) + vec3(stepSize), step(vec3(0.0), ray.direction));

        vec3 tMax = (nextPlanes - ray.origin) * invRayDir;
        vec3 tDelta = stepSize * abs(invRayDir);
        previouslyPopped = false;
        //Tree64NodeData parentData = tree64(nodeStackDataIndex[nodeStackQuantity - 2u]);
        for (int i = 0; i < 12; i++) {
            // Actually do DDA

            // Step exactly ONE axis per iteration: the axis whose next cell boundary is
            // nearest. Ties (the ray crossing an edge or corner where two/three tMax
            // components are equal) are broken deterministically x > y > z, so the ray
            // advances one axis at a time and enters the face-adjacent voxel first,
            // reaching the corner voxel on the following iteration. Stepping multiple
            // axes at once (as step(tMax, min) would on a tie) moves diagonally and
            // skips the voxel that only touches that edge/corner, which is what caused
            // the speckle/clip-through artifacts. Single-axis stepping also keeps the
            // node-coordinate diff single-axis so getBoundariesCrossed stays well-defined.
            float sx = float(tMax.x <= tMax.y && tMax.x <= tMax.z);
            float sy = float(tMax.y <= tMax.x && tMax.y <= tMax.z) * (1.0 - sx);
            float sz = 1.0 - sx - sy;
            vec3 axisMask = vec3(sx, sy, sz);   // exactly one component is 1.0

            rayT = dot(tMax, axisMask);         // t of the stepped axis (== the minimum)

            tMax += axisMask * tDelta;

            ivec3 directionSteppedIn = ivec3(axisMask) * stepI;

            ivec3 proposedTraversalPosition = traversalPosition + directionSteppedIn * stepSize;
            if (proposedTraversalPosition.x < 0 || proposedTraversalPosition.y < 0 || proposedTraversalPosition.z < 0 ||
              proposedTraversalPosition.x >= tree64Resolution || proposedTraversalPosition.y >= tree64Resolution || proposedTraversalPosition.z >= tree64Resolution) {
                returnData.foundBox.position = vec3(0);
                returnData.foundBox.size = -1;
                returnData.steps = stepCount;
                return returnData;
            }
            // Get our coordinates in node space. Shift right is the same as traversalPosition / 4^stepSize
            ivec3 previousNodeCoordinate = traversalPosition >> (shift); // Used to be uint(log2(stepSize));
            ivec3 proposedNodeCoordinate = proposedTraversalPosition >> (shift);
            uint boundariesCrossed = getBoundariesCrossed(previousNodeCoordinate, proposedNodeCoordinate, candidateNodeLevel);

            if (boundariesCrossed != 0) { // If we cross boundaires, pop.
                nodeStackQuantity -= boundariesCrossed;
                candidateNodeLevel += boundariesCrossed;
                shift = 2 * candidateNodeLevel;
                stepSize = 1u << (shift);
                //traversalPosition = roundVoxelCoordinatesBasedOnNodeSize(traversalPosition, stepSize);
                traversalPosition = (traversalPosition >> shift) << shift;
                previouslyPopped = true;
                data = dataCache[nodeStackQuantity - 2u];
                break;
            }

            nodeStackZOrder[nodeStackQuantity - 1u] = moveZOrder(nodeStackZOrder[nodeStackQuantity - 1u], directionSteppedIn);
            traversalPosition = proposedTraversalPosition;
            hitNormal = vec3(-directionSteppedIn);

            // Zero-measure guard: on an exact edge/corner crossing two tMax components
            // tie, so the cell we just stepped into is exited at the SAME t on the other
            // tied axis — the ray only touches it at a point. Treating it as occupied is
            // what produced the boundary fireflies (a solid cell the ray never actually
            // travels through gets returned as a hit, and descending into it hands the
            // push a corner-exact rayT). Skip its validity check entirely; the next
            // iteration steps the tied axis at the same rayT and lands in the diagonal
            // cell, which is then tested with real measure.
            float cellExitT = min(min(tMax.x, tMax.y), tMax.z);

            // Only check for if its valid after we advance.
            // This is because if we are advancing it could mean that we just popped out of a valid candidate,
            // checking if its valid before we advance would just put us right back into the one we popped out of.
            // Used to be 17 -
            if (cellExitT > rayT) {
                if (checkZOrderInValidMasks(data.data1, data.data2, nodeStackZOrder[nodeStackQuantity - 1u])) { // New valid z order found!!
                    if ((data.data3 & 0b1) == 1) {
                        returnData.foundBox.position = traversalPosition;
                        returnData.foundBox.size = stepSize;
                        returnData.steps = stepCount;
                        returnData.rayT = rayT;
                        returnData.normal = hitNormal;
                        return returnData;
                        // Leaf found, handle accordingly.
                    }
                    break;
                }
                // Cell is not occupied — check if any valid cell exists ahead of the ray
                // in this node. If not, skip to the node's exit in one step.
                if (!anyValidAhead(data.data1, data.data2,
                                   nodeStackZOrder[nodeStackQuantity - 1u],
                                   stepI)) {
                    ivec3 nodeOrigin = (traversalPosition >> (shift + 2)) << (shift + 2);
                    BoxAABB nodeBox;
                    nodeBox.position = vec3(nodeOrigin);
                    nodeBox.size = float(stepSize * 4);
                    IntersectionResult exitResult = getRayBoxExitForSureHit(ray, nodeBox);
                    rayT = exitResult.distance;

                    ivec3 exitStep = ivec3(sign(exitResult.normal));
                    ivec3 proposedTraversalPosition = traversalPosition + exitStep * int(stepSize * 4);

                    if (proposedTraversalPosition.x < 0 || proposedTraversalPosition.y < 0 || proposedTraversalPosition.z < 0 ||
                        proposedTraversalPosition.x >= tree64Resolution || proposedTraversalPosition.y >= tree64Resolution || proposedTraversalPosition.z >= tree64Resolution) {
                        returnData.foundBox.position = vec3(0);
                        returnData.foundBox.size = -1;
                        returnData.steps = stepCount;
                        return returnData;
                    }

                    ivec3 previousNodeCoordinate = traversalPosition >> (shift);
                    ivec3 proposedNodeCoordinate = proposedTraversalPosition >> (shift);
                    uint boundariesCrossed = getBoundariesCrossed(previousNodeCoordinate, proposedNodeCoordinate, candidateNodeLevel);

                    if (boundariesCrossed < 1u) boundariesCrossed = 1u;

                    if (boundariesCrossed >= nodeStackQuantity) {
                        returnData.foundBox.position = vec3(0);
                        returnData.foundBox.size = -1;
                        returnData.steps = stepCount;
                        return returnData;
                    }

                    nodeStackQuantity -= boundariesCrossed;
                    candidateNodeLevel += boundariesCrossed;
                    shift = 2u * candidateNodeLevel;
                    stepSize = 1u << (shift);
                    traversalPosition = (traversalPosition >> shift) << shift;
                    previouslyPopped = true;
                    data = dataCache[nodeStackQuantity - 2u];

                    nodeStackZOrder[nodeStackQuantity - 1u] =
                        getZOrderInParentFromThisNodesLevel(traversalPosition, candidateNodeLevel);

                    while (previouslyPopped &&
                           nodeStackQuantity > 2u &&
                           !checkZOrderInValidMasks(data.data1, data.data2, nodeStackZOrder[nodeStackQuantity - 1u]) &&
                           !anyValidAhead(data.data1, data.data2, nodeStackZOrder[nodeStackQuantity - 1u], stepI)) {
                        nodeStackQuantity -= 1u;
                        candidateNodeLevel += 1u;
                        shift = 2u * candidateNodeLevel;
                        stepSize = 1u << (shift);
                        traversalPosition = (traversalPosition >> shift) << shift;
                        nodeStackZOrder[nodeStackQuantity - 1u] =
                            getZOrderInParentFromThisNodesLevel(traversalPosition, candidateNodeLevel);
                        data = dataCache[nodeStackQuantity - 2u];
                    }

                    break;
                }
            }
        }
    }
    returnData.foundBox.position = vec3(0);
    returnData.foundBox.size = -1;
    returnData.steps = stepCount;
    return returnData;
}

//Cast the ray through the tree64 bounding box.
SceneIntersectData castRayThroughTree64(Ray ray, RayQuery rayQuery, uint headerIndex) {
    chunkHeader header = headers(headerIndex);

    uint tree64StartIndex = header.geometryStartIndex;
    uint tree64EndIndex = header.geometryEndIndex;

    uint voxelTypeDataStartIndex = header.voxelTypeDataStartIndex;
    uint voxelTypeDataEndIndex = header.voxelTypeDataEndIndex;

    BoxAABB tree64BoundingBox;
    //tree64BoundingBox.position = vec3(header.positionX, header.positionY, header.positionZ);
    //tree64BoundingBox.size = header.scale;
    tree64BoundingBox.position = vec3(0.0, 0.0, 0.0);
    tree64BoundingBox.size = header.resolution; // Allows each voxel to be of scale 1 for ray marching.

    // Bring the ray into the chunk's local (axis-aligned) voxel frame. The chunk is placed
    // in the world by translation P, rotation R and uniform scale (scale/resolution), so the
    // inverse is: subtract P, rotate by R^-1 = transpose(R), then scale to voxel units.
    // R^-1 is orthonormal so ray length is preserved and the rayT scaling below is unchanged.
    mat3 R = rotationFromQuat(header.rotation);
    mat3 Rinv = transpose(R);
    vec3 P = vec3(header.positionX, header.positionY, header.positionZ);
    Ray transformedRay;
    transformedRay.origin = (Rinv * (ray.origin - P)) * (tree64BoundingBox.size / header.scale);
    transformedRay.direction = Rinv * ray.direction;

    float epsilon = 1e-6;
    if (abs(transformedRay.direction.x) < epsilon) transformedRay.direction.x = epsilon;
    if (abs(transformedRay.direction.y) < epsilon) transformedRay.direction.y = epsilon;
    if (abs(transformedRay.direction.z) < epsilon) transformedRay.direction.z = epsilon;

    IntersectionResult rootIntersect = getRayBoxEntry(transformedRay, tree64BoundingBox);

    SceneIntersectData tree64Intersect;
    tree64Intersect.foundBox.size = -1;
    tree64Intersect.rayT = -1.0;
    tree64Intersect.normal = vec3(0.0);
    if(rootIntersect.distance >= 0){
        tree64Intersect = marchRayThroughTree64_DDA(transformedRay, rayQuery, tree64BoundingBox, tree64StartIndex, tree64EndIndex, header.resolution);
    }
    // size should be 1 for full res, 2 for half res, 4 for quarter res etc. So we multiply the size by what the voxel scale actually is.
    tree64Intersect.foundBox.size *= header.scale/tree64BoundingBox.size;
    vec3 normalizedBoxPosition = tree64Intersect.foundBox.position/tree64BoundingBox.size;
    // Back to world: apply uniform scale, rotate by R, then translate by P (inverse of the
    // world->local transform applied to the ray above).
    tree64Intersect.foundBox.position = R * (normalizedBoxPosition * header.scale) + P;
    // The DDA's hit normal is in the chunk's local voxel frame; rotate it back to world.
    tree64Intersect.normal = R * tree64Intersect.normal;
    // The march ran in voxel units (origin scaled by resolution/scale, direction kept
    // length-preserving), so t values convert back to world with the inverse uniform scale.
    // A miss's -1 stays negative. Rotation preserves distance, so it does not affect rayT.
    tree64Intersect.rayT *= header.scale/tree64BoundingBox.size;
    return tree64Intersect;
}
//Given the integer postions inside of the voxel grid, find the type data for that voxel.
int findVoxelTypeDataIndexExact(int x, int y, int z, uint voxelGridResolution, uint voxelTypeDataStartIndex, uint voxelTypeDataEndIndex) {
    uint ZOrder = calculateZOrderIndex(x, y, z, voxelGridResolution);
    int beginningIndex = 0;
    int endIndex = int((voxelTypeDataEndIndex-voxelTypeDataStartIndex)/VOXEL_TYPEDATA_SLICES);
    int middleIndex = (beginningIndex+endIndex)/2;
    for(int i = 0; i < 100; i++){
        if(voxelTypeDatas(middleIndex*VOXEL_TYPEDATA_SLICES + voxelTypeDataStartIndex) == ZOrder){
            return middleIndex*VOXEL_TYPEDATA_SLICES;
        } else {
            if(voxelTypeDatas(middleIndex * VOXEL_TYPEDATA_SLICES + voxelTypeDataStartIndex) < ZOrder){
                beginningIndex = middleIndex + 1; 
            } else {
                endIndex = middleIndex - 1;  
            }
        }
        middleIndex = (beginningIndex+endIndex)/2;
    }
    return -1;       
}

int findVoxelTypeDataIndex(int x, int y, int z,
                           uint voxelGridResolution,
                           uint voxelTypeDataStartIndex,
                           uint voxelTypeDataEndIndex)
{
    uint ZOrder = calculateZOrderIndex(x, y, z, voxelGridResolution);

    int count = int((voxelTypeDataEndIndex - voxelTypeDataStartIndex) / VOXEL_TYPEDATA_SLICES);

    int beginningIndex = 0;
    int endIndex = count - 1;
    int middleIndex = 0;

    // ---------- Exact binary search ----------
    for (int i = 0; i < 100 && beginningIndex <= endIndex; i++) {
        middleIndex = (beginningIndex + endIndex) / 2;

        uint value =
            voxelTypeDatas(middleIndex * VOXEL_TYPEDATA_SLICES + voxelTypeDataStartIndex);

        if (value == ZOrder) {
            return middleIndex * VOXEL_TYPEDATA_SLICES;
        } else if (value < ZOrder) {
            beginningIndex = middleIndex + 1;
        } else {
            endIndex = middleIndex - 1;
        }
    }

    // ---------- Fast approximate fallback ----------
    // beginningIndex is now the insertion point
    const int SEARCH_RADIUS = 8; // tweak: 4–16 is usually enough

    int start = max(0, beginningIndex - SEARCH_RADIUS);
    int end   = min(count - 1, beginningIndex + SEARCH_RADIUS);

    for (int i = start; i <= end; i++) {
        return i * VOXEL_TYPEDATA_SLICES;
    }

    return -1;
}



//Return the coordinate in the voxel grid of this voxel
ivec3 voxelGridPosition(BoxAABB voxelBoundingBox, BoxAABB voxelGridBoundingBox, uint voxelGridResolution){
    //Calculate zeroed position within the voxel grid
    vec3 zeroedPosition = voxelBoundingBox.position - voxelGridBoundingBox.position;
    
    //Normalize to unit coordinates 0-1
    vec3 unitPosition = zeroedPosition / voxelGridBoundingBox.size;
    
    //Ensure unitPosition is within 0-1
    unitPosition = clamp(unitPosition, 0.0, 1.0 - 1e-6);
    
    //Map to grid positions based off of resolution and unit pos.
    vec3 gridPos = unitPosition * float(voxelGridResolution);
    
    ivec3 voxelGridPos;
    voxelGridPos.x = int(gridPos.x);
    voxelGridPos.y = int(gridPos.y);
    voxelGridPos.z = int(gridPos.z);
    
    // Clamp indices to valid range 0 to voxelGridResolution - 1
    voxelGridPos.x = clamp(voxelGridPos.x, 0, int(voxelGridResolution) - 1);
    voxelGridPos.y = clamp(voxelGridPos.y, 0, int(voxelGridResolution) - 1);
    voxelGridPos.z = clamp(voxelGridPos.z, 0, int(voxelGridResolution) - 1);
    
    return voxelGridPos;
}

uint findVoxelIndex(BoxAABB voxelBoundingBox, uint headerIndex){
    chunkHeader h = headers(headerIndex);
    uint voxelGridResolution = h.resolution;
    vec3 P = vec3(h.positionX, h.positionY, h.positionZ);
    mat3 Rinv = transpose(rotationFromQuat(h.rotation));

    // The hit box comes back in world space (rotated). Un-rotate it about P into the chunk's
    // axis-aligned local frame so the grid-coordinate mapping below stays valid under rotation.
    BoxAABB localBox;
    localBox.position = Rinv * (voxelBoundingBox.position - P) + P;
    localBox.size = voxelBoundingBox.size;

    BoxAABB voxelGridBoundingBox;
    voxelGridBoundingBox.position = P;
    voxelGridBoundingBox.size = h.scale;
    ivec3 voxelGridPos = voxelGridPosition(localBox, voxelGridBoundingBox, voxelGridResolution);
    uint voxelTypeDataIndex = findVoxelTypeDataIndex(voxelGridPos.x, voxelGridPos.y, voxelGridPos.z, voxelGridResolution, h.voxelTypeDataStartIndex, h.voxelTypeDataEndIndex);
    return voxelTypeDataIndex;
}

//Given the voxel index, return the voxel data.
Voxel fetchVoxelData(BoxAABB voxelBoundingBox, uint headerIndex){
    uint voxelIndex = findVoxelIndex(voxelBoundingBox, headerIndex);
    //Assume 6-bit color channels ()
    //Assume 4-bit normal vector channels ()
    uint voxelTypeDataStartIndex = headers(headerIndex).voxelTypeDataStartIndex;
    Voxel voxel;
    uint SerializedColor = voxelTypeDatas(voxelIndex+1+voxelTypeDataStartIndex);
    uint SerializedNormals = voxelTypeDatas(voxelIndex+2+voxelTypeDataStartIndex);
    uint R10 = (SerializedColor >> 20) & 0x3FF;
    uint G10 = (SerializedColor >> 10) & 0x3FF;
    uint B10 = (SerializedColor >> 00) & 0x3FF;
    vec3 color = vec3(float(R10)/1023, float(G10)/1023, float(B10)/1023);
    uint normalX9 = (SerializedNormals >> 20) & 0x1FF;
    uint normalY9 = (SerializedNormals >> 10) & 0x1FF;
    uint normalZ9 = (SerializedNormals >> 00) & 0x1FF;
    uint normalXSign = (SerializedNormals >> 29) & 0x1;
    uint normalYSign = (SerializedNormals >> 19) & 0x1;
    uint normalZSign = (SerializedNormals >> 9) & 0x1;
    vec3 normal = normalize(vec3((float(normalX9)/511) * (float(normalXSign)*2-1), (float(normalY9)/511) * (float(normalYSign)*2-1), (float(normalZ9)/511) * (float(normalZSign)*2-1)));
    voxel.index = voxelIndex;
    voxel.color = color;
    voxel.normal = normal;

    return voxel;
}

// ------------------------------------------------------------
// Top-level uniform-grid acceleration (see gpu_interface.cpp / compose_io.cpp).
// gridInfo texel0 = {gridCount, looseChunkCount, headerCount, 0}; each grid then occupies
// 3 texels: {origin.xyz, cellSize}, {dims.xyz, cellMapOffset}, {rotation quat xyzw}.
// ------------------------------------------------------------
struct Grid {
    vec3 origin;
    float cellSize;
    ivec3 dims;
    uint cellMapOffset;
    vec4 rotation;
};

uint sceneGridCount()  { return texelFetch(gridInfo, ivec2(0, 0), 0).r; }
uint sceneLooseCount() { return texelFetch(gridInfo, ivec2(0, 0), 0).g; }

Grid getGrid(int i) {
    int base = 1 + i * 3;
    uvec4 p0 = texelFetch(gridInfo, ivec2(base + 0, 0), 0);
    uvec4 p1 = texelFetch(gridInfo, ivec2(base + 1, 0), 0);
    uvec4 p2 = texelFetch(gridInfo, ivec2(base + 2, 0), 0);
    Grid g;
    g.origin = vec3(uintBitsToFloat(p0.r), uintBitsToFloat(p0.g), uintBitsToFloat(p0.b));
    g.cellSize = uintBitsToFloat(p0.a);
    g.dims = ivec3(int(p1.r), int(p1.g), int(p1.b));
    g.cellMapOffset = p1.a;
    g.rotation = vec4(uintBitsToFloat(p2.r), uintBitsToFloat(p2.g), uintBitsToFloat(p2.b), uintBitsToFloat(p2.a));
    return g;
}

// cellMap is flattened exactly like voxelTypeData (4 uints per RGBA32U texel).
uint cellMapValue(uint index) {
    ivec2 texSize = textureSize(cellMap, 0);
    int pixelIndex = int(index) / 4;
    int x = pixelIndex % texSize.x;
    int y = pixelIndex / texSize.x;
    int colorIndex = int(index) % 4;
    uvec4 pixel = texelFetch(cellMap, ivec2(x, y), 0);
    return pixel[colorIndex];
}

// looseList holds the header rows (chunk handles) of the loose chunks, flattened like cellMap.
// The shader iterates [0, looseCount) of these instead of a positional header prefix, so a loose
// chunk can be added/removed without any ordering constraint on the header texture.
uint looseListValue(uint index) {
    ivec2 texSize = textureSize(looseList, 0);
    int pixelIndex = int(index) / 4;
    int x = pixelIndex % texSize.x;
    int y = pixelIndex / texSize.x;
    int colorIndex = int(index) % 4;
    uvec4 pixel = texelFetch(looseList, ivec2(x, y), 0);
    return pixel[colorIndex];
}

// Walks one grid volume with a uniform-grid DDA, front-to-back, and returns the nearest hit
// closer than maxDistance (or a miss). The DDA runs in grid-local space (cells axis-aligned),
// but each occupied cell is marched with the original WORLD ray via castRayThroughTree64 —
// the chunk header already carries world placement + rotation. R^-1 is orthonormal, so the
// grid-local ray parameter t matches world rayT and is directly comparable to maxDistance.
SceneIntersectData marchGrid(Ray ray, RayQuery rayQuery, int gridIndex, float maxDistance) {
    SceneIntersectData miss;
    miss.foundBox.size = -1;
    miss.rayT = -1.0;
    miss.normal = vec3(0.0);
    miss.steps = 0;

    Grid g = getGrid(gridIndex);

    mat3 Rinv = transpose(rotationFromQuat(g.rotation));
    vec3 lo = Rinv * (ray.origin - g.origin);
    vec3 ld = Rinv * ray.direction;
    float eps = 1e-6;
    if (abs(ld.x) < eps) ld.x = eps;
    if (abs(ld.y) < eps) ld.y = eps;
    if (abs(ld.z) < eps) ld.z = eps;
    vec3 invD = 1.0 / ld;

    // Slab test against the (possibly non-cubic) grid box [0, dims*cellSize].
    vec3 gridMax = vec3(g.dims) * g.cellSize;
    vec3 t0 = (vec3(0.0) - lo) * invD;
    vec3 t1 = (gridMax - lo) * invD;
    vec3 tsmall = min(t0, t1);
    vec3 tbig = max(t0, t1);
    float tEnter = max(max(tsmall.x, tsmall.y), tsmall.z);
    float tExit = min(min(tbig.x, tbig.y), tbig.z);
    if (tEnter > tExit || tExit < 0.0) return miss;
    float tStart = max(tEnter, 0.0);

    // Entry cell + DDA setup.
    vec3 p = lo + ld * tStart;
    vec3 cellF = clamp(floor(p / g.cellSize), vec3(0.0), vec3(g.dims) - 1.0);
    ivec3 cell = ivec3(cellF);
    ivec3 stepDir = ivec3(sign(ld));
    vec3 tDelta = abs(g.cellSize * invD);
    vec3 nextBoundary = (vec3(cell) + step(vec3(0.0), ld)) * g.cellSize;
    vec3 tMax = (nextBoundary - lo) * invD;

    float tCurrent = tStart;
    int maxSteps = g.dims.x + g.dims.y + g.dims.z + 3;
    for (int i = 0; i < maxSteps; i++) {
        // No cell entered beyond the closest hit so far can improve on it.
        if (tCurrent >= maxDistance) break;

        if (cell.x >= 0 && cell.y >= 0 && cell.z >= 0 &&
            cell.x < g.dims.x && cell.y < g.dims.y && cell.z < g.dims.z) {
            uint lin = uint(cell.x + g.dims.x * (cell.y + g.dims.y * cell.z));
            uint chunkIdx = cellMapValue(g.cellMapOffset + lin);
            if (chunkIdx != 0xFFFFFFFFu) {
                SceneIntersectData hit = castRayThroughTree64(ray, rayQuery, int(chunkIdx));
                if (hit.foundBox.size > 0 && hit.rayT >= 0.0 && hit.rayT < maxDistance) {
                    hit.headerIndex = chunkIdx;
                    // Front-to-back: the first occupied cell with a hit holds this grid's nearest hit.
                    return hit;
                }
            }
        }

        // Advance to the next cell along the single nearest axis boundary.
        if (tMax.x <= tMax.y && tMax.x <= tMax.z) { tCurrent = tMax.x; cell.x += stepDir.x; tMax.x += tDelta.x; }
        else if (tMax.y <= tMax.z)                { tCurrent = tMax.y; cell.y += stepDir.y; tMax.y += tDelta.y; }
        else                                      { tCurrent = tMax.z; cell.z += stepDir.z; tMax.z += tDelta.z; }

        if (cell.x < 0 || cell.y < 0 || cell.z < 0 ||
            cell.x >= g.dims.x || cell.y >= g.dims.y || cell.z >= g.dims.z) break;
    }
    return miss;
}

SceneIntersectData raySceneIntersect(Ray ray, RayQuery rayQuery) {
    float closestDistance = 100000000;
    SceneIntersectData sceneIntersect;
    sceneIntersect.foundBox.size = -1;
    sceneIntersect.steps = 0;
    sceneIntersect.rayT = -1.0;
    sceneIntersect.normal = vec3(0.0);

    // Loose (transform-placed) chunks: brute-force over the explicit loose-handle list (not a
    // positional prefix), so add/remove needs no header reordering. Broadphase in each chunk's
    // local frame so rotated boxes are tested correctly.
    int looseCount = int(sceneLooseCount());
    for(int i = 0; i < looseCount; i++){
        uint headerIndex = looseListValue(uint(i));
        chunkHeader h = headers(int(headerIndex));
        // A freed/dead slot carries a degenerate header (scale <= 0); skip it.
        if(h.scale <= 0.0) continue;
        vec3 P = vec3(h.positionX, h.positionY, h.positionZ);
        mat3 Rinv = transpose(rotationFromQuat(h.rotation));
        Ray localRay;
        localRay.origin = Rinv * (ray.origin - P);
        localRay.direction = Rinv * ray.direction;
        BoxAABB tree64BoundingBox;
        tree64BoundingBox.position = vec3(0.0);
        tree64BoundingBox.size = h.scale;
        float tBB = getRayBoxEntryDistance(localRay, tree64BoundingBox);
        if(tBB < 0 || tBB >= closestDistance) continue;

        SceneIntersectData tree64Intersect = castRayThroughTree64(ray, rayQuery, int(headerIndex));
        // Rank by the march's own rayT (re-intersecting foundBox analytically flips
        // boundary-exact hits into misses by ULPs). rayT == 0 is a real hit.
        if(tree64Intersect.foundBox.size > 0 && tree64Intersect.rayT >= 0 && tree64Intersect.rayT < closestDistance){
            sceneIntersect = tree64Intersect;
            sceneIntersect.headerIndex = headerIndex;
            closestDistance = tree64Intersect.rayT;
        }
    }

    // Grid volumes: top-level uniform-grid DDA, pruned by the closest loose hit so far.
    int gridCount = int(sceneGridCount());
    for(int gi = 0; gi < gridCount; gi++){
        SceneIntersectData gh = marchGrid(ray, rayQuery, gi, closestDistance);
        if(gh.foundBox.size > 0 && gh.rayT >= 0 && gh.rayT < closestDistance){
            sceneIntersect = gh;
            closestDistance = gh.rayT;
        }
    }
    return sceneIntersect;
}

