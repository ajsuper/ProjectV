$input v_color0 
$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(resourceTexture, 0);
#include <pjv_utils_DDA.sc>

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 frameCount;
uniform vec4 cameraDir;

const float PI = 3.1415;

// Fresnel-Schlick approximation
float fresnel_schlick(float cos_theta, float F0) {
    return F0 + (1.0f - F0) * pow(1.0f - cos_theta, 5.0f);
}

// Beckmann microfacet distribution
float beckmann_distribution(float alpha, float cos_theta_h) {
    float tan_theta_h = sqrt(1.0f / (cos_theta_h * cos_theta_h) - 1.0f);
    float tan_theta_h2 = tan_theta_h * tan_theta_h;
    return exp(-tan_theta_h2 / (alpha * alpha)) / (alpha * alpha * cos_theta_h * cos_theta_h);
}

// Geometry term (Schlick-Grossman)
float geometry_term(float cos_theta_i, float cos_theta_o) {
    return min(1.0f, 2.0f * cos_theta_i * cos_theta_o / (cos_theta_i + cos_theta_o));
}

// Cook-Torrance specular BRDF
float cook_torrance_specular(vec3 wi, vec3 wo, vec3 normal, float F0, float alpha) {
    vec3 h = normalize((wi + wo)); // Half vector

    float cos_theta_i = dot(wi, normal);
    float cos_theta_o = dot(wo, normal);
    float cos_theta_h = dot(h, normal);

    // Compute microfacet distribution (Beckmann)
    float D = beckmann_distribution(alpha, cos_theta_h);

    // Compute Fresnel term
    float F = fresnel_schlick(cos_theta_h, F0);

    // Compute geometric attenuation
    float G = geometry_term(cos_theta_i, cos_theta_o);

    return (D * F * G) / (4.0f * cos_theta_i * cos_theta_o);
}

float diffuseBRDF() {
    return 1/PI;
}

struct GBuffer {
    vec4 albedo; // 4th value is depth.
    vec4 normal; // 4th value is ray steps.
};

float specularBRDF(vec3 incomingDirection, vec3 outgoingDirection, vec3 normal, float specularSmoothness, float specularChance) {
    vec3 perfectReflection = reflect(incomingDirection, normal);
    float cosineBetweenPerfectAndActual = dot(outgoingDirection, perfectReflection);
    float normalizationFactor = (specularSmoothness + 1)/(2*PI);
    return normalizationFactor * pow(cosineBetweenPerfectAndActual, specularSmoothness);
}

float diffusePDF(float cosBetweenSampleDirectionAndNormal, float probabilityOfThisSample){
    return cosBetweenSampleDirectionAndNormal/(PI) * probabilityOfThisSample;
}

