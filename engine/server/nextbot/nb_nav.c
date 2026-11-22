/*
 * Nav mesh: generation from the BSP, file I/O, lookups.
 * Copyright (C) 2026 fteqw-sv contributors. GPLv2 or later; see nb_local.h.
 *
 * Generation walks a 16-unit grid from every spawn point, item, objective and teleport destination,
 * testing each step with the player hull exactly as a player moves (step up 18, jump up 44, drop),
 * with doors, lifts, trains and buttons made non-solid so the mesh goes through them. The grid
 * nodes are then merged into rectangular areas, linked to each other, and teleporters and lifts get
 * special links. Meshes are cached in <gamedir>/nav/<map>.qnav, tagged with the BSP checksum.
 */
#include "nb_local.h"

#ifndef CLIENTONLY

nbmesh_t nb_mesh;

#define QNAV_MAGIC   (('V'<<24)|('A'<<16)|('N'<<8)|'Q')
#define QNAV_VERSION 6

static const vec3_t nb_hullmins = { -16, -16, -24 };
static const vec3_t nb_hullmaxs = { 16, 16, 32 };

/* ------------------------------------------------------------------------------------------------
 * traces
 */
static trace_t NB_HullTrace(const vec3_t start, const vec3_t end)
{
	return World_Move(&sv.world, (float*)start, (float*)nb_hullmins, (float*)nb_hullmaxs, (float*)end, MOVE_NOMONSTERS, (wedict_t*)sv.world.edicts);
}

static int NB_Contents(const vec3_t p)
{
	return World_PointContentsWorldOnly(&sv.world, (float*)p);
}

/* entities made non-solid during generation */
static const char *nb_passable[] = { "func_door", "func_door_secret", "func_plat", "plat", "func_train", "train", "func_button", "door", "func_door_rotating", NULL };

static qboolean NB_IsPassableBrush(edict_t *ed)
{
	const char *cn = NB_Classname(ed);
	int i;
	for (i = 0; nb_passable[i]; i++)
		if (!strcmp(cn, nb_passable[i]))
			return true;
	return false;
}

typedef struct { edict_t *ed; float solid; } nbsaved_t;
static nbsaved_t *nb_saved;
static int nb_numsaved;

static void NB_PassableBegin(void)
{
	int i;
	nb_saved = Z_Malloc(sizeof(*nb_saved) * sv.world.num_edicts);
	nb_numsaved = 0;
	for (i = 1; i < sv.world.num_edicts; i++)
	{
		edict_t *ed = EDICT_NUM_PB(svprogfuncs, i);
		if (ED_ISFREE(ed) || ed->v->solid != SOLID_BSP)
			continue;
		if (!NB_IsPassableBrush(ed))
			continue;
		nb_saved[nb_numsaved].ed = ed;
		nb_saved[nb_numsaved].solid = ed->v->solid;
		nb_numsaved++;
		ed->v->solid = SOLID_NOT;
		World_LinkEdict(&sv.world, (wedict_t*)ed, false);
	}
}

static void NB_PassableEnd(void)
{
	int i;
	for (i = 0; i < nb_numsaved; i++)
	{
		nb_saved[i].ed->v->solid = nb_saved[i].solid;
		World_LinkEdict(&sv.world, (wedict_t*)nb_saved[i].ed, false);
	}
	Z_Free(nb_saved);
	nb_saved = NULL;
	nb_numsaved = 0;
}

/* ------------------------------------------------------------------------------------------------
 * generation: grid nodes
 */
typedef struct
{
	int ix, iy;
	float z;               /* origin z (feet + 24) */
	int link[4];           /* neighbour node per direction, -1 none */
	unsigned char ltype[4];
	unsigned int flags;
	int area;
	int hashnext;
} nbnode_t;

#define NB_MAXNODES 400000
#define NB_HASHSIZE 65536

static nbnode_t *gn;
static int gnum;
static int *ghash;
static int *gqueue;
static int gqhead, gqtail;

static const int dirx[4] = { 1, 0, -1, 0 };
static const int diry[4] = { 0, 1, 0, -1 };

static unsigned int NB_Hash(int ix, int iy)
{
	return ((unsigned int)(ix * 73856093) ^ (unsigned int)(iy * 19349663)) & (NB_HASHSIZE - 1);
}

static int NB_FindNode(int ix, int iy, float z, float ztol)
{
	int n;
	for (n = ghash[NB_Hash(ix, iy)]; n >= 0; n = gn[n].hashnext)
		if (gn[n].ix == ix && gn[n].iy == iy && fabs(gn[n].z - z) <= ztol)
			return n;
	return -1;
}

static int NB_AddNode(int ix, int iy, float z, unsigned int flags)
{
	int n = NB_FindNode(ix, iy, z, 8);
	unsigned int h;
	if (n >= 0)
		return n;
	if (gnum >= NB_MAXNODES)
		return -1;
	n = gnum++;
	gn[n].ix = ix;
	gn[n].iy = iy;
	gn[n].z = z;
	gn[n].link[0] = gn[n].link[1] = gn[n].link[2] = gn[n].link[3] = -1;
	gn[n].flags = flags;
	gn[n].area = -1;
	h = NB_Hash(ix, iy);
	gn[n].hashnext = ghash[h];
	ghash[h] = n;
	gqueue[gqtail++] = n;
	return n;
}

