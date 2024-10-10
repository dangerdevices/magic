/*
 * rtrDcmpose.c --
 *
 * Channel decomposition module.
 *
 * Create a cell tile plane where each space tile in the error
 * plane represents a channel to be separately routed by the
 * channel router.
 *
 * Enumerate cell tile corners, choosing the shortest horizontal or
 * vertical extention from a corner to another cell or a previously
 * defined channel boundary.  Split or merge tiles accordingly.
 *
 * The ti_client field of space tiles is used is a boolean flag
 * in order to distinguish between horizontal edges generated by
 * the original plane and horizontal edges defining channels.  This
 * is done in the new, generated plane--not in the original plane.
 *
 *     *********************************************************************
 *     * Copyright (C) 1985, 1990 Regents of the University of California. *
 *     * Permission to use, copy, modify, and distribute this              *
 *     * software and its documentation for any purpose and without        *
 *     * fee is hereby granted, provided that the above copyright          *
 *     * notice appear in all copies.  The University of California        *
 *     * makes no representations about the suitability of this            *
 *     * software for any purpose.  It is provided "as is" without         *
 *     * express or implied warranty.  Export of this software outside     *
 *     * of the United States of America may require an export license.    *
 *     *********************************************************************
 */

#ifndef lint
static char rcsid[] __attribute__ ((unused)) = "$Header: /usr/cvsroot/magic-8.0/router/rtrDcmpose.c,v 1.1.1.1 2008/02/03 20:43:50 tim Exp $";
#endif  /* not lint */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/times.h>

#include "utils/magic.h"
#include "textio/textio.h"
#include "utils/geometry.h"
#include "utils/geofast.h"
#include "tiles/tile.h"
#include "utils/hash.h"
#include "database/database.h"
#include "windows/windows.h"
#include "utils/main.h"
#include "dbwind/dbwind.h"
#include "utils/heap.h"
#include "utils/undo.h"
#include "router/router.h"
#include "router/rtrDcmpose.h"
#include "gcr/gcr.h"
#include "grouter/grouter.h"
#include "utils/netlist.h"
#include "utils/styles.h"
#include "utils/malloc.h"
#include "netmenu/netmenu.h"
#include "debug/debug.h"

/* C99 compat */
#include "router/routerInt.h"

/* The following tile types are used during channel decomposition */
#define	CELLTILE	1	/* Cell tile -- no channels here */
#define	USERCHAN	2	/* User-defined channel */

bool rtrDidInit = FALSE;	/* TRUE when rtrTileToChannel initialized */

/* Area being routed; set in RtrDecompose */
Rect RouteArea;

/* Forward declarations */
extern int rtrSrCells();
extern void rtrRoundRect();
extern void rtrHashKill();
extern void rtrSplitToArea();
extern void rtrMarkChannel();
extern void rtrMerge();

bool rtrUseCorner();

/*
 * ----------------------------------------------------------------------------
 *
 * RtrDecomposeName --
 *
 * Interface to commands module; perform channel decomposition
 * over the area 'area', as though we would be routing the netlist
 * with the name 'name'.  If 'name' is NULL, don't assume any
 * netlist; if it is the string "-", use the current netlist.
 *
 * Results:
 *	Pointer to the def holding the decomposed channel tiles.  If
 *	the area is too small to be useful, returns NULL.
 *
 * Side effects:
 *	See RtrDecompose().
 *
 * ----------------------------------------------------------------------------
 */

CellDef *
RtrDecomposeName(routeUse, area, name)
    CellUse *routeUse;	/* Cell to be decomposed */
    Rect *area;		/* Confine channels to this area */
    char *name;		/* Name of netlist if non-NULL; otherwise, use the
			 * name of the current netlist or that of routeUse
			 * as described above.
			 */
{
    NLNetList netList, *netListPtr = (NLNetList *) NULL;
    CellDef *def;

    if (name)
    {
	if (strcmp(name, "-") == 0)
	    name = routeUse->cu_def->cd_name;
	NMNewNetlist(name);

	if (NLBuild(routeUse, &netList) <= 0)
	    TxError("No nets in netlist.\n");
	else
	    netListPtr = &netList;
    }

    def = RtrDecompose(routeUse, area, netListPtr);

    /* Clean up global routing information */
    if (netListPtr)
	NLFree(netListPtr);

    return (def);
}

