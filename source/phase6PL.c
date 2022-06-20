#include "Doom.h"
#include <IntMath.h>

#include "string.h"

#define MAX_WALL_PARTS 300

// OLD POLY (trick to avoid Y tiling) fails on real hardware (black textures and FPS almost freezes)
//#define OLD_POLY

typedef struct {
    int xLeft, xRight;
    int scaleLeft, scaleRight;
    int textureOffset, textureLength;
    Word shadeLight;
} wallpart_t;

static wallpart_t wallParts[MAX_WALL_PARTS];
static int wallPartsCount;

static int texColumnOffset[MAX_WALL_PARTS];
static bool texColumnOffsetPrepared;

static drawtex_t drawtex;

static Word pixcLight;

static PolyCCB CCBQuadWallFlat;
static PolyCCB CCBQuadWallTextured[MAX_WALL_PARTS];

static const int flatTexWidth = 8;  static const int flatTexWidthShr = 3;
static const int flatTexHeight = 1;  static const int flatTexHeightShr = 0;
static const int flatTexStride = 8;
static unsigned char *texBufferFlat;

static const int mode8bpp = 5;
static const int mode4bpp = 3;

static uint16 coloredPolyWallPals[16];
static Word *LightTablePtr = LightTable;

#define RECIPROCAL_MAX_NUM 1024
#define RECIPROCAL_FP 16
static int reciprocalLength[RECIPROCAL_MAX_NUM];

int texLeft, texRight;

void initCCBQuadWallFlat()
{
    const int flatTexSize = flatTexStride * flatTexHeight;
    texBufferFlat = (unsigned char*)AllocAPointer(flatTexSize * sizeof(unsigned char));
    memset(texBufferFlat, 0, flatTexSize);

    CCBQuadWallFlat.ccb_Flags = CCB_SPABS|CCB_LDSIZE|CCB_LDPRS|CCB_LDPPMP|CCB_CCBPRE|CCB_YOXY|CCB_ACW|CCB_ACCW|CCB_ACE|CCB_BGND|CCB_NOBLK|CCB_PPABS|CCB_LDPLUT|CCB_ACSC|CCB_ALSC|CCB_LAST;
    CCBQuadWallFlat.ccb_PRE0 = mode8bpp | ((flatTexHeight - 1) << 6);
    CCBQuadWallFlat.ccb_PRE1 = (((flatTexStride >> 2) - 2) << 16) | (flatTexWidth - 1) | 0x5000;

    CCBQuadWallFlat.ccb_HDX = 0;
    CCBQuadWallFlat.ccb_HDDX = 0;

    CCBQuadWallFlat.ccb_SourcePtr = (CelData*)texBufferFlat;
}

void initCCBQuadWallTextured()
{
	PolyCCB *CCBPtr;
	int i;

	CCBPtr = CCBQuadWallTextured;
	for (i=0; i<MAX_WALL_PARTS; ++i) {
		CCBPtr->ccb_NextPtr = (PolyCCB *)(sizeof(PolyCCB)-8);	// Create the next offset

        CCBPtr->ccb_Flags = CCB_SPABS|CCB_LDSIZE|CCB_LDPRS|CCB_LDPPMP|CCB_CCBPRE|CCB_YOXY|CCB_ACW|CCB_ACCW|CCB_ACE|CCB_BGND|CCB_NOBLK|CCB_PPABS|CCB_ACSC|CCB_ALSC;
        if (i==0) CCBPtr->ccb_Flags |= CCB_LDPLUT;  // First CEL column will set the palette and shading for the rest

        CCBPtr->ccb_HDX = 0;
        CCBPtr->ccb_HDDX = 0;

        ++CCBPtr;
	}

    for (i=0; i<RECIPROCAL_MAX_NUM; ++i) {
        reciprocalLength[i] = (1 << RECIPROCAL_FP) / i;
    }
}

