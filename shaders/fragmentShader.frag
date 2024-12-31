//Last edited on: 31-12-2024
// Data structures explained in (docs/data_structures/octree_data_structure.md) and (docs/data_structures/voxel_type_data_structure.md).
// The algorithm this shader uses is depicted in the Nvidia paper in (docs/SOURCES.md).

/*
This shader is responsible for casting rays through the octree and rendering the scene. It uses an effecient heirarchical ray marching algorithm that is similar to the DDA algorithm used for effecient voxel traversal,
but with modifications to allow for use with sparse data structures. The algorithm is relatively fast and memory effecient, allowing for real-time rendering of large voxel scenes. Customize what is rendered by changing the code in main.
Ideally implement a more user friendly approach in the future.
*/

#version 460 core
out vec4 FragColor;
in vec2 TexCoords;
uniform float time;
uniform vec2 resolution;
uniform vec3 cameraPos;
uniform vec3 cameraDir;

#define MAX_STACK_SIZE 13
#define MAX_RAY_STEPS 800
#define RENDER_MODE 1 //1-8
#define OCTREE_HEADER_SIZE 0
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

//Access the boxes buffer for raw rendering (Temporary just for debugging)
layout(std430, binding = 0) buffer BoxesBuffer {
    BoxAABB boxes[];
};

//Access the octree buffer (Temporary, not final data structure layout)
layout(std430, binding = 1) buffer OctreeBuffer {
    uint octree[];
};

layout(std430, binding = 2) buffer VoxelTypeDataBuffer {
    uint voxelTypeData[];
};

//Do not need a stack for the boxes, just store 2 instead
BoxAABB masterParentBox;
BoxAABB masterChildBox;
//Global constants that change between frames
vec3 inverseRayDirection;
//The arrays that will act like stacks that are used to march the ray through the sparse voxel octrees (Ideally allocate once but that isn't yet implemented)
uint nodeIndexStack[MAX_STACK_SIZE];
uint octant1Stack[MAX_STACK_SIZE];
//The amount of items in each array (Allows for stack-like functionality)
int nodeIndexStackQuantity = 0;
int octant1StackQuantity = 0;