/*
 * ----------------------------------------------------------------------------
 *
 * RtrDecompose --
 *
 * Top level function of the channel decomposition code.  Initialize
 * and then enumerate subcells of the edit cell for processing.
 * Channels can currently appear only in empty space where there
 * are no subcells.
 *
 * The list of all nets to route is pointed to by 'netList'; this
 * will eventually be used when support for over-cell channels is
 * put back in.
 *
 * Results:
 *	Pointer to the def holding the decomposed channel tiles.  If
 *	the area is too small to be useful, returns NULL.
 *
 * Side effects:
 *	The DRC error plane of the returned cell def is marked with space
 *	tiles [ NO LONGER MAXIMAL HORIZONTAL ] representing channels.
 *	Modifies area to round it down to even grid points.  Modifies
 *	RouteArea to hold final routing area.
 *
 * ----------------------------------------------------------------------------
 */

CellDef *
RtrDecompose(routeUse, area, netList)
    CellUse *routeUse;
    Rect *area;
    NLNetList *netList;
{
    SearchContext scx;
    CellDef *cdTo;
    int tmp;

    /*
     * Redoing the channel structure invalidates the RtrTileToChannel table.
     * Reinitialize the hash table before proceeding.
     */
    if (rtrDidInit) rtrHashKill(&RtrTileToChannel);
    HashInit(&RtrTileToChannel, 128, 1);
    rtrDidInit = TRUE;

    /*
     * Round area up so that its edges are at the canonical places
     * halfway between grid points.
     */
    tmp = RTR_GRIDUP(area->r_xtop, RtrOrigin.p_x) - RtrGridSpacing/2;
    if (tmp < area->r_xtop) area->r_xtop = tmp + RtrGridSpacing;
    else area->r_xtop = tmp;
    tmp = RTR_GRIDUP(area->r_xbot, RtrOrigin.p_x) - RtrGridSpacing/2;
    if (tmp > area->r_xbot) area->r_xbot = tmp - RtrGridSpacing;
    else area->r_xbot = tmp;
    tmp = RTR_GRIDUP(area->r_ytop, RtrOrigin.p_y) - RtrGridSpacing/2;
    if (tmp < area->r_ytop) area->r_ytop = tmp + RtrGridSpacing;
    else area->r_ytop = tmp;
    tmp = RTR_GRIDUP(area->r_ybot, RtrOrigin.p_y) - RtrGridSpacing/2;
    if (tmp > area->r_ybot) area->r_ybot = tmp - RtrGridSpacing;
    else area->r_ybot = tmp;
    RouteArea = *area;
    if (GEO_RECTNULL(area)) return NULL;

    cdTo = RtrFindChannelDef();

    /*
     * Paint non-space tiles in both the DRC check and error planes where
     * cells are in the source def.  Pass the search area to rtrSrCells()
     * vi the global RouteArea.  The code in rtrSrCells() takes care of
     * leaving empty space wherever there are __CHANNEL__ labels.
     *
     * We make two copies of the channel information because it isn't
     * safe to be both searching and updating the same plane.  Thus,
     * one plane (DRC check) is used for searching, but updates are
     * made in the other plane.
     */
    UndoDisable();
    DBClearPaintPlane(cdTo->cd_planes[PL_DRC_ERROR]);
    DBClearPaintPlane(cdTo->cd_planes[PL_DRC_CHECK]);

    scx.scx_use = routeUse;
    scx.scx_area = RouteArea;
    scx.scx_trans = GeoIdentityTransform;
    (void) DBCellSrArea(&scx, rtrSrCells, (ClientData) cdTo);

    /* Split space tiles to the edges of the routing area */
    rtrSplitToArea(&RouteArea, cdTo);

    /*
     * Clear the valid flags for horizontal edges for all space tiles in
     * the error plane of the result cell.
     */
    (void) DBSrPaintArea((Tile *) NULL, cdTo->cd_planes[PL_DRC_ERROR],
		&RouteArea, &DBAllTypeBits, rtrSrClear,
		(ClientData) &RouteArea);

    /*
     * Enumerate all tiles in the given area.
     * If a tile is not a space tile, then perform the corner
     * extension algorithm.
     */
    (void) DBSrPaintArea((Tile *) NULL, cdTo->cd_planes[PL_DRC_CHECK],
		&RouteArea, &DBAllTypeBits, rtrSrFunc,
		(ClientData) (cdTo->cd_planes[PL_DRC_ERROR]));

    /* Allow the modified area to be redisplayed if the cell is visible */
    DBReComputeBbox(cdTo);
    DBWAreaChanged(cdTo, &RouteArea, DBW_ALLWINDOWS, &DBAllButSpaceBits);
    UndoEnable();

    return (cdTo);
}