returnStruct castRay(vec2 uv_coord, GBuffer gBuffer) {
    float specularChance = 0.4;
    float specularSmoothness = 6;
    float diffuseChance = 1.0;
    float mixtureValue = (specularChance)/(diffuseChance + specularChance); // Mixture sampling with respect to specular.
    Ray ray;
    ray.origin = cameraPos;
    ray.direction = rayStartDirection(
        uv_coord,
        windowRes.xy,
        cameraPos.xyz,
        normalize(vec3(cameraDir.x, 0.0, cameraDir.z)),
        60.0
    );

    uint bounces = 1;
    vec3 radiance = vec3(0.0);
    vec3 throughput = vec3(1.0);

    //vec3 sunDirection = normalize(vec3(1.4, 4.0, -1.5));
    vec3 sunDirection = normalize(vec3(-0.5, 0.9, 0.6));
    //vec3 sunDirection = normalize(vec3(sin(frameCount.x/100) * 5, cos(frameCount.x/100) * 5, -1));
    float sunSolidAngle = PI/1200;
    // sunSolidAngle: total solid angle of the sun in steradians
    float thetaMax = acos(1.0 - sunSolidAngle / (2.0 * PI));
    //vec3 sunRadiance = vec3(1.0, 0.4, 0.2) * 9000; // Assuming incoming radiance Li from the sky is uniform.
    vec3 sunRadiance = vec3(1.0, 0.8, 0.4) * 19000;

    vec3 color;
    float balenceHeuristicBRDF = 1;
    for (uint step = 0; step < bounces + 1; step++) {
        RayQuery intersectRQ;
        intersectRQ.maxRaySteps = 200;
        intersectRQ.startLOD = 0;
        intersectRQ.finishLOD = 2;
        intersectRQ.distanceToFinishLOD = 30;
        if (step > 0) {
            //intersectRQ.maxRaySteps = randomFloat0to1(vec2(uv_coord), frameCount.x % 200) * 3 + 5;
        }

        SceneIntersectData intersectHit = raySceneIntersect(ray, intersectRQ);
        IntersectionResult voxelIntersection = getRayBoxEntry(ray, intersectHit.foundBox);
        if (voxelIntersection.distance <= 0) { // Ray hits the sky.
            if (dot(sunDirection, ray.direction) >= cos(thetaMax)) {
                // Li(x, wo) = vec3(2.0, 1.9, 0.8) + (rest of equation equates to 0 for simplicity). Light coming from a point x at direction wo if we hit the sky is j
                radiance += balenceHeuristicBRDF * throughput * sunRadiance; // Hit sky
            } else {
                vec3 skyRadiance = vec3(0.3, 0.45, 0.9) * 9.0; // Assuming incoming radiance Li from the sky is uniform.
                radiance += throughput * skyRadiance; // Hit sky
            }
            break;
        }
        // Calculate the world space point that our ray intersects the scene. Do a small offset by the normal to account for imprecision in ray marching algorithm.
        vec3 intersectPoint = ray.origin + ray.direction * voxelIntersection.distance + voxelIntersection.normal * 0.01f;

        vec3 albedo = vec3(1);

        Voxel voxel = fetchVoxelData(intersectHit.foundBox, intersectHit.headerIndex);
        albedo = voxel.color; 
       //vec3 albedo = vec3(1.0, 0.8, 1.0);

        specularChance = 0.6;
        specularSmoothness = 21.0;
        diffuseChance = 1.0;
        mixtureValue = (specularChance)/(diffuseChance + specularChance);
        if (albedo.r == albedo.g && albedo.r == albedo.b || voxelIntersection.normal.y > 0) {
            specularChance = 0.6;
            specularSmoothness = 21.0;
            diffuseChance = 1.0;
            mixtureValue = (specularChance)/(diffuseChance + specularChance);
        }
        bool doSpecular = randomFloat0to1(uv_coord, sin(frameCount.x % 200 + (step + 1) * 2)) <= mixtureValue;

        vec2 uv_coord_offset = vec2(
            randomFloat0to1(vec2(1.0), frameCount.x % 200),
            randomFloat0to1(vec2(1.0), frameCount.x % 200) 
        );

        ivec2 pixel = ivec2((uv_coord + uv_coord_offset) * windowRes.xy * 1.0);

        ivec2 noiseCoord = (pixel + ivec2(frameCount.x, frameCount.x)) % 256;


        vec2 randoms = texelFetch(resourceTexture, noiseCoord, 0).rg;

        // Generate a random direction uniformly over the hemisphere.
        //vec2 randoms = vec2(
            //randomFloat0to1(uv_coord, sin(frameCount.x/20) + (step + 1) * 31),
            //randomFloat0to1(uv_coord, cos(frameCount.x/30) + (step + 1) * 19)

        //);
        float phi;
        float cosTheta;
        float sinTheta;
        vec3 N;

        if (!doSpecular) {
            phi = randoms[0] * 2 * PI; // [0, 2pi] (rotation around normal)
            cosTheta = sqrt(1.0 - randoms[1]); // Distribute based on the cosine of theta (distance from horizontal plane)
            sinTheta = sqrt(1 - cosTheta * cosTheta); 
            N = normalize(voxelIntersection.normal);
            // We should rotate it to be around the normal for diffuse.
        } else {
            phi = randoms[0] * 2 * PI;
            cosTheta = pow(randoms[1], 1/(specularSmoothness + 1));
            sinTheta = sqrt(1 - cosTheta * cosTheta);
            // We should rotate it to be around the mirror reflection for specular.
            N = normalize(reflect(ray.direction, voxelIntersection.normal));
        }

        vec3 hemisphereDir = vec3(
            cos(phi) * sinTheta,
            sin(phi) * sinTheta,
            cosTheta
        );

        vec3 T;
        if (abs(N.z) < 0.999) {
            T = normalize(cross(vec3(0.0, 0.0, 1.0), N));
        } else {
            T = normalize(cross(vec3(0.0, 1.0, 0.0), N));
        }

        vec3 B = cross(N, T);

        mat3 TBN = mat3(T, B, N);
        vec3 randomDirectionOverHemisphere = TBN * hemisphereDir;
        
        float cosThetaBetweenRandomDirAndNormal = dot(randomDirectionOverHemisphere, voxelIntersection.normal);
        if (cosThetaBetweenRandomDirAndNormal <= 0) {
            break;
        }

        float pdfBRDF;
        
        if (!doSpecular) {
            // cosine weighted PDF.
            pdfBRDF = max(0.001, diffusePDF(cosThetaBetweenRandomDirAndNormal, (1-mixtureValue)));
        } else {
            pdfBRDF = max(0.001, mixtureValue * (specularSmoothness + 1)/(2*PI) * pow(dot(reflect(ray.direction, voxelIntersection.normal), randomDirectionOverHemisphere), specularSmoothness));
        }
        // That is the same as this: just we multiply each term by it's sample count (either 0 or 1 in this case, so we just use if statement instead).
        //pdfBRDF = max(0.001, diffusePDF(cosThetaBetweenRandomDirAndNormal, (1-mixtureValue)) +  mixtureValue * (specularSmoothness + 1)/(2*PI) * pow(dot(reflect(ray.direction, voxelIntersection.normal), randomDirectionOverHemisphere), specularSmoothness));

        vec3 diffuseTerm = albedo * diffuseBRDF() * cosThetaBetweenRandomDirAndNormal;

        vec3 specularTerm =
                        albedo *
                        specularBRDF(ray.direction,
                        randomDirectionOverHemisphere,
                        voxelIntersection.normal,
                        specularSmoothness,
                        specularChance)
            * fresnel_schlick(cosThetaBetweenRandomDirAndNormal, 0.4);

        vec3 BRDFBRDF = diffuseTerm + max(0, specularTerm);

        ivec2 noiseCoordNEE = (pixel + ivec2(frameCount.x + 30, frameCount.x + 30)) % 256;

        vec2 randomsNEE = texelFetch(resourceTexture, noiseCoordNEE, 0).rg;
        float cosThetaNEE = mix(cos(thetaMax), 1.0, randomsNEE[0]); // u1 ∈ [0,1]
        float sinThetaNEE = sqrt(1.0 - cosThetaNEE*cosThetaNEE);
        float phiNEE = 2.0 * PI * randomsNEE[1];

        vec3 dirLocal = vec3(
            cos(phiNEE) * sinThetaNEE,
            sin(phiNEE) * sinThetaNEE,
            cosThetaNEE
        );

        vec3 TNEE, BNEE;
        vec3 NEE = normalize(sunDirection);

        if (abs(NEE.z) < 0.999) {
            TNEE = normalize(cross(vec3(0.0, 0.0, 1.0), NEE));
        } else {
            TNEE = normalize(cross(vec3(0.0, 1.0, 0.0), NEE));
        }
        TNEE = normalize(cross(vec3(0,1,0), sunDirection));
        BNEE = cross(sunDirection, TNEE);
        mat3 sunTBN = mat3(TNEE, BNEE, sunDirection);
        vec3 NEEDirection = sunTBN * dirLocal;
        //vec3 NEEDirection = normalize(sunDirection);
        float cosThetaSurface = dot(voxelIntersection.normal, NEEDirection);

        vec3 diffuseTermNEE = albedo * diffuseBRDF() * cosThetaSurface;
        vec3 specularTermNEE = max(0, albedo * specularBRDF(ray.direction, NEEDirection, voxelIntersection.normal, specularSmoothness, specularChance) * fresnel_schlick(cosThetaSurface, 0.4));
        vec3 NEEBRDF = diffuseTermNEE + specularTermNEE;
        float pdfNEE = 1.0 / sunSolidAngle;
        // This is what it should be if we sample each once:
        //float pdfBRDFforNEE = diffusePDF(cosThetaSurface, (1-mixtureValue)) + mixtureValue * (specularSmoothness + 1)/(2*PI) * pow(dot(reflect(ray.direction, voxelIntersection.normal), NEEDirection), specularSmoothness);
        float pdfBRDFforNEE;
        if (doSpecular) { // But since we only use 1 sample between these two, this essentially multiplies each one by either 1 or 0:
            pdfBRDFforNEE = max(0.00001, mixtureValue * (specularSmoothness + 1)/(2*PI) * pow(dot(reflect(ray.direction, voxelIntersection.normal), NEEDirection), specularSmoothness));
        } else {
            pdfBRDFforNEE = max(0.00001, diffusePDF(cosThetaSurface, (1-mixtureValue)));
        }

        Ray NEERay;
        NEERay.origin = intersectPoint + voxelIntersection.normal * 0.0001f;
        NEERay.direction = NEEDirection;
        SceneIntersectData NEEIntersect = raySceneIntersect(NEERay, intersectRQ);
        IntersectionResult NEEVoxelIntersection = getRayBoxEntry(NEERay, NEEIntersect.foundBox);
        float balenceHeuristicNEE = pow(pdfNEE, 2) / (pdfNEE*pdfNEE + pdfBRDFforNEE*pdfBRDFforNEE);
        if (NEEIntersect.foundBox.size < 0 && dot(NEEDirection, voxelIntersection.normal) > 0) {
            radiance += max(0, balenceHeuristicNEE * sunRadiance * (throughput * NEEBRDF)/pdfNEE);
        }
        balenceHeuristicBRDF = pow(pdfBRDF, 2) / (pdfNEE*pdfNEE + pdfBRDF*pdfBRDF);
        throughput *= max(0, BRDFBRDF / pdfBRDF);

        //throughput *= brdf;
        // simplifies to throughput *= brdf * PI; for cosine weighted.
        
        ray.origin = intersectPoint + voxelIntersection.normal * 0.0001f;
        ray.direction = randomDirectionOverHemisphere;
    }
    returnStruct returnValues;
    returnValues.globalIllumination = vec4(radiance, 1.0);
    return returnValues;
}