//Given an intersectedSide and whether or not the normal is positive, return a vector pointing in the normal direction.
vec3 calculateAABBNormal(float intersectedSide, float isNormalPositive){
    vec3 normal = vec3(0, 0, 0);
    if(isNormalPositive == 1){
        if(intersectedSide == 1){
            normal = vec3(1, 0, 0);
        } else
        if(intersectedSide == 2){
            normal = vec3(0, 1, 0);
        } else
        if(intersectedSide == 3){
            normal = vec3(0, 0, 1);
        } 
    } else{
        if(intersectedSide == 1){
            normal = vec3(-1, 0, 0);
        } else
        if(intersectedSide == 2){
            normal = vec3(0, -1, 0);
        } else
        if(intersectedSide == 3){
            normal = vec3(0, 0, -1);
        }        
    }
    return normal;
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

//Returns a random float.
float randomFloat0to1(vec2 uv, float timeX) {
    vec3 coords = vec3(uv * timeX, timeX);

    coords = fract(coords * vec3(443.8975, 441.4234, 437.195));
    coords += dot(coords, coords.yzx + 19.19);

    float n = dot(coords, vec3(12.9898, 78.233, 45.164));
    return fract(sin(n) * 43758.5453123);
}

//Calculate the starting ray direction for the inital ray trace
vec3 rayStartDirection(vec2 uv, vec2 res, vec3 cameraPosition, vec3 cameraDirection, float fov){
    float aspectRatio = res.x / res.y;
    float scale = tan(radians(fov * 0.5));
    vec2 pixelNDC = vec2(
        ((-uv.x) * scale) * aspectRatio,
        uv.y * scale
    );
    vec3 targetPosition = cameraPosition + cameraDirection;
    vec3 lookat = normalize(targetPosition - cameraPosition);
    vec3 right = cross(lookat, vec3(0, 1, 0));
    vec3 actualUp = cross(right, lookat);
    vec3 rayDirection = normalize(pixelNDC.x * right + pixelNDC.y * actualUp + lookat);
    return rayDirection;
}

//Pushes the indexies of a node in the octree buffer into it's stack.
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

//Moves an octant to its adjacent octant
uint moveOctant1(uint octant1, uint sideToMoveTo) {
    uint newOctant1 = 0;

    if(sideToMoveTo == 1) {
        return octant1 ^ (1 << 2);
    } else if(sideToMoveTo == 2) {     
        return octant1 ^ (1 << 1);
    } else if(sideToMoveTo == 3) {
        return octant1 ^ 1;
    }
    
    return newOctant1;
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

//Calculates the distance to the exit point of a box and the side it exits it from.
vec2 boxRayIntersectInverted(BoxAABB box, Ray castedRay) {
    //BUG: Occasionally will advance in the wrong direction resulting in holes in the rendered image.
    vec3 rayInverse = 1.0/castedRay.direction;
    highp vec3 t0 = (box.position - castedRay.origin) * rayInverse;
    highp vec3 t1 = (box.position + box.size - castedRay.origin) * rayInverse;

    highp vec3 tsmaller = min(t0, t1);
    highp vec3 tbigger = max(t0, t1);

    highp float tmin_candidate = max(max(tsmaller.x, tsmaller.y), tsmaller.z);
    highp float tmax_candidate = min(min(tbigger.x, tbigger.y), tbigger.z);

    uint exitSide = 3u; //Default to Z-axis.
    if (tmax_candidate == tbigger.x) {
        exitSide = 1u; //X-axis.
    } else if (tmax_candidate == tbigger.y) {
        exitSide = 2u; //Y-axis.
    }

    //Check if the ray origin is inside the box.
    bool isInside = all(greaterThanEqual(castedRay.origin, box.position)) &&
                    all(lessThanEqual(castedRay.origin, box.position + box.size));

    //Adjust tmin_candidate if the ray starts inside the box.
    if (isInside) {
        tmin_candidate = 0.0;
    }

    if (tmin_candidate > tmax_candidate || tmax_candidate < 0.0) {
        return vec2(-1.0, float(exitSide));
    }

    //Return tmax_candidate and exitSide.
    return vec2(tmax_candidate, float(exitSide));
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
    vec3 intersectData = boxRayIntersect(box, ray);
    vec3 intersectPosition = ray.origin+ray.direction*intersectData[0];
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
    uint checkOctant1 = octant1Stack[octant1StackQuantity-1];
    uint lastNodeIndex = nodeIndexStack[nodeIndexStackQuantity-1];
    uint lastNodeData = octree[lastNodeIndex];

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
        uint newNodeAddress = calculateSiblingsBeforeThisOctant1(lastNodeData, checkOctant1) + lastNodeDataChildPointer + lastNodeIndex + OCTREE_HEADER_SIZE;
        pushNodeIndexStack(newNodeAddress);
        masterParentBox = masterChildBox;

        pushOctant1Stack(newOctant1);
        masterChildBox = newChildFromParentAndOctant1(masterChildBox, octant1Stack[octant1StackQuantity-1]);

        return 1; //continueMarching = true, successfullyPushed = true. Needs to push.
    }

    return 2; //continueMarching = true, successfullyPushed = false. Needs to advance.
}

//Moves the ray into its adjacent sibling based off of which side it leaves the current child. Detects if a leaf is found or if the current node is a parent.
//Returns 0, 1, or 2
//0-> continueMarching = false, leaveChild = false
//1-> cotinueMarching = true, leaveChild = false
//2-> continueMarching = true, leaveChild = true 
uint advance(Ray ray) {
    uint parentNodeData = octree[nodeIndexStack[nodeIndexStackQuantity - 1u]];
    uint parentValidMask = (parentNodeData >> 1u) & 0x000000FF;
    uint leafMask = parentNodeData & 0x1;
    uint lastOctant1 = octant1Stack[octant1StackQuantity - 1u];
    vec2 parentIntersectData = boxRayIntersectInverted(masterParentBox, ray);
    //Loop over each possible octant.
    for (uint i = 0u; i < 4u; i++) {
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
                octant1Stack[octant1StackQuantity - 1u] = lastOctant1;
                return 0u; //Found a leaf node. Needs to end.
            }
        } else {
            if((parentValidMask & (1u << (7u - lastOctant1))) > 0u){
                octant1Stack[octant1StackQuantity - 1u] = lastOctant1;
                return 1u; //cotinueMarching = true, leaveChild = false. Needs to push
            }
        }             
    }
    octant1Stack[octant1StackQuantity - 1u] = lastOctant1;
    return 1u; //cotinueMarching = true, leaveChild = false. Continue traversal
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

