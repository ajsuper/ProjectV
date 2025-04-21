#version 460 core

layout (location = 0) out vec3 FragColor;

in vec2 TexCoords;
uniform float time;
uniform vec2 resolution;
uniform vec3 cameraPos;
uniform vec3 cameraDir;

#define MAX_STACK_SIZE 12
#define MAX_RAY_STEPS 100
#define RENDER_MODE 3 //1-8
#define VOXEL_TYPEDATA_SLICES 3

//Ray structure with an origin, direction, and color.
struct Ray {
    vec3 origin;
    vec3 direction;
};

//Box structure with a position(min of x, y, z) and a size.
struct BoxAABB {
    vec3 position;
    float size;
};

struct Voxel {
    uint index;
    vec3 color;
    vec3 normal;
};

struct rayMarchReturnStruct {
    BoxAABB foundBox;
    int raySteps;
};

struct CombinedNode{
    uint nodeIndex;
    uint octant1;
};

struct chunkHeader {
    float positionX;
    float positionY;
    float positionZ;
    uint scale;
    uint resolution;
    uint geometryStartIndex;
    uint geometryEndIndex;
    uint voxelTypeDataStartIndex;
    uint voxelTypeDataEndIndex;
};

struct sceneIntersectData {
    uint headerIndex;
    BoxAABB foundBox;
};

//Access the boxes buffer for raw rendering (Temporary just for debugging)
layout(std430, binding = 0) buffer BoxesBuffer {
    BoxAABB boxes[];
};

layout(std430, binding = 3) buffer HeaderBuffer {
    chunkHeader headers[];
};

layout(std430, binding = 4) buffer GeometryBuffer {
    uint octrees[];
};

layout(std430, binding = 5) buffer VoxelTypeDatasBuffer {
    uint voxelTypeDatas[];
};

//Do not need a stack for the boxes, just store 2 instead
BoxAABB masterParentBox;
BoxAABB masterChildBox;
//Global constants that change between frames
vec3 inverseRayDirection;
//The arrays that will act like stacks that are used to march the ray through the sparse voxel octrees (Ideally allocate once but that isn't yet implemented)
//uint nodeIndexStack[MAX_STACK_SIZE];
//uint octant1Stack[MAX_STACK_SIZE];
CombinedNode combinedNodeStack[MAX_STACK_SIZE];
//The amount of items in each array (Allows for stack-like functionality)
//int nodeIndexStackQuantity = 0;
//int octant1StackQuantity = 0;
int combinedNodeStackQuantity = 0;


//Returns a random float.
float randomFloat0to1(vec2 uv, float timeX) {
    vec3 coords = vec3(uv * timeX, timeX);

    coords = fract(coords * vec3(443.8975, 441.4234, 437.195));
    coords += dot(coords, coords.yzx + 19.19);

    float n = dot(coords, vec3(12.9898, 78.233, 45.164));
    return fract(sin(n) * 43758.5453123);
}


//Given an intersectedSide and whether or not the normal is positive, return a vector pointing in the normal direction.
vec3 calculateAABBNormal(float intersectedSide, float isNormalPositive){
    vec3 normal = vec3(0, 0, 0);
    float isNormalPositiveMultiplier = isNormalPositive*2-1;
    normal[int(intersectedSide-1)] = isNormalPositiveMultiplier;
    return normal;
}

float rayPlaneIntersectionX(float planeX, Ray ray) {
    // Check if the ray is parallel to the X plane
    if (abs(ray.direction.x) > 1e-6) {
        // Calculate t using the X component
        float t = (planeX - ray.origin.x) / ray.direction.x;
        return t;
    }
    return -1; // No intersection
}

float rayPlaneIntersectionY(float planeY, Ray ray) {
    // Check if the ray is parallel to the Y plane
    if (abs(ray.direction.y) > 1e-6) {
        // Calculate t using the Y component
        float t = (planeY - ray.origin.y) / ray.direction.y;
        return t;
    }
    return -1; // No intersection
}

