#pragma once
// =============================================================================
//  renderer.h — 3-D ship renderer
//
//  Pipeline:
//    1. Build 3×3 rotation matrix from Euler angles (Ry * Rx * Rz)
//    2. Auto-scale ship vertices to fit viewport
//    3. Perspective-project all vertices onto 2-D screen
//    4. Back-face cull each face using rotated face normals (dot with +Z)
//    5. Wireframe: draw visible edges only
//    6. Shaded:  painter's-sort faces back-to-front, fill + edge highlight
//
//  COORDINATE SYSTEM
//    +X = right,  +Y = up,  +Z = towards camera.
//    Camera is at (0, 0, camZ); ship centred at origin.
//
//  SPRITE DIMENSIONS
//    The sprite is 240 × SPRITE_H (208) pixels, pushed at y=HDR_H on TFT.
//    SHIP_VIEW_X/Y are the centre within the sprite.
// =============================================================================

#include <TFT_eSPI.h>
#include <math.h>
#include "ships.h"

// ── Ship colour palette ───────────────────────────────────────────────────────
//   0=cyan, 1=yellow, 2=green, 3=red, 4=white  (index from ShipDef::colour)
const uint16_t SHIP_COLOURS[5] = {
    TFT_CYAN, TFT_YELLOW, TFT_GREEN, TFT_RED, TFT_WHITE
};

// ── Viewport: ship sprite is 180 × 218 px, pushed at (150, 22) ──────────────
// Landscape 480x320: left panel 150px + ship 180px + right panel 150px
// Header 22px + ship 218px + radar 80px = 320px
#define SHIP_VIEW_X    90    // centre of 180px sprite
#define SHIP_VIEW_Y   109    // centre of 218px sprite
#define SHIP_VIEW_R    82    // safe rendering radius
#define SHIP_FOV      200    // perspective focal length

// ── Float 3-D vector ─────────────────────────────────────────────────────────
struct V3f { float x, y, z; };

// =============================================================================
class Renderer {
public:
    float angleX =  15.0f;   // elevation (degrees) — set by main.cpp per frame
    float angleY =   0.0f;   // azimuth  (degrees) — incremented each frame
    float angleZ =   0.0f;   // roll     (degrees) — unused by default
    bool  shaded  = false;

    // Projected screen coords + visibility flags (up to 32 verts per ship)
    int16_t sx[32], sy[32];
    bool    vVis[32];

    // ==========================================================================
    //  render()  — called every frame with the sprite as 'disp'
    // ==========================================================================
    void render(TFT_eSPI &disp, const ShipDef &ship) {

        // ── Build rotation matrix R = Ry * Rx * Rz ───────────────────────
        float rx  = DEG_TO_RAD * angleX;
        float ry  = DEG_TO_RAD * angleY;
        float rz  = DEG_TO_RAD * angleZ;

        float cx = cosf(rx), sx_ = sinf(rx);
        float cy = cosf(ry), sy_ = sinf(ry);
        float cz = cosf(rz), sz_ = sinf(rz);

        // Composite: m = Ry(ry) * Rx(rx) * Rz(rz)
        float m[3][3];
        m[0][0] =  cy*cz + sy_*sx_*sz_;
        m[0][1] =  cx*sz_;
        m[0][2] = -sy_*cz + cy*sx_*sz_;

        m[1][0] = -cy*sz_ + sy_*sx_*cz;
        m[1][1] =  cx*cz;
        m[1][2] =  sy_*sz_ + cy*sx_*cz;

        m[2][0] =  cx*sy_;
        m[2][1] = -sx_;
        m[2][2] =  cx*cy;

        // ── Auto-scale: fit bounding sphere to SHIP_VIEW_R ────────────────
        float maxR = 1.0f;
        for (int i = 0; i < ship.numVerts; i++) {
            float d = sqrtf((float)ship.verts[i].x * ship.verts[i].x +
                            (float)ship.verts[i].y * ship.verts[i].y +
                            (float)ship.verts[i].z * ship.verts[i].z);
            if (d > maxR) maxR = d;
        }
        float scale = (float)SHIP_VIEW_R / maxR;
        float camZ  = 2.8f * maxR * scale;

        // ── Transform all vertices into camera space ───────────────────────
        V3f world[32];
        for (int i = 0; i < ship.numVerts; i++) {
            float vx = ship.verts[i].x * scale;
            float vy = ship.verts[i].y * scale;
            float vz = ship.verts[i].z * scale;

            world[i].x = m[0][0]*vx + m[0][1]*vy + m[0][2]*vz;
            world[i].y = m[1][0]*vx + m[1][1]*vy + m[1][2]*vz;
            world[i].z = m[2][0]*vx + m[2][1]*vy + m[2][2]*vz + camZ;
        }

        // ── Perspective project ────────────────────────────────────────────
        for (int i = 0; i < ship.numVerts; i++) {
            float dz = (world[i].z < 0.01f) ? 0.01f : world[i].z;
            sx[i] = (int16_t)(SHIP_VIEW_X + world[i].x * SHIP_FOV / dz);
            sy[i] = (int16_t)(SHIP_VIEW_Y - world[i].y * SHIP_FOV / dz);
        }

        // ── Back-face culling ─────────────────────────────────────────────
        // Face visible if rotated outward normal has positive Z component
        // (faces toward camera, which is along +Z).
        // A small tolerance avoids cracking on silhouette edges.
        bool fVis[64] = {};
        for (int f = 0; f < ship.numFaces; f++) {
            const Face &face = ship.faces[f];
            float nx = face.normal.x;
            float ny = face.normal.y;
            float nz = face.normal.z;
            float wz = m[2][0]*nx + m[2][1]*ny + m[2][2]*nz;
            fVis[f] = (wz > -0.05f);
        }

        // ── Vertex visibility: visible if any adjacent face is visible ─────
        for (int v = 0; v < ship.numVerts; v++) vVis[v] = false;
        for (int e = 0; e < ship.numEdges; e++) {
            if (fVis[ship.edges[e].f1] || fVis[ship.edges[e].f2]) {
                vVis[ship.edges[e].v1] = true;
                vVis[ship.edges[e].v2] = true;
            }
        }

        // ── Dispatch to render mode ────────────────────────────────────────
        if (shaded) {
            renderShaded(disp, ship, fVis, world, m);
        } else {
            renderWireframe(disp, ship, fVis);
        }
    }

private:
    // ==========================================================================
    //  WIREFRAME
    // ==========================================================================
    void renderWireframe(TFT_eSPI &disp, const ShipDef &ship, const bool *fVis) {
        uint16_t col = SHIP_COLOURS[ship.colour];
        for (int e = 0; e < ship.numEdges; e++) {
            const Edge &ed = ship.edges[e];
            if (!fVis[ed.f1] && !fVis[ed.f2]) continue;
            if (!vVis[ed.v1] || !vVis[ed.v2])  continue;
            disp.drawLine(sx[ed.v1], sy[ed.v1],
                          sx[ed.v2], sy[ed.v2], col);
        }
    }