//Main function for casting a ray through an octree.
/*
Basic overview of the loop:
1. Exit if we are not supposed to continue
2. Push one level into the octree
3. If it didn't push then advance
4. If we have to go back up(tried to advance into a different parents child), then continously pop until we can succesfully advance again
This loop repeats MAX_RAY_STEPS number of times until either a leaf voxel is found, we leave the tree, or we reach the maximum number of ray steps.
*/
BoxAABB marchRayThroughOctree(Ray ray, BoxAABB boundingBox, vec3 uvInBoundingBox) {
    int LOD = MAX_STACK_SIZE-8;
    
    int rayStepLimit = MAX_RAY_STEPS;

    //Avoid even ray origins to avoid the rays starting directly on the boundary of 2 nodes.
    if(fract(ray.origin.x * 0.5) == 0.0) ray.origin.x += 0.00001;
    if(fract(ray.origin.y * 0.5) == 0.0) ray.origin.y += 0.00001;
    if(fract(ray.origin.z * 0.5) == 0.0) ray.origin.z += 0.00001;

    //Set flags
    bool continueMarching = true;
    bool successfullyPushed = false;
    bool leaveChild = false;
    bool rayInsideMasterChild = (boxRayIntersect(masterChildBox, ray).x == 0);

    //Reset the stacks
    nodeIndexStackQuantity = 0;
    octant1StackQuantity = 0;

    //Perform the initial push.
    uint firstChildOctant1 = calculateOctant1BasedOfEdgePos(uvInBoundingBox);
    masterChildBox = newChildFromParentAndOctant1(boundingBox, firstChildOctant1);
    masterParentBox = boundingBox;
    pushNodeIndexStack(OCTREE_HEADER_SIZE);
    pushOctant1Stack(firstChildOctant1); 
    
    //Perform as many pushes as possible after initial push.
    while(octant1StackQuantity < MAX_STACK_SIZE-1){
        splitPushCase(push(ray), continueMarching, successfullyPushed);
        if(!successfullyPushed || !continueMarching){
            break;
        }
    }

    //Main ray marching loop.
    int raySteps = 0;
    for(int rayStep = 0; rayStep < rayStepLimit; rayStep++){
        //Break if break conditions are met.
        if(!continueMarching && !rayInsideMasterChild){
            break;                
        }
        //Push as far as possible
        while(octant1StackQuantity < MAX_STACK_SIZE-1){
            splitPushCase(push(ray), continueMarching, successfullyPushed);
            if(!successfullyPushed || !continueMarching){
                break;
            }
        }
        //Once push is no longer possible, advance.
        if(!successfullyPushed){
            //Gather advance data.
            splitAdvanceCase(advance(ray), continueMarching, leaveChild);
            //If it needs to pop, pop until it can ssuccesffuly advance or needs to exit the octree.
            while(leaveChild == true){
                //Exit the octree if the ray leaves the octree.
                if(octant1StackQuantity < 2){
                    BoxAABB falseBox;
                    falseBox.size = -1.0;
                    return falseBox;
                }
                //If ray doesn't exit octree, continue to pop and repeat.
                masterChildBox = masterParentBox;
                masterParentBox = newParentFromChildAndOctant1(masterChildBox, octant1Stack[octant1StackQuantity-2]);
                octant1StackQuantity -= 1;
                nodeIndexStackQuantity -= 1;
                splitAdvanceCase(advance(ray), continueMarching, leaveChild); 
            }
        }
        raySteps += 1;
    }
    return masterChildBox;     
}

//Cast the ray through the octree bounding box.
BoxAABB castRayThroughOctree(Ray ray, BoxAABB octreeBoundingBox) {
    vec3 octreeBoundingBoxIntersectData = boxRayIntersect(octreeBoundingBox, ray);
    float tBB = octreeBoundingBoxIntersectData[0];
    vec3 uvInBoundingBox;
    vec3 intersectionPoint = ray.origin + ray.direction * tBB;
    uvInBoundingBox = abs((octreeBoundingBox.position - intersectionPoint)) / octreeBoundingBox.size;
    
    BoxAABB octreeMarchData;
    octreeMarchData.size = -1;
    if(tBB >= 0){
        octreeMarchData = marchRayThroughOctree(ray, octreeBoundingBox, uvInBoundingBox);
    }
    return octreeMarchData;
}

