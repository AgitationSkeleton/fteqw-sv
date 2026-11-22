/*
 * sv_heatmap: player position / death / movement sampler for heatmap sites. Off by default.
 * Copyright (C) 2026 fteqw-sv contributors. GPLv2 or later; see nb_local.h.
 *
 * Every second, every alive non-spectator client (bots too) is bucketed:
 *   kind 0  grounded presence   32-unit cell
 *   kind 3  airborne presence   32-unit cell
 *   kind 1  death               32-unit cell (where the player died; checked every frame)
 *   kind 2  movement edge       64-unit cells, from the previous sample's cell to this one
 *                               (skipped for jumps over 512 units: teleports, respawns)
 * keyed by team (TF team_no, 0 in deathmatch), class (TF playerclass, 0 otherwise) and bot (0/1).
 * Every sv_heatmap_flush seconds (60) and at map change the buckets are written atomically to
 *   <gamedir>/heatmap/<unixtime>_<port>_<seq>.hm
 * as text:
 *   HM1 game=<gamedir> map=<map> port=<port>
 *   T <team> <name>                                    (TF: the teams that appeared)
 *   <kind> <team> <class> <bot> <x> <y> <z> <x2> <y2> <z2> <count>
 * Coordinates are cell centres; x2/y2/z2 are 0 0 0 except for kind 2. Buckets are capped at
 * sv_heatmap_maxbuckets (40000); samples that would need a new bucket beyond that are dropped.
 */
#include "nb_local.h"

#ifndef CLIENTONLY

#include <time.h>

cvar_t sv_heatmap            = CVARD("sv_heatmap", "0", "Write player heatmap samples to <gamedir>/heatmap/*.hm (for heatmap sites).");
cvar_t sv_heatmap_flush      = CVARD("sv_heatmap_flush", "60", "Seconds between heatmap file writes (also written at map change).");
cvar_t sv_heatmap_maxbuckets = CVARD("sv_heatmap_maxbuckets", "40000", "Most heatmap buckets kept between writes.");

typedef struct
{
	short kind, team, pclass, bot;
	int c[6];        /* cell indices: x y z, and x2 y2 z2 for edges */
	unsigned int count;
	int next;
} hmbucket_t;

#define HM_HASH 16384
static hmbucket_t *hm_b;
static int hm_num, hm_max;
static int hm_hash[HM_HASH];
static float hm_nextsample, hm_nextflush;
static unsigned int hm_seq;
static unsigned int hm_teamsseen;
static struct { qboolean valid, alive; vec3_t pos; } hm_prev[MAX_CLIENTS];

static void HM_Reset(void)
{
	hm_num = 0;
	memset(hm_hash, 0xff, sizeof(hm_hash));
	hm_teamsseen = 0;
}

static void HM_Add(int kind, int team, int pclass, int bot, const int *c)
{
	unsigned int h = (unsigned int)(kind * 31 + team * 7 + pclass * 131 + bot * 977);
	int i;
	for (i = 0; i < 6; i++)
		h = h * 2654435761u + (unsigned int)c[i];
	h &= HM_HASH - 1;
	for (i = hm_hash[h]; i >= 0; i = hm_b[i].next)
		if (hm_b[i].kind == kind && hm_b[i].team == team && hm_b[i].pclass == pclass && hm_b[i].bot == bot && !memcmp(hm_b[i].c, c, sizeof(hm_b[i].c)))
		{
			hm_b[i].count++;
			return;
		}
	if (hm_num >= sv_heatmap_maxbuckets.ival)
		return;
	if (hm_num >= hm_max)
	{
		hm_max = hm_max ? hm_max * 2 : 4096;
		if (hm_max > sv_heatmap_maxbuckets.ival)
			hm_max = sv_heatmap_maxbuckets.ival;
		hm_b = BZ_Realloc(hm_b, sizeof(*hm_b) * hm_max);
	}
	i = hm_num++;
	hm_b[i].kind = kind;
	hm_b[i].team = team;
	hm_b[i].pclass = pclass;
	hm_b[i].bot = bot;
	memcpy(hm_b[i].c, c, sizeof(hm_b[i].c));
	hm_b[i].count = 1;
	hm_b[i].next = hm_hash[h];
	hm_hash[h] = i;
	if (team >= 1 && team <= 31)
		hm_teamsseen |= 1u << team;
}

static int HM_Cell(float v, int size)
{
	return (int)floor(v / size);
}

static void HM_Point(int kind, int team, int pclass, int bot, const vec3_t p, int size)
{
	int c[6] = { HM_Cell(p[0], size), HM_Cell(p[1], size), HM_Cell(p[2], size), 0, 0, 0 };
	HM_Add(kind, team, pclass, bot, c);
}

static qboolean HM_Alive(client_t *cl)
{
	edict_t *e = cl->edict;
	return cl->state == cs_spawned && !cl->spectator && e && e->v->health > 0 && e->v->deadflag == 0 && e->v->solid != SOLID_NOT;
}

static void HM_Who(client_t *cl, int *team, int *pclass, int *bot)
{
	extern int NB_TeamOf(edict_t *ed);
	*team = nb_game == NBGAME_TF ? (int)NB_GetFloat(cl->edict, "team_no") : 0;
	*pclass = nb_game == NBGAME_TF ? (int)NB_GetFloat(cl->edict, "playerclass") : 0;
	*bot = cl->protocol == SCP_BAD;
}

static const char *HM_Port(void)
{
	cvar_t *v = Cvar_FindVar("sv_port");
	return v ? v->string : "0";
}