    // ==========================================================================
    //  FLAT-SHADED (painter's algorithm — back to front)
    // ==========================================================================
    void renderShaded(TFT_eSPI &disp, const ShipDef &ship, const bool *fVis,
                      const V3f *world, float m[3][3]) {

        // ── Light direction (camera space, normalised) ────────────────────
        // Directional light slightly above-right, toward camera (+Z).
        const float LX =  0.45f, LY = 0.65f, LZ = 0.62f;

        // ── Average depth per face for painter's sort ─────────────────────
        int   order[64];
        float fDepth[64];
        for (int f = 0; f < ship.numFaces; f++) {
            order[f]  = f;
            fDepth[f] = 1e9f;
            if (!fVis[f]) continue;
            float sum = 0;
            for (int i = 0; i < ship.faces[f].n; i++) {
                sum += world[ship.faces[f].verts[i]].z;
            }
            fDepth[f] = sum / ship.faces[f].n;
        }
        // Insertion sort — faces per ship is always small (≤24)
        for (int a = 1; a < ship.numFaces; a++) {
            int   kIdx  = order[a];
            float kDepth = fDepth[kIdx];
            int   b     = a - 1;
            while (b >= 0 && fDepth[order[b]] < kDepth) {
                order[b + 1] = order[b];
                b--;
            }
            order[b + 1] = kIdx;
        }

        uint16_t baseCol = SHIP_COLOURS[ship.colour];

        // ── Draw faces back → front ────────────────────────────────────────
        for (int fi = 0; fi < ship.numFaces; fi++) {
            int f = order[fi];
            if (!fVis[f]) continue;

            const Face &face = ship.faces[f];

            // Rotate face normal into camera space
            float nx = face.normal.x / 127.0f;
            float ny = face.normal.y / 127.0f;
            float nz = face.normal.z / 127.0f;
            float wx = m[0][0]*nx + m[0][1]*ny + m[0][2]*nz;
            float wy = m[1][0]*nx + m[1][1]*ny + m[1][2]*nz;
            float wz = m[2][0]*nx + m[2][1]*ny + m[2][2]*nz;

            // Lambertian diffuse + ambient term
            float diff = wx*LX + wy*LY + wz*LZ;
            if (diff < 0) diff = 0;
            float I = 0.22f + 0.78f * diff;
            if (I > 1.0f) I = 1.0f;

            // Slightly darker fill, brighter edges — gives crisp cel-shade look
            uint16_t fillCol = scaledColour(baseCol, I * 0.80f);
            uint16_t edgeCol = scaledColour(baseCol, I);

            // Screen-space polygon vertices
            int16_t px[8], py[8];
            for (int i = 0; i < face.n; i++) {
                px[i] = sx[face.verts[i]];
                py[i] = sy[face.verts[i]];
            }

            // Fan-triangulate and fill
            for (int i = 1; i < face.n - 1; i++) {
                disp.fillTriangle(px[0], py[0],
                                  px[i], py[i],
                                  px[i+1], py[i+1], fillCol);
            }

            // Edge outline
            for (int i = 0; i < face.n; i++) {
                int j = (i + 1) % face.n;
                disp.drawLine(px[i], py[i], px[j], py[j], edgeCol);
            }
        }
    }

    // ── Scale RGB565 colour by intensity t in [0,1] ───────────────────────
    static uint16_t scaledColour(uint16_t c, float t) {
        if (t <= 0.0f) return 0u;
        if (t >= 1.0f) return c;
        uint8_t r = (uint8_t)(((c >> 11) & 0x1Fu) * t);
        uint8_t g = (uint8_t)(((c >>  5) & 0x3Fu) * t);
        uint8_t b = (uint8_t)( (c        & 0x1Fu) * t);
        return (uint16_t)((r << 11) | (g << 5) | b);
    }
};