static void DrawWallSegmentFlatPoly(drawtex_t *tex)
{
	int topLeft, topRight;
	int bottomLeft, bottomRight;
	int lengthLeft, lengthRight, lengthDiff;

    const int run = (tex->topheight - tex->bottomheight) >> HEIGHTBITS;

    wallpart_t *wp = wallParts;

    const int xLeft = wp->xLeft;
    const int xRight = wp->xRight;
    const int xLength = xRight - xLeft;

    int scaleLeft;
    int scaleRight;

    if (run <= 0) return;
    if (xLength < 1) return;


	scaleLeft = wp->scaleLeft;
    scaleRight = wp->scaleRight;

	topLeft = CenterY - ((scaleLeft * tex->topheight) >> (HEIGHTBITS+SCALEBITS)) - 1;
	topRight = CenterY - ((scaleRight * tex->topheight) >> (HEIGHTBITS+SCALEBITS)) - 1;
	bottomLeft = topLeft + ((run * scaleLeft) >> SCALEBITS) + 1;
	bottomRight = topRight + ((run * scaleRight) >> SCALEBITS) + 1;


    lengthLeft = bottomLeft - topLeft + 1;
    lengthRight = bottomRight - topRight + 1;
    lengthDiff = lengthRight - lengthLeft;


    CCBQuadWallFlat.ccb_XPos = xLeft << 16;
    CCBQuadWallFlat.ccb_YPos = bottomLeft << 16;
    CCBQuadWallFlat.ccb_VDX = (xLength << (16 - flatTexHeightShr));
    CCBQuadWallFlat.ccb_VDY = (bottomRight - bottomLeft) << (16 - flatTexHeightShr);

    CCBQuadWallFlat.ccb_HDX = 0;
    CCBQuadWallFlat.ccb_HDY = -lengthLeft << (20 - flatTexWidthShr);

    CCBQuadWallFlat.ccb_HDDX = 0;
    CCBQuadWallFlat.ccb_HDDY = -lengthDiff << (20 - flatTexWidthShr - flatTexHeightShr);


    CCBQuadWallFlat.ccb_PLUTPtr = &tex->color;
    CCBQuadWallFlat.ccb_PIXC = pixcLight;

    DrawCels(VideoItem,(CCB*)&CCBQuadWallFlat);
}

void drawCCBarrayPoly(PolyCCB *lastCCB, PolyCCB *CCBArrayPtr)
{
    PolyCCB *polyCCBstart, *polyCCBend;

	polyCCBstart = CCBArrayPtr;                // First poly CEL of the wall segment
	polyCCBend = lastCCB;                      // Last poly CEL of the wall segment

	polyCCBend->ccb_Flags |= CCB_LAST;         // Mark last colume CEL as the last one in the linked list
    DrawCels(VideoItem,(CCB*)polyCCBstart);    // Draw all the cels of a single wall in one shot
    polyCCBend->ccb_Flags ^= CCB_LAST;         // remember to flip off that CCB_LAST flag, since we don't reinit the flags for all polys every time
}

static void DrawWallSegmentTexturedQuadSubdivided(drawtex_t *tex, int run, Word pre0part, Word pre1part, Word frac)
{
    PolyCCB *CCBPtr;

    Byte *texBitmap = &tex->data[32];

    wallpart_t *wp = wallParts;

    int count = wallPartsCount;

    const int recHeight = reciprocalLength[run];

    if (count < 1) return;


    CCBPtr = CCBQuadWallTextured;
    do {
        int topLeft, topRight;
        int bottomLeft, bottomRight;
        int lengthLeft, lengthRight, lengthDiff;

        Word colnum;

        const int texLength = wp->textureLength;
        const int recWidth = reciprocalLength[texLength];

        const int xLeft = wp->xLeft;
        const int xRight = wp->xRight;
        const int xLength = xRight - xLeft;

        const int scaleLeft = wp->scaleLeft;
        const int scaleRight = wp->scaleRight;

        const int textureOffset = wp->textureOffset;

        ++wp;

        if (xLength < 1) continue;

        colnum = textureOffset;	// Get the starting column offset
        colnum = (colnum*tex->height)+frac;	// Index to the shape

        colnum >>= 1;           // Pixel to byte offset
        colnum &= ~3;			// Long word align the source

        if (optGraphics->depthShading >= DEPTH_SHADING_DITHERED) {
            int textureLight = ((scaleLeft*lightcoef)>>16) - lightsub;
            if (textureLight < lightmin) textureLight = lightmin;
            if (textureLight > lightmax) textureLight = lightmax;
            pixcLight = LightTablePtr[textureLight>>LIGHTSCALESHIFT];
        }

        topLeft = CenterY - ((scaleLeft * tex->topheight) >> (HEIGHTBITS+SCALEBITS)) - 1;
        topRight = CenterY - ((scaleRight * tex->topheight) >> (HEIGHTBITS+SCALEBITS)) - 1;
        bottomLeft = topLeft + ((run * scaleLeft) >> SCALEBITS) + 1;
        bottomRight = topRight + ((run * scaleRight) >> SCALEBITS) + 1;

        lengthLeft = bottomLeft - topLeft + 1;
        lengthRight = bottomRight - topRight + 1;
        lengthDiff = lengthRight - lengthLeft;

        CCBPtr->ccb_XPos = xLeft << 16;
        CCBPtr->ccb_YPos = topLeft << 16;
        CCBPtr->ccb_VDX = ((xLength * recWidth) << (16 - RECIPROCAL_FP))  + ((recWidth >> 2) << (16 - RECIPROCAL_FP)); // >>2 or a quarter of a pixel seems to eliminate edge overdrawing without leaving gaps, don't know why (checked by enabling blending over every surface)
        CCBPtr->ccb_VDY = ((topRight - topLeft) * recWidth) << (16 - RECIPROCAL_FP);

        CCBPtr->ccb_HDY = (lengthLeft * recHeight) << (20 - RECIPROCAL_FP);

        CCBPtr->ccb_HDDY = ((lengthDiff * recWidth) << (20 - RECIPROCAL_FP)) / run;


        CCBPtr->ccb_SourcePtr = (CelData*)&texBitmap[colnum];
        CCBPtr->ccb_PIXC = pixcLight;// | 0x0080; // alpha test for overdrawing


        CCBPtr->ccb_PRE0 = pre0part | ((texLength - 1) << 6);
        CCBPtr->ccb_PRE1 = pre1part | (run - 1);

        ++CCBPtr;
    } while(--count != 0);

    drawCCBarrayPoly(--CCBPtr, CCBQuadWallTextured);
}


