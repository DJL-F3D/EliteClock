#pragma once
// =============================================================================
//  renderer.h — 3-D ship renderer
//  Implements rotation, perspective projection, back-face culling,
//  wireframe mode and flat-shaded mode for the Elite ship definitions.
//  Targets the 240×320 TFT_eSPI display on the ESP32-C3.
// =============================================================================

#include <TFT_eSPI.h>
#include <math.h>
#include "ships.h"

// ── Colour palette (Elite monochrome palette, rendered as bright on black) ──
//  0=cyan, 1=yellow, 2=green, 3=red, 4=white
static const uint16_t SHIP_COLOURS[5] = {
    TFT_CYAN, TFT_YELLOW, TFT_GREEN, TFT_RED, TFT_WHITE
};

// ── Rendering parameters ─────────────────────────────────────────────────────
#define SHIP_VIEW_X   120   // Centre of ship view area (x)
#define SHIP_VIEW_Y   120   // Centre of ship view area (y) — top 240px area
#define SHIP_VIEW_R   100   // Radius of usable area
#define FOV           160   // Perspective focal length (pixels)

// ── Float 3D types ───────────────────────────────────────────────────────────
struct V3 { float x, y, z; };

// ── Renderer state ───────────────────────────────────────────────────────────
class Renderer {
public:
    float  angleX = 15.0f;   // degrees
    float  angleY = 0.0f;
    float  angleZ = 0.0f;
    float  rotSpeedY = 0.6f; // degrees per frame
    bool   shaded   = false;

    // Projected screen coordinates for each vertex (max 32 ships)
    int16_t  sx[32], sy[32];
    bool     vVis[32]; // vertex visible flag

    // ── Main entry — render one ship onto tft ────────────────────────────────
    void render(TFT_eSPI &tft, const ShipDef &ship) {
        // Build rotation matrix from Euler angles
        float rx = DEG_TO_RAD * angleX;
        float ry = DEG_TO_RAD * angleY;
        float rz = DEG_TO_RAD * angleZ;

        float cosX = cosf(rx), sinX = sinf(rx);
        float cosY = cosf(ry), sinY = sinf(ry);
        float cosZ = cosf(rz), sinZ = sinf(rz);

        // Composite rotation matrix (Ry * Rx * Rz)
        float m[3][3];
        m[0][0] =  cosY*cosZ + sinX*sinY*sinZ;
        m[0][1] =  cosX*sinZ;
        m[0][2] = -sinY*cosZ + sinX*cosY*sinZ;
        m[1][0] = -cosY*sinZ + sinX*sinY*cosZ;
        m[1][1] =  cosX*cosZ;
        m[1][2] =  sinY*sinZ + sinX*cosY*cosZ;
        m[2][0] =  cosX*sinY;
        m[2][1] = -sinX;
        m[2][2] =  cosX*cosY;

        // Determine auto-scale: fit bounding sphere to viewport
        float maxR = 1.0f;
        for (int i = 0; i < ship.numVerts; i++) {
            float d = sqrtf((float)ship.verts[i].x*(float)ship.verts[i].x +
                            (float)ship.verts[i].y*(float)ship.verts[i].y +
                            (float)ship.verts[i].z*(float)ship.verts[i].z);
            if (d > maxR) maxR = d;
        }
        float scale = (float)SHIP_VIEW_R / maxR;
        float camZ  = 3.0f * maxR;   // camera z distance for perspective

        // Transform all vertices
        V3 world[32];
        for (int i = 0; i < ship.numVerts; i++) {
            float vx = ship.verts[i].x * scale;
            float vy = ship.verts[i].y * scale;
            float vz = ship.verts[i].z * scale;

            world[i].x = m[0][0]*vx + m[0][1]*vy + m[0][2]*vz;
            world[i].y = m[1][0]*vx + m[1][1]*vy + m[1][2]*vz;
            world[i].z = m[2][0]*vx + m[2][1]*vy + m[2][2]*vz + camZ;
        }

        // Perspective project
        for (int i = 0; i < ship.numVerts; i++) {
            float dz = world[i].z;
            if (dz < 0.01f) dz = 0.01f;
            sx[i] = (int16_t)SHIP_VIEW_X + (int16_t)(world[i].x * FOV / dz);
            sy[i] = (int16_t)SHIP_VIEW_Y - (int16_t)(world[i].y * FOV / dz);
        }

        // Determine face visibility (back-face culling)
        // A face is visible if its normal (rotated into view space) has +z
        bool fVis[64] = {};
        for (int f = 0; f < ship.numFaces; f++) {
            const Face &face = ship.faces[f];
            float nx = face.normal.x;
            float ny = face.normal.y;
            float nz = face.normal.z;
            // Rotate normal
            float wx = m[0][0]*nx + m[0][1]*ny + m[0][2]*nz;
            float wy = m[1][0]*nx + m[1][1]*ny + m[1][2]*nz;
            float wz = m[2][0]*nx + m[2][1]*ny + m[2][2]*nz;
            // View direction is +Z; face visible if dot(wn, view_dir) > 0
            // We also add a small offset for edge faces
            fVis[f] = (wz > -0.1f);
            (void)wx; (void)wy;
        }

        // Vertex visibility: visible if any adjacent face is visible
        for (int v = 0; v < ship.numVerts; v++) vVis[v] = false;
        for (int e = 0; e < ship.numEdges; e++) {
            const Edge &ed = ship.edges[e];
            if (fVis[ed.f1] || fVis[ed.f2]) {
                vVis[ed.v1] = true;
                vVis[ed.v2] = true;
            }
        }

        if (shaded) {
            renderShaded(tft, ship, fVis, world, m, camZ, scale);
        } else {
            renderWireframe(tft, ship, fVis);
        }
    }

private:
    // ── Wireframe mode ────────────────────────────────────────────────────────
    void renderWireframe(TFT_eSPI &tft, const ShipDef &ship, const bool *fVis) {
        uint16_t col = SHIP_COLOURS[ship.colour];
        for (int e = 0; e < ship.numEdges; e++) {
            const Edge &ed = ship.edges[e];
            if (!fVis[ed.f1] && !fVis[ed.f2]) continue;
            if (!vVis[ed.v1] || !vVis[ed.v2])  continue;
            tft.drawLine(sx[ed.v1], sy[ed.v1],
                         sx[ed.v2], sy[ed.v2], col);
        }
    }

