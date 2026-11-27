/*
 * Paths over the nav mesh: A* between areas, then a list of points through the area portals.
 * Copyright (C) 2026 fteqw-sv contributors. GPLv2 or later; see nb_local.h.
 */
#include "nb_local.h"

#ifndef CLIENTONLY

static unsigned int *nb_heap;
static unsigned int nb_heapsize, nb_heapmax;

static void NBHeap_Push(unsigned int a)
{
	unsigned int i;
	if (nb_heapsize >= nb_heapmax)
	{
		nb_heapmax = nb_heapmax ? nb_heapmax * 2 : 1024;
		nb_heap = BZ_Realloc(nb_heap, sizeof(*nb_heap) * nb_heapmax);
	}
	i = nb_heapsize++;
	while (i > 0)
	{
		unsigned int p = (i - 1) / 2;
		if (nb_mesh.areas[nb_heap[p]].f <= nb_mesh.areas[a].f)
			break;
		nb_heap[i] = nb_heap[p];
		nb_mesh.areas[nb_heap[i]].heapindex = i;
		i = p;
	}
	nb_heap[i] = a;
	nb_mesh.areas[a].heapindex = i;
}

static void NBHeap_Up(unsigned int i)
{
	unsigned int a = nb_heap[i];
	while (i > 0)
	{
		unsigned int p = (i - 1) / 2;
		if (nb_mesh.areas[nb_heap[p]].f <= nb_mesh.areas[a].f)
			break;
		nb_heap[i] = nb_heap[p];
		nb_mesh.areas[nb_heap[i]].heapindex = i;
		i = p;
	}
	nb_heap[i] = a;
	nb_mesh.areas[a].heapindex = i;
}

static unsigned int NBHeap_Pop(void)
{
	unsigned int top = nb_heap[0], last, i = 0;
	last = nb_heap[--nb_heapsize];
	while (1)
	{
		unsigned int l = i * 2 + 1, r = l + 1, m = i;
		float fm = nb_mesh.areas[last].f;
		if (l < nb_heapsize && nb_mesh.areas[nb_heap[l]].f < fm)
		{
			m = l;
			fm = nb_mesh.areas[nb_heap[l]].f;
		}
		if (r < nb_heapsize && nb_mesh.areas[nb_heap[r]].f < fm)
			m = r;
		if (m == i)
			break;
		nb_heap[i] = nb_heap[m];
		nb_mesh.areas[nb_heap[i]].heapindex = i;
		i = m;
	}
	if (nb_heapsize)
	{
		nb_heap[i] = last;
		nb_mesh.areas[last].heapindex = i;
	}
	return top;
}

static float NB_DefaultCost(const nbarea_t *from, const nbarea_t *to, const nblink_t *link)
{
	float d = Distance(from->center, to->center);
	float c = d + link->cost;
	if (link->type == NBLINK_TELEPORT)
		c = 64 + link->cost;
	if (to->flags & (NBAREA_LAVA | NBAREA_SLIME))
		c += 2000;
	if (to->flags & NBAREA_HURT)
		c += 3000;
	if (to->flags & NBAREA_WATER)
		c += d;   /* swimming is slow */
	if (link->type == NBLINK_DROP)
	{
		float h = from->center[2] - to->center[2];
		if (h > 200)
			c += (h - 200) * 4;   /* fall damage */
	}
	return c;
}

/* A* from area s to area g. team: 1..4 for team-blocked areas (0 = none). Returns false if no path */
static qboolean NB_AStar(int s, int g, int team, nbcostfn_t cost, void *ctx, float maxcost)
{
	unsigned int searchid = ++nb_mesh.searchid;
	unsigned int teambit = (team >= 1 && team <= 4) ? (1u << (team - 1)) : 0;
	nb_heapsize = 0;
	nb_mesh.areas[s].g = 0;
	nb_mesh.areas[s].f = Distance(nb_mesh.areas[s].center, nb_mesh.areas[g].center);
	nb_mesh.areas[s].parent = ~0u;
	nb_mesh.areas[s].openid = searchid;
	NBHeap_Push(s);
	while (nb_heapsize)
	{
		unsigned int cur = NBHeap_Pop(), l;
		nbarea_t *ca = &nb_mesh.areas[cur];
		if (cur == (unsigned int)g)
			return true;
		ca->closedid = searchid;
		for (l = 0; l < ca->numlinks; l++)
		{
			nblink_t *link = &nb_mesh.links[ca->firstlink + l];
			nbarea_t *na = &nb_mesh.areas[link->to];
			float step, ng;
			/* standing in a teleporter that works for us: the only way on is through it */
			if (nb_mesh.areatele && link->type != NBLINK_TELEPORT && cur != (unsigned int)s
				&& (nb_mesh.areatele[cur] & (0x10 | (teambit ? teambit : 0xf))))
				continue;
			if (na->closedid == searchid)
				continue;
			if (teambit && (na->teamblock & teambit) && link->to != (unsigned int)g)
				continue;
			if (link->to != (unsigned int)g && NBNav_DoorShut(link->to))
				continue;   /* a shut door only a button or a shot opens */
			if (teambit && nb_mesh.linkteams && (nb_mesh.linkteams[ca->firstlink + l] & teambit))
				continue;   /* a teleporter this team can't use */
			step = cost ? cost(ctx, ca, na, link) : NB_DefaultCost(ca, na, link);
			if (step < 0)
				continue;
			ng = ca->g + step;
			if (maxcost > 0 && ng > maxcost)
				continue;
			if (na->openid == searchid)
			{
				if (ng >= na->g)
					continue;
				na->g = ng;
				na->f = ng + Distance(na->center, nb_mesh.areas[g].center);
				na->parent = cur;
				na->parentlink = l;
				NBHeap_Up(na->heapindex);
			}
			else
			{
				na->openid = searchid;
				na->g = ng;
				na->f = ng + Distance(na->center, nb_mesh.areas[g].center);
				na->parent = cur;
				na->parentlink = l;
				NBHeap_Push(link->to);
			}
		}
	}
	return false;
}

