$input v_color0
$input v_texcoord0

// =============================================================================
// hud.frag -- the party's HUD, a pass of its own after the shared renderer's
// display pass (examples/shared/voxelRenderer): the finished frame, and over it
// the game and the clock, everyone's points beside their colour, a banner, the
// one rule of the game being introduced, the bomb's fuse, how soon you can shove
// and throw again, your blocks, where a bomb you are winding up will land, and in
// first person a crosshair and the cell a cube would go in.
//
// There is no text renderer in the engine, so text is a 3x5 dot font drawn here.
// The program packs each line into two vec4s: 24 characters, three to a float,
// six bits each (packText in party.h). Codes: 0 space, 1-10 digits, 11-36 A-Z,
// then : ! - / . > +
// =============================================================================

#include <bgfx_shader.sh>

SAMPLER2D(frame, 0);

uniform vec4 windowRes;
uniform vec4 cameraPos;
uniform vec4 cameraDir;
uniform vec4 hudTop0;
uniform vec4 hudTop1;
uniform vec4 hudBanner0;
uniform vec4 hudBanner1;
uniform vec4 hudBannerColour; // the banner's colour; w 0 hides it
uniform vec4 hudHint0;
uniform vec4 hudHint1;
uniform vec4 hudScore0;       // the scoreboard: four lines, best first
uniform vec4 hudScore1;
uniform vec4 hudScore2;
uniform vec4 hudScore3;
uniform vec4 hudScore4;
uniform vec4 hudScore5;
uniform vec4 hudScore6;
uniform vec4 hudScore7;
uniform vec4 hudScoreSlots;   // each line's player (its colour), -1 for none
uniform vec4 hudMe;           // x your line on the scoreboard (-1 none), y alive, z shoved flash 0..1
uniform vec4 hudBars;         // x the fuse left 0..1 (-1 no bomb), y 1 if it is yours, z shove cooldown, w throw cooldown (-1 none)
uniform vec4 hudMarker;       // where a bomb thrown now would land; w 1 when shown
uniform vec4 hudInfo0;        // a line under the bars: your blocks
uniform vec4 hudInfo1;
uniform vec4 hudCharge;       // x how wound up the throw is, 0..1 (-1 not)
uniform vec4 hudAim;          // first person: where a cube would go, w 1; w -1 just the crosshair; 0 neither

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

