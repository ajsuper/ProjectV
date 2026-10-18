$input v_color0
$input v_texcoord0

// =============================================================================
// hud.frag -- the siege game's HUD, a pass of its own after the shared renderer's
// display pass (examples/shared/voxelRenderer): the finished frame, and over it
// a health bar, the three item slots with their cooldowns, both flags, the
// clock, a banner, a hint, a crosshair, and where a cannonball will land.
//
// There is no text renderer in the engine, so text is a 3x5 dot font drawn here.
// The program packs each line into two vec4s: 24 characters, three to a float,
// six bits each (packText in main.cpp). Codes: 0 space, 1-10 digits, 11-36 A-Z,
// then : ! - / .
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(frame, 0);

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 cameraDir;
uniform vec4 hudState;        // health 0..1, selected item (0..2), mounted on a cannon, hurt flash 0..1
uniform vec4 hudCooldown;     // what is left of each cooldown, 0..1: bullet, block, blast, cannon
uniform vec4 hudFlags;        // blue flag up, red flag up, your team (0 blue, 1 red), alive
uniform vec4 hudMarker;       // where a cannonball would land; w 1 when shown
uniform vec4 hudBannerColour; // the banner's colour; w 0 hides it
uniform vec4 hudBanner0;
uniform vec4 hudBanner1;
uniform vec4 hudTop0;
uniform vec4 hudTop1;
uniform vec4 hudHint0;
uniform vec4 hudHint1;

#define FOV 60.0   // the shared renderer's (grr_common.sc's GRR_FOV)

// The view ray through `uv`, as the shared renderer makes it.
vec3 viewRay(vec2 uv, vec2 res) {
    vec3 forward = normalize(cameraDir.xyz);
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(forward, worldUp));
    vec3 up = normalize(cross(right, forward));
    vec2 ndc = vec2(uv.x, 1.0 - uv.y) * 2.0 - 1.0;
    float scale = tan(radians(FOV * 0.5));
    return normalize(right * (ndc.x * scale * res.x / res.y) + up * (ndc.y * scale) + forward);
}

// ---- The dot font -------------------------------------------------------------------------------

// A character's 3x5 dots, rows top to bottom, three bits a row, the left dot the high bit.
float glyph(float c) {
    if (c < 0.5) return 0.0;
    if (c < 1.5) return 31599.0;   // 0
    if (c < 2.5) return 11415.0;   // 1
    if (c < 3.5) return 29671.0;   // 2
    if (c < 4.5) return 29647.0;   // 3
    if (c < 5.5) return 23497.0;   // 4
    if (c < 6.5) return 31183.0;   // 5
    if (c < 7.5) return 31215.0;   // 6
    if (c < 8.5) return 29257.0;   // 7
    if (c < 9.5) return 31727.0;   // 8
    if (c < 10.5) return 31695.0;  // 9
    if (c < 11.5) return 11245.0;  // A
    if (c < 12.5) return 27566.0;  // B
    if (c < 13.5) return 14627.0;  // C
    if (c < 14.5) return 27502.0;  // D
    if (c < 15.5) return 31143.0;  // E
    if (c < 16.5) return 31140.0;  // F
    if (c < 17.5) return 14699.0;  // G
    if (c < 18.5) return 23533.0;  // H
    if (c < 19.5) return 29847.0;  // I
    if (c < 20.5) return 4714.0;   // J
    if (c < 21.5) return 23469.0;  // K
    if (c < 22.5) return 18727.0;  // L
    if (c < 23.5) return 24557.0;  // M
    if (c < 24.5) return 27501.0;  // N
    if (c < 25.5) return 11114.0;  // O
    if (c < 26.5) return 27556.0;  // P
    if (c < 27.5) return 11123.0;  // Q
    if (c < 28.5) return 27565.0;  // R
    if (c < 29.5) return 14478.0;  // S
    if (c < 30.5) return 29842.0;  // T
    if (c < 31.5) return 23407.0;  // U
    if (c < 32.5) return 23402.0;  // V
    if (c < 33.5) return 23549.0;  // W
    if (c < 34.5) return 23213.0;  // X
    if (c < 35.5) return 23186.0;  // Y
    if (c < 36.5) return 29351.0;  // Z
    if (c < 37.5) return 1040.0;   // :
    if (c < 38.5) return 9346.0;   // !
    if (c < 39.5) return 448.0;    // -
    if (c < 40.5) return 4772.0;   // /
    return 2.0;                    // .
}

// Bit `bit` of a whole number held in a float. The half keeps the division clear of rounding.
float bitOf(float value, float bit) {
    return mod(floor((value + 0.5) / exp2(bit)), 2.0);
}

// Whether `pixel` is on a dot of one character whose top-left corner is `corner`, dots `size` px.
float character(vec2 pixel, vec2 corner, float size, float code) {
    vec2 cell = floor((pixel - corner) / size);
    if (cell.x < 0.0 || cell.y < 0.0 || cell.x > 2.0 || cell.y > 4.0) return 0.0;
    return bitOf(glyph(code), (4.0 - cell.y) * 3.0 + (2.0 - cell.x));
}