/*
 * ----------------------------------------------------------------------------
 *
 * RtrFindChannelDef --
 *
 * Return a pointer to the __CHANNEL__ cell def that holds the
 * channel structure.  Creates this cell if it doesn't exist
 *
 * Results:
 *	Pointer to the __CHANNEL__ def.
 *
 * Side effects:
 *	May create the __CHANNEL__ def if it doesn't already exist.
 *	If it creates the def, marks it as CDINTERNAL.
 *
 * ----------------------------------------------------------------------------
 */

CellDef *
RtrFindChannelDef()
{
    CellDef *def;

    /* Create our target cell */
    if ((def = DBCellLookDef("__CHANNEL__")) == (CellDef *) NULL)
    {
        def = DBCellNewDef("__CHANNEL__");
	DBCellSetAvail(def);
	def->cd_flags |= CDINTERNAL;
    }

    return (def);
}

/*
 * ----------------------------------------------------------------------------
 *
 * rtrSrCells --
 *
 * Paints a silhouette of the cell tile plane.  For each cell, paint
 * error paint into the error plane of the CellDef 'def'.  Clip any
 * paints to the global RouteArea.  If the cell is an array, enumerate
 * each of its instances separately; this allows connections on interior
 * edges of the array.
 *
 * Results:
 *	Returns 0 to keep DBCellSrArea from aborting the search.
 *
 * Side effects:
 *	Paints into both the DRC check and DRC error planes of the celldef
 *	given by 'def'.  The area of each cell is expanded before painting,
 *	out to points midway between grid lines.  The points are chosen
 *	so that any routing on grid lines outside the painted area will
 *	be far enough from the cell not to cause design-rule violations
 *	(this distance is determined by RtrSubcellSep).  In addition, one
 *	extra grid line is left along side cells to jog terminals over
 *	to grid points.
 *
 * ----------------------------------------------------------------------------
 */

int
rtrSrCells(scx, targetDef)
    SearchContext *scx;	/* The cell to be painted */
    CellDef *targetDef;	/* The def into which the silhouette is painted */
{
    CellDef *def = scx->scx_use->cu_def;
    Rect rootBbox, gridBbox;

    /*
     * Transform the enumerated cell use outlines to get the outline
     * of the cell within its parent.
     */
    RtrMilestonePrint();
    GeoTransRect(&scx->scx_trans, &def->cd_bbox, &rootBbox);

    /*
     * First, move down the bottom and left boundaries of the cell
     * to a safe point midway between grid lines.
     */
    gridBbox = rootBbox;
    rtrRoundRect(&gridBbox, RtrSubcellSepUp, RtrSubcellSepDown, TRUE);

    /* Clip to the routing area and paint into the channel planes */
    GeoClip(&gridBbox, &RouteArea);
    (void) DBPaintPlane(targetDef->cd_planes[PL_DRC_CHECK], &gridBbox,
		DBStdWriteTbl(CELLTILE), (PaintUndoInfo *) NULL);
    (void) DBPaintPlane(targetDef->cd_planes[PL_DRC_ERROR], &gridBbox,
		DBStdWriteTbl(CELLTILE), (PaintUndoInfo *) NULL);
    return (0);
}

/*
 * ----------------------------------------------------------------------------
 *
 * rtrRoundRect --
 *
 * Round a rectangle out to the nearest grid line, and
 * extend to a point halfway to the next grid point (if
 * doRoundUp is TRUE) or back half a grid from the nearest
 * grid line (if doRoundUp is FALSE).
 *
 * The halfway points are chosen to be RtrGridSpacing/2
 * down or to the left from grid lines.  Before rounding,
 * we add sepUp to the top and right, and sepDown to the
 * bottom and left.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Modifies 'r' as indicated above.
 *
 * ----------------------------------------------------------------------------
 */

