#define MAX_STACK_SIZE 12
#define RENDER_MODE 3 //1-8
#define VOXEL_TYPEDATA_SLICES 3

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
    Tree64NodeData cachedData; // This line is the issue.
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
    uint padding[2];
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
};

static CombinedNode64 nodeStack[5];
static uint nodeStackQuantity = 0;

// LUT's
const uint MOVE_LUT[448] = {
    0u,     1u,     0u,     2u,     0u,     4u,     0u,
    1u,     8u,     0u,     3u,     1u,     5u,     1u,
    2u,     3u,     2u,     16u,     0u,     6u,     2u,
    3u,     10u,     2u,     17u,     1u,     7u,     3u,
    4u,     5u,     4u,     6u,     4u,     32u,     0u,
    5u,     12u,     4u,     7u,     5u,     33u,     1u,
    6u,     7u,     6u,     20u,     4u,     34u,     2u,
    7u,     14u,     6u,     21u,     5u,     35u,     3u,
    8u,     9u,     1u,     10u,     8u,     12u,     8u,
    9u,     9u,     8u,     11u,     9u,     13u,     9u,
    10u,     11u,     3u,     24u,     8u,     14u,     10u,
    11u,     11u,     10u,     25u,     9u,     15u,     11u,
    12u,     13u,     5u,     14u,     12u,     40u,     8u,
    13u,     13u,     12u,     15u,     13u,     41u,     9u,
    14u,     15u,     7u,     28u,     12u,     42u,     10u,
    15u,     15u,     14u,     29u,     13u,     43u,     11u,
    16u,     17u,     16u,     18u,     2u,     20u,     16u,
    17u,     24u,     16u,     19u,     3u,     21u,     17u,
    18u,     19u,     18u,     18u,     16u,     22u,     18u,
    19u,     26u,     18u,     19u,     17u,     23u,     19u,
    20u,     21u,     20u,     22u,     6u,     48u,     16u,
    21u,     28u,     20u,     23u,     7u,     49u,     17u,
    22u,     23u,     22u,     22u,     20u,     50u,     18u,
    23u,     30u,     22u,     23u,     21u,     51u,     19u,
    24u,     25u,     17u,     26u,     10u,     28u,     24u,
    25u,     25u,     24u,     27u,     11u,     29u,     25u,
    26u,     27u,     19u,     26u,     24u,     30u,     26u,
    27u,     27u,     26u,     27u,     25u,     31u,     27u,
    28u,     29u,     21u,     30u,     14u,     56u,     24u,
    29u,     29u,     28u,     31u,     15u,     57u,     25u,
    30u,     31u,     23u,     30u,     28u,     58u,     26u,
    31u,     31u,     30u,     31u,     29u,     59u,     27u,
    32u,     33u,     32u,     34u,     32u,     36u,     4u,
    33u,     40u,     32u,     35u,     33u,     37u,     5u,
    34u,     35u,     34u,     48u,     32u,     38u,     6u,
    35u,     42u,     34u,     49u,     33u,     39u,     7u,
    36u,     37u,     36u,     38u,     36u,     36u,     32u,
    37u,     44u,     36u,     39u,     37u,     37u,     33u,
    38u,     39u,     38u,     52u,     36u,     38u,     34u,
    39u,     46u,     38u,     53u,     37u,     39u,     35u,
    40u,     41u,     33u,     42u,     40u,     44u,     12u,
    41u,     41u,     40u,     43u,     41u,     45u,     13u,
    42u,     43u,     35u,     56u,     40u,     46u,     14u,
    43u,     43u,     42u,     57u,     41u,     47u,     15u,
    44u,     45u,     37u,     46u,     44u,     44u,     40u,
    45u,     45u,     44u,     47u,     45u,     45u,     41u,
    46u,     47u,     39u,     60u,     44u,     46u,     42u,
    47u,     47u,     46u,     61u,     45u,     47u,     43u,
    48u,     49u,     48u,     50u,     34u,     52u,     20u,
    49u,     56u,     48u,     51u,     35u,     53u,     21u,
    50u,     51u,     50u,     50u,     48u,     54u,     22u,
    51u,     58u,     50u,     51u,     49u,     55u,     23u,
    52u,     53u,     52u,     54u,     38u,     52u,     48u,
    53u,     60u,     52u,     55u,     39u,     53u,     49u,
    54u,     55u,     54u,     54u,     52u,     54u,     50u,
    55u,     62u,     54u,     55u,     53u,     55u,     51u,
    56u,     57u,     49u,     58u,     42u,     60u,     28u,
    57u,     57u,     56u,     59u,     43u,     61u,     29u,
    58u,     59u,     51u,     58u,     56u,     62u,     30u,
    59u,     59u,     58u,     59u,     57u,     63u,     31u,
    60u,     61u,     53u,     62u,     46u,     60u,     56u,
    61u,     61u,     60u,     63u,     47u,     61u,     57u,
    62u,     63u,     55u,     62u,     60u,     62u,     58u,
    63u,     63u,     62u,     63u,     61u,     63u,     59u
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
    int index = headerIndex * 3;

    uvec4 pixel0 = texelFetch(headerData, ivec2(index + 0, 0), 0);  
    uvec4 pixel1 = texelFetch(headerData, ivec2(index + 1, 0), 0);  
    uvec4 pixel2 = texelFetch(headerData, ivec2(index + 2, 0), 0);  

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
      
    return header;
}