// Whether `pixel` is on a dot of a 24-character line packed in `a` and `b`, centred on `centre`.
float textLine(vec2 pixel, vec2 centre, float size, vec4 a, vec4 b) {
    vec2 corner = centre - vec2(48.0 * size, 2.5 * size);   // 24 characters, 4 dots each
    vec2 cell = floor((pixel - corner) / size);
    if (cell.x < 0.0 || cell.y < 0.0 || cell.x >= 96.0 || cell.y > 4.0) return 0.0;
    float index = floor(cell.x / 4.0);
    float column = cell.x - index * 4.0;
    if (column > 2.5) return 0.0;
    float slot = floor(index / 3.0);
    float within = index - slot * 3.0;
    vec4 packed4 = slot < 3.5 ? a : b;
    float which = slot < 3.5 ? slot : slot - 4.0;
    float packed = dot(packed4, vec4(which < 0.5 ? 1.0 : 0.0, abs(which - 1.0) < 0.5 ? 1.0 : 0.0,
                                     abs(which - 2.0) < 0.5 ? 1.0 : 0.0, which > 2.5 ? 1.0 : 0.0));
    float code = mod(floor((packed + 0.5) / exp2(6.0 * within)), 64.0);
    return bitOf(glyph(code), (4.0 - cell.y) * 3.0 + (2.0 - column));
}

// A line with a drop shadow, over `colour`.
vec3 drawText(vec3 colour, vec2 pixel, vec2 centre, float size, vec4 a, vec4 b, vec3 ink) {
    float shadow = textLine(pixel - vec2(size * 0.5, size * 0.5), centre, size, a, b);
    colour = mix(colour, vec3(0.0, 0.0, 0.0), shadow * 0.7);
    return mix(colour, ink, textLine(pixel, centre, size, a, b));
}

float inBox(vec2 pixel, vec2 lo, vec2 hi) {
    return (pixel.x >= lo.x && pixel.y >= lo.y && pixel.x < hi.x && pixel.y < hi.y) ? 1.0 : 0.0;
}

// A flag: a pole and a cloth, at `corner`, lit if it stands and grey and struck through if not.
vec3 drawFlag(vec3 colour, vec2 pixel, vec2 corner, vec3 cloth, float up) {
    vec3 tint = up > 0.5 ? cloth : vec3(0.35, 0.35, 0.35);
    colour = mix(colour, vec3(0.15, 0.15, 0.15), inBox(pixel, corner, corner + vec2(4.0, 40.0)));
    colour = mix(colour, tint, inBox(pixel, corner + vec2(4.0, 2.0), corner + vec2(30.0, 20.0)));
    if (up < 0.5) {
        vec2 p = pixel - corner;
        if (p.x > -2.0 && p.x < 34.0 && p.y > -2.0 && p.y < 42.0 && abs(p.y - p.x * 1.2) < 2.5) colour = vec3(0.9, 0.1, 0.1);
    }
    return colour;
}