void
rtrRoundRect(r, sepUp, sepDown, doRoundUp)
    Rect *r;
    int sepUp, sepDown;
    bool doRoundUp;
{
    int halfGrid = RtrGridSpacing / 2;

    r->r_xbot = RTR_GRIDDOWN(r->r_xbot - sepDown, RtrOrigin.p_x);
    r->r_ybot = RTR_GRIDDOWN(r->r_ybot - sepDown, RtrOrigin.p_y);
    if (doRoundUp)
    {
	r->r_xbot -= halfGrid;
	r->r_ybot -= halfGrid;
    }
    else
    {
	r->r_xbot += RtrGridSpacing - halfGrid;
	r->r_ybot += RtrGridSpacing - halfGrid;
    }

    /*
     * Move up the top and right boundaries. Note:  it's important
     * that we always SUBTRACT halfgrid from a grid point rather
     * than adding sometimes:  if RtrGridSpacing is odd, then adding
     * and subtracting give different results.
     */
    r->r_xtop = RTR_GRIDUP(r->r_xtop + sepUp, RtrOrigin.p_x);
    r->r_ytop = RTR_GRIDUP(r->r_ytop + sepUp, RtrOrigin.p_y);
    if (doRoundUp)
    {
	r->r_xtop += RtrGridSpacing - halfGrid;
	r->r_ytop += RtrGridSpacing - halfGrid;
    }
    else
    {
	r->r_xtop -= halfGrid;
	r->r_ytop -= halfGrid;
    }
}

/*
 * ----------------------------------------------------------------------------
 *
 * rtrHashKill --
 *
 * Free the remaining storage in channels in the hash table.
 * Kill the table.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Memory gets freed.  The global RtrTileToChannel gets cleared.
 *
 * ----------------------------------------------------------------------------
 */

void
rtrHashKill(ht)
    HashTable *ht;
{
    HashEntry *he;
    HashSearch hs;

    HashStartSearch(&hs);
    while ((he = HashNext(ht, &hs)))
	GCRFreeChannel((GCRChannel *) HashGetValue(he));
    HashKill(ht);
}

/*
 * ----------------------------------------------------------------------------
 *
 * rtrSplitToArea --
 *
 * Clip space tiles to the edges of the (given) routing area.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Changes tiles in the data base.
 *
 * ----------------------------------------------------------------------------
 */

void
rtrSplitToArea(area, def)
    Rect *area;		/* Routing area */
    CellDef *def;	/* Def holding routing results */
{
    Tile *tile;
    Point p;

    /*
     * First split top and bottom space tiles, if any.
     * Note, there is at most one space tile spanning the top
     * of the routing area, due to the horizontal strip property
     * plus the earlier clipping of cell tiles to the routing area.
     */
    p = area->r_ur;
    tile = TiSrPoint((Tile *) NULL, def->cd_planes[PL_DRC_ERROR], &p);
    if ((TOP(tile) > area->r_ytop) && (BOTTOM(tile) < area->r_ytop))
	(void) TiSplitY(tile, area->r_ytop);

    p.p_y = area->r_ll.p_y - 1;
    tile = TiSrPoint((Tile *) NULL, def->cd_planes[PL_DRC_ERROR], &p);
    if ((BOTTOM(tile) < area->r_ybot) && (TOP(tile) > area->r_ybot))
	tile = TiSplitY(tile, area->r_ybot);

    /*
     * Search up the left edge of the routing area,
     * looking for space tiles spanning the edge.
     * If found, split them.
     */
    p = area->r_ll;
    while (p.p_y < area->r_ytop)
    {
	tile = TiSrPoint(tile, def->cd_planes[PL_DRC_ERROR], &p);
	if ((LEFT(tile) < p.p_x) && (RIGHT(tile) > p.p_x))
	    tile = TiSplitX(tile, p.p_x);
	p.p_y = TOP(tile);
    }

    /* Do the right edge of the routing area in the same manner */
    p.p_x = area->r_xtop;
    p.p_y = area->r_ybot;
    while (p.p_y < area->r_ytop)
    {
	tile = TiSrPoint(tile, def->cd_planes[PL_DRC_ERROR], &p);
	if ((LEFT(tile) < p.p_x) && (RIGHT(tile) > p.p_x))
	    tile = TiSplitX(tile, p.p_x);
	p.p_y = TOP(tile);
    }
}