/* find a standing spot (origin) at grid column (x,y) near height z; false if none */
static qboolean NB_StandAt(float x, float y, float ztop, float maxdown, vec3_t out, unsigned int *flags, qboolean *steep)
{
	vec3_t a, b;
	trace_t tr;
	int c;
	VectorSet(a, x, y, ztop);
	VectorSet(b, x, y, ztop - maxdown);
	tr = NB_HullTrace(a, b);
	if (tr.startsolid || tr.allsolid || tr.fraction >= 1)
		return false;
	*steep = tr.plane.normal[2] < 0.7f;
	VectorCopy(tr.endpos, out);
	c = NB_Contents(out);
	*flags = 0;
	if (c & FTECONTENTS_LAVA)  *flags |= NBAREA_LAVA;
	if (c & FTECONTENTS_SLIME) *flags |= NBAREA_SLIME;
	if (c & FTECONTENTS_WATER) *flags |= NBAREA_WATER;
	if (c & FTECONTENTS_SKY)   return false;
	{	/* feet in liquid also counts (shallow water is fine to walk) */
		vec3_t feet;
		VectorSet(feet, out[0], out[1], out[2] - 20);
		c = NB_Contents(feet);
		if (c & FTECONTENTS_LAVA)  *flags |= NBAREA_LAVA;
		if (c & FTECONTENTS_SLIME) *flags |= NBAREA_SLIME;
	}
	return true;
}

/* try to move from node n in direction d. Returns the new node or -1, and the link type */
static int NB_Expand(int n, int d, unsigned char *ltype)
{
	vec3_t o, up, side, dest;
	trace_t tr;
	float tx, ty;
	unsigned int flags;
	qboolean steep;
	int ix = gn[n].ix + dirx[d], iy = gn[n].iy + diry[d];
	int tries;

	VectorSet(o, gn[n].ix * NB_GEN_STEP, gn[n].iy * NB_GEN_STEP, gn[n].z);
	tx = ix * NB_GEN_STEP;
	ty = iy * NB_GEN_STEP;

	/* lifts tried: a step up (18), none (a low doorway at the top of a ramp or stairs leaves no
	   room to lift the hull), then a jump (44) */
	static const float lifts[3] = { NB_STEPSIZE, 0, NB_JUMPHEIGHT };
	for (tries = 0; tries < 3; tries++)
	{
		float lift = lifts[tries];
		qboolean jump = tries == 2;
		VectorSet(up, o[0], o[1], o[2] + lift);
		if (lift > 0)
		{
			tr = NB_HullTrace(o, up);
			if (tr.startsolid)
				return -1;
			VectorCopy(tr.endpos, up);
			if (jump && up[2] < o[2] + NB_STEPSIZE + 4)
				return -1;   /* no headroom to jump */
		}
		VectorSet(side, tx, ty, up[2]);
		tr = NB_HullTrace(up, side);
		if (tr.fraction < 1 || tr.startsolid)
			continue;    /* blocked at this height: try the next */
		if (!NB_StandAt(tx, ty, up[2], lift + NB_MAXDROP, dest, &flags, &steep))
			return -1;   /* no floor (void / sky) */
		if (steep)
			return -1;
		if (flags & NBAREA_LAVA)
			return -1;
		if (dest[2] > o[2] + NB_STEPSIZE + 0.5f)
		{
			if (!jump)
				continue;    /* higher than a step: it's a jump */
			*ltype = NBLINK_JUMP;
		}
		else if (dest[2] < o[2] - NB_STEPSIZE - 0.5f)
			*ltype = NBLINK_DROP;
		else
			*ltype = NBLINK_WALK;
		return NB_AddNode(ix, iy, dest[2], flags);
	}
	return -1;
}

static void NB_Seed(const vec3_t p)
{
	vec3_t dest;
	unsigned int flags;
	qboolean steep;
	int ix = (int)floor(p[0] / NB_GEN_STEP + 0.5f), iy = (int)floor(p[1] / NB_GEN_STEP + 0.5f);
	int dx, dy;
	/* the exact grid column may be inside a wall next to the item: try the 3x3 around it */
	for (dy = 0; dy < 3; dy++)
		for (dx = 0; dx < 3; dx++)
		{
			int cx = ix + (dx == 0 ? 0 : (dx == 1 ? 1 : -1));
			int cy = iy + (dy == 0 ? 0 : (dy == 1 ? 1 : -1));
			if (NB_StandAt(cx * NB_GEN_STEP, cy * NB_GEN_STEP, p[2] + 40, 300, dest, &flags, &steep) && !steep && !(flags & NBAREA_LAVA))
			{
				NB_AddNode(cx, cy, dest[2], flags);
				return;
			}
		}
}

static void NB_SeedFromEntities(void)
{
	int i;
	for (i = 1; i < sv.world.num_edicts; i++)
	{
		edict_t *ed = EDICT_NUM_PB(svprogfuncs, i);
		const char *cn;
		vec3_t p;
		if (ED_ISFREE(ed))
			continue;
		cn = NB_Classname(ed);
		if (!strncmp(cn, "info_player", 11) || !strncmp(cn, "item_", 5) || !strncmp(cn, "weapon_", 7)
			|| !strcmp(cn, "info_teleport_destination") || !strcmp(cn, "info_tfgoal") || !strcmp(cn, "i_t_g")
			|| !strcmp(cn, "info_player_teamspawn") || !strncmp(cn, "ammo_", 5))
		{
			VectorAdd(ed->v->absmin, ed->v->absmax, p);
			VectorScale(p, 0.5f, p);
			if (VectorCompare(ed->v->absmin, ed->v->absmax))
				VectorCopy(ed->v->origin, p);
			NB_Seed(p);
		}
	}
}