int headersLength() {
    ivec2 texSize = textureSize(headerData, 0);
    return int(texSize.x / 3);
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
    for(int i = 0; i < log2(gridResolution); i++){
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
    vec2 flippedUV = vec2(uv.x, 1-uv.y);
    vec2 uvNegativeOneToOne = flippedUV * 2.0 - 1.0;
    float aspectRatio = res.x / res.y;
    float scale = tan(radians(fov * 0.5));
    vec2 pixelNDC = vec2(
        ((uvNegativeOneToOne.x) * scale) * aspectRatio,
        uvNegativeOneToOne.y * scale
    );
    vec3 targetPosition = cameraPosition + cameraDirection;
    vec3 lookat = normalize(targetPosition - cameraPosition);
    vec3 right = cross(lookat, vec3(0, 1, 0));
    vec3 actualUp = cross(right, lookat);
    vec3 rayDirection = normalize(pixelNDC.x * right + pixelNDC.y * actualUp + lookat);
    return rayDirection;
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
// Z-order movement via LUT (bit-exact to original implementation)
// Layout: XYZXYZ (2 bits per axis)
// ------------------------------------------------------------

// Encode direction (-1,0,1)^3 → [0..26]
// Convert direction to index (only 7 values)
uint encodeDir(vec3 d) {
    return (d.x == 1) * 1u
         + (d.x == -1) * 2u
         + (d.y == 1) * 3u
         + (d.y == -1) * 4u
         + (d.z == 1) * 5u
         + (d.z == -1) * 6u;
}

// Fast move
uint moveZOrder(uint zOrder, vec3 direction)
{
    return MOVE_LUT[zOrder * 7u + encodeDir(direction)];
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
    float rayBoxIntersectDistance = getRayBoxEntryDistanceForSureHit(ray, boundingBox);
    
    float eps = 1e-4;
    vec3 intersectPos = ray.origin + ray.direction * (rayBoxIntersectDistance + eps);
    
    ivec3 voxelCoordinate = ivec3(floor(intersectPos));
    
    // Direction-aware correction for exact boundaries
    bvec3 exactBoundary = equal(intersectPos, floor(intersectPos));
    bvec3 negativeDir = lessThan(ray.direction, vec3(0.0));
    voxelCoordinate -= ivec3(exactBoundary) * ivec3(negativeDir);
    
    ivec3 boxMin = boundingBox.position;
    ivec3 boxMax = boundingBox.position + boundingBox.size - ivec3(1);
    voxelCoordinate = clamp(voxelCoordinate, boxMin, boxMax);
    
    return voxelCoordinate;
}

// Resolution is the resolution that we want it to find the intersect at. 
// BoxAABB boundingBox is assumed to be positionted correctly in traversal space. (bottom left corner is where it actually lies in voxel coordinates, size is how many voxels it takes up)
ivec3 determineTraversalCoordinatesFromRayAndBoxAndRayDistance(Ray ray, BoxAABB boundingBox, float rayT) { 
    vec3 intersectPos = ray.origin + ray.direction * (rayT + 1e-6);
    
    ivec3 voxelCoordinate = ivec3(floor(intersectPos));
    
    // Direction-aware correction for exact boundaries
    bvec3 exactBoundary = equal(intersectPos, floor(intersectPos));
    bvec3 negativeDir = lessThan(ray.direction, vec3(0.0));
    voxelCoordinate -= ivec3(exactBoundary) * ivec3(negativeDir);
    
    ivec3 boxMin = boundingBox.position;
    ivec3 boxMax = boundingBox.position + boundingBox.size - ivec3(1);
    voxelCoordinate = clamp(voxelCoordinate, boxMin, boxMax);
    
    return voxelCoordinate;
}


// Rounds our voxel coordinates down.
// Takes in high res voxel coordinates, node size.
// Then rounds voxel coordinates down to the nearest multiple of node size.
ivec3 roundVoxelCoordinatesBasedOnNodeSize(ivec3 highResPosition, uint targetNodeSize) {
    ivec3 nodeSpaceCoordinate = highResPosition >> uint(log2(targetNodeSize));
    return nodeSpaceCoordinate << uint(log2(targetNodeSize)); // Could also be bit shift, targetNodeSize is always power of 4.
}

SceneIntersectData marchRayThroughTree64_DDA(Ray ray, RayQuery rayQuery, BoxAABB boundingBox, uint tree64StartIndex, uint tree64EndIndex, uint tree64Resolution) {
    SceneIntersectData returnData;
    vec3 invRayDir = 1.0/ray.direction;
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

    nodeStack[0].thisNodeZOrderInParent = 0;
    nodeStack[0].dataIndex = tree64StartIndex;
    nodeStack[1].thisNodeZOrderInParent = getZOrderInParentFromThisNodesLevel(traversalPosition, candidateNodeLevel);
    nodeStack[1].dataIndex = 0;
    nodeStackQuantity = 2;

    //traversalPosition = roundVoxelCoordinatesBasedOnNodeSize(traversalPosition, tree64Resolution/4);
    traversalPosition = (traversalPosition >> shift) << shift;
    ivec3 stepI = ivec3(sign(ray.direction));
    float rayT = getRayBoxEntryDistanceForSureHit(ray, boundingBox);

    uint stepCount = 0;
    bool previouslyPopped = false;
    Tree64NodeData data = tree64(tree64StartIndex);
    for (; stepCount < rayQuery.maxRaySteps; stepCount++) {
        //stepCount += 1;
        while (candidateNodeLevel >= 0) {
            //stepCount -= 1;
            if (previouslyPopped) break;
            if (checkZOrderInValidMasks(data.data1, data.data2, nodeStack[nodeStackQuantity - 1u].thisNodeZOrderInParent)) {
                if((data.data3 & 0b1) == 1u) { 
                    returnData.foundBox.position = traversalPosition;
                    returnData.foundBox.size = stepSize;
                    returnData.steps = stepCount;
                    return returnData;
                    // Leaf found! Handle accordingly.
                }
                BoxAABB candidateBox;
                candidateBox.position = vec3(traversalPosition);
                candidateBox.size = float(stepSize);
                ivec3 highResPosition = determineTraversalCoordinatesFromRayAndBoxAndRayDistance(ray, candidateBox, rayT);
                uint bottomChildPointer = (data.data3 >> 1) & 0b01111111111111111111111111111111;
                uint parentDataIndex = nodeStack[nodeStackQuantity - 2u].dataIndex;
                uint childrenBeforeThisNode = calculateSiblingsBeforeThisZOrder(4, data.data1, data.data2, nodeStack[nodeStackQuantity - 1u].thisNodeZOrderInParent);
                nodeStack[nodeStackQuantity - 1u].dataIndex = bottomChildPointer + parentDataIndex + childrenBeforeThisNode;
                nodeStack[nodeStackQuantity].thisNodeZOrderInParent = getZOrderInParentFromThisNodesLevel(highResPosition, candidateNodeLevel - 1); // off by 1 error?
                nodeStack[nodeStackQuantity].dataIndex = 0;
                nodeStackQuantity += 1;
                candidateNodeLevel -= 1;
                // equates to : pow(4, candidateNodeLevel);
                shift = 2u * candidateNodeLevel;
                stepSize = 1u << (shift);
                //traversalPosition = roundVoxelCoordinatesBasedOnNodeSize(highResPosition, stepSize);
                traversalPosition = (highResPosition >> shift) << shift;


                data = tree64(nodeStack[nodeStackQuantity - 2u].dataIndex);
            } else {
                break;
            }
        }

        vec3 nextPlanes = mix(vec3(traversalPosition), vec3(traversalPosition) + vec3(stepSize), step(vec3(0.0), ray.direction));

        vec3 tMax = (nextPlanes - ray.origin) * invRayDir;
        vec3 tDelta = stepSize * abs(invRayDir);
        previouslyPopped = false;
        //Tree64NodeData parentData = tree64(nodeStack[nodeStackQuantity - 2u].dataIndex);
        for (int i = 0; i < 12; i++) {
            // Actually do DDA

            rayT = min(tMax.x, min(tMax.y, tMax.z));

            vec3 axisMask = step(tMax, vec3(rayT));

            tMax += axisMask * tDelta;

            ivec3 directionSteppedIn = ivec3(round(axisMask)) * stepI;

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
                data = tree64(nodeStack[nodeStackQuantity - 2u].dataIndex);
                break;
            }

            nodeStack[nodeStackQuantity - 1u].thisNodeZOrderInParent = moveZOrder(nodeStack[nodeStackQuantity - 1u].thisNodeZOrderInParent, directionSteppedIn);
            traversalPosition = proposedTraversalPosition;

            // Only check for if its valid after we advance.
            // This is because if we are advancing it could mean that we just popped out of a valid candidate, 
            // checking if its valid before we advance would just put us right back into the one we popped out of.
            // Used to be 17 - 
            if (checkZOrderInValidMasks(data.data1, data.data2, nodeStack[nodeStackQuantity - 1u].thisNodeZOrderInParent)) { // New valid z order found!!
                if ((data.data3 & 0b1) == 1) {
                    returnData.foundBox.position = traversalPosition;
                    returnData.foundBox.size = stepSize;
                    returnData.steps = stepCount;
                    return returnData;
                    // Leaf found, handle accordingly.
                }
                break;
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
    Ray transformedRay;
    transformedRay.origin = ((ray.origin - (vec3(header.positionX, header.positionY, header.positionZ) - tree64BoundingBox.position)) * tree64BoundingBox.size)/header.scale;
    transformedRay.direction = ray.direction;

    float epsilon = 1e-6;
    if (abs(transformedRay.direction.x) < epsilon) transformedRay.direction.x = epsilon;
    if (abs(transformedRay.direction.y) < epsilon) transformedRay.direction.y = epsilon;
    if (abs(transformedRay.direction.z) < epsilon) transformedRay.direction.z = epsilon;

    IntersectionResult rootIntersect = getRayBoxEntry(transformedRay, tree64BoundingBox);

    SceneIntersectData tree64Intersect;
    tree64Intersect.foundBox.size = -1;
    if(rootIntersect.distance >= 0){
        tree64Intersect = marchRayThroughTree64_DDA(transformedRay, rayQuery, tree64BoundingBox, tree64StartIndex, tree64EndIndex, header.resolution);
    }
    // size should be 1 for full res, 2 for half res, 4 for quarter res etc. So we multiply the size by what the voxel scale actually is.
    tree64Intersect.foundBox.size *= header.scale/tree64BoundingBox.size;
    vec3 normalizedBoxPosition = tree64Intersect.foundBox.position/tree64BoundingBox.size;
    tree64Intersect.foundBox.position = normalizedBoxPosition*header.scale + (vec3(header.positionX, header.positionY, header.positionZ) - tree64BoundingBox.position);
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
    uint voxelGridResolution = headers(headerIndex).resolution;
    BoxAABB voxelGridBoundingBox;
    voxelGridBoundingBox.position = vec3(headers(headerIndex).positionX, headers(headerIndex).positionY, headers(headerIndex).positionZ);
    voxelGridBoundingBox.size = headers(headerIndex).scale;
    ivec3 voxelGridPos = voxelGridPosition(voxelBoundingBox, voxelGridBoundingBox, voxelGridResolution);
    uint voxelTypeDataIndex = findVoxelTypeDataIndex(voxelGridPos.x, voxelGridPos.y, voxelGridPos.z, voxelGridResolution, headers(headerIndex).voxelTypeDataStartIndex, headers(headerIndex).voxelTypeDataEndIndex);
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

SceneIntersectData raySceneIntersect(Ray ray, RayQuery rayQuery) {
    float closestDistance = 100000000;
    BoxAABB closestBox;
    uint closestHeaderIndex = 0;
    SceneIntersectData sceneIntersect;
    sceneIntersect.foundBox.size = -1;
    sceneIntersect.steps = 0;
    for(int i = 0; i < headersLength(); i++){ // HEADER_LENGTH should be headers.length();
        BoxAABB tree64BoundingBox;
        tree64BoundingBox.position = vec3(headers(i).positionX, headers(i).positionY, headers(i).positionZ);
        tree64BoundingBox.size = headers(i).scale;
        float tBB = getRayBoxEntryDistance(ray, tree64BoundingBox);
        if(tBB < 0 || tBB >= closestDistance) continue;

        SceneIntersectData tree64Intersect = castRayThroughTree64(ray, rayQuery, i);
        float intersectionDistance = getRayBoxEntryDistance(ray, tree64Intersect.foundBox);
        if(tree64Intersect.foundBox.size > 0 && intersectionDistance > 0 && intersectionDistance < closestDistance){
            sceneIntersect = tree64Intersect;
            sceneIntersect.headerIndex = i;
            closestDistance = intersectionDistance;
        }
    }
    return sceneIntersect;
}