/*
 * ----------------------------------------------------------------------------
 *
 * rtrSrClear --
 *
 * DBSrPaintArea function for each tile in the error plane of the __CHANNEL__
 * def.  Sets the flags to 0 in internal space tiles, marking horizontal
 * invalid.  Mark edges at the boundary of the routing region as valid.
 *
 * Results:
 *	Always returns 0.
 *
 * Side effects:
 *	Sets flags in tiles.
 *
 * ----------------------------------------------------------------------------
 */

int
rtrSrClear(tile, area)
    Tile *tile;
    Rect *area;
{
    /* Clear all */
    rtrCLEAR(tile, -1);

    if (TiGetBody(tile) == (ClientData) NULL)
    {
	/* Mark horizontal edges touching box */
	if (TOP(tile) == area->r_ytop)
	{
	    /* Mark top */
	    rtrMARK(tile, rtrNW);
	    rtrMARK(tile, rtrNE);
	}
	if (BOTTOM(tile) == area->r_ytop)
	{
	    /* Mark bottom */
	    rtrMARK(tile, rtrSW);
	    rtrMARK(tile, rtrSE);
	}
    }
    else
    {
	/* Mark all flags in a non-space tile */

	    /* Mark top */
	rtrMARK(tile, rtrNW);
	rtrMARK(tile, rtrNE);

	    /* Mark bottom */
	rtrMARK(tile, rtrSW);
	rtrMARK(tile, rtrSE);
    }

    return (0);
}

/*
 * ----------------------------------------------------------------------------
 *
 * rtrSrFunc --
 *
 * Search function called from DBSrPaintArea for each tile in the
 * plane.  Do this search in the OLD TILE PLANE.  Process corners
 * bordering space tiles.
 *
 * Results:
 *	Returns a 0 to DBSrPaintArea so it won't abort the search.
 *
 * Side effects:
 *	Modifies the result plane to reflect the channel structure.
 *
 * ----------------------------------------------------------------------------
 */

int
rtrSrFunc(tile, plane)
    Tile *tile;		/* Candidate cell tile */
    Plane *plane;	/* Plane in which searches take place */
{
    Tile *tiles[3];
    Point p;

    /* Ignore space tiles */
    if (TiGetBody(tile) == (ClientData) NULL)
	return (0);

    /*
     * Check each corner of this cell tile to see if it is convex,
     * and no marked boundary is incident upon it.
     */
    p = tile->ti_ll;
    if (rtrUseCorner(&p, rtrSW, plane, tiles))
	rtrMarkChannel(plane, tiles, &p, rtrSW);

    p.p_y = TOP(tile);
    if (rtrUseCorner(&p, rtrNW, plane, tiles))
	rtrMarkChannel(plane, tiles, &p, rtrNW);

    p.p_x = RIGHT(tile);
    if (rtrUseCorner(&p, rtrNE, plane, tiles))
	rtrMarkChannel(plane, tiles, &p, rtrNE);

    p.p_y = BOTTOM(tile);
    if (rtrUseCorner(&p, rtrSE, plane, tiles))
	rtrMarkChannel(plane, tiles, &p, rtrSE);

    return (0);
}

/*
 * ----------------------------------------------------------------------------
 *
 * rtrUseCorner --
 *
 * Search for legal corners upon which to apply the channel definition
 * algorithm.  Check both horizontal tiles for markings, since only
 * one (the shorter) might be marked.
 *
 * Results:
 *	Return FALSE if the corner is not convex or a legal boundary already
 *	extends from the corner.  Otherwise return TRUE.
 *
 * Side effects:
 * 	Return pointers to space tiles adjacent to the corner.
 *		tiles[0] is not modified by this routine.
 *		tiles[1] is the spanning tile above or below the corner.
 *		tiles[2] is the side tile left or right of the corner.
 *
 * ----------------------------------------------------------------------------
 */