/* ------------------------------------------------------------------------------------------------
 * generation: areas
 */
typedef struct { int x0, y0, x1, y1; float zmin, zmax; unsigned int flags; } nbrect_t;

static qboolean NB_NodeWalkLink(int a, int d, int b)
{
	return gn[a].link[d] == b && gn[a].ltype[d] == NBLINK_WALK;
}

/* node at grid column (ix,iy) continuing the floor of node 'from' via a walk link in direction d */
static int NB_WalkNeighbour(int from, int d)
{
	int b = gn[from].link[d];
	if (b < 0 || gn[from].ltype[d] != NBLINK_WALK)
		return -1;
	if (gn[b].link[(d + 2) & 3] != from || gn[b].ltype[(d + 2) & 3] != NBLINK_WALK)
		return -1;   /* only two-way walk links merge */
	return b;
}

#define NB_MAXAREASIDE 12

static void NB_BuildAreas(nbarea_t **areas, unsigned int *numareas)
{
	int n, maxareas = gnum;
	nbarea_t *out = Z_Malloc(sizeof(nbarea_t) * maxareas);
	int count = 0;
	int *rowstart = Z_Malloc(sizeof(int) * (NB_MAXAREASIDE + 1) * (NB_MAXAREASIDE + 1));

	for (n = 0; n < gnum; n++)
	{
		int w, h, x, y, best_w = 1, best_h = 1;
		int grid[NB_MAXAREASIDE][NB_MAXAREASIDE];
		float zmin, zmax;
		if (gn[n].area >= 0)
			continue;
		/* grow a rectangle: first along +x, then add rows along +y while every node matches */
		grid[0][0] = n;
		for (w = 1; w < NB_MAXAREASIDE; w++)
		{
			int prev = grid[0][w - 1];
			int nx = NB_WalkNeighbour(prev, 0);
			if (nx < 0 || gn[nx].area >= 0 || gn[nx].flags != gn[n].flags)
				break;
			grid[0][w] = nx;
		}
		best_w = w;
		zmin = zmax = gn[n].z;
		for (x = 0; x < best_w; x++)
		{
			if (gn[grid[0][x]].z < zmin) zmin = gn[grid[0][x]].z;
			if (gn[grid[0][x]].z > zmax) zmax = gn[grid[0][x]].z;
		}
		for (h = 1; h < NB_MAXAREASIDE; h++)
		{
			qboolean ok = true;
			float rzmin = zmin, rzmax = zmax;
			for (x = 0; x < best_w; x++)
			{
				int up = NB_WalkNeighbour(grid[h - 1][x], 1);
				if (up < 0 || gn[up].area >= 0 || gn[up].flags != gn[n].flags)
				{
					ok = false;
					break;
				}
				if (x > 0 && !NB_NodeWalkLink(grid[h][x - 1], 0, up))
				{
					ok = false;
					break;
				}
				grid[h][x] = up;
				if (gn[up].z < rzmin) rzmin = gn[up].z;
				if (gn[up].z > rzmax) rzmax = gn[up].z;
			}
			if (!ok || rzmax - rzmin > 24)
				break;
			zmin = rzmin;
			zmax = rzmax;
		}
		best_h = h;
		{
			nbarea_t *a = &out[count];
			int c00 = grid[0][0], c10 = grid[0][best_w - 1], c11 = grid[best_h - 1][best_w - 1], c01 = grid[best_h - 1][0];
			memset(a, 0, sizeof(*a));
			a->mins[0] = gn[c00].ix * NB_GEN_STEP - NB_GEN_STEP / 2;
			a->mins[1] = gn[c00].iy * NB_GEN_STEP - NB_GEN_STEP / 2;
			a->maxs[0] = gn[c11].ix * NB_GEN_STEP + NB_GEN_STEP / 2;
			a->maxs[1] = gn[c11].iy * NB_GEN_STEP + NB_GEN_STEP / 2;
			a->z[0] = gn[c00].z + NB_HULL_MINS_Z;
			a->z[1] = gn[c10].z + NB_HULL_MINS_Z;
			a->z[2] = gn[c11].z + NB_HULL_MINS_Z;
			a->z[3] = gn[c01].z + NB_HULL_MINS_Z;
			a->mins[2] = zmin + NB_HULL_MINS_Z;
			a->maxs[2] = zmax + NB_HULL_MINS_Z;
			a->flags = gn[n].flags;
			a->center[0] = (a->mins[0] + a->maxs[0]) * 0.5f;
			a->center[1] = (a->mins[1] + a->maxs[1]) * 0.5f;
			a->center[2] = (a->z[0] + a->z[1] + a->z[2] + a->z[3]) * 0.25f;
			for (y = 0; y < best_h; y++)
				for (x = 0; x < best_w; x++)
					gn[grid[y][x]].area = count;
			count++;
		}
	}
	Z_Free(rowstart);
	*areas = out;
	*numareas = count;
}

typedef struct { unsigned int from, to; unsigned char type; } nbrawlink_t;

static int NB_RawLinkCmp(const void *a, const void *b)
{
	const nbrawlink_t *x = a, *y = b;
	if (x->from != y->from) return x->from < y->from ? -1 : 1;
	if (x->to != y->to) return x->to < y->to ? -1 : 1;
	return (int)x->type - (int)y->type;
}

static nbrawlink_t *nb_raw;
static int nb_numraw, nb_maxraw;