void NBHeat_Flush(void)
{
	char path[MAX_OSPATH], tmp[MAX_OSPATH];
	char *buf;
	size_t size, len = 0;
	int i, t;
	static const char *tfteams[5] = { "", "blue", "red", "yellow", "green" };
	if (!hm_num)
		return;
	size = 256 + 32 * 8 + (size_t)hm_num * 96;
	buf = BZ_Malloc(size);
	{ Q_snprintfz(buf + len, size - len, "HM1 game=%s map=%s port=%s\n", FS_GetGamedir(true), svs.name, HM_Port()); len += strlen(buf + len); }
	if (nb_game == NBGAME_TF)
		for (t = 1; t <= 4; t++)
			if (hm_teamsseen & (1u << t))
				{ Q_snprintfz(buf + len, size - len, "T %d %s\n", t, tfteams[t]); len += strlen(buf + len); }
	for (i = 0; i < hm_num && len < size - 100; i++)
	{
		hmbucket_t *b = &hm_b[i];
		int s = b->kind == 2 ? 64 : 32;
		if (b->kind == 2)
			{ Q_snprintfz(buf + len, size - len, "%d %d %d %d %d %d %d %d %d %d %u\n", b->kind, b->team, b->pclass, b->bot,
				b->c[0] * s + s / 2, b->c[1] * s + s / 2, b->c[2] * s + s / 2, b->c[3] * s + s / 2, b->c[4] * s + s / 2, b->c[5] * s + s / 2, b->count); len += strlen(buf + len); }
		else
			{ Q_snprintfz(buf + len, size - len, "%d %d %d %d %d %d %d 0 0 0 %u\n", b->kind, b->team, b->pclass, b->bot,
				b->c[0] * s + s / 2, b->c[1] * s + s / 2, b->c[2] * s + s / 2, b->count); len += strlen(buf + len); }
	}
	Q_snprintfz(path, sizeof(path), "%s/heatmap/%u_%s_%u.hm", FS_GetGamedir(true), (unsigned int)time(NULL), HM_Port(), hm_seq++);
	Q_snprintfz(tmp, sizeof(tmp), "%s.tmp", path);
	/* atomic for the reader: write a temporary file, then rename it into place */
	if (FS_WriteFile(tmp, buf, len, FS_ROOT))
	{
		if (FS_Rename(tmp, path, FS_ROOT))
			Con_Printf("sv_heatmap: couldn't rename %s\n", tmp);
	}
	else
		Con_Printf("sv_heatmap: couldn't write %s\n", tmp);
	BZ_Free(buf);
	HM_Reset();
}

void NBHeat_Frame(void)
{
	int i;
	float now = sv.time;
	if (!sv_heatmap.ival || sv.state != ss_active || !svprogfuncs)
	{
		hm_nextsample = hm_nextflush = 0;
		return;
	}
	if (!hm_nextflush)
	{
		HM_Reset();
		memset(hm_prev, 0, sizeof(hm_prev));
		hm_nextsample = now + 1;
		hm_nextflush = now + max(5, sv_heatmap_flush.value);
	}
	/* deaths: every frame, so the spot is where the player died */
	for (i = 0; i < sv.allocated_client_slots && i < MAX_CLIENTS; i++)
	{
		client_t *cl = &svs.clients[i];
		qboolean alive = HM_Alive(cl);
		if (hm_prev[i].alive && !alive && cl->state == cs_spawned && cl->edict)
		{
			int team, pclass, bot;
			HM_Who(cl, &team, &pclass, &bot);
			HM_Point(1, team, pclass, bot, cl->edict->v->origin, 32);
		}
		hm_prev[i].alive = alive;
	}
	if (now >= hm_nextsample)
	{
		hm_nextsample = now + 1;
		for (i = 0; i < sv.allocated_client_slots && i < MAX_CLIENTS; i++)
		{
			client_t *cl = &svs.clients[i];
			int team, pclass, bot;
			edict_t *e = cl->edict;
			if (!HM_Alive(cl))
			{
				hm_prev[i].valid = false;
				continue;
			}
			HM_Who(cl, &team, &pclass, &bot);
			HM_Point(((int)e->v->flags & FL_ONGROUND) ? 0 : 3, team, pclass, bot, e->v->origin, 32);
			if (hm_prev[i].valid && Distance(hm_prev[i].pos, e->v->origin) <= 512)
			{
				int c[6] = { HM_Cell(hm_prev[i].pos[0], 64), HM_Cell(hm_prev[i].pos[1], 64), HM_Cell(hm_prev[i].pos[2], 64),
					HM_Cell(e->v->origin[0], 64), HM_Cell(e->v->origin[1], 64), HM_Cell(e->v->origin[2], 64) };
				if (c[0] != c[3] || c[1] != c[4] || c[2] != c[5])
					HM_Add(2, team, pclass, bot, c);
			}
			VectorCopy(e->v->origin, hm_prev[i].pos);
			hm_prev[i].valid = true;
		}
	}
	if (now >= hm_nextflush)
	{
		NBHeat_Flush();
		hm_nextflush = now + max(5, sv_heatmap_flush.value);
	}
}

/* map change: write what this map collected, start fresh on the next */
void NBHeat_MapChange(void)
{
	if (sv_heatmap.ival)
		NBHeat_Flush();
	HM_Reset();
	hm_nextsample = hm_nextflush = 0;
}

void NBHeat_Init(void)
{
	Cvar_Register(&sv_heatmap, "Bots");
	Cvar_Register(&sv_heatmap_flush, "Bots");
	Cvar_Register(&sv_heatmap_maxbuckets, "Bots");
	HM_Reset();
}

#endif
