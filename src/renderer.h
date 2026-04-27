#pragma once
// =============================================================================
//  renderer.h — 3-D ship renderer  (LovyanGFX version)
//
//  API is identical to TFT_eSPI — only the display type changed:
//    TFT_eSPI & → lgfx::LovyanGFX &
//  Both LGFX and LGFX_Sprite inherit from lgfx::LovyanGFX, so this single
//  parameter type accepts both the main display and the off-screen sprite.
// =============================================================================

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <math.h>
#include "ships.h"

// Ship colour — all white as per BBC Elite authentic look
const uint16_t SHIP_COLOURS[5] = {
    TFT_WHITE, TFT_WHITE, TFT_WHITE, TFT_WHITE, TFT_WHITE
};

// Viewport — 250×218px sprite (enlarged ship area), centred
#define SHIP_VIEW_X   125   // SHIP_SW/2 = 250/2
#define SHIP_VIEW_Y   109   // SHIP_SH/2 = 218/2
#define SHIP_VIEW_R   112   // safe radius (~90% of 125)
#define SHIP_FOV      220   // slightly wider FOV for larger viewport

struct V3f { float x, y, z; };

class Renderer {
public:
    float angleX =  15.0f;
    float angleY =   0.0f;
    float angleZ =   0.0f;
    bool  shaded  = false;

    int16_t sx[32], sy[32];
    bool    vVis[32];

    void render(lgfx::LovyanGFX &disp, const ShipDef &ship) {
        float rx = DEG_TO_RAD * angleX;
        float ry = DEG_TO_RAD * angleY;
        float rz = DEG_TO_RAD * angleZ;
        float cx = cosf(rx), sx_ = sinf(rx);
        float cy = cosf(ry), sy_ = sinf(ry);
        float cz = cosf(rz), sz_ = sinf(rz);

        float m[3][3];
        m[0][0] =  cy*cz + sy_*sx_*sz_;  m[0][1] =  cx*sz_;  m[0][2] = -sy_*cz + cy*sx_*sz_;
        m[1][0] = -cy*sz_ + sy_*sx_*cz;  m[1][1] =  cx*cz;   m[1][2] =  sy_*sz_ + cy*sx_*cz;
        m[2][0] =  cx*sy_;               m[2][1] = -sx_;      m[2][2] =  cx*cy;

        float maxR = 1.0f;
        for (int i = 0; i < ship.numVerts; i++) {
            float d = sqrtf((float)ship.verts[i].x*ship.verts[i].x +
                            (float)ship.verts[i].y*ship.verts[i].y +
                            (float)ship.verts[i].z*ship.verts[i].z);
            if (d > maxR) maxR = d;
        }
        float scale = (float)SHIP_VIEW_R / maxR;
        float camZ  = 2.8f * maxR * scale;

        V3f world[32];
        for (int i = 0; i < ship.numVerts; i++) {
            float vx = ship.verts[i].x * scale;
            float vy = ship.verts[i].y * scale;
            float vz = ship.verts[i].z * scale;
            world[i].x = m[0][0]*vx + m[0][1]*vy + m[0][2]*vz;
            world[i].y = m[1][0]*vx + m[1][1]*vy + m[1][2]*vz;
            world[i].z = m[2][0]*vx + m[2][1]*vy + m[2][2]*vz + camZ;
        }

        for (int i = 0; i < ship.numVerts; i++) {
            float dz = (world[i].z < 0.01f) ? 0.01f : world[i].z;
            sx[i] = (int16_t)(SHIP_VIEW_X + world[i].x * SHIP_FOV / dz);
            sy[i] = (int16_t)(SHIP_VIEW_Y - world[i].y * SHIP_FOV / dz);
        }

        bool fVis[64] = {};
        for (int f = 0; f < ship.numFaces; f++) {
            const Face &face = ship.faces[f];
            float wz = m[2][0]*face.normal.x + m[2][1]*face.normal.y + m[2][2]*face.normal.z;
            fVis[f] = (wz > -0.05f);
        }

        for (int v = 0; v < ship.numVerts; v++) vVis[v] = false;
        for (int e = 0; e < ship.numEdges; e++) {
            if (fVis[ship.edges[e].f1] || fVis[ship.edges[e].f2]) {
                vVis[ship.edges[e].v1] = true;
                vVis[ship.edges[e].v2] = true;
            }
        }

        if (shaded) renderShaded(disp, ship, fVis, world, m);
        else        renderWireframe(disp, ship, fVis);
    }

private:
    void renderWireframe(lgfx::LovyanGFX &disp, const ShipDef &ship, const bool *fVis) {
        uint16_t col = SHIP_COLOURS[ship.colour];
        for (int e = 0; e < ship.numEdges; e++) {
            const Edge &ed = ship.edges[e];
            if (!fVis[ed.f1] && !fVis[ed.f2]) continue;
            if (!vVis[ed.v1] || !vVis[ed.v2])  continue;
            disp.drawLine(sx[ed.v1], sy[ed.v1], sx[ed.v2], sy[ed.v2], col);
        }
    }