static void NB_AddRaw(unsigned int from, unsigned int to, unsigned char type)
{
	if (from == to)
		return;
	if (nb_numraw >= nb_maxraw)
	{
		nb_maxraw = nb_maxraw ? nb_maxraw * 2 : 4096;
		nb_raw = BZ_Realloc(nb_raw, sizeof(*nb_raw) * nb_maxraw);
	}
	nb_raw[nb_numraw].from = from;
	nb_raw[nb_numraw].to = to;
	nb_raw[nb_numraw].type = type;
	nb_numraw++;
}

static qboolean NB_RectOverlap(const nbarea_t *a, const vec3_t mins, const vec3_t maxs, float pad)
{
	return a->maxs[0] > mins[0] - pad && a->mins[0] < maxs[0] + pad
		&& a->maxs[1] > mins[1] - pad && a->mins[1] < maxs[1] + pad
		&& a->maxs[2] + 64 > mins[2] && a->mins[2] - 8 < maxs[2];
}

/* a lift (func_plat, elevator door): link the floor under it to the floor beside its top */
static void NB_LiftLink(edict_t *ed, float topz)
{
	vec3_t c, top;
	int j, bottom, upper = -1;
	float bestd = 1e9;
	VectorAdd(ed->v->absmin, ed->v->absmax, c);
	VectorScale(c, 0.5f, c);
	/* the floor a rider stands on at the bottom: under the lift's (lowered) top surface; the lift
	   brush itself may reach below the room's floor */
	c[2] = ed->v->absmax[2] + 24;
	bottom = NBNav_AreaAt(c, 1024);
	if (bottom < 0)
		bottom = NBNav_NearestArea(c, 128);
	VectorSet(top, c[0], c[1], topz + 1);
	for (j = 0; j < (int)nb_mesh.numareas; j++)
	{
		nbarea_t *a = &nb_mesh.areas[j];
		float d;
		vec3_t cl;
		if (fabs(a->center[2] - topz) > 26)
			continue;
		cl[0] = bound(a->mins[0], top[0], a->maxs[0]);
		cl[1] = bound(a->mins[1], top[1], a->maxs[1]);
		d = (cl[0] - top[0]) * (cl[0] - top[0]) + (cl[1] - top[1]) * (cl[1] - top[1]);
		if (d < bestd)
		{
			bestd = d;
			upper = j;
		}
	}
	for (j = 0; j < (int)nb_mesh.numareas; j++)
		if (NB_RectOverlap(&nb_mesh.areas[j], ed->v->absmin, ed->v->absmax, 0) && nb_mesh.areas[j].maxs[2] < topz - 30)
			nb_mesh.areas[j].flags |= NBAREA_PLAT;
	if (qw_bot_debug.ival)
		Con_Printf("nav: lift %s at %.0f %.0f: bottom area %d (from z %.0f), top z %.0f area %d (%.0f away)\n", NB_Classname(ed),
			c[0], c[1], bottom, c[2], topz, upper, sqrt(bestd));
	if (bottom >= 0 && upper >= 0 && bottom != upper && bestd < 96 * 96)
	{
		NB_AddRaw(bottom, upper, NBLINK_PLAT);
		NB_AddRaw(upper, bottom, NBLINK_DROP);
	}
}