float rayPlaneIntersectionZ(float planeZ, Ray ray) {
    // Check if the ray is parallel to the Z plane
    if (abs(ray.direction.z) > 1e-6) {
        // Calculate t using the Z component
        float t = (planeZ - ray.origin.z) / ray.direction.z;
        return t;
    }
    return -1; // No intersection
}


//Given an integer x, y, z, and the resolution of the voxel grid, return the ZOrderIndex of that point.
uint calculateZOrderIndex(uint x, uint y, uint z, uint octreeWholeResolution){
    uint ZOrder = 0;
    for(int i = 0; i < log2(octreeWholeResolution); i++){
        uint bitY = (z >> i) & 1u;
        uint bitX = (y >> i) & 1u;
        uint bitZ = (x >> i) & 1u;

        ZOrder |= (bitY << (3 * i)) | (bitX << (3 * i + 1)) | (bitZ << (3 * i + 2));
    }
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
    vec2 uvNegativeOneToOne = uv * 2.0 - 1.0;
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

//Pushes the indexies of a node in the octree buffer into it's stack.

/*
void pushNodeIndexStack(uint nodeToBePushed) {
    if(nodeIndexStackQuantity < MAX_STACK_SIZE){
        nodeIndexStack[nodeIndexStackQuantity] = nodeToBePushed;
        nodeIndexStackQuantity += 1;
    }
}
//Pushes an octant1 (0-7) into it's stack.
void pushOctant1Stack(uint octant1ToBePushed) {
    if(octant1StackQuantity < MAX_STACK_SIZE){
        octant1Stack[octant1StackQuantity] = octant1ToBePushed;
        octant1StackQuantity += 1;
    }
}
*/

void pushCombinedNodeStack(uint octant1ToBePushed, uint nodeIndexToBePushed) {
    if(combinedNodeStackQuantity < MAX_STACK_SIZE){
        combinedNodeStack[combinedNodeStackQuantity].octant1 = octant1ToBePushed;
        combinedNodeStack[combinedNodeStackQuantity].nodeIndex = nodeIndexToBePushed;
        combinedNodeStackQuantity += 1;
    }
}

uint moveOctant1(uint octant1, uint sideToMoveTo){
    return octant1 ^ (1 << (3-sideToMoveTo));
    // Equivalent to:
    /*
    if(sideToMoveTo == 1) {
        return octant1 ^ (1 << 2);
    } else if(sideToMoveTo == 2) {     
        return octant1 ^ (1 << 1);
    } else if(sideToMoveTo == 3) {
        return octant1 ^ 1;
    }
    */
}

float boxRayDistance(BoxAABB box, Ray castedRay) {
    vec3 rayInverse = 1.0/castedRay.direction;
    vec3 t0 = (box.position - castedRay.origin) * rayInverse;
    vec3 t1 = (box.position + box.size - castedRay.origin) * rayInverse;

    vec3 tsmaller = min(t0, t1);
    vec3 tbigger = max(t0, t1);

    float tmin_candidate = max(max(tsmaller.x, tsmaller.y), tsmaller.z);
    float tmax_candidate = min(min(tbigger.x, tbigger.y), tbigger.z);

    //Check if the ray origin is inside the box.
    bool isInside = all(greaterThanEqual(castedRay.origin, box.position)) &&
                    all(lessThanEqual(castedRay.origin, box.position + box.size));

    //Adjust tmin_candidate if the ray starts inside the box.
    if (isInside) {
        tmin_candidate = 0.0;
    }

    //Check if there is a valid intersection.
    if (tmin_candidate > tmax_candidate || tmax_candidate < 0.0) {
        return -1.0;  //No valid intersection.
    }

    return tmin_candidate;
}
//Calculates the closest intersect point of a box and the side it enters it in.
vec3 boxRayIntersect(BoxAABB box, Ray castedRay) {
    vec3 rayInverse = 1.0/castedRay.direction;
    vec3 t0 = (box.position - castedRay.origin) * rayInverse;
    vec3 t1 = (box.position + box.size - castedRay.origin) * rayInverse;

    vec3 tsmaller = min(t0, t1);
    vec3 tbigger = max(t0, t1);

    float tmin_candidate = max(max(tsmaller.x, tsmaller.y), tsmaller.z);
    float tmax_candidate = min(min(tbigger.x, tbigger.y), tbigger.z);

    uint intersectedSide = 3u; //Default to Z-axis.
    if (tmin_candidate == tsmaller.x) {
        intersectedSide = 1u; //X-axis.
    } else if (tmin_candidate == tsmaller.y) {
        intersectedSide = 2u; //Y-axis.
    }

    //Check if the ray origin is inside the box.
    bool isInside = all(greaterThanEqual(castedRay.origin, box.position)) &&
                    all(lessThanEqual(castedRay.origin, box.position + box.size));

    //Adjust tmin_candidate if the ray starts inside the box.
    if (isInside) {
        tmin_candidate = 0.0;
    }

    //Check if there is a valid intersection.
    if (tmin_candidate > tmax_candidate || tmax_candidate < 0.0) {
        return vec3(-1.0, float(intersectedSide), -1.0);  //No valid intersection.
    }

    //Calculate the normal direction based on the intersected axis.
    float isNormalPositive = 1.0;  //Assume positive normal.

    if (intersectedSide == 1u) {  //X-axis.
        isNormalPositive = castedRay.direction.x < 0.0 ? 1.0 : -1.0;  //Properly handle negative X.
    } else if (intersectedSide == 2u) {  //Y-axis.
        isNormalPositive = castedRay.direction.y < 0.0 ? 1.0 : -1.0;
    } else if (intersectedSide == 3u) {  //Z-axis.
        isNormalPositive = castedRay.direction.z < 0.0 ? 1.0 : -1.0;
    }

    return vec3(tmin_candidate, float(intersectedSide), isNormalPositive);
}
vec2 boxRayIntersectInverted2(BoxAABB box, Ray castedRay) {
    vec3 invDir = 1.0 / castedRay.direction;
    vec3 boxMin = box.position;
    vec3 boxMax = box.position + box.size;

    // Compute t values for all slabs (x, y, z planes)
    vec3 tMin = (boxMin - castedRay.origin) * invDir;
    vec3 tMax = (boxMax - castedRay.origin) * invDir;

    // Ensure tMin < tMax for each axis
    vec3 tNear = min(tMin, tMax);
    vec3 tFar = max(tMin, tMax);

    // Find the intersection interval [tEntry, tExit]
    float tEntry = max(max(tNear.x, tNear.y), tNear.z);
    float tExit = min(min(tFar.x, tFar.y), tFar.z);

    // Determine exit side
    uint exitSide = tExit == tFar.x ? 1u : (tExit == tFar.y ? 2u : 3u);

    // Check if the ray is outside the box or goes away from it
    if (tEntry > tExit || tExit < 0.0) {
        return vec2(-1.0, float(exitSide));
    }

    // Check if the ray starts inside the box
    bool isInside = all(greaterThanEqual(castedRay.origin, boxMin)) &&
                    all(lessThanEqual(castedRay.origin, boxMax));

    // If inside the box, set tEntry to 0
    if (isInside) {
        tEntry = 0.0;
    }

    return vec2(tExit, float(exitSide));
}

vec2 boxRayIntersectInverted(BoxAABB box, Ray castedRay) {
    vec3 invDir = 1.0 / castedRay.direction;
    vec3 boxMax = box.position + box.size;

    // Compute t values for exiting the box (only the "far" values matter)
    vec3 tFar = (boxMax - castedRay.origin) * invDir;

    // Handle potential negative directions
    vec3 tMin = (box.position - castedRay.origin) * invDir;
    vec3 tMax = max(tMin, tFar);

    // Find the axis with the largest t value (the exit point)
    float tExit = min(min(tMax.x, tMax.y), tMax.z);

    // Determine which side is exited
    uint exitSide = tExit == tMax.x ? 1u : (tExit == tMax.y ? 2u : 3u);

    // Check if the ray misses the box entirely
    if (tExit < 0.0) {
        return vec2(-1.0, float(exitSide));
    }

    return vec2(tExit, float(exitSide));
}


//Calculates the octant of a vector of unit components in a box
uint calculateOctant1BasedOfEdgePos(vec3 rayEdgePos) {
    uint newOctant1 = 0;
    if(rayEdgePos.z >= 0.5) { newOctant1 += 1; }
    if(rayEdgePos.y >= 0.5) { newOctant1 += 2; }
    if(rayEdgePos.x >= 0.5) { newOctant1 += 4; }
    return newOctant1;
}

//Calculates how many siblings this octant has that are before it in the tree
uint calculateSiblingsBeforeThisOctant1(uint node, uint checkOctant1) {
    uint siblingsBeforeThisOct = 0;
    uint childCount = 0;
    uint validMask = (node >> 1) & 0x00FF;
    for(uint i = 0; i < 8; i++){
        if(i == checkOctant1){ siblingsBeforeThisOct = childCount; }
        if((validMask & (1 << (7-i))) != 0){ childCount += 1; }
    }
    return siblingsBeforeThisOct;
}

//Calulates the octant that a ray intersects a box in
uint rayBoxOctant1(Ray ray, BoxAABB box) {
    float intersectData = boxRayDistance(box, ray);
    vec3 intersectPosition = ray.origin+ray.direction*intersectData;
    vec3 intersectPositionUV = (intersectPosition-box.position)/box.size;
    uint octant1 = calculateOctant1BasedOfEdgePos(intersectPositionUV);
    return octant1;
}

//Given a parent box and an octant, return the octants corresponding child box.
BoxAABB newChildFromParentAndOctant1(BoxAABB parentBox, uint checkOctant1) {
    BoxAABB childBox;
    childBox.size = parentBox.size*0.5;

    childBox.position = parentBox.position+vec3(
        float((checkOctant1 & (1u << 2u)) != 0u) * childBox.size, 
        float((checkOctant1 & (1u << 1u)) != 0u) * childBox.size, 
        float((checkOctant1 & 1u) != 0u) * childBox.size
    );

    return childBox;
}

//Inverse of newChildFromParentAndOctant1. Given the octant that a child is inside of a parent and the child box,
//return the parent that the child is in.
BoxAABB newParentFromChildAndOctant1(BoxAABB childBox, uint thisChildOctant1) {
    BoxAABB parentBox;
    parentBox.size = childBox.size * 2.0;

    vec3 offsetPosition = vec3(
        float((thisChildOctant1 & (1u << 2u)) != 0u) * childBox.size,
        float((thisChildOctant1 & (1u << 1u)) != 0u) * childBox.size,
        float((thisChildOctant1 & 1u) != 0u) * childBox.size
    );

    parentBox.position = childBox.position - offsetPosition;
    return parentBox;
}

//Goes into the parents child based off of the octant we are checking for. Detects if the child we pushed into is a leaf or if it is still a parent.
//Returns 0, 1, or 2. 
//0->has pushed, child found. 
//1->has pushed, no child found. 
//2->has not pushed, no child found
uint push(Ray ray) {
    //uint checkOctant1 = octant1Stack[octant1StackQuantity-1];
    CombinedNode node = combinedNodeStack[combinedNodeStackQuantity - 1u];
    uint checkOctant1 = node.octant1;
    //uint lastNodeIndex = nodeIndexStack[nodeIndexStackQuantity-1];
    uint lastNodeIndex = node.nodeIndex;
    uint lastNodeData = octrees[lastNodeIndex];

    //If octant we are checking is a valid node.
    if((lastNodeData & (1 << 8-checkOctant1)) != 0){
        //Generate pushed into octant.
        uint newOctant1 = rayBoxOctant1(ray, masterChildBox);
        if((lastNodeData & 1u) != 0){
            masterParentBox = masterChildBox;
            masterChildBox = newChildFromParentAndOctant1(masterChildBox, newOctant1);
            return 0; //continueMarching = false, successfullyPushed = true. Needs to end.
        }
        uint lastNodeDataChildPointer = (lastNodeData >> 9) & 0x7FFFFF;
        uint newNodeAddress = calculateSiblingsBeforeThisOctant1(lastNodeData, checkOctant1) + lastNodeDataChildPointer + lastNodeIndex;
        //pushNodeIndexStack(newNodeAddress);
        masterParentBox = masterChildBox;

        //pushOctant1Stack(newOctant1);
        pushCombinedNodeStack(newOctant1, newNodeAddress);
        masterChildBox = newChildFromParentAndOctant1(masterChildBox, newOctant1);

        return 1; //continueMarching = true, successfullyPushed = true. Needs to push.
    }

    return 2; //continueMarching = true, successfullyPushed = false. Needs to advance.
}

//Moves the ray into its adjacent sibling based off of which side it leaves the current child. Detects if a leaf is found or if the current node is a parent.
//Returns 0, 1, or 2
//0-> continueMarching = false, leaveChild = false
//1-> cotinueMarching = true, leaveChild = false
//2-> continueMarching = true, leaveChild = true 
uint advance2(Ray ray) {
    //uint parentNodeData = octree[nodeIndexStack[nodeIndexStackQuantity - 1u]];
    CombinedNode node = combinedNodeStack[combinedNodeStackQuantity - 1u];
    uint parentNodeData = octrees[node.nodeIndex];
    uint parentValidMask = (parentNodeData >> 1u) & 0x000000FF;
    uint leafMask = parentNodeData & 0x1;
    //uint lastOctant1 = octant1Stack[octant1StackQuantity - 1u];
    uint lastOctant1 = node.octant1;
    vec2 parentIntersectData = boxRayIntersectInverted(masterParentBox, ray);
    //Loop over each possible octant.
    for (uint i = 0u; i < 3u; i++) {
        //Find which axis to move along to get the next intersected box.
        vec2 intersectData = boxRayIntersectInverted(masterChildBox, ray);

        // Move to the new octant
        lastOctant1 = moveOctant1(lastOctant1, uint(intersectData.y));
        masterChildBox = newChildFromParentAndOctant1(masterParentBox, lastOctant1);
   
        if (intersectData.x == parentIntersectData.x) {
            return 2u; //continueMarching = false, leaveChild = false. Needs to pop.
        }
        
        if (leafMask == 1u) {
            if((parentValidMask & (1u << (7u - lastOctant1))) > 0u){
                //octant1Stack[octant1StackQuantity - 1u] = lastOctant1;
                combinedNodeStack[combinedNodeStackQuantity - 1u].octant1 = lastOctant1;
                return 0u; //Found a leaf node. Needs to end.
            }
        } else {
            if((parentValidMask & (1u << (7u - lastOctant1))) > 0u){
                //octant1Stack[octant1StackQuantity - 1u] = lastOctant1;
                combinedNodeStack[combinedNodeStackQuantity - 1u].octant1 = lastOctant1;
                return 1u; //cotinueMarching = true, leaveChild = false. Needs to push
            }
        }             
    }
    //octant1Stack[octant1StackQuantity - 1u] = lastOctant1;
    return 2u; //cotinueMarching = true, leaveChild = false. Continue traversal
}


//Splits the 0-2 value returned by push into its bool continueMarching and successfullyPushed flags
void splitPushCase(uint pushCase, inout bool continueMarching, inout bool successfullyPushed) {
    if(pushCase > 0){
        continueMarching = true;
    } else {
        continueMarching = false;
    }
    if(pushCase < 2){
        successfullyPushed = true;
    } else {
        successfullyPushed = false;
    }
}

//Splits the 0-2 value returned by advance into its bool continueMarching and leaveChild flags
void splitAdvanceCase(uint advanceCase, inout bool continueMarching, inout bool leaveChild) {
    if(advanceCase == 2){
        leaveChild = true;
    } else {
        leaveChild = false;
    }
    if(advanceCase > 0){
        continueMarching = true;
    } else {
        continueMarching = false;
    }
}


BoxAABB marchRayThroughOctree(Ray ray, BoxAABB boundingBox, vec3 uvInBoundingBox, uint octreeStartIndex, uint octreeEndIndex) {    
    int rayStepLimit = MAX_RAY_STEPS;

    //Avoid even ray origins to avoid the rays starting directly on the boundary of 2 nodes.
    if(fract(ray.origin.x * 0.5) == 0.0) ray.origin.x += 0.00001;
    if(fract(ray.origin.y * 0.5) == 0.0) ray.origin.y += 0.00001;
    if(fract(ray.origin.z * 0.5) == 0.0) ray.origin.z += 0.00001;

    //Set flags
    bool continueMarching = true;
    bool successfullyPushed = false;
    bool leaveChild = false;
    //Reset the stacks
    //nodeIndexStackQuantity = 0;
    //octant1StackQuantity = 0;
    combinedNodeStackQuantity = 0;

    //Perform the initial push.
    uint firstChildOctant1 = calculateOctant1BasedOfEdgePos(uvInBoundingBox);
    masterChildBox = newChildFromParentAndOctant1(boundingBox, firstChildOctant1);
    masterParentBox = boundingBox;
    pushCombinedNodeStack(firstChildOctant1, octreeStartIndex);
    //pushNodeIndexStack(OCTREE_HEADER_SIZE); 
    //pushOctant1Stack(firstChildOctant1); 
    
    //Main ray marching loop.
    int raySteps = 0;
    for(int rayStep = 0; rayStep < rayStepLimit; rayStep++){
        //Break if break conditions are met.
        if(!continueMarching){
            break;                
        }
        //Push as far as possible
        
        while(combinedNodeStackQuantity < MAX_STACK_SIZE-1){
            splitPushCase(push(ray), continueMarching, successfullyPushed);
            if(!successfullyPushed || !continueMarching){
                break;
            }
        }        
        
        //Once push is no longer possible, advance.
        if(!successfullyPushed){
            //Gather advance data.
            splitAdvanceCase(advance2(ray), continueMarching, leaveChild);
            //If it needs to pop, pop until it can succesfully advance or needs to exit the octree.
            while(leaveChild){
                //Exit the octree if the ray leaves the octree.
                if(combinedNodeStackQuantity < 2){
                    BoxAABB falseBox;
                    falseBox.size = -1.0;
                    return falseBox;
                }
                //If ray doesn't exit octree, continue to pop and repeat.
                masterChildBox = masterParentBox;
                masterParentBox = newParentFromChildAndOctant1(masterChildBox, combinedNodeStack[combinedNodeStackQuantity-2].octant1);
                //octant1StackQuantity -= 1;
                //nodeIndexStackQuantity -= 1;
                combinedNodeStackQuantity -= 1;
                splitAdvanceCase(advance2(ray), continueMarching, leaveChild); 
            }
        }
        raySteps += 1;
    }
    return masterChildBox;     
}

//Cast the ray through the octree bounding box2.
BoxAABB castRayThroughOctree(Ray ray, uint headerIndex) {
    chunkHeader header = headers[headerIndex];

    uint octreeStartIndex = header.geometryStartIndex;
    uint octreeEndIndex = header.geometryEndIndex;

    uint voxelTypeDataStartIndex = header.voxelTypeDataStartIndex;
    uint voxelTypeDataEndIndex = header.voxelTypeDataEndIndex;

    BoxAABB octreeBoundingBox;
    octreeBoundingBox.position = vec3(header.positionX, header.positionY, header.positionZ);
    octreeBoundingBox.size = header.scale;


    float tBB = boxRayDistance(octreeBoundingBox, ray);
    vec3 uvInBoundingBox;
    vec3 intersectionPoint = ray.origin + ray.direction * tBB;
    uvInBoundingBox = abs((octreeBoundingBox.position - intersectionPoint)) / octreeBoundingBox.size;
    
    BoxAABB octreeMarchData;
    octreeMarchData.size = -1;
    if(tBB >= 0){
        octreeMarchData = marchRayThroughOctree(ray, octreeBoundingBox, uvInBoundingBox, octreeStartIndex, octreeEndIndex);
    }
    return octreeMarchData;
}


sceneIntersectData castRayThroughScene(Ray ray){
    float closestDistance = 100000000;
    BoxAABB closestBox;
    uint closestHeaderIndex = -1;
    for(int i = 0; i < headers.length(); i++){
        BoxAABB octreeBoundingBox;
        octreeBoundingBox.position = vec3(headers[i].positionX, headers[i].positionY, headers[i].positionZ);
        octreeBoundingBox.size = headers[i].scale;
        float tBB = boxRayDistance(octreeBoundingBox, ray);
        if(tBB < 0 || tBB >= closestDistance) continue;

        BoxAABB octreeMarchBox = castRayThroughOctree(ray, i);
        vec3 intersectData = boxRayIntersect(octreeMarchBox, ray);
        if(octreeMarchBox.size > 0 && intersectData.x > 0 && intersectData.x < closestDistance){
            closestDistance = intersectData.x;
            closestBox = octreeMarchBox;
            closestHeaderIndex = i;
        }
    }
    sceneIntersectData intersectData;
    intersectData.foundBox = closestBox;
    intersectData.headerIndex = closestHeaderIndex;

    return intersectData;
}

//Given the integer postions inside of the voxel grid, find the type data for that voxel.
int findVoxelTypeDataIndex(int x, int y, int z, uint voxelGridResolution, uint voxelTypeDataStartIndex, uint voxelTypeDataEndIndex) {
    uint ZOrder = calculateZOrderIndex(x, y, z, voxelGridResolution);
    int beginningIndex = 0;
    int endIndex = int((voxelTypeDataEndIndex-voxelTypeDataStartIndex)/VOXEL_TYPEDATA_SLICES);
    int middleIndex = (beginningIndex+endIndex)/2;
    for(int i = 0; i < 100; i++){
        if(voxelTypeDatas[middleIndex*VOXEL_TYPEDATA_SLICES + voxelTypeDataStartIndex] == ZOrder){
            return middleIndex*VOXEL_TYPEDATA_SLICES;
        } else {
            if(voxelTypeDatas[middleIndex * VOXEL_TYPEDATA_SLICES + voxelTypeDataStartIndex] < ZOrder){
                beginningIndex = middleIndex + 1; 
            } else {
                endIndex = middleIndex - 1;  
            }
        }
        middleIndex = (beginningIndex+endIndex)/2;
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
    uint voxelGridResolution = headers[headerIndex].resolution;
    BoxAABB voxelGridBoundingBox;
    voxelGridBoundingBox.position = vec3(headers[headerIndex].positionX, headers[headerIndex].positionY, headers[headerIndex].positionZ);
    voxelGridBoundingBox.size = headers[headerIndex].scale;
    ivec3 voxelGridPos = voxelGridPosition(voxelBoundingBox, voxelGridBoundingBox, voxelGridResolution);
    uint voxelTypeDataIndex = findVoxelTypeDataIndex(voxelGridPos.x, voxelGridPos.y, voxelGridPos.z, voxelGridResolution, headers[headerIndex].voxelTypeDataStartIndex, headers[headerIndex].voxelTypeDataEndIndex);
    return voxelTypeDataIndex;
}
//Given the voxel index, return the voxel data.
Voxel fetchVoxelData(BoxAABB voxelBoundingBox, uint headerIndex){
    uint voxelIndex = findVoxelIndex(voxelBoundingBox, headerIndex);
    //Assume 6-bit color channels ()
    //Assume 4-bit normal vector channels ()
    uint voxelTypeDataStartIndex = headers[headerIndex].voxelTypeDataStartIndex;
    Voxel voxel;
    uint SerializedColor = voxelTypeDatas[voxelIndex+1+voxelTypeDataStartIndex];
    uint SerializedNormals = voxelTypeDatas[voxelIndex+2+voxelTypeDataStartIndex];
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

//Setup the ray and cast it into our scene and return the results from the cast as colors.
vec4 castRayWithOffset(vec2 offset){
    Ray ray;
    ray.direction = rayStartDirection(TexCoords + offset, resolution, cameraPos, normalize(vec3(cameraDir.x, 0, cameraDir.z)), 65);
    ray.origin = cameraPos;

    //Define the root box for the octree
    BoxAABB octreeBoundingBox;
    octreeBoundingBox.position = vec3(0.0, 0.0, 0.0);
    octreeBoundingBox.size = 40;
    vec4 color = vec4(1);

    //vec3 sunDirection = normalize(vec3((cos(float(time*0.01f))), abs(sin(float(time*0.01f))), 0.2));
    vec3 sunDirection = normalize(vec3(0.7, 0.99, -0.9));

    sceneIntersectData intersectData = castRayThroughScene(ray);
    vec3 firstIntersectData = boxRayIntersect(intersectData.foundBox, ray);

    if(firstIntersectData.x < 0.0){
        return vec4(0, 0.0, 1.0, 1.0);
    }

    vec3 normal = calculateAABBNormal(firstIntersectData.y, firstIntersectData.z);
    ray.origin = ray.origin + ray.direction*(firstIntersectData.x);
    ray.origin += normal*0.001f;
    ray.direction = sunDirection;
    sceneIntersectData sunIntersect;
    if(dot(sunDirection, normal) > 0.0){
        sunIntersect = castRayThroughScene(ray);
    }
    vec3 sunIntersectData = boxRayIntersect(sunIntersect.foundBox, ray);
    Voxel hitVoxelData;
    ivec3 voxelGridPos;

    hitVoxelData = fetchVoxelData(intersectData.foundBox, intersectData.headerIndex);
    color *= vec4(hitVoxelData.color, 0);
    //color *= vec4(1, 0.8, 0.8, 0);
    if(sunIntersectData.x < 0.0f && dot(sunDirection, normal) > 0.0){
        return color * (dot(sunDirection, normal)/2+0.5);
    } else {
        return color * (dot(normal, vec3(0, 1, 0))/2+0.5)/2;
    }
    return color;
}

vec4 castRay(){
    vec4 color = vec4(0.0);

    // Define the number of anti-aliasing points (must be a perfect square)
    const int AA_POINTS = 1;
    const int AA_DIM = int(sqrt(float(AA_POINTS)));

    // Accumulate colors from each offset
    for(int y = 0; y < AA_DIM; y++){
        for(int x = 0; x < AA_DIM; x++){
            vec2 offset = vec2(
                ((float(x) / float(AA_DIM) - 0.5) / float(AA_DIM)) * 2.0,
                ((float(y) / float(AA_DIM) - 0.5) / float(AA_DIM)) * 2.0
            );
            color += castRayWithOffset(offset / resolution);
        }
    }

    // Average the accumulated colors
    return color / float(AA_POINTS);
}

void main(){
    FragColor = castRay().rgb;
}