/* point on the shared edge of a and b, closest to 'from' */
static void NB_PortalPoint(const nbarea_t *a, const nbarea_t *b, const vec3_t from, vec3_t out, int how)
{
	float lo, hi;
	float maxup = how == NBLINK_WALK ? NB_STEPSIZE + 4 : (how == NBLINK_JUMP ? NB_JUMPHEIGHT : 99999);
	/* overlap in x and y; areas touch along one axis */
	float ox0 = max(a->mins[0], b->mins[0]), ox1 = min(a->maxs[0], b->maxs[0]);
	float oy0 = max(a->mins[1], b->mins[1]), oy1 = min(a->maxs[1], b->maxs[1]);
	if (ox1 - ox0 >= oy1 - oy0)
	{	/* shared edge runs along x */
		lo = ox0 + 12; hi = ox1 - 12;
		if (lo > hi) lo = hi = (ox0 + ox1) * 0.5f;
		out[0] = bound(lo, from[0], hi);
		out[1] = (oy0 + oy1) * 0.5f;
		/* merged areas only connect where the node links were: a floor strip beside a ramp shares a long
		   edge with it but can only step onto it where the ramp is low. Cross where the climb is possible. */
		if (NBNav_FloorZ(b, out[0], out[1]) - NBNav_FloorZ(a, out[0], out[1]) > maxup)
		{
			float x, bestd = 1e9, bx = out[0];
			for (x = lo; x <= hi; x += 8)
				if (NBNav_FloorZ(b, x, out[1]) - NBNav_FloorZ(a, x, out[1]) <= maxup && fabs(x - from[0]) < bestd)
				{
					bestd = fabs(x - from[0]);
					bx = x;
				}
			out[0] = bx;
		}
	}
	else
	{
		lo = oy0 + 12; hi = oy1 - 12;
		if (lo > hi) lo = hi = (oy0 + oy1) * 0.5f;
		out[1] = bound(lo, from[1], hi);
		out[0] = (ox0 + ox1) * 0.5f;
		if (NBNav_FloorZ(b, out[0], out[1]) - NBNav_FloorZ(a, out[0], out[1]) > maxup)
		{
			float y, bestd = 1e9, by = out[1];
			for (y = lo; y <= hi; y += 8)
				if (NBNav_FloorZ(b, out[0], y) - NBNav_FloorZ(a, out[0], y) <= maxup && fabs(y - from[1]) < bestd)
				{
					bestd = fabs(y - from[1]);
					by = y;
				}
			out[1] = by;
		}
	}
	if (ox1 < ox0 || oy1 < oy0)
	{	/* not adjacent (jump/drop across a gap, teleport): aim at b's nearest point */
		out[0] = bound(b->mins[0] + 8, from[0], b->maxs[0] - 8);
		out[1] = bound(b->mins[1] + 8, from[1], b->maxs[1] - 8);
	}
	out[2] = NBNav_FloorZ(b, out[0], out[1]);
}