static void NB_SpecialLinks(void)
{
	int i, j;
	for (i = 1; i < sv.world.num_edicts; i++)
	{
		edict_t *ed = EDICT_NUM_PB(svprogfuncs, i);
		const char *cn;
		if (ED_ISFREE(ed))
			continue;
		cn = NB_Classname(ed);
		if (!strcmp(cn, "trigger_teleport"))
		{
			const char *target = PR_GetString(svprogfuncs, ed->v->target);
			int k, dest = -1;
			if (!*target)
				continue;
			for (k = 1; k < sv.world.num_edicts; k++)
			{
				edict_t *d = EDICT_NUM_PB(svprogfuncs, k);
				if (ED_ISFREE(d) || d == ed)
					continue;
				if (!strcmp(PR_GetString(svprogfuncs, d->v->targetname), target))
				{
					dest = NBNav_AreaAt(d->v->origin, 300);
					if (dest < 0)
						dest = NBNav_NearestArea(d->v->origin, 256);
					break;
				}
			}
			if (dest < 0)
				continue;
			for (j = 0; j < (int)nb_mesh.numareas; j++)
				if (NB_RectOverlap(&nb_mesh.areas[j], ed->v->absmin, ed->v->absmax, 0))
					NB_AddRaw(j, dest, NBLINK_TELEPORT);
		}
		else if (!strcmp(cn, "func_plat") || !strcmp(cn, "plat"))
		{
			/* a lift: link the shaft bottom to the floor at the top of the lift */
			float topz = ed->v->absmax[2];
			if (NB_HasField("pos1"))
			{
				vec3_t p1;
				NB_GetVector(ed, "pos1", p1);
				topz = ed->v->absmax[2] + (p1[2] - ed->v->origin[2]);
			}
			NB_LiftLink(ed, topz);
		}
		else if (!strncmp(cn, "func_door", 9) || !strcmp(cn, "door"))
		{
			vec3_t p1, p2;
			for (j = 0; j < (int)nb_mesh.numareas; j++)
				if (NB_RectOverlap(&nb_mesh.areas[j], ed->v->absmin, ed->v->absmax, 8))
					nb_mesh.areas[j].flags |= NBAREA_DOOR;
			/* a door that moves up a long way and is wide enough to stand on is an elevator */
			if (NB_HasField("pos1") && NB_HasField("pos2"))
			{
				NB_GetVector(ed, "pos1", p1);
				NB_GetVector(ed, "pos2", p2);
				if (p2[2] - p1[2] > 48 && fabs(p2[0] - p1[0]) < 8 && fabs(p2[1] - p1[1]) < 8
					&& ed->v->absmax[0] - ed->v->absmin[0] >= 48 && ed->v->absmax[1] - ed->v->absmin[1] >= 48)
					NB_LiftLink(ed, ed->v->absmax[2] + (p2[2] - p1[2]));
			}
		}
		else if (!strcmp(cn, "trigger_push"))
		{
			/* jump pad: simulate the launch (velocity = speed * 10 * movedir, gravity) to the landing
			   area, and link the pad's areas to it like a teleporter */
			vec3_t pos, vel, prev;
			float speed = NB_GetFloat(ed, "speed") > 0 ? NB_GetFloat(ed, "speed") : 1000;
			int step, land = -1;
			VectorAdd(ed->v->absmin, ed->v->absmax, pos);
			VectorScale(pos, 0.5f, pos);
			pos[2] = ed->v->absmin[2] + 24;
			VectorScale(ed->v->movedir, speed * 10, vel);
			if (VectorLength(vel) < 100)
				continue;
			for (step = 0; step < 300; step++)
			{
				trace_t tr;
				VectorCopy(pos, prev);
				VectorMA(pos, 0.02f, vel, pos);
				vel[2] -= movevars.gravity * 0.02f;
				tr = NB_HullTrace(prev, pos);
				if (tr.fraction < 1)
				{
					VectorCopy(tr.endpos, pos);
					if (tr.plane.normal[2] > 0.7f && vel[2] < 0)
					{
						land = NBNav_AreaAt(pos, 64);
						break;
					}
					/* hit a wall: slide */
					vel[0] = vel[1] = 0;
				}
			}
			if (land < 0)
				continue;
			for (j = 0; j < (int)nb_mesh.numareas; j++)
				if (j != land && NB_RectOverlap(&nb_mesh.areas[j], ed->v->absmin, ed->v->absmax, 0))
					NB_AddRaw(j, land, NBLINK_TELEPORT);
		}
		else if (!strcmp(cn, "trigger_hurt"))
		{
			vec3_t size;
			VectorSubtract(ed->v->absmax, ed->v->absmin, size);
			if (size[0] > 6000 && size[1] > 6000)
				continue;   /* a map-wide kill volume (round end): not a hazard to path around */
			for (j = 0; j < (int)nb_mesh.numareas; j++)
				if (NB_RectOverlap(&nb_mesh.areas[j], ed->v->absmin, ed->v->absmax, 0))
					nb_mesh.areas[j].flags |= NBAREA_HURT;
		}
		else if (!strncmp(cn, "info_player", 11))
		{
			int a = NBNav_AreaAt(ed->v->origin, 128);
			if (a >= 0)
				nb_mesh.areas[a].flags |= NBAREA_SPAWN;
		}
	}
}

static void NB_BuildGrid(void)
{
	unsigned int i;
	int x, y, total = 0;
	vec3_t mins = { 1e9, 1e9, 1e9 }, maxs = { -1e9, -1e9, -1e9 };
	unsigned int *counts;
	if (nb_mesh.gridstart) Z_Free(nb_mesh.gridstart);
	if (nb_mesh.gridareas) Z_Free(nb_mesh.gridareas);
	nb_mesh.gridstart = NULL;
	nb_mesh.gridareas = NULL;
	if (!nb_mesh.numareas)
		return;
	for (i = 0; i < nb_mesh.numareas; i++)
	{
		AddPointToBounds(nb_mesh.areas[i].mins, mins, maxs);
		AddPointToBounds(nb_mesh.areas[i].maxs, mins, maxs);
	}
	nb_mesh.gridcell = 128;
	VectorCopy(mins, nb_mesh.gridmins);
	nb_mesh.gridw = (int)((maxs[0] - mins[0]) / nb_mesh.gridcell) + 1;
	nb_mesh.gridh = (int)((maxs[1] - mins[1]) / nb_mesh.gridcell) + 1;
	counts = Z_Malloc(sizeof(unsigned int) * nb_mesh.gridw * nb_mesh.gridh);
	for (i = 0; i < nb_mesh.numareas; i++)
	{
		nbarea_t *a = &nb_mesh.areas[i];
		int x0 = (int)((a->mins[0] - mins[0]) / nb_mesh.gridcell), x1 = (int)((a->maxs[0] - mins[0]) / nb_mesh.gridcell);
		int y0 = (int)((a->mins[1] - mins[1]) / nb_mesh.gridcell), y1 = (int)((a->maxs[1] - mins[1]) / nb_mesh.gridcell);
		x1 = min(x1, nb_mesh.gridw - 1);
		y1 = min(y1, nb_mesh.gridh - 1);
		for (y = y0; y <= y1; y++)
			for (x = x0; x <= x1; x++)
			{
				counts[y * nb_mesh.gridw + x]++;
				total++;
			}
	}
	nb_mesh.gridstart = Z_Malloc(sizeof(unsigned int) * (nb_mesh.gridw * nb_mesh.gridh + 1));
	nb_mesh.gridareas = Z_Malloc(sizeof(unsigned int) * (total + 1));
	{
		unsigned int run = 0;
		for (x = 0; x < nb_mesh.gridw * nb_mesh.gridh; x++)
		{
			nb_mesh.gridstart[x] = run;
			run += counts[x];
			counts[x] = 0;
		}
		nb_mesh.gridstart[nb_mesh.gridw * nb_mesh.gridh] = run;
	}
	for (i = 0; i < nb_mesh.numareas; i++)
	{
		nbarea_t *a = &nb_mesh.areas[i];
		int x0 = (int)((a->mins[0] - mins[0]) / nb_mesh.gridcell), x1 = (int)((a->maxs[0] - mins[0]) / nb_mesh.gridcell);
		int y0 = (int)((a->mins[1] - mins[1]) / nb_mesh.gridcell), y1 = (int)((a->maxs[1] - mins[1]) / nb_mesh.gridcell);
		x1 = min(x1, nb_mesh.gridw - 1);
		y1 = min(y1, nb_mesh.gridh - 1);
		for (y = y0; y <= y1; y++)
			for (x = x0; x <= x1; x++)
			{
				int c = y * nb_mesh.gridw + x;
				nb_mesh.gridareas[nb_mesh.gridstart[c] + counts[c]++] = i;
			}
	}
	Z_Free(counts);
}