static void DrawWallSegmentTexturedQuad(drawtex_t *tex, void *texPal, viswall_t *segl)
{
    Word frac;
    Word colnum7;
    Word pre0part, pre1part;

    int texHeight = tex->height;
    int run = (tex->topheight - tex->bottomheight) >> HEIGHTBITS;

    Word texStride = texHeight >> 1;
    if (texStride < 8) texStride = 8;

    if (run <= 0 || run >= RECIPROCAL_MAX_NUM) return;

    if (optGraphics->depthShading >= DEPTH_SHADING_DITHERED) {
        const int l = segl->seglightlevel;
        lightmin = lightmins[l];
        lightmax = l;
        lightsub = lightsubs[l];
        lightcoef = lightcoefs[l];
    }


    frac = tex->texturemid - (tex->topheight<<FIXEDTOHEIGHT);	// Get the anchor point
    frac >>= FRACBITS;
    while (frac&0x8000) {
        frac += texHeight;		// Make sure it's on the shape
    }
    frac&=0x7f;		// Zap unneeded bits
    colnum7 = frac & 7;     // Get the pixel skip


    if (run + colnum7 > texHeight) {
        colnum7 = 0;   // temporary hack for some tall walls that are buggy (and can't figure logic atm). Don't do it for shorter walls, it will make doors jerky.
    } else {
        run += colnum7;
    }

    pre0part = (colnum7 << 24) | mode4bpp;
    pre1part = (((texStride >> 2) - 2) << 24) | 0x5000; // 5000 for the LSB of blue to match exact colors as in original Doom column renderer

	CCBQuadWallTextured[0].ccb_PLUTPtr = texPal;    // plut pointer only for first element

	#ifdef OLD_POLY
	// OLD Poly, no Y tiling (fails on real hardware)
		DrawWallSegmentTexturedQuadSubdivided(tex, run, pre0part, pre1part, frac);
	#else
	// New Poly, Y tiling
	while(run > texHeight) {
		DrawWallSegmentTexturedQuadSubdivided(tex, texHeight, pre0part, pre1part, frac);
		tex->topheight -= (texHeight << HEIGHTBITS);
		run-= texHeight;
	};
	if (run > 0)
		DrawWallSegmentTexturedQuadSubdivided(tex, run, pre0part, pre1part, frac);
	#endif
}


/**********************************

	Draw a single wall texture.
	Also save states for pending ceiling, floor and future clipping

**********************************/