bool
rtrUseCorner(point, corner, plane, tiles)
    Point *point;	/* Point at which a cell corner is found */
    int corner;		/* Selects NE, NW, SE, or SW cell corner */
    Plane *plane;	/* Plane to be searched for tiles */
    Tile *tiles[];	/* Return pointers to found space tiles */
{
    Point  p0, p1;
    Tile * tile;

    /* Reject a corner if it lies on the boundary of the routing area */
    if (point->p_x <= RouteArea.r_xbot
	    || point->p_x >= RouteArea.r_xtop
	    || point->p_y <= RouteArea.r_ybot
	    || point->p_y >= RouteArea.r_ytop)
    {
	return (FALSE);
    }

    /*
     * Search the area above (below) the corner.  If two space tiles, then a
     * vertical boundary marks a channel edge.  If one top (bottom) tile and
     * one side tile, and the horizontal edge is not marked, then the corner
     * is okay.
     */
    p1 = p0 = *point;
    switch (corner)
    {
	case rtrNE:
	    p1.p_y--;
	    break;
	case rtrNW:
	    p1.p_x--;
	    p1.p_y--;
	    break;
	case rtrSE:
	    p0.p_y--;
	    break;
	case rtrSW:
	    p0.p_y--;
	    p1.p_x--;
	    break;
	default:
	    ASSERT(FALSE, "rtrUseCorner corner botch");
	    break;
    }

    tile = tiles[1] = TiSrPoint((Tile *) NULL, plane, &p0);
    if( (TiGetBody(tile) != (ClientData) NULL) || (LEFT(tile) == point->p_x)
	|| (RIGHT(tile) == point->p_x) )
	return(FALSE);	/* Vertical boundary at corner */

    tile = tiles[2] = TiSrPoint((Tile *) NULL, plane, &p1);
    if(TiGetBody(tile) != (ClientData) NULL)
	return(FALSE);	/* Not a corner */

    switch(corner)
    {
	case rtrNE: return(!rtrMARKED(tile, rtrNW));  break;
	case rtrNW: return(!rtrMARKED(tile, rtrNE));  break;
	case rtrSE: return(!rtrMARKED(tile, rtrSW));  break;
	case rtrSW: return(!rtrMARKED(tile, rtrSE));  break;
    }
    return(FALSE);
}

/*
 * ----------------------------------------------------------------------------
 *
 * rtrMarkChannel --
 *
 * Find the shortest segment from the corner to another boundary.
 * Split and merge space tiles to reflect channel structure.  Update
 * edge status in the tile plane.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Modifies the result plane to reflect channel definition.
 *
 * ----------------------------------------------------------------------------
 */