static void NB_FinishLinks(void)
{
	int i, n;
	qsort(nb_raw, nb_numraw, sizeof(*nb_raw), NB_RawLinkCmp);
	/* dedupe (keep the easiest type per pair: walk < jump < drop) */
	n = 0;
	for (i = 0; i < nb_numraw; i++)
	{
		if (n > 0 && nb_raw[n - 1].from == nb_raw[i].from && nb_raw[n - 1].to == nb_raw[i].to)
			continue;
		nb_raw[n++] = nb_raw[i];
	}
	if (nb_mesh.links)
		Z_Free(nb_mesh.links);
	nb_mesh.links = Z_Malloc(sizeof(nblink_t) * (n + 1));
	nb_mesh.numlinks = n;
	for (i = 0; i < (int)nb_mesh.numareas; i++)
		nb_mesh.areas[i].numlinks = 0;
	for (i = 0; i < n; i++)
	{
		nbarea_t *a = &nb_mesh.areas[nb_raw[i].from];
		if (!a->numlinks)
			a->firstlink = i;
		a->numlinks++;
		nb_mesh.links[i].to = nb_raw[i].to;
		nb_mesh.links[i].type = nb_raw[i].type;
		nb_mesh.links[i].cost = (nb_raw[i].type == NBLINK_JUMP) ? 64 : (nb_raw[i].type == NBLINK_PLAT ? 200 : 0);
	}
	BZ_Free(nb_raw);
	nb_raw = NULL;
	nb_numraw = nb_maxraw = 0;
}

qboolean NBNav_Generate(void)
{
	int n, d;
	double t0 = Sys_DoubleTime();
	if (!sv.world.worldmodel || sv.state != ss_active)
	{
		Con_Printf("nav: no map loaded\n");
		return false;
	}
	NBNav_Free();
	gn = BZ_Malloc(sizeof(nbnode_t) * NB_MAXNODES);
	ghash = BZ_Malloc(sizeof(int) * NB_HASHSIZE);
	gqueue = BZ_Malloc(sizeof(int) * NB_MAXNODES);
	memset(ghash, 0xff, sizeof(int) * NB_HASHSIZE);
	gnum = gqhead = gqtail = 0;

	NB_PassableBegin();
	NB_SeedFromEntities();
	while (gqhead < gqtail)
	{
		n = gqueue[gqhead++];
		for (d = 0; d < 4; d++)
		{
			unsigned char lt = NBLINK_WALK;
			int m = NB_Expand(n, d, &lt);
			if (m < 0)
				continue;
			gn[n].link[d] = m;
			gn[n].ltype[d] = lt;
		}
	}
	NB_PassableEnd();

	NB_BuildAreas(&nb_mesh.areas, &nb_mesh.numareas);
	/* area links from node links */
	for (n = 0; n < gnum; n++)
		for (d = 0; d < 4; d++)
		{
			int m = gn[n].link[d];
			if (m < 0 || gn[m].area == gn[n].area)
				continue;
			NB_AddRaw(gn[n].area, gn[m].area, gn[n].ltype[d]);
		}
	NB_BuildGrid();   /* needed by the area lookups in the special links */
	NB_SpecialLinks();
	NB_FinishLinks();
	Q_strncpyz(nb_mesh.mapname, svs.name, sizeof(nb_mesh.mapname));
	nb_mesh.bspchecksum = sv.world.worldmodel->checksum2;

	BZ_Free(gn);
	BZ_Free(ghash);
	BZ_Free(gqueue);
	gn = NULL;
	ghash = gqueue = NULL;
	Con_Printf("nav: %s: %d nodes -> %u areas, %u links (%.1f s)\n", svs.name, gnum, nb_mesh.numareas, nb_mesh.numlinks, Sys_DoubleTime() - t0);
	return nb_mesh.numareas > 0;
}

/* ------------------------------------------------------------------------------------------------
 * file I/O
 */
typedef struct
{
	int magic, version;
	unsigned int checksum;
	unsigned int numareas, numlinks;
} qnavheader_t;

typedef struct
{
	float mins[3], maxs[3], z[4];
	unsigned int flags, firstlink;
	unsigned short numlinks;
	unsigned char teamblock, pad;
} qnavarea_t;