qboolean NBPath_Compute(nbpath_t *path, const vec3_t start, const vec3_t goal, int team, nbcostfn_t cost, void *ctx)
{
	int s, g, n, i;
	unsigned int chain[NB_MAX_PATH * 4];
	unsigned int cur;
	vec3_t prev;

	path->valid = false;
	path->numsegs = 0;
	path->cur = 0;
	path->computed = NB_Time();
	VectorCopy(goal, path->goal);
	if (!nb_mesh.numareas)
		return false;
	s = NBNav_AreaAt(start, 96);
	if (s < 0 && (World_PointContentsWorldOnly(&sv.world, (float*)start) & (FTECONTENTS_WATER | FTECONTENTS_SLIME)))
		s = NBNav_AreaAt(start, 600);   /* swimming: the floor is a long way down */
	if (s < 0)
		s = NBNav_NearestArea(start, 200);
	g = NBNav_AreaAt(goal, 160);
	if (g < 0)
		g = NBNav_NearestArea(goal, 400);
	if (s < 0 || g < 0)
		return false;
	if (s != g && !NB_AStar(s, g, team, cost, ctx, 0))
		return false;

	/* walk back */
	n = 0;
	for (cur = g; cur != (unsigned int)s && n < (int)countof(chain); cur = nb_mesh.areas[cur].parent)
		chain[n++] = cur;
	if (n >= (int)countof(chain))
		return false;
	VectorCopy(start, prev);
	prev[2] -= 24;
	/* chain is goal..first step; emit forwards */
	for (i = n - 1; i >= 0 && path->numsegs < NB_MAX_PATH - 1; i--)
	{
		nbarea_t *to = &nb_mesh.areas[chain[i]];
		nbarea_t *from = &nb_mesh.areas[nb_mesh.areas[chain[i]].parent];
		nblink_t *link = &nb_mesh.links[from->firstlink + to->parentlink];
		nbpathseg_t *seg;
		if (link->type == NBLINK_RUNJUMP && path->numsegs < NB_MAX_PATH - 2)
		{	/* both areas are single cells (the takeoff and landing spots) */
			vec3_t u, takeoff;
			takeoff[0] = from->center[0];
			takeoff[1] = from->center[1];
			takeoff[2] = NBNav_FloorZ(from, takeoff[0], takeoff[1]);
			VectorSet(u, to->center[0] - takeoff[0], to->center[1] - takeoff[1], 0);
			VectorNormalize(u);
			/* the points walked to near the takeoff (it is at an edge) are replaced by the run-up:
			   heading for them first can carry a running bot over the edge */
			while (path->numsegs > 0 && path->seg[path->numsegs - 1].how == NBLINK_WALK && path->seg[path->numsegs - 1].area >= 0
				&& (path->seg[path->numsegs - 1].pos[0] - takeoff[0]) * (path->seg[path->numsegs - 1].pos[0] - takeoff[0])
				 + (path->seg[path->numsegs - 1].pos[1] - takeoff[1]) * (path->seg[path->numsegs - 1].pos[1] - takeoff[1]) < (NB_RUNUP + 16) * (NB_RUNUP + 16))
				path->numsegs--;
			/* the run-up point, then straight through the takeoff spot at the landing spot: that
			   segment jumps where the floor ends */
			seg = &path->seg[path->numsegs++];
			seg->area = -1;
			seg->how = NBLINK_WALK;
			VectorSet(seg->pos, takeoff[0] - u[0] * NB_RUNUP, takeoff[1] - u[1] * NB_RUNUP, takeoff[2]);
			seg = &path->seg[path->numsegs++];
			seg->area = chain[i];
			seg->how = NBLINK_RUNJUMP;
			seg->pos[0] = to->center[0];
			seg->pos[1] = to->center[1];
			seg->pos[2] = NBNav_FloorZ(to, seg->pos[0], seg->pos[1]);
			VectorCopy(seg->pos, prev);
			continue;
		}
		seg = &path->seg[path->numsegs++];
		seg->area = chain[i];
		seg->how = link->type;
		if (link->type == NBLINK_TELEPORT)
		{	/* walk into the teleporter: aim at the middle of the source area */
			VectorCopy(from->center, seg->pos);
		}
		else
			NB_PortalPoint(from, to, prev, seg->pos, link->type);
		VectorCopy(seg->pos, prev);
	}
	{
		nbpathseg_t *seg = &path->seg[path->numsegs++];
		seg->area = g;
		seg->how = NBLINK_WALK;
		VectorCopy(goal, seg->pos);
		seg->pos[2] = NBNav_FloorZ(&nb_mesh.areas[g], goal[0], goal[1]);
		if (fabs(seg->pos[2] - (goal[2] - 24)) > 40)
			seg->pos[2] = goal[2] - 24;
	}
	path->valid = true;
	return true;
}

float NBPath_TravelDistance(const vec3_t start, const vec3_t goal, int team, float maxdist)
{
	int s = NBNav_AreaAt(start, 96), g = NBNav_AreaAt(goal, 160);
	if (s < 0) s = NBNav_NearestArea(start, 200);
	if (g < 0) g = NBNav_NearestArea(goal, 400);
	if (s < 0 || g < 0)
		return -1;
	if (s == g)
		return Distance(start, goal);
	if (!NB_AStar(s, g, team, NULL, NULL, maxdist))
		return -1;
	return nb_mesh.areas[g].g;
}

#endif