void
rtrMarkChannel(plane, tiles, point, corner)
    Plane *plane;	/* Plane for searching */
    Tile *tiles[];	/* Bordering space tiles */
    Point *point;	/* Coordinates of corner */
    int	corner;		/* Corner of tile to process */
{
    int xDist, yDist, d1, d2, lastY;
    Tile *tile, *new;
    Point curPt;
    bool pos;

    pos = ((corner == rtrNE) || (corner == rtrSE));
    xDist = rtrXDist(tiles, point->p_x, pos);
    yDist = rtrYDist(tiles, point, ((corner==rtrNE) || (corner==rtrNW)), plane);

    if (xDist < yDist)	/* Choose and mark the horizontal boundary */
    {
	if(pos)
	{
	    d1 = RIGHT(tiles[1]);
	    d2 = RIGHT(tiles[2]);
	    if(corner == rtrNE)
	    {
		rtrMARK(tiles[2], rtrNW);
		if(d1 >= d2) rtrMARK(tiles[2], rtrNE);
		if(d1 <= d2) rtrMARK(tiles[1], rtrSE);
	    }
	    else
	    {
		rtrMARK(tiles[2], rtrSW);
		if(d1 >= d2) rtrMARK(tiles[2], rtrSE);
		if(d1 <= d2) rtrMARK(tiles[1], rtrNE);
	    }
	}
	else
	{
	    d1 = LEFT(tiles[1]);
	    d2 = LEFT(tiles[2]);
	    if(corner == rtrNW)
	    {
		rtrMARK(tiles[2], rtrNE);
		if(d1 >= d2) rtrMARK(tiles[2], rtrNW);
		if(d1 <= d2) rtrMARK(tiles[1], rtrSW);
	    }
	    else
	    {
		rtrMARK(tiles[2], rtrSE);
		if(d1 >= d2) rtrMARK(tiles[2], rtrSW);
		if(d1 <= d2) rtrMARK(tiles[1], rtrNW);
	    }
	}
    }
    else	/* Choose the vertical boundary */
    {
	/*
	 * Split a sequence of space tiles starting with tiles[0]
	 * (the bottom tile), for yDist at the point->p_y.
	 * Merge tiles where possible.
	 */
	tile=tiles[0];
	curPt.p_x=point->p_x;
	curPt.p_y=BOTTOM(tile);

	lastY = point->p_y;
	if((corner == rtrNW) || (corner == rtrNE)) lastY += yDist;

	while(TRUE)
	{
	    ASSERT(TiGetBody(tile) ==  (ClientData)NULL,
		    "rtrMerge:  merge cell tile");
	    new = TiSplitX(tile, curPt.p_x);
	    ASSERT(TiGetBody(new) == (ClientData)NULL, "rtrMerge:  merge cell new");

	/* Fix horizontal flags in 'new' tile and (old) 'tile' tile */
	    if (rtrMARKED(tile,rtrNE)) rtrMARK(new,rtrNE);
	    else rtrCLEAR(new,rtrNE);
	    if (rtrMARKED(tile,rtrSE)) rtrMARK(new,rtrSE);
	    else rtrCLEAR(new,rtrNE);

	    /*
	     * Clear these flags:
	     * couldn't cross the boundary unless it was clear.
	     */
	    rtrCLEAR(new, rtrNW);
	    rtrCLEAR(new, rtrSW);
	    rtrCLEAR(tile, rtrNE);
	    rtrCLEAR(tile, rtrSE);

	    /* Merge tile and new with lower neighbors if possible */
	    rtrMerge(new, LB(new), plane);
	    rtrMerge(tile, LB(tile), plane);

	    /* Find next (higher) tile to split */
	    if (TOP(tile) >= lastY) break;
	    curPt.p_y = TOP(tile);
	    tile=TiSrPoint(tile, plane, &curPt);
	}

	/* Merge new and tile with upper neighbors if possible */
        rtrMerge(RT(new), new, plane);
        rtrMerge(RT(tile), tile, plane);
    }
}

/*
 * ----------------------------------------------------------------------------
 *
 * rtrYDist --
 *
 * Finds the distance from a point to an upper or lower channel boundary.
 *
 * Results:
 *	The integer distance from the point to the boundary.
 *
 * Side effects:
 *	Return a pointer to the bottom tile in the split sequence.
 *
 * ----------------------------------------------------------------------------
 */

int
rtrYDist(tiles, point, up, plane)
    Tile *tiles[];	/* Start tile in [1].  Put bottom tile in [0] */
    Point *point;	/* Point from which distance is measure */
    bool up;		/* TRUE if search up, FALSE if down */
    Plane *plane;	/* Cell plane for search */
{
    Tile *current = tiles[1], *next;
    int x, yStart, flag;
    Point p;

    p = *point;
    x = p.p_x;
    yStart = p.p_y;

    for (;;)
    {
	if (up)
	{
	    p.p_y = TOP(current);
	    if (p.p_y >= RouteArea.r_ytop) break;
	}
	else
	{
	    p.p_y = BOTTOM(current);
	    if (p.p_y <= RouteArea.r_ybot) break;
	    p.p_y--;
	}

	/*
	 * See if we ran into a cell tile.  Since the cell tile defines
	 * the boundary of a channel, terminate the search.  If going
	 * down, reset the y coordinate to the bottom of the last good
	 * channel.
	 */
	next = TiSrPoint(current, plane, &p);
	if (TiGetBody(next) != (ClientData) NULL)
	{
	    if (!up) p.p_y++;
	    break;
	}

	/* Done if a vertical boundary */
	if (LEFT(next) == x || RIGHT(next) == x)
	    break;

	/*
	 * Classify as one of the following cases:
	 *
	 * __|_n_|__   |___c___|   __|_n__|   |__ c|__   |__n|__   __|_c__|
	 * |   c   |     | n |     |   c|       | n  |     |c  |   |   n|
	 *    (A)         (B)         (C)        (D)	   (E)        (F)
	 */
	if (LEFT(current) < LEFT(next))
	{
	    if (RIGHT(current) > RIGHT(next))
	    {
		 if (up) flag = rtrMARKED(next, rtrSW);		/*(A)*/
		 else flag = rtrMARKED(next, rtrNW);		/*(B)*/
	    }
	    else
	    {
		 if (up) flag = rtrMARKED(current, rtrNE);	/*(C)*/
		 else flag = rtrMARKED(current, rtrSE);		/*(D)*/
	    }
	}
	else
	{
	    if (up) flag = rtrMARKED(current, rtrNW);		/*(E)*/
	    else flag = rtrMARKED(current, rtrSW); 		/*(F)*/
	}

	if (flag)
	{
	    if (!up) p.p_y = BOTTOM(current);
	    break;
	}
	current = next;
    }

    if (up)
    {
	tiles[0] = tiles[1];
	return (p.p_y - yStart);
    }
    else
    {
	tiles[0] = current;
	return (yStart - p.p_y);
    }
}