GBuffer renderGBuffer(vec2 uv_coord) {
    GBuffer gBuffer;

    // Create ray.
    Ray ray;
    ray.origin = cameraPos;
    ray.direction = rayStartDirection(
        uv_coord,
        windowRes.xy,
        cameraPos.xyz,
        normalize(vec3(cameraDir.x, 0.0, cameraDir.z)),
        60.0
    );

    // Define ray query parameters.
    RayQuery intersectRQ;
    intersectRQ.maxRaySteps = 100;
    intersectRQ.startLOD = 0;
    intersectRQ.finishLOD = 0;
    intersectRQ.distanceToFinishLOD = 100;

    SceneIntersectData intersectHit = raySceneIntersect(ray, intersectRQ);
    IntersectionResult voxelIntersection = getRayBoxEntry(ray, intersectHit.foundBox);
    if (voxelIntersection.distance <= 0) { // Ray hits the sky.
        gBuffer.albedo = vec4(1, 1, 1, voxelIntersection.distance);
        gBuffer.normal = vec4(0, 0, 0, intersectHit.steps);
        return gBuffer;
    }
    // Calculate the world space point that our ray intersects the scene. Do a small offset by the normal to account for imprecision in ray marching algorithm.
    vec3 intersectPoint = ray.origin + ray.direction * voxelIntersection.distance + voxelIntersection.normal * 0.001f;

    Voxel voxel = fetchVoxelData(intersectHit.foundBox, intersectHit.headerIndex);
    vec3 albedo = voxel.color;
    //vec3 albedo = vec3(1);

    gBuffer.albedo = vec4(albedo, voxelIntersection.distance);
    gBuffer.normal = vec4(voxelIntersection.normal.rgb, 0.0);
    return gBuffer;
}

void main(){
    vec2 uv_coord = (v_texcoord0);
    returnStruct returnValues;
    returnValues.globalIllumination = vec4(0);
    returnValues.directIllumination = vec4(0);
    returnValues.albedo = vec4(0);
    returnValues.normal = vec4(0);

    GBuffer gBuffer = renderGBuffer(uv_coord);
    ivec2 pixel = ivec2(uv_coord * windowRes.xy * 1.0);

    ivec2 noiseCoord = (pixel + ivec2(frameCount.x - 40, frameCount.x - 40)) % 256;

    noiseCoord = noiseCoord % 256;

    vec2 randoms = texelFetch(resourceTexture, noiseCoord, 0).rg;

    returnValues = castRay(uv_coord + randoms * 0.0008, gBuffer);
    gl_FragData[0] = vec4(returnValues.globalIllumination.rgb, 1.0);
    gl_FragData[1] = vec4(returnValues.directIllumination.rgb, 1.0);
    gl_FragData[2] = vec4((gBuffer.normal.rgb + 1) / 2, 1.0);
    gl_FragData[3] = vec4(gBuffer.albedo.rgb, 1.0);
}