//Given the integer postions inside of the voxel grid, find the type data for that voxel.
int findVoxelTypeDataIndex(int x, int y, int z, uint voxelGridResolution) {
    uint ZOrder = calculateZOrderIndex(x, y, z, voxelGridResolution);
    int beginningIndex = 0;
    int endIndex = voxelTypeData.length()/VOXEL_TYPEDATA_SLICES;
    int middleIndex = (beginningIndex+endIndex)/VOXEL_TYPEDATA_SLICES;
    for(int i = 0; i < 100; i++){
        if(voxelTypeData[middleIndex*VOXEL_TYPEDATA_SLICES] == ZOrder){
            return middleIndex*VOXEL_TYPEDATA_SLICES;
        } else {
            if(voxelTypeData[middleIndex * VOXEL_TYPEDATA_SLICES] < ZOrder){
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

//Given the voxel index, return the voxel data.
Voxel fetchVoxelData(uint voxelIndex){
    //Assume 6-bit color channels ()
    //Assume 4-bit normal vector channels ()
    Voxel voxel;
    uint SerializedColor = voxelTypeData[voxelIndex+1];
    uint SerializedNormals = voxelTypeData[voxelIndex+2];
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


vec3 cosineWeightedRandomDirection(vec3 normal, vec2 randomSeed) {
    // Generate two random numbers for sampling the disk
    float r1 = randomFloat0to1(TexCoords, randomSeed.x);
    float r2 = randomFloat0to1(TexCoords, randomSeed.y);
    
    // Convert to polar coordinates for disk sampling
    float theta = 2.0 * 3.14159265 * r1;
    float radius = sqrt(r2);
    
    // Calculate tangent and bitangent vectors orthogonal to the normal
    vec3 tangent = normalize(cross(normal, abs(normal.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 bitangent = cross(normal, tangent);
    
    // Sample a point on the disk for cosine-weighted hemisphere sampling
    vec3 sampledPoint = tangent * cos(theta) * radius + bitangent * sin(theta) * radius;
    
    // Final direction, slightly biased toward the normal based on the cosine weight
    vec3 cosineWeightedDirection = normalize(sampledPoint + normal * sqrt(1.0 - r2));
    
    return cosineWeightedDirection;
}

//Setup the ray and cast it into our scene and return the results from the cast as colors.
vec4 castRay(){
    //return vec4(1, 0, 0, 0);
    Ray ray;
    ray.direction = rayStartDirection(TexCoords, resolution, cameraPos, normalize(vec3(cameraDir.x, 0, cameraDir.z)), 100);
    ray.origin = cameraPos;

    //Define the root box for the octree
    BoxAABB octreeBoundingBox;
    octreeBoundingBox.position = vec3(20.0, 0.0, 0.0);
    octreeBoundingBox.size = 20.0;
    vec4 color = vec4(1);

    vec3 sunDirection = normalize(vec3(cos(float(time*10)), sin(float(time*10)), 2));
    if (RENDER_MODE == 1){
        BoxAABB firstBox = castRayThroughOctree(ray, octreeBoundingBox);
        vec3 firstIntersectData = boxRayIntersect(firstBox, ray);
        //return vec4(ray.direction.x, ray.direction.y, ray.direction.z, 1);
        if(firstIntersectData.x < 0.0f){
            return vec4(0.3, 0.5, 0.9, 1.0);
        }
        
        ray.origin = ray.origin + ray.direction*(firstIntersectData.x*0.999f);
        ray.direction = sunDirection;
        BoxAABB sunBox = castRayThroughOctree(ray, octreeBoundingBox);
        vec3 sunIntersectData = boxRayIntersect(sunBox, ray);
        vec3 normal = calculateAABBNormal(firstIntersectData.y, firstIntersectData.z);
        
        Voxel hitVoxelData;
        ivec3 voxelGridPos;
        voxelGridPos = voxelGridPosition(firstBox, octreeBoundingBox, 512);
        uint hitVoxelIndex = findVoxelTypeDataIndex(voxelGridPos.x, voxelGridPos.y, voxelGridPos.z, 512);
        hitVoxelData = fetchVoxelData(hitVoxelIndex);
        //color *= vec4(hitVoxelData.color, 0);
        if(sunIntersectData.x < 0.0f){
            return color * (dot(sunDirection, normal)/2+0.5f);
        } else {
            return color*0.5f*((dot(sunDirection, normal)+1)/1.6);
        }
    }
    return vec4(color);
}

void main(){
    FragColor = castRay();
}