qboolean NBNav_Save(void)
{
	char path[MAX_QPATH];
	size_t size;
	qbyte *buf, *p;
	qnavheader_t h;
	unsigned int i;
	if (!nb_mesh.numareas)
		return false;
	size = sizeof(h) + nb_mesh.numareas * sizeof(qnavarea_t) + nb_mesh.numlinks * sizeof(nblink_t);
	buf = p = BZ_Malloc(size);
	h.magic = QNAV_MAGIC;
	h.version = QNAV_VERSION;
	h.checksum = nb_mesh.bspchecksum;
	h.numareas = nb_mesh.numareas;
	h.numlinks = nb_mesh.numlinks;
	memcpy(p, &h, sizeof(h)); p += sizeof(h);
	for (i = 0; i < nb_mesh.numareas; i++)
	{
		qnavarea_t qa;
		nbarea_t *a = &nb_mesh.areas[i];
		memset(&qa, 0, sizeof(qa));
		VectorCopy(a->mins, qa.mins);
		VectorCopy(a->maxs, qa.maxs);
		memcpy(qa.z, a->z, sizeof(qa.z));
		qa.flags = a->flags;
		qa.firstlink = a->firstlink;
		qa.numlinks = a->numlinks;
		memcpy(p, &qa, sizeof(qa)); p += sizeof(qa);
	}
	memcpy(p, nb_mesh.links, nb_mesh.numlinks * sizeof(nblink_t));
	/* <gamedir>/nav/<map>.qnav: qw and fortress keep their own meshes even for maps with the same name */
	Q_snprintfz(path, sizeof(path), "%s/nav/%s.qnav", FS_GetGamedir(true), nb_mesh.mapname);
	if (!FS_WriteFile(path, buf, size, FS_ROOT))
	{
		BZ_Free(buf);
		Con_Printf("nav: couldn't write %s\n", path);
		return false;
	}
	BZ_Free(buf);
	Con_Printf("nav: wrote %s (%u areas)\n", path, nb_mesh.numareas);
	return true;
}

qboolean NBNav_Load(const char *mapname)
{
	char path[MAX_QPATH];
	size_t size;
	qbyte *buf, *p;
	qnavheader_t h;
	unsigned int i;
	NBNav_Free();
	Q_snprintfz(path, sizeof(path), "nav/%s.qnav", mapname);
	buf = FS_LoadMallocFile(path, &size);
	if (!buf)
		return false;
	if (size < sizeof(h))
	{
		BZ_Free(buf);
		return false;
	}
	memcpy(&h, buf, sizeof(h));
	if (h.magic != QNAV_MAGIC || h.version != QNAV_VERSION
		|| size != sizeof(h) + h.numareas * sizeof(qnavarea_t) + h.numlinks * sizeof(nblink_t))
	{
		Con_Printf("nav: %s is from another version, regenerate it\n", path);
		BZ_Free(buf);
		return false;
	}
	if (sv.world.worldmodel && h.checksum != sv.world.worldmodel->checksum2)
	{
		Con_Printf("nav: %s was made for a different version of the map, regenerate it\n", path);
		BZ_Free(buf);
		return false;
	}
	p = buf + sizeof(h);
	nb_mesh.numareas = h.numareas;
	nb_mesh.numlinks = h.numlinks;
	nb_mesh.areas = Z_Malloc(sizeof(nbarea_t) * (h.numareas + 1));
	nb_mesh.links = Z_Malloc(sizeof(nblink_t) * (h.numlinks + 1));
	for (i = 0; i < h.numareas; i++)
	{
		qnavarea_t qa;
		nbarea_t *a = &nb_mesh.areas[i];
		memcpy(&qa, p, sizeof(qa)); p += sizeof(qa);
		VectorCopy(qa.mins, a->mins);
		VectorCopy(qa.maxs, a->maxs);
		memcpy(a->z, qa.z, sizeof(a->z));
		a->flags = qa.flags;
		a->firstlink = qa.firstlink;
		a->numlinks = qa.numlinks;
		a->center[0] = (a->mins[0] + a->maxs[0]) * 0.5f;
		a->center[1] = (a->mins[1] + a->maxs[1]) * 0.5f;
		a->center[2] = (a->z[0] + a->z[1] + a->z[2] + a->z[3]) * 0.25f;
	}
	memcpy(nb_mesh.links, p, h.numlinks * sizeof(nblink_t));
	BZ_Free(buf);
	Q_strncpyz(nb_mesh.mapname, mapname, sizeof(nb_mesh.mapname));
	nb_mesh.bspchecksum = h.checksum;
	NB_BuildGrid();
	return true;
}

void NBNav_Free(void)
{
	if (nb_mesh.areas) Z_Free(nb_mesh.areas);
	if (nb_mesh.links) Z_Free(nb_mesh.links);
	if (nb_mesh.gridstart) Z_Free(nb_mesh.gridstart);
	if (nb_mesh.gridareas) Z_Free(nb_mesh.gridareas);
	if (nb_mesh.linkteams) Z_Free(nb_mesh.linkteams);
	if (nb_mesh.areatele) Z_Free(nb_mesh.areatele);
	memset(&nb_mesh, 0, sizeof(nb_mesh));
}

/* ------------------------------------------------------------------------------------------------
 * lookups
 */