static void calcColumnOffsets(viswall_t *segl)
{
    const int xRight = segl->RightX;
    int x = segl->LeftX;
    int *texColOffset = texColumnOffset;

    *texColOffset++ = texLeft; ++x;
    if (x < xRight) {
		do {
			*texColOffset++ = (segl->offset-((finetangent[(segl->CenterAngle+xtoviewangle[x])>>ANGLETOFINESHIFT] * segl->distance)>>(FRACBITS-VISWALL_DISTANCE_PRESHIFT)))>>FRACBITS;
		} while(++x < xRight);
    }
    *texColOffset = texRight;
}

static void PrepareBrokenWallParts(viswall_t *segl, Word texWidth, int *scaleData)
{
    wallpart_t *wp = wallParts;
    int *texColOffset = texColumnOffset;

    int x = segl->LeftX;
    int texOffsetPrev = *texColOffset++ & (texWidth - 1);
    int texLength;

    wallPartsCount = 0;

    wp->xLeft = x;
    wp->scaleLeft = *scaleData;
    wp->textureOffset = texOffsetPrev;
    do {
        int texOffset = *texColOffset++ & (texWidth - 1);
        if (texOffset < texOffsetPrev || x==segl->RightX) {
            wp->xRight = x + 1; // to fill some gaps
            wp->scaleRight = *scaleData;
            texLength = texOffsetPrev - wp->textureOffset + 1;
            if (texLength < 1) texLength = 1;
            wp->textureLength = texLength;

            ++wallPartsCount;

            if (x < segl->RightX) {
                ++wp;
                wp->xLeft = x + 1; //because it's the next column compared to the current one
                wp->scaleLeft = *(scaleData + 1);
                wp->textureOffset = texOffset;
            }
        }
        texOffsetPrev = texOffset;
        ++scaleData;
    } while(++x <= segl->RightX);
}

static void PrepareWallParts(viswall_t *segl, Word texWidth, int *scaleData)
{
	int texL = texLeft;
	int texR = texRight;
    int texMinOff = texL;

    texL &= (texWidth - 1);
    texMinOff -= texL;
    texR -= texMinOff;

    if (texL > -3 && texR < texWidth + 2) {
        int texOffset, texLength;
        wallpart_t *wp = wallParts;

        if (texL < 0) texL = 0;
        if (texR > texWidth - 1) texR = texWidth - 1;
        texOffset = texL;
        texLength = texR - texL + 1;

        if (texLength < 1) texLength = 1;

        wp->textureOffset = texOffset;
        wp->textureLength = texLength;

        wp->scaleLeft = *scaleData;
        wp->scaleRight = *(scaleData + segl->RightX - segl->LeftX);

        wp->xLeft = segl->LeftX;
        wp->xRight = segl->RightX + 1;

        wallPartsCount = 1;
    } else {
        if (!texColumnOffsetPrepared) {
            calcColumnOffsets(segl);
            texColumnOffsetPrepared = true;
        }
        PrepareBrokenWallParts(segl, texWidth, scaleData);
    }
}

static void PrepareWallPartsFlat(viswall_t *segl, int *scaleData)
{
    wallpart_t *wp = wallParts;
    const int length = segl->RightX - segl->LeftX;

    wp->scaleLeft = *scaleData;
    wp->scaleRight = *(scaleData + length);

    wp->xLeft = segl->LeftX;
    wp->xRight = segl->RightX + 1;
}

static void DrawSegAnyPoly(viswall_t *segl, int *scaleData, bool isTop, bool shouldPrepareWallParts)
{
    texture_t *tex;
    void *texPal;

    if (isTop) {
        tex = segl->t_texture;

        drawtex.topheight = segl->t_topheight;
        drawtex.bottomheight = segl->t_bottomheight;
        drawtex.texturemid = segl->t_texturemid;
    } else {
        tex = segl->b_texture;

        drawtex.topheight = segl->b_topheight;
        drawtex.bottomheight = segl->b_bottomheight;
        drawtex.texturemid = segl->b_texturemid;
    }
    drawtex.width = tex->width;
    drawtex.height = tex->height;
    drawtex.color = tex->color;
    drawtex.data = (Byte *)*tex->data;

	if (segl->color==0) {
		texPal = drawtex.data;
	} else {
		texPal = coloredPolyWallPals;
		initColoredPals((uint16*)drawtex.data, texPal, 16, segl->color);
	}

	if (segl->special & SEC_SPEC_FOG) {
		LightTablePtr = LightTableFog;
	} else {
		LightTablePtr = LightTable;
	}

    if (optGraphics->wallQuality > WALL_QUALITY_LO) {
        if (shouldPrepareWallParts) PrepareWallParts(segl, tex->width, scaleData);
        if (wallPartsCount > 0 && wallPartsCount < MAX_WALL_PARTS) DrawWallSegmentTexturedQuad(&drawtex, texPal, segl);
    } else {
        PrepareWallPartsFlat(segl, scaleData);
        DrawWallSegmentFlatPoly(&drawtex);
    }
}