    void renderShaded(lgfx::LovyanGFX &disp, const ShipDef &ship, const bool *fVis,
                      const V3f *world, float m[3][3]) {
        const float LX = 0.45f, LY = 0.65f, LZ = 0.62f;
        int   order[64];
        float fDepth[64];
        for (int f = 0; f < ship.numFaces; f++) {
            order[f] = f; fDepth[f] = 1e9f;
            if (!fVis[f]) continue;
            float sum = 0;
            for (int i = 0; i < ship.faces[f].n; i++) sum += world[ship.faces[f].verts[i]].z;
            fDepth[f] = sum / ship.faces[f].n;
        }
        for (int a = 1; a < ship.numFaces; a++) {
            int kIdx = order[a]; float kD = fDepth[kIdx]; int b = a-1;
            while (b >= 0 && fDepth[order[b]] < kD) { order[b+1] = order[b]; b--; }
            order[b+1] = kIdx;
        }
        uint16_t baseCol = SHIP_COLOURS[ship.colour];
        for (int fi = 0; fi < ship.numFaces; fi++) {
            int f = order[fi];
            if (!fVis[f]) continue;
            const Face &face = ship.faces[f];
            float nx = face.normal.x/127.0f, ny = face.normal.y/127.0f, nz = face.normal.z/127.0f;
            float wx = m[0][0]*nx+m[0][1]*ny+m[0][2]*nz;
            float wy = m[1][0]*nx+m[1][1]*ny+m[1][2]*nz;
            float wz = m[2][0]*nx+m[2][1]*ny+m[2][2]*nz;
            float diff = wx*LX + wy*LY + wz*LZ;
            if (diff < 0) diff = 0;
            float I = 0.22f + 0.78f * diff;
            if (I > 1.0f) I = 1.0f;
            uint16_t fillCol = scaledColour(baseCol, I * 0.80f);
            uint16_t edgeCol = scaledColour(baseCol, I);
            int16_t px[8], py[8];
            for (int i = 0; i < face.n; i++) { px[i]=sx[face.verts[i]]; py[i]=sy[face.verts[i]]; }
            for (int i = 1; i < face.n-1; i++)
                disp.fillTriangle(px[0],py[0], px[i],py[i], px[i+1],py[i+1], fillCol);
            for (int i = 0; i < face.n; i++) {
                int j = (i+1)%face.n;
                disp.drawLine(px[i],py[i], px[j],py[j], edgeCol);
            }
        }
    }

    static uint16_t scaledColour(uint16_t c, float t) {
        if (t <= 0.0f) return 0u;
        if (t >= 1.0f) return c;
        uint8_t r = (uint8_t)(((c>>11)&0x1Fu)*t);
        uint8_t g = (uint8_t)(((c>> 5)&0x3Fu)*t);
        uint8_t b = (uint8_t)( (c     &0x1Fu)*t);
        return (uint16_t)((r<<11)|(g<<5)|b);
    }
};