void main() {
    vec3 colour = texture2D(frame, v_texcoord0).rgb;
    vec2 res = windowRes.xy;
    vec2 pixel = v_texcoord0 * res;
    bool alive = hudFlags.w > 0.5;
    vec3 blue = vec3(0.25, 0.45, 1.0);
    vec3 red = vec3(0.95, 0.22, 0.18);

    // Where the cannonball would land: a ring on the spot, seen through anything.
    if (hudMarker.w > 0.5) {
        vec3 direction = viewRay(v_texcoord0, res);
        vec3 toMarker = hudMarker.xyz - cameraPos.xyz;
        float along = dot(toMarker, direction);
        if (along > 0.0) {
            float off = length(toMarker - direction * along);
            float width = max(0.06, along * 0.003);
            if (abs(off - 1.3) < width) colour = mix(colour, vec3(1.0, 0.55, 0.1), 0.9);
            if (off < width * 1.5) colour = vec3(1.0, 0.8, 0.2);
        }
    }

    // Hurt: the edges go red.
    if (hudState.w > 0.0) {
        vec2 uv = v_texcoord0 - 0.5;
        float edge = smoothstep(0.25, 0.75, length(uv) * 1.4);
        colour = mix(colour, vec3(0.8, 0.0, 0.0), edge * hudState.w * 0.8);
    }

    // The crosshair: two short bars through the centre, inverted. Not while dead.
    vec2 offset = abs(pixel - res * 0.5);
    bool bar = (offset.x < 1.0 && offset.y < 9.0 && offset.y > 3.0) || (offset.y < 1.0 && offset.x < 9.0 && offset.x > 3.0);
    if (bar && alive) colour = vec3(1.0, 1.0, 1.0) - colour * 0.8;

    // Flags, top left (blue) and top right (red), and the clock and head counts between them.
    colour = drawFlag(colour, pixel, vec2(20.0, 16.0), blue, hudFlags.x);
    colour = drawFlag(colour, pixel, vec2(res.x - 54.0, 16.0), red, hudFlags.y);
    colour = drawText(colour, pixel, vec2(res.x * 0.5, 34.0), 4.0, hudTop0, hudTop1, vec3(1.0, 1.0, 1.0));

    // The banner and the hint.
    if (hudBannerColour.w > 0.5) {
        float size = max(6.0, floor(res.y / 110.0));
        colour = drawText(colour, pixel, vec2(res.x * 0.5, res.y * 0.3), size, hudBanner0, hudBanner1, hudBannerColour.rgb);
    }
    colour = drawText(colour, pixel, vec2(res.x * 0.5, res.y * 0.5 + 48.0), 3.0, hudHint0, hudHint1, vec3(1.0, 0.92, 0.6));

    if (alive) {
        // Health, bottom left: the bar goes from green to red as it empties.
        vec2 barLo = vec2(24.0, res.y - 44.0);
        vec2 barHi = vec2(284.0, res.y - 22.0);
        colour = mix(colour, vec3(0.05, 0.05, 0.05), inBox(pixel, barLo - 3.0, barHi + 3.0) * 0.75);
        float fill = barLo.x + (barHi.x - barLo.x) * clamp(hudState.x, 0.0, 1.0);
        vec3 healthColour = mix(vec3(0.9, 0.15, 0.1), vec3(0.2, 0.85, 0.3), clamp(hudState.x * 1.4 - 0.2, 0.0, 1.0));
        colour = mix(colour, healthColour, inBox(pixel, barLo, vec2(fill, barHi.y)));

        if (hudState.z > 0.5) {
            // On a cannon: one bar, its reload.
            vec2 lo = vec2(res.x * 0.5 - 130.0, res.y - 44.0);
            vec2 hi = vec2(res.x * 0.5 + 130.0, res.y - 22.0);
            colour = mix(colour, vec3(0.05, 0.05, 0.05), inBox(pixel, lo - 3.0, hi + 3.0) * 0.75);
            float ready = 1.0 - clamp(hudCooldown.w, 0.0, 1.0);
            vec3 tint = ready > 0.999 ? vec3(1.0, 0.6, 0.15) : vec3(0.55, 0.4, 0.25);
            colour = mix(colour, tint, inBox(pixel, lo, vec2(lo.x + (hi.x - lo.x) * ready, hi.y)));
        } else {
            // The three items, bottom centre: the selected one outlined, a cooldown darkening it from
            // the top, its key in the corner.
            for (int i = 0; i < 3; i++) {
                float fi = float(i);
                vec2 lo = vec2(res.x * 0.5 - 98.0 + fi * 68.0, res.y - 76.0);
                vec2 hi = lo + vec2(60.0, 60.0);
                bool selected = abs(hudState.y - fi) < 0.5;
                float border = inBox(pixel, lo - 3.0, hi + 3.0) * (1.0 - inBox(pixel, lo, hi));
                colour = mix(colour, selected ? vec3(1.0, 0.85, 0.3) : vec3(0.35, 0.35, 0.35), border);
                colour = mix(colour, vec3(0.08, 0.08, 0.08), inBox(pixel, lo, hi) * 0.7);
                vec2 d = pixel - (lo + hi) * 0.5;
                if (i == 0) {   // a bullet
                    if (abs(d.x) < 4.0 && d.y > -12.0 && d.y < 12.0 && (d.y > -8.0 || length(vec2(d.x, d.y + 8.0)) < 4.0))
                        colour = vec3(0.95, 0.8, 0.3);
                } else if (i == 1) {   // a block
                    if (abs(d.x) < 13.0 && abs(d.y) < 13.0)
                        colour = (abs(d.x) > 10.0 || abs(d.y) > 10.0) ? vec3(0.35, 0.22, 0.12) : vec3(0.72, 0.55, 0.33);
                } else {   // a blast: a star
                    float angle = atan2(d.y, d.x);
                    float reach = 9.0 + 7.0 * abs(cos(angle * 4.0));
                    if (length(d) < reach) colour = mix(vec3(1.0, 0.85, 0.2), vec3(1.0, 0.35, 0.05), length(d) / reach);
                }
                float left = i == 0 ? hudCooldown.x : (i == 1 ? hudCooldown.y : hudCooldown.z);
                if (left > 0.0 && pixel.y < lo.y + 60.0 * left) colour = mix(colour, vec3(0.0, 0.0, 0.0), inBox(pixel, lo, hi) * 0.6);
                colour = mix(colour, vec3(1.0, 1.0, 1.0), character(pixel, lo + vec2(4.0, 4.0), 2.0, fi + 2.0));
            }
        }
    }

    gl_FragColor = vec4(colour, 1.0);
}