// The players' colours (arena.h's PLAYER_COLOURS).
vec3 playerColour(float slot) {
    if (slot < 0.5) return vec3(0.9, 0.22, 0.18);
    if (slot < 1.5) return vec3(0.2, 0.42, 0.95);
    if (slot < 2.5) return vec3(0.25, 0.8, 0.3);
    return vec3(0.98, 0.82, 0.15);
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
    if (c < 41.5) return 2.0;      // .
    if (c < 42.5) return 17492.0;  // >
    return 1488.0;                 // +
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

// A bar: a dark box, filled `fill` of the way in `ink`.
vec3 drawBar(vec3 colour, vec2 pixel, vec2 lo, vec2 hi, float fill, vec3 ink) {
    colour = mix(colour, vec3(0.04, 0.04, 0.05), inBox(pixel, lo - 3.0, hi + 3.0) * 0.75);
    return mix(colour, ink, inBox(pixel, lo, vec2(lo.x + (hi.x - lo.x) * clamp(fill, 0.0, 1.0), hi.y)));
}

void main() {
    vec3 colour = texture2D(frame, v_texcoord0).rgb;
    vec2 res = windowRes.xy;
    vec2 pixel = v_texcoord0 * res;

    // Where the bomb would land: a ring on the spot, seen through anything.
    if (hudMarker.w > 0.5) {
        vec3 direction = viewRay(v_texcoord0, res);
        vec3 toMarker = hudMarker.xyz - cameraPos.xyz;
        float along = dot(toMarker, direction);
        if (along > 0.0) {
            float off = length(toMarker - direction * along);
            float width = max(0.05, along * 0.003);
            if (abs(off - 2.2) < width) colour = mix(colour, vec3(1.0, 0.55, 0.1), 0.9);
            if (off < width * 1.5) colour = vec3(1.0, 0.8, 0.2);
        }
    }

    // First person: a crosshair, and a small ring round the cell a cube would go in.
    if (abs(hudAim.w) > 0.5) {
        vec2 offset = abs(pixel - res * 0.5);
        bool bar = (offset.x < 1.0 && offset.y < 9.0 && offset.y > 3.0) || (offset.y < 1.0 && offset.x < 9.0 && offset.x > 3.0);
        if (bar) colour = vec3(1.0, 1.0, 1.0) - colour * 0.8;
    }
    if (hudAim.w > 0.5) {
        vec3 direction = viewRay(v_texcoord0, res);
        vec3 toAim = hudAim.xyz - cameraPos.xyz;
        float along = dot(toAim, direction);
        if (along > 0.0) {
            float off = length(toAim - direction * along);
            float width = max(0.02, along * 0.0025);
            if (abs(off - 0.55) < width) colour = mix(colour, vec3(0.6, 1.0, 0.6), 0.85);
        }
    }

    // Shoved: the edges flash white.
    if (hudMe.z > 0.0) {
        vec2 uv = v_texcoord0 - 0.5;
        float edge = smoothstep(0.3, 0.75, length(uv) * 1.4);
        colour = mix(colour, vec3(1.0, 1.0, 1.0), edge * hudMe.z * 0.6);
    }

    // The game, the round and the clock, top middle.
    colour = drawText(colour, pixel, vec2(res.x * 0.5, 34.0), 4.0, hudTop0, hudTop1, vec3(1.0, 1.0, 1.0));

    // The scoreboard, top left: a swatch of each player's colour, its name and points; yours marked.
    for (int i = 0; i < 4; i++) {
        float fi = float(i);
        float slot = i == 0 ? hudScoreSlots.x : (i == 1 ? hudScoreSlots.y : (i == 2 ? hudScoreSlots.z : hudScoreSlots.w));
        if (slot < -0.5) continue;
        vec4 a = i == 0 ? hudScore0 : (i == 1 ? hudScore2 : (i == 2 ? hudScore4 : hudScore6));
        vec4 b = i == 0 ? hudScore1 : (i == 1 ? hudScore3 : (i == 2 ? hudScore5 : hudScore7));
        float y = 78.0 + fi * 26.0;
        vec3 tint = playerColour(slot);
        if (abs(hudMe.x - fi) < 0.5) colour = mix(colour, vec3(1.0, 1.0, 1.0), inBox(pixel, vec2(10.0, y - 11.0), vec2(330.0, y + 11.0)) * 0.18);
        colour = mix(colour, tint, inBox(pixel, vec2(18.0, y - 8.0), vec2(34.0, y + 8.0)));
        colour = drawText(colour, pixel, vec2(44.0 + 48.0 * 3.0, y), 3.0, a, b, mix(tint, vec3(1.0, 1.0, 1.0), 0.55));
    }

    // The bomb's fuse, under the clock: orange, red when low; framed in red, pulsing, when it is yours.
    if (hudBars.x >= 0.0) {
        vec2 lo = vec2(res.x * 0.5 - 200.0, 58.0);
        vec2 hi = vec2(res.x * 0.5 + 200.0, 72.0);
        vec3 ink = mix(vec3(0.95, 0.15, 0.1), vec3(1.0, 0.65, 0.15), clamp(hudBars.x * 2.0, 0.0, 1.0));
        if (hudBars.y > 0.5) {
            float pulse = 0.5 + 0.5 * sin(hudBars.x * 120.0);
            colour = mix(colour, vec3(1.0, 0.1, 0.05), (inBox(pixel, lo - 7.0, hi + 7.0) - inBox(pixel, lo - 3.0, hi + 3.0)) * pulse);
        }
        colour = drawBar(colour, pixel, lo, hi, hudBars.x, ink);
    }

    // The banner and the hint.
    if (hudBannerColour.w > 0.5) {
        float size = max(6.0, floor(res.y / 110.0));
        colour = drawText(colour, pixel, vec2(res.x * 0.5, res.y * 0.3), size, hudBanner0, hudBanner1, hudBannerColour.rgb);
    }
    colour = drawText(colour, pixel, vec2(res.x * 0.5, res.y * 0.42), 4.0, hudHint0, hudHint1, vec3(1.0, 0.92, 0.6));

    // How soon you can shove (F) and throw (R) again: bottom middle, full when ready; the throw being
    // wound up above them; your blocks under them.
    if (hudMe.y > 0.5) {
        float left = hudBars.w >= 0.0 ? res.x * 0.5 - 150.0 : res.x * 0.5 - 70.0;
        vec2 lo = vec2(left, res.y - 40.0);
        vec3 ready = vec3(0.95, 0.95, 0.9);
        colour = drawBar(colour, pixel, lo, lo + vec2(140.0, 16.0), 1.0 - hudBars.z, hudBars.z <= 0.0 ? ready : vec3(0.45, 0.45, 0.45));
        colour = mix(colour, vec3(1.0, 1.0, 1.0), character(pixel, lo + vec2(-16.0, 2.0), 2.4, 16.0));   // F
        if (hudBars.w >= 0.0) {
            vec2 lo2 = vec2(res.x * 0.5 + 30.0, res.y - 40.0);
            colour = drawBar(colour, pixel, lo2, lo2 + vec2(140.0, 16.0), 1.0 - hudBars.w, hudBars.w <= 0.0 ? vec3(1.0, 0.55, 0.15) : vec3(0.45, 0.45, 0.45));
            colour = mix(colour, vec3(1.0, 1.0, 1.0), character(pixel, lo2 + vec2(-16.0, 2.0), 2.4, 28.0));   // R
        }
        if (hudCharge.x >= 0.0) {
            vec2 lo3 = vec2(res.x * 0.5 - 150.0, res.y - 70.0);
            colour = drawBar(colour, pixel, lo3, lo3 + vec2(320.0, 12.0), hudCharge.x, mix(vec3(1.0, 0.85, 0.3), vec3(1.0, 0.35, 0.1), hudCharge.x));
        }
        colour = drawText(colour, pixel, vec2(res.x * 0.5, res.y - 14.0), 2.0, hudInfo0, hudInfo1, vec3(0.9, 0.9, 0.85));
    }

    gl_FragColor = vec4(colour, 1.0);
}