/*
 * ----------------------------------------------------------------------------
 *
 * rtrXDist --
 *
 * 	Finds the distance from a point to a left or right channel boundary.
 *
 * Results:
 *	The integer distance from the point to the boundary.
 *
 * Side effects:
 *	None.
 *
 * ----------------------------------------------------------------------------
 */

int
rtrXDist(tiles, x, isRight)
    Tile *tiles[];	/* Space tiles bordering the corner */
    int x;		/* Starting x for distance calculation */
    bool isRight;		/* TRUE if right, FALSE if left */
{
    int l0, l1;

    if (isRight)
        l0 = RIGHT(tiles[1]) - x, l1 = RIGHT(tiles[2]) - x;
    else
	l0 = x - LEFT(tiles[1]), l1 = x - LEFT(tiles[2]);

    return (MIN(l0, l1));
}

/*
 * ----------------------------------------------------------------------------
 *
 * rtrMerge --
 *
 * 	Merge two space tiles provided they share a common horizontal edge.
 *	The upper is the first argument tile.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Updates the horizontal flags in the resulting tile.
 *
 * ----------------------------------------------------------------------------
 */

void
rtrMerge(tup, tdn, plane)
    Tile *tup, *tdn;
    Plane *plane;
{
    Tile *side;

    /* Skip if either is a cell tile */
    if (TiGetBody(tup) != (ClientData) NULL
		|| TiGetBody(tdn) != (ClientData) NULL)
	return;

    if (LEFT(tdn) != LEFT(tup) || RIGHT(tdn) != RIGHT(tup))
	return;

    /*
     * Set flags for the result.
     * Relies on TiJoinY to preserve the first arg as the composite tile.
     */
    ASSERT( (BOTTOM(tdn)>=RouteArea.r_ybot) && (TOP(tup)<=RouteArea.r_ytop),
	    "rtrMerge:  merging with a tile outside the routing area");

    if (rtrMARKED(tdn, rtrSW)) rtrMARK(tup, rtrSW); else rtrCLEAR(tup, rtrSW);
    if (rtrMARKED(tdn, rtrSE)) rtrMARK(tup, rtrSE); else rtrCLEAR(tup, rtrSE);
    TiJoinY(tup, tdn, plane);

    /*
     * Merge sideways if the result of the join matches a tile on either side,
     * provided the neighbor is a space tile and is inside the routing area.
     */
    side = BL(tup);
    if (TiGetBody(side) == (ClientData) NULL
		&& LEFT(side) >= RouteArea.r_xbot
		&& TOP(side) == TOP(tup)
		&& BOTTOM(side) == BOTTOM(tup))
	TiJoinX(tup, side, plane);

    side = TR(tup);
    if (TiGetBody(side) == (ClientData) NULL
		&& RIGHT(side) <= RouteArea.r_xtop
		&& TOP(side) == TOP(tup)
		&& BOTTOM(side) == BOTTOM(tup))
	TiJoinX(tup, side, plane);
}