    // ── Flat-shaded mode ─────────────────────────────────────────────────────
    void renderShaded(TFT_eSPI &tft, const ShipDef &ship, const bool *fVis,
                      const V3 *world, float m[3][3], float camZ, float scale) {
        // Light direction (normalised) — slightly above-right from camera
        const float LX =  0.5f, LY =  0.7f, LZ = -0.5f;

        // Painter's sort — render faces back to front
        // Simple: sort by average Z of rotated face centroid (descending)
        int order[64];
        float fZ[64];
        for (int f = 0; f < ship.numFaces; f++) {
            order[f] = f;
            fZ[f] = 0;
            if (!fVis[f]) { fZ[f] = -1e9f; continue; }
            for (int i = 0; i < ship.faces[f].n; i++) {
                fZ[f] += world[ship.faces[f].verts[i]].z;
            }
            fZ[f] /= ship.faces[f].n;
        }
        // Bubble sort (face count is small)
        for (int a = 0; a < ship.numFaces - 1; a++) {
            for (int b = 0; b < ship.numFaces - 1 - a; b++) {
                if (fZ[order[b]] < fZ[order[b+1]]) {
                    int tmp = order[b]; order[b] = order[b+1]; order[b+1] = tmp;
                }
            }
        }

        uint16_t baseCol = SHIP_COLOURS[ship.colour];

        for (int fi = 0; fi < ship.numFaces; fi++) {
            int f = order[fi];
            if (!fVis[f]) continue;

            const Face &face = ship.faces[f];

            // Rotated face normal
            float nx = face.normal.x / 127.0f;
            float ny = face.normal.y / 127.0f;
            float nz = face.normal.z / 127.0f;
            float wx = m[0][0]*nx + m[0][1]*ny + m[0][2]*nz;
            float wy = m[1][0]*nx + m[1][1]*ny + m[1][2]*nz;
            float wz = m[2][0]*nx + m[2][1]*ny + m[2][2]*nz;

            // Diffuse lighting
            float dot = wx*LX + wy*LY + wz*LZ;
            if (dot < 0) dot = 0;
            float ambient = 0.25f;
            float intensity = ambient + (1.0f - ambient) * dot;
            if (intensity > 1.0f) intensity = 1.0f;

            uint16_t fc = scaledColour(baseCol, intensity);

            // Fill polygon (fan triangulation from vertex 0)
            int16_t px[8], py[8];
            for (int i = 0; i < face.n; i++) {
                px[i] = sx[face.verts[i]];
                py[i] = sy[face.verts[i]];
            }
            fillPolygon(tft, px, py, face.n, fc);

            // Draw edges on top (dim)
            uint16_t edgeCol = scaledColour(baseCol, intensity * 0.5f + 0.1f);
            for (int i = 0; i < face.n; i++) {
                int j = (i+1) % face.n;
                tft.drawLine(px[i], py[i], px[j], py[j], edgeCol);
            }
        }
    }

    // ── Polygon fill (fan-triangulated, scanline within TFT_eSPI) ─────────────
    void fillPolygon(TFT_eSPI &tft, int16_t *px, int16_t *py, int n, uint16_t col) {
        // Fan triangulation from vertex 0
        for (int i = 1; i < n - 1; i++) {
            fillTriangle(tft, px[0], py[0], px[i], py[i], px[i+1], py[i+1], col);
        }
    }

    void fillTriangle(TFT_eSPI &tft,
                      int16_t x0, int16_t y0,
                      int16_t x1, int16_t y1,
                      int16_t x2, int16_t y2,
                      uint16_t col) {
        tft.fillTriangle(x0, y0, x1, y1, x2, y2, col);
    }

    // ── Scale a 16-bit RGB colour by an intensity factor ──────────────────────
    static uint16_t scaledColour(uint16_t c, float t) {
        // RGB565: RRRRR GGGGGG BBBBB
        uint8_t r = ((c >> 11) & 0x1F);
        uint8_t g = ((c >>  5) & 0x3F);
        uint8_t b =  (c        & 0x1F);
        r = (uint8_t)(r * t);
        g = (uint8_t)(g * t);
        b = (uint8_t)(b * t);
        return (uint16_t)((r << 11) | (g << 5) | b);
    }
};