void DrawSegPoly(viswall_t *segl, int *scaleData)
{
    const Word ActionBits = segl->WallActions;
    const bool topTexOn = (bool)(ActionBits & AC_TOPTEXTURE);
    const bool bottomTexOn = (bool)(ActionBits & AC_BOTTOMTEXTURE);

    const bool shouldPrepareAgain = !(topTexOn && bottomTexOn && (segl->t_texture->width == segl->b_texture->width));
    Word ambientLight;

	if (!(topTexOn || bottomTexOn)) return;
	
    ambientLight = segl->seglightlevel;
    if (optGraphics->depthShading == DEPTH_SHADING_DARK) ambientLight = lightmins[ambientLight];
    pixcLight = LightTablePtr[ambientLight>>LIGHTSCALESHIFT];


    texColumnOffsetPrepared = false;

    if (topTexOn)
        DrawSegAnyPoly(segl, scaleData, true, true);

    if (bottomTexOn)
        DrawSegAnyPoly(segl, scaleData, false, shouldPrepareAgain);
}


// ============ Wireframe renderer ============


static void DrawWallSegmentWireframe(drawtex_t *tex)
{
	int topLeft, topRight;
	int bottomLeft, bottomRight;

    const int run = (tex->topheight - tex->bottomheight) >> HEIGHTBITS;

    const int screenCenterX = ScreenWidth >> 1;
    const int screenCenterY = ScreenHeight >> 1;

    const int xLeft = wallParts->xLeft - screenCenterX;
    const int xRight = wallParts->xRight - screenCenterX;
    const int xLength = xRight - xLeft;

    const int scaleLeft = wallParts->scaleLeft;
    const int scaleRight = wallParts->scaleRight;

    const Word color = *((Word*)tex->data);

    if (run <= 0) return;
    if (xLength < 1) return;

	topLeft = ScreenHeight - (CenterY - ((scaleLeft * tex->topheight) >> (HEIGHTBITS+SCALEBITS))) - screenCenterY;
	topRight = ScreenHeight - (CenterY - ((scaleRight * tex->topheight) >> (HEIGHTBITS+SCALEBITS))) - screenCenterY;
	bottomLeft = topLeft - ((run * scaleLeft) >> SCALEBITS);
	bottomRight = topRight - ((run * scaleRight) >> SCALEBITS);

    DrawThickLine(xLeft, topLeft, xRight, topRight, color);
    DrawThickLine(xRight, topRight, xRight, bottomRight, color);
    DrawThickLine(xRight, bottomRight, xLeft, bottomLeft, color);
    DrawThickLine(xLeft, bottomLeft, xLeft, topLeft, color);
}

static void DrawSegWireframeAny(viswall_t *segl, int *scaleData, bool isTop)
{
    texture_t *tex;
    if (isTop) {
        tex = segl->t_texture;

        drawtex.topheight = segl->t_topheight;
        drawtex.bottomheight = segl->t_bottomheight;
    } else {
        tex = segl->b_texture;

        drawtex.topheight = segl->b_topheight;
        drawtex.bottomheight = segl->b_bottomheight;
    }
    drawtex.data = (Byte *)*tex->data;

    PrepareWallPartsFlat(segl, scaleData);
    DrawWallSegmentWireframe(&drawtex);
}

void DrawSegWireframe(viswall_t *segl, int *scaleData)
{
    const Word ActionBits = segl->WallActions;
    const bool topTexOn = (bool)(ActionBits & AC_TOPTEXTURE);
    const bool bottomTexOn = (bool)(ActionBits & AC_BOTTOMTEXTURE);

	if (!(topTexOn || bottomTexOn)) return;

    if (topTexOn)
        DrawSegWireframeAny(segl, scaleData, true);

    if (bottomTexOn)
        DrawSegWireframeAny(segl, scaleData, false);
}