float NBNav_FloorZ(const nbarea_t *a, float x, float y)
{
	float u = (a->maxs[0] > a->mins[0]) ? (x - a->mins[0]) / (a->maxs[0] - a->mins[0]) : 0.5f;
	float v = (a->maxs[1] > a->mins[1]) ? (y - a->mins[1]) / (a->maxs[1] - a->mins[1]) : 0.5f;
	float z0, z1;
	u = bound(0, u, 1);
	v = bound(0, v, 1);
	z0 = a->z[0] + (a->z[1] - a->z[0]) * u;
	z1 = a->z[3] + (a->z[2] - a->z[3]) * u;
	return z0 + (z1 - z0) * v;
}

qboolean NBNav_Contains(const nbarea_t *a, const vec3_t pos, float ztol)
{
	float fz;
	if (pos[0] < a->mins[0] || pos[0] > a->maxs[0] || pos[1] < a->mins[1] || pos[1] > a->maxs[1])
		return false;
	fz = NBNav_FloorZ(a, pos[0], pos[1]);
	return pos[2] >= fz - 8 && pos[2] <= fz + ztol;
}

void NBNav_RandomPoint(const nbarea_t *a, vec3_t out)
{
	float inset = 8;
	float w = a->maxs[0] - a->mins[0] - inset * 2, h = a->maxs[1] - a->mins[1] - inset * 2;
	out[0] = a->mins[0] + inset + (w > 0 ? frandom() * w : 0);
	out[1] = a->mins[1] + inset + (h > 0 ? frandom() * h : 0);
	out[2] = NBNav_FloorZ(a, out[0], out[1]);
}

int NBNav_AreaAt(const vec3_t pos, float maxdrop)
{
	int cx, cy, c;
	unsigned int i, best = ~0u;
	float bestdz = 1e9;
	vec3_t feet;
	if (!nb_mesh.numareas || !nb_mesh.gridstart)
		return -1;
	/* pos may be an origin (feet at -24) or a floor point: accept both */
	VectorCopy(pos, feet);
	cx = (int)((pos[0] - nb_mesh.gridmins[0]) / nb_mesh.gridcell);
	cy = (int)((pos[1] - nb_mesh.gridmins[1]) / nb_mesh.gridcell);
	if (cx < 0 || cy < 0 || cx >= nb_mesh.gridw || cy >= nb_mesh.gridh)
		return -1;
	c = cy * nb_mesh.gridw + cx;
	for (i = nb_mesh.gridstart[c]; i < nb_mesh.gridstart[c + 1]; i++)
	{
		nbarea_t *a = &nb_mesh.areas[nb_mesh.gridareas[i]];
		float fz, dz;
		if (feet[0] < a->mins[0] || feet[0] > a->maxs[0] || feet[1] < a->mins[1] || feet[1] > a->maxs[1])
			continue;
		fz = NBNav_FloorZ(a, feet[0], feet[1]);
		dz = feet[2] - fz;
		if (dz < -26 || dz > maxdrop + 26)
			continue;   /* area far above or far below */
		dz = fabs(dz - 24);  /* prefer the floor a standing origin is on */
		if (dz < bestdz)
		{
			bestdz = dz;
			best = nb_mesh.gridareas[i];
		}
	}
	return best == ~0u ? -1 : (int)best;
}

int NBNav_NearestArea(const vec3_t pos, float maxdist)
{
	unsigned int i;
	int best = NBNav_AreaAt(pos, 64);
	float bestd = maxdist * maxdist;
	if (best >= 0)
		return best;
	for (i = 0; i < nb_mesh.numareas; i++)
	{
		nbarea_t *a = &nb_mesh.areas[i];
		vec3_t cl, d;
		cl[0] = bound(a->mins[0], pos[0], a->maxs[0]);
		cl[1] = bound(a->mins[1], pos[1], a->maxs[1]);
		cl[2] = NBNav_FloorZ(a, cl[0], cl[1]) + 24;
		VectorSubtract(cl, pos, d);
		d[2] *= 2;   /* prefer the same floor */
		if (DotProduct(d, d) < bestd)
		{
			bestd = DotProduct(d, d);
			best = i;
		}
	}
	return best;
}

#endif

#ifndef CLIENTONLY
/* walking into a teleporter's volume teleports you: remember which areas do that, for whom */
void NBNav_MarkTeleporters(void)
{
	int i;
	unsigned int j;
	if (nb_mesh.areatele)
		Z_Free(nb_mesh.areatele);
	nb_mesh.areatele = NULL;
	if (!nb_mesh.numareas)
		return;
	nb_mesh.areatele = Z_Malloc(nb_mesh.numareas);
	for (i = 1; i < sv.world.num_edicts; i++)
	{
		edict_t *e = EDICT_NUM_PB(svprogfuncs, i);
		int team, bits;
		if (ED_ISFREE(e) || strcmp(NB_Classname(e), "trigger_teleport"))
			continue;
		team = (int)NB_GetFloat(e, "team_no");
		bits = (team >= 1 && team <= 4) ? (1 << (team - 1)) : 0x1f;
		for (j = 0; j < nb_mesh.numareas; j++)
		{
			nbarea_t *a = &nb_mesh.areas[j];
			/* the area's middle must be inside the trigger (edges brushing it don't teleport) */
			if (a->center[0] >= e->v->absmin[0] && a->center[0] <= e->v->absmax[0]
				&& a->center[1] >= e->v->absmin[1] && a->center[1] <= e->v->absmax[1]
				&& a->center[2] + 40 >= e->v->absmin[2] && a->center[2] - 8 <= e->v->absmax[2])
				nb_mesh.areatele[j] |= bits;
		}
	}
}
#endif
