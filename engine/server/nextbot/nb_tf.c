/*
 * Team Fortress behaviour: TeamFortress QuakeWorld 2.8/2.9 game code (the classic TF progs) and
 * FortressOne (server-qwprogs), detected at map start.
 * Copyright (C) 2026 fteqw-sv contributors. GPLv2 or later; see nb_local.h.
 *
 * The map's TF goal entities are read live every think (item_tfgoal flags, info_tfgoal capture and
 * touch goals, detpack and spanner goals, resupply, hazards, team-only doors and triggers), so the
 * same logic plays CTF maps (2fort5r, well6), escort maps (border1) and Hunted.
 *
 *   TFRoot ── TFJoin        pick team and class like a player (impulses), until spawned
 *          ├─ TFFight       class weapons, grenades, sniper charge, strafing
 *          ├─ TFCarry       carrying an enemy item: go to the capture point
 *          ├─ TFFetch       go get the enemy flag / a dropped item
 *          ├─ TFObjective   touch goals and team triggers that score (escort, Hunted escape)
 *          ├─ TFDefend      guard the own flag / capture point; hunt the carrier
 *          ├─ TFBuild       engineer: sentry and dispenser near the defence point, maintain it
 *          ├─ TFHeal        medic: heal hurt teammates
 *          ├─ TFDetpack     demoman: detpack goals (well6 grates)
 *          ├─ TFEscortVIP   Hunted bodyguards / allied team: stay with the VIP
 *          └─ TFResupply    low on ammo or health: resupply goal
 */
#include "nb_local.h"

#ifndef CLIENTONLY

extern int NB_TeamOf(edict_t *ed);
extern float NB_Global(const char *name);
extern int NB_ForcedClass(void);
extern cvar_t qw_bot_debug;
void NB_LeadTarget(nbbot_t *bot, edict_t *enemy, float speed, vec3_t out);
void NB_CombatMove(nbbot_t *bot, float dist, float wantmin, float wantmax, int *strafedir, float *nextstrafe);
qboolean NB_PickRoamPoint(nbbot_t *bot, vec3_t out, float mindist);

/* ------------------------------------------------------------------------------------------------
 * game profiles
 */
enum { TFP_TF28, TFP_FO };
static int tf_profile;

/* impulses that differ between the classic TF progs and FortressOne */
typedef struct
{
	int team1;          /* + team-1 */
	int class1;         /* + class-1 (scout = 1) */
	int classrandom;
	int prime1, prime2, throwgren;
	int detpack;        /* 20 s detpack */
	int detpipe;
	int medikit;
	int buildmenu;      /* engineer build menu (TF28) */
	int buildsentry, builddispenser;  /* direct build impulses (FO), 0 = use the menu */
	int spymenu;
	int feign;
	int disguiseenemy;  /* one impulse: disguise as the enemy team (FO), 0 = use the spy menu */
} tfimpulses_t;

static const tfimpulses_t tf28_imp = { 140, 101, 110, 150, 151, 152, 166, 170, 176, 179, 0, 0, 177, 178, 0 };
static const tfimpulses_t fo_imp   = { 102, 112, 121, 14, 15, 16, 53, 55, 176, 0, 89, 90, 0, 61, 66 };
static const tfimpulses_t *tfi = &tf28_imp;

#define PC_SCOUT    1
#define PC_SNIPER   2
#define PC_SOLDIER  3
#define PC_DEMOMAN  4
#define PC_MEDIC    5
#define PC_HVYWEAP  6
#define PC_PYRO     7
#define PC_SPY      8
#define PC_ENGINEER 9
#define PC_CIVILIAN 11

/* illegalclasses bits (TF info_tfdetect) */
static const int tf_classbit[12] = { 0, 1, 2, 4, 8, 16, 32, 64, 256, 512, 128, 0 };

/* ------------------------------------------------------------------------------------------------
 * map census
 */
#define TF_MAXGOALS 128
typedef struct
{
	edict_t *ed;
	int kind;
	int team;        /* team_no (0 = any) */
	int ownedby;     /* owned_by (items) */
	int goalno;
	int needitem;    /* items_allowed: goal_no of the item to carry */
	int score[5];    /* score per team when it fires (count goes to the activator's team) */
	qboolean hurts;  /* negative health: avoid */
	qboolean carriedonly;
} tfgoal_t;

enum { TFG_ITEM, TFG_CAPTURE, TFG_TOUCH, TFG_TRIGGER, TFG_DETPACK, TFG_SPANNER, TFG_RESUPPLY };

static tfgoal_t tf_goals[TF_MAXGOALS];
static int tf_numgoals;
static int tf_numteams;
static int tf_civilianteams;      /* bit (team-1) */
static int tf_allies[5];          /* bit (team-1) of allied teams, per team */

/* escape vehicles (border1's truck): a door that travels far and holds a team trigger targeting it */
typedef struct { edict_t *door, *trigger; int team; vec3_t pos1, pos2; } tfvehicle_t;
static tfvehicle_t tf_vehicles[4];
static int tf_numvehicles;
static float tf_nextcensus;

static vec3_t *NB_EdCenter(edict_t *e, vec3_t out)
{
	if (e->v->absmax[0] > e->v->absmin[0])
	{
		VectorAdd(e->v->absmin, e->v->absmax, out);
		VectorScale(out, 0.5f, out);
	}
	else
		VectorCopy(e->v->origin, out);
	return (vec3_t*)out;
}

static edict_t *NB_FindByTargetname(const char *name, edict_t *after)
{
	int i = after ? NUM_FOR_EDICT(svprogfuncs, after) + 1 : 1;
	if (!name || !*name)
		return NULL;
	for (; i < sv.world.num_edicts; i++)
	{
		edict_t *e = EDICT_NUM_PB(svprogfuncs, i);
		if (!ED_ISFREE(e) && !strcmp(PR_GetString(svprogfuncs, e->v->targetname), name))
			return e;
	}
	return NULL;
}

static void TF_GoalScores(edict_t *g, int activatorteam, int *score)
{
	int t;
	char f[24];
	for (t = 1; t <= 4; t++)
	{
		Q_snprintfz(f, sizeof(f), "increase_team%d", t);
		score[t] += (int)NB_GetFloat(g, f);
	}
	if (activatorteam >= 1 && activatorteam <= 4)
		score[activatorteam] += (int)NB_GetFloat(g, "count");
}

static void TF_AddGoal(edict_t *e, int kind)
{
	tfgoal_t *g;
	if (tf_numgoals >= TF_MAXGOALS)
		return;
	g = &tf_goals[tf_numgoals++];
	memset(g, 0, sizeof(*g));
	g->ed = e;
	g->kind = kind;
	g->team = (int)NB_GetFloat(e, "team_no");
	g->ownedby = (int)NB_GetFloat(e, "owned_by");
	g->goalno = (int)NB_GetFloat(e, "goal_no");
	g->needitem = (int)NB_GetFloat(e, "items_allowed");
	g->hurts = e->v->health < 0;
	TF_GoalScores(e, g->team, g->score);
}

static void TF_Census(void)
{
	int i, t;
	tf_numgoals = 0;
	memset(tf_allies, 0, sizeof(tf_allies));
	tf_numteams = (int)NB_Global("number_of_teams");
	if (tf_numteams < 2 || tf_numteams > 4)
		tf_numteams = 2;
	tf_civilianteams = (int)NB_Global("civilianteams");
	for (i = 1; i < sv.world.num_edicts; i++)
	{
		edict_t *e = EDICT_NUM_PB(svprogfuncs, i);
		const char *cn;
		int act;
		if (ED_ISFREE(e))
			continue;
		cn = NB_Classname(e);
		if (!strcmp(cn, "item_tfgoal"))
			TF_AddGoal(e, TFG_ITEM);
		else if (!strcmp(cn, "info_tfgoal") || !strcmp(cn, "i_t_g"))
		{
			act = (int)NB_GetFloat(e, "goal_activation");
			if (act & 2)
				TF_AddGoal(e, TFG_DETPACK);
			else if (act & 8)
				TF_AddGoal(e, TFG_SPANNER);
			else if (act & 1)
			{
				int needitem = (int)NB_GetFloat(e, "items_allowed");
				int sc[5] = { 0 };
				TF_GoalScores(e, (int)NB_GetFloat(e, "team_no"), sc);
				if (needitem)
					TF_AddGoal(e, TFG_CAPTURE);
				else if (sc[1] > 0 || sc[2] > 0 || sc[3] > 0 || sc[4] > 0)
					TF_AddGoal(e, TFG_TOUCH);
				else if (e->v->health >= 0 && (NB_GetFloat(e, "ammo_shells") > 0 || NB_GetFloat(e, "ammo_rockets") > 0
					|| NB_GetFloat(e, "ammo_cells") > 0 || e->v->health > 0 || e->v->armorvalue > 0))
					TF_AddGoal(e, TFG_RESUPPLY);
				else if (e->v->health < 0)
					TF_AddGoal(e, TFG_TOUCH);   /* hazard: kept so paths avoid it */
			}
		}
		else if (!strcmp(cn, "trigger_multiple") || !strcmp(cn, "trigger_once"))
		{
			/* a team trigger whose target chain scores: an objective (border1's truck/win) */
			const char *target = PR_GetString(svprogfuncs, e->v->target);
			edict_t *g = NB_FindByTargetname(target, NULL);
			int sc[5] = { 0 };
			int team = (int)NB_GetFloat(e, "team_no");
			qboolean hurts = false;
			while (g)
			{
				TF_GoalScores(g, team, sc);
				if (g->v->health < 0)
					hurts = true;
				g = NB_FindByTargetname(target, g);
			}
			if (sc[1] > 0 || sc[2] > 0 || sc[3] > 0 || sc[4] > 0 || hurts)
			{
				TF_AddGoal(e, TFG_TRIGGER);
				memcpy(tf_goals[tf_numgoals - 1].score, sc, sizeof(sc));
				tf_goals[tf_numgoals - 1].hurts = hurts && !(sc[1] > 0 || sc[2] > 0 || sc[3] > 0 || sc[4] > 0);
			}
		}
	}
	tf_numvehicles = 0;
	for (i = 1; i < sv.world.num_edicts && tf_numvehicles < (int)countof(tf_vehicles); i++)
	{
		edict_t *d = EDICT_NUM_PB(svprogfuncs, i), *tr;
		const char *tn;
		vec3_t p1, p2;
		int k;
		if (ED_ISFREE(d) || (strncmp(NB_Classname(d), "func_door", 9) && strcmp(NB_Classname(d), "door")))
			continue;
		tn = PR_GetString(svprogfuncs, d->v->targetname);
		if (!*tn)
			continue;
		NB_GetVector(d, "pos1", p1);
		NB_GetVector(d, "pos2", p2);
		if (Distance(p1, p2) < 512)
			continue;
		for (k = 1; k < sv.world.num_edicts; k++)
		{
			vec3_t c;
			tr = EDICT_NUM_PB(svprogfuncs, k);
			if (ED_ISFREE(tr) || strcmp(NB_Classname(tr), "trigger_multiple") || strcmp(PR_GetString(svprogfuncs, tr->v->target), tn))
				continue;
			if (NB_GetFloat(tr, "team_no") < 1)
				continue;
			VectorAdd(tr->v->absmin, tr->v->absmax, c);
			VectorScale(c, 0.5f, c);
			/* the trigger sits inside the vehicle's start position (compare against where the door
			   is when it's at pos1, wherever it is now) */
			{
				vec3_t ofs;
				VectorSubtract(d->v->origin, p1, ofs);
				if (c[0] < d->v->absmin[0] - ofs[0] - 16 || c[0] > d->v->absmax[0] - ofs[0] + 16
					|| c[1] < d->v->absmin[1] - ofs[1] - 16 || c[1] > d->v->absmax[1] - ofs[1] + 16)
					continue;
			}
			tf_vehicles[tf_numvehicles].door = d;
			tf_vehicles[tf_numvehicles].trigger = tr;
			tf_vehicles[tf_numvehicles].team = (int)NB_GetFloat(tr, "team_no");
			VectorCopy(p1, tf_vehicles[tf_numvehicles].pos1);
			VectorCopy(p2, tf_vehicles[tf_numvehicles].pos2);
			tf_numvehicles++;
			break;
		}
	}
	/* items without owned_by (4f4): a team owns the item its capture points don't take */
	for (i = 0; i < tf_numgoals; i++)
	{
		tfgoal_t *it = &tf_goals[i];
		int owner = 0, cands = 0;
		if (it->kind != TFG_ITEM || it->ownedby)
			continue;
		for (t = 1; t <= tf_numteams; t++)
		{
			int k, hascaps = 0, takes = 0;
			for (k = 0; k < tf_numgoals; k++)
				if (tf_goals[k].kind == TFG_CAPTURE && tf_goals[k].team == t)
				{
					hascaps = 1;
					if (tf_goals[k].needitem == it->goalno)
						takes = 1;
				}
			if (hascaps && !takes)
			{
				owner = t;
				cands++;
			}
		}
		if (cands == 1)
			it->ownedby = owner;
	}
	/* teams that score from the same goal fight on the same side (Hunted, border1) */
	for (i = 0; i < tf_numgoals; i++)
	{
		int a, b;
		for (a = 1; a <= 4; a++)
			for (b = 1; b <= 4; b++)
				if (a != b && tf_goals[i].score[a] > 0 && tf_goals[i].score[b] > 0)
					tf_allies[a] |= 1 << (b - 1);
	}
	(void)t;
}

/* areas behind team-only doors / under team-only hurt triggers are blocked for the other teams */
static void TF_TeamBlocks(void)
{
	int i;
	unsigned int j;
	for (j = 0; j < nb_mesh.numareas; j++)
		nb_mesh.areas[j].teamblock = 0;
	for (i = 1; i < sv.world.num_edicts; i++)
	{
		edict_t *e = EDICT_NUM_PB(svprogfuncs, i);
		const char *cn;
		int team, block;
		if (ED_ISFREE(e))
			continue;
		cn = NB_Classname(e);
		if (strncmp(cn, "func_door", 9) && strcmp(cn, "door"))
			continue;
		team = (int)NB_GetFloat(e, "team_no");
		if (team < 1 || team > 4)
			continue;
		block = 0xf & ~(1 << (team - 1)) & ~tf_allies[team];
		for (j = 0; j < nb_mesh.numareas; j++)
		{
			nbarea_t *a = &nb_mesh.areas[j];
			if (a->maxs[0] > e->v->absmin[0] - 4 && a->mins[0] < e->v->absmax[0] + 4
				&& a->maxs[1] > e->v->absmin[1] - 4 && a->mins[1] < e->v->absmax[1] + 4
				&& a->center[2] + 64 > e->v->absmin[2] && a->center[2] - 8 < e->v->absmax[2])
				a->teamblock |= block;
		}
	}
	/* team-only teleporters (Hunted): the other teams can't use their links */
	if (nb_mesh.linkteams)
		Z_Free(nb_mesh.linkteams);
	nb_mesh.linkteams = Z_Malloc(nb_mesh.numlinks + 1);
	for (i = 1; i < sv.world.num_edicts; i++)
	{
		edict_t *e = EDICT_NUM_PB(svprogfuncs, i);
		int team, block;
		if (ED_ISFREE(e) || strcmp(NB_Classname(e), "trigger_teleport"))
			continue;
		team = (int)NB_GetFloat(e, "team_no");
		if (team < 1 || team > 4)
			continue;
		block = 0xf & ~(1 << (team - 1));
		for (j = 0; j < nb_mesh.numareas; j++)
		{
			nbarea_t *a = &nb_mesh.areas[j];
			unsigned int l;
			if (!(a->maxs[0] > e->v->absmin[0] && a->mins[0] < e->v->absmax[0] && a->maxs[1] > e->v->absmin[1] && a->mins[1] < e->v->absmax[1]
				&& a->center[2] + 64 > e->v->absmin[2] && a->center[2] - 8 < e->v->absmax[2]))
				continue;
			for (l = 0; l < a->numlinks; l++)
				if (nb_mesh.links[a->firstlink + l].type == NBLINK_TELEPORT)
					nb_mesh.linkteams[a->firstlink + l] |= block;
		}
	}
	/* hazards a team must not walk into (border1's "kill" triggers, Hunted's President-only hazard) */
	for (i = 0; i < tf_numgoals; i++)
	{
		tfgoal_t *g = &tf_goals[i];
		edict_t *e = g->ed;
		if (!g->hurts)
			continue;
		for (j = 0; j < nb_mesh.numareas; j++)
		{
			nbarea_t *a = &nb_mesh.areas[j];
			if (a->maxs[0] > e->v->absmin[0] && a->mins[0] < e->v->absmax[0]
				&& a->maxs[1] > e->v->absmin[1] && a->mins[1] < e->v->absmax[1]
				&& a->center[2] + 64 > e->v->absmin[2] && a->center[2] - 8 < e->v->absmax[2])
				a->teamblock |= (g->team >= 1 && g->team <= 4) ? (1 << (g->team - 1)) : 0xf;
		}
	}
}

int NBTF_TeamBlockBits(edict_t *door)
{
	int team = (int)NB_GetFloat(door, "team_no");
	return (team >= 1 && team <= 4) ? (0xf & ~(1 << (team - 1))) : 0;
}

void NBTF_MapStart(void)
{
	tf_profile = (NB_HasField("csqc_maxspeed") || NB_HasField("has_flag")) ? TFP_FO : TFP_TF28;
	tfi = tf_profile == TFP_FO ? &fo_imp : &tf28_imp;
	tf_nextcensus = 0;
	TF_Census();
	TF_TeamBlocks();
	if (qw_bot_debug.ival)
		Con_Printf("[nb] TF profile %s, %d teams, %d goals, civilian teams %x\n", tf_profile == TFP_FO ? "FortressOne" : "TF 2.8/2.9", tf_numteams, tf_numgoals, tf_civilianteams);
}

static void TF_MaybeCensus(void)
{
	if (NB_Time() < tf_nextcensus)
		return;
	tf_nextcensus = NB_Time() + 5;   /* goals don't move; re-read in case the map spawned some late */
	TF_Census();
}

/* ------------------------------------------------------------------------------------------------
 * team relations
 */
qboolean NBTF_Allied(int a, int b)
{
	if (a == b)
		return true;
	if (a < 1 || a > 4 || b < 1 || b > 4)
		return false;
	return (tf_allies[a] & (1 << (b - 1))) != 0;
}

qboolean NBTF_IsEnemy(nbbot_t *bot, edict_t *other)
{
	int mine = NB_TeamOf(NB_Self(bot)), theirs;
	int n = NUM_FOR_EDICT(svprogfuncs, other);
	const char *cn;
	if (n >= 1 && n <= sv.allocated_client_slots)
	{
		if (svs.clients[n - 1].state != cs_spawned || !NB_IsAlive(other))
			return false;
		theirs = NB_TeamOf(other);
		if (theirs <= 0 || mine <= 0)
			return false;
		/* a disguised spy looks like his disguise team */
		if (NB_GetFloat(other, "is_undercover") >= 1 && NBTF_Allied(mine, (int)NB_GetFloat(other, "undercover_team")) && frandom() > 0.05f)
			return false;
		if (NB_GetFloat(other, "is_feigning") > 0 && frandom() > 0.02f)
			return false;
		return !NBTF_Allied(mine, theirs);
	}
	/* buildings */
	cn = NB_Classname(other);
	if (!strcmp(cn, "building_sentrygun") || !strcmp(cn, "building_dispenser"))
	{
		edict_t *owner = NB_GetEntity(other, "real_owner");
		theirs = owner ? NB_TeamOf(owner) : (int)NB_GetFloat(other, "team_no");
		return theirs > 0 && mine > 0 && !NBTF_Allied(mine, theirs) && other->v->health > 0;
	}
	return false;
}

/* ------------------------------------------------------------------------------------------------
 * per-bot TF state
 */
typedef struct
{
	int phase;            /* join: 0 wait, 1 team sent, 2 class sent, 3 playing */
	float nextjoin;
	int team, pclass;
	int classtries;
	int role;             /* 0 attack, 1 defend */
	float primetime;      /* grenade primed at (0 = none) */
	int primeslot;
	float nextgren;
	float nextspecial;
	float sniperstart;
	qboolean wasjoined;
	float disguisestep_t;
	int disguisestep;
	int buildstep;
	float buildstep_t;
} tfbot_t;

static tfbot_t *TF(nbbot_t *bot)
{
	if (!bot->gamedata)
		bot->gamedata = Z_Malloc(sizeof(tfbot_t));
	return (tfbot_t*)bot->gamedata;
}

static int TF_Class(edict_t *e)  { return (int)NB_GetFloat(e, "playerclass"); }

static int TF_TeamCount(int team)
{
	int i, n = 0;
	for (i = 0; i < sv.allocated_client_slots; i++)
		if (svs.clients[i].state == cs_spawned && svs.clients[i].edict && NB_TeamOf(svs.clients[i].edict) == team)
			n++;
	return n;
}

static int TF_ClassCount(int team, int pclass)
{
	int i, n = 0;
	for (i = 0; i < sv.allocated_client_slots; i++)
	{
		edict_t *e = svs.clients[i].edict;
		if (svs.clients[i].state == cs_spawned && e && NB_TeamOf(e) == team
			&& (TF_Class(e) == pclass || (int)NB_GetFloat(e, "nextpc") == pclass))
			n++;
	}
	return n;
}

static int TF_PickTeam(nbbot_t *bot)
{
	int t, best = 1, bestn = 999;
	char g[32];
	for (t = 1; t <= tf_numteams; t++)
	{
		int n = TF_TeamCount(t), maxp;
		Q_snprintfz(g, sizeof(g), "team%dmaxplayers", t);
		maxp = (int)NB_Global(g);
		if (maxp > 0 && n >= maxp)
			continue;
		if (n < bestn || (n == bestn && (rand() & 1)))
		{
			bestn = n;
			best = t;
		}
	}
	return best;
}

static qboolean TF_ClassLegal(int team, int pclass)
{
	char g[32];
	int ill = (int)NB_Global("illegalclasses");
	Q_snprintfz(g, sizeof(g), "illegalclasses%d", team);
	ill |= (int)NB_Global(g);
	if (pclass >= 1 && pclass <= 10 && (ill & tf_classbit[pclass]))
		return false;
	if (pclass == PC_SPY && NB_Global("spy_off") > 0)
		return false;
	return true;
}

static int TF_PickClass(nbbot_t *bot, int team)
{
	/* a mix like a pub team: one of each core role before doubling up */
	static const int order[] = { PC_SOLDIER, PC_ENGINEER, PC_MEDIC, PC_DEMOMAN, PC_SCOUT, PC_HVYWEAP, PC_SNIPER, PC_PYRO, PC_SPY, PC_SOLDIER, PC_MEDIC, PC_DEMOMAN };
	int forced = NB_ForcedClass(), i, best = PC_SOLDIER, bestn = 999;
	if (bot->wantclass >= 1 && bot->wantclass <= 9 && TF_ClassLegal(team, bot->wantclass))
		return bot->wantclass;
	if (forced >= 1 && forced <= 9 && TF_ClassLegal(team, forced))
		return forced;
	for (i = 0; i < (int)countof(order); i++)
	{
		int c = order[i], n;
		if (!TF_ClassLegal(team, c))
			continue;
		n = TF_ClassCount(team, c) * 2 + i / 9;
		if (n < bestn || (n == bestn && frandom() < 0.3f))
		{
			bestn = n;
			best = c;
		}
	}
	return best;
}

void NBTF_ClientJoin(nbbot_t *bot)
{
	tfbot_t *t = TF(bot);
	memset(t, 0, sizeof(*t));
	t->nextjoin = NB_Time() + 1 + frandom();
	t->role = (rand() % 100) < 55 ? 0 : 1;
	if (tf_profile == TFP_FO)
		bot->cl->csqcactive = true;   /* FortressOne drops clients without CSQC */
}

/* send one impulse; the TF code handles one per frame after attack_finished */
static void TF_Impulse(nbbot_t *bot, int imp)
{
	bot->impulse = imp;
}

/* ------------------------------------------------------------------------------------------------
 * goals for a team
 */
static qboolean TF_ItemCarried(tfgoal_t *g)
{
	return (int)NB_GetFloat(g->ed, "goal_state") == 1 && g->ed->v->owner;
}

static edict_t *TF_ItemCarrier(tfgoal_t *g)
{
	if (!TF_ItemCarried(g))
		return NULL;
	return (edict_t*)PROG_TO_EDICT(svprogfuncs, g->ed->v->owner);
}

/* item the bot carries */
static tfgoal_t *TF_CarriedItem(nbbot_t *bot)
{
	int i;
	edict_t *me = NB_Self(bot);
	for (i = 0; i < tf_numgoals; i++)
		if (tf_goals[i].kind == TFG_ITEM && TF_ItemCarrier(&tf_goals[i]) == me)
			return &tf_goals[i];
	return NULL;
}

static qboolean TF_TeamMay(int goalteam, int team)
{
	return goalteam == 0 || goalteam == team;
}

/* an enemy item this team can steal (at home or dropped) */
static tfgoal_t *TF_ItemToSteal(nbbot_t *bot, edict_t **carrier)
{
	int i, team = NB_TeamOf(NB_Self(bot));
	tfgoal_t *best = NULL;
	*carrier = NULL;
	for (i = 0; i < tf_numgoals; i++)
	{
		tfgoal_t *g = &tf_goals[i];
		int state;
		if (g->kind != TFG_ITEM || !TF_TeamMay(g->team, team) || NBTF_Allied(g->ownedby, team))
			continue;
		state = (int)NB_GetFloat(g->ed, "goal_state");
		if (state == 3)
			continue;
		if (TF_ItemCarried(g))
		{
			edict_t *c = TF_ItemCarrier(g);
			if (c && NBTF_Allied(NB_TeamOf(c), team))
			{
				*carrier = c;
				best = g;
			}
			continue;
		}
		/* is there anywhere to bring it? */
		{
			int k;
			for (k = 0; k < tf_numgoals; k++)
				if (tf_goals[k].kind == TFG_CAPTURE && tf_goals[k].needitem == g->goalno && TF_TeamMay(tf_goals[k].team, team))
					return g;
		}
		if (!best)
			best = g;
	}
	return best;
}

static tfgoal_t *TF_CaptureFor(nbbot_t *bot, tfgoal_t *item)
{
	int i, team = NB_TeamOf(NB_Self(bot));
	tfgoal_t *best = NULL;
	float bestd = 1e9;
	for (i = 0; i < tf_numgoals; i++)
	{
		tfgoal_t *g = &tf_goals[i];
		vec3_t c;
		float d;
		if (g->kind != TFG_CAPTURE || g->needitem != item->goalno || !TF_TeamMay(g->team, team))
			continue;
		NB_EdCenter(g->ed, c);
		d = Distance(c, NB_Self(bot)->v->origin);
		if (d < bestd)
		{
			bestd = d;
			best = g;
		}
	}
	return best;
}

/* touch goal / team trigger that scores for this team (escape zones, escort finish lines) */
static tfgoal_t *TF_ObjectiveFor(nbbot_t *bot)
{
	int i, team = NB_TeamOf(NB_Self(bot));
	int pclass = TF_Class(NB_Self(bot));
	tfgoal_t *best = NULL;
	float bestd = 1e9;
	for (i = 0; i < tf_numgoals; i++)
	{
		tfgoal_t *g = &tf_goals[i];
		vec3_t c;
		float d;
		if ((g->kind != TFG_TOUCH && g->kind != TFG_TRIGGER) || g->hurts)
			continue;
		if (!TF_TeamMay(g->team, team) || g->score[team] <= 0)
			continue;
		if (g->kind == TFG_TOUCH && (int)NB_GetFloat(g->ed, "goal_state") != 2)
			continue;   /* not ready (active / removed) */
		/* class-only goals */
		if (NB_GetFloat(g->ed, "playerclass") > 0 && (int)NB_GetFloat(g->ed, "playerclass") != pclass)
			continue;
		NB_EdCenter(g->ed, c);
		d = Distance(c, NB_Self(bot)->v->origin);
		if (d < bestd)
		{
			bestd = d;
			best = g;
		}
	}
	return best;
}

/* the point a defender guards: own item (home or dropped), else the enemy's capture point for it */
static qboolean TF_DefencePoint(nbbot_t *bot, vec3_t out, edict_t **carrier)
{
	int i, team = NB_TeamOf(NB_Self(bot));
	*carrier = NULL;
	for (i = 0; i < tf_numgoals; i++)
	{
		tfgoal_t *g = &tf_goals[i];
		if (g->kind != TFG_ITEM || g->ownedby != team)
			continue;
		if (TF_ItemCarried(g))
		{
			edict_t *c = TF_ItemCarrier(g);
			if (c && !NBTF_Allied(NB_TeamOf(c), team))
			{
				*carrier = c;
				VectorCopy(c->v->origin, out);
				return true;
			}
			continue;
		}
		NB_EdCenter(g->ed, out);
		return true;
	}
	/* no own item: guard the objective the enemy wants (escort finish, escape) */
	for (i = 0; i < tf_numgoals; i++)
	{
		tfgoal_t *g = &tf_goals[i];
		int t;
		if (g->kind != TFG_TOUCH && g->kind != TFG_TRIGGER)
			continue;
		for (t = 1; t <= 4; t++)
			if (t != team && !NBTF_Allied(t, team) && g->score[t] > 0 && TF_TeamMay(g->team, t))
			{
				NB_EdCenter(g->ed, out);
				return true;
			}
	}
	return false;
}

/* the allied VIP (civilian on an allied civilian team), for bodyguards */
static edict_t *TF_AlliedVIP(nbbot_t *bot)
{
	int i, team = NB_TeamOf(NB_Self(bot));
	for (i = 0; i < sv.allocated_client_slots; i++)
	{
		edict_t *e = svs.clients[i].edict;
		int t;
		if (svs.clients[i].state != cs_spawned || !e || !NB_IsAlive(e))
			continue;
		t = NB_TeamOf(e);
		if (t != team && NBTF_Allied(t, team) && TF_Class(e) == PC_CIVILIAN)
			return e;
	}
	return NULL;
}

/* ------------------------------------------------------------------------------------------------
 * weapons per class
 */
typedef struct { int pclass, impulse; float speed, mindist, maxdist, pref; } tfweapon_t;
static const tfweapon_t tf_weapons[] = {
	{ PC_SCOUT, 4, 1000, 0, 900, 60 },      { PC_SCOUT, 2, 0, 0, 1500, 40 },
	{ PC_SNIPER, 2, 0, 400, 4000, 80 },     { PC_SNIPER, 3, 0, 0, 900, 60 },   { PC_SNIPER, 4, 1000, 0, 600, 30 },
	{ PC_SOLDIER, 7, 900, 120, 1600, 100 }, { PC_SOLDIER, 3, 0, 0, 450, 60 },  { PC_SOLDIER, 2, 0, 0, 1500, 30 },
	{ PC_DEMOMAN, 6, 600, 150, 750, 90 },   { PC_DEMOMAN, 2, 0, 0, 1500, 30 },
	{ PC_MEDIC, 5, 1000, 0, 1000, 80 },     { PC_MEDIC, 3, 0, 0, 450, 60 },    { PC_MEDIC, 2, 0, 0, 1500, 30 },
	{ PC_HVYWEAP, 7, 0, 0, 900, 100 },      { PC_HVYWEAP, 3, 0, 0, 450, 60 },  { PC_HVYWEAP, 2, 0, 0, 1500, 30 },
	{ PC_PYRO, 6, 600, 0, 320, 90 },        { PC_PYRO, 7, 600, 150, 900, 80 }, { PC_PYRO, 2, 0, 0, 1500, 30 },
	{ PC_SPY, 3, 0, 0, 450, 70 },           { PC_SPY, 4, 1000, 0, 900, 60 },   { PC_SPY, 2, 1500, 0, 1500, 40 },
	{ PC_ENGINEER, 3, 0, 0, 450, 70 },      { PC_ENGINEER, 2, 1500, 0, 1500, 60 },
	{ PC_CIVILIAN, 1, 0, 0, 64, 10 },
};

/* FortressOne: weapons are slots (1 primary, 2 secondary, 3 sidearm, 4 melee) */
static const tfweapon_t fo_weapons[] = {
	{ PC_SCOUT, 1, 1000, 0, 900, 60 },      { PC_SCOUT, 2, 0, 0, 1500, 40 },
	{ PC_SNIPER, 1, 0, 400, 4000, 80 },     { PC_SNIPER, 2, 0, 0, 900, 60 },
	{ PC_SOLDIER, 1, 900, 120, 1600, 100 }, { PC_SOLDIER, 2, 0, 0, 450, 60 },
	{ PC_DEMOMAN, 1, 600, 150, 750, 90 },   { PC_DEMOMAN, 3, 0, 0, 1500, 30 },
	{ PC_MEDIC, 1, 1000, 0, 1000, 80 },     { PC_MEDIC, 2, 0, 0, 450, 60 },
	{ PC_HVYWEAP, 1, 0, 0, 900, 100 },      { PC_HVYWEAP, 2, 0, 0, 450, 60 },
	{ PC_PYRO, 1, 600, 0, 320, 90 },        { PC_PYRO, 2, 600, 150, 900, 80 },
	{ PC_SPY, 2, 0, 0, 450, 70 },           { PC_SPY, 1, 1500, 0, 1500, 50 },
	{ PC_ENGINEER, 2, 0, 0, 450, 70 },      { PC_ENGINEER, 1, 1500, 0, 1500, 60 },
	{ PC_CIVILIAN, 4, 0, 0, 64, 10 },
};

static const tfweapon_t *TF_ChooseWeapon(int pclass, float dist)
{
	unsigned int i;
	const tfweapon_t *table = tf_profile == TFP_FO ? fo_weapons : tf_weapons;
	unsigned int count = tf_profile == TFP_FO ? countof(fo_weapons) : countof(tf_weapons);
	const tfweapon_t *best = NULL;
	float bests = -1;
	for (i = 0; i < count; i++)
	{
		const tfweapon_t *w = &table[i];
		float s;
		if (w->pclass != pclass)
			continue;
		s = w->pref;
		if (dist < w->mindist) s *= 0.3f;
		if (dist > w->maxdist) s *= 0.3f;
		if (s > bests)
		{
			bests = s;
			best = w;
		}
	}
	return best;
}

/* ------------------------------------------------------------------------------------------------
 * actions
 */
typedef struct { nbaction_t base; } tfroot_t;
typedef struct { nbaction_t base; const tfweapon_t *weapon; float nextweapon; int strafedir; float nextstrafe; } tffight_t;
typedef struct { nbaction_t base; vec3_t pos; float until; edict_t *ent; int kind; } tfgoto_t;
typedef struct { nbaction_t base; vec3_t spot; float nextspot; float until; } tfdefend_t;
typedef struct { nbaction_t base; edict_t *patient; float until; } tfheal_t;
typedef struct { nbaction_t base; vec3_t spot; float until; int step; float steptime; } tfbuild_t;

static const nbactionvt_t TF_RootVT, TF_FightVT, TF_GotoVT, TF_DefendVT, TF_HealVT, TF_BuildVT, TF_DetpackVT;

/* --- grenades (both slots, while fighting) ---------------------------------------------------- */
static void TF_Grenades(nbbot_t *bot, float dist)
{
	tfbot_t *t = TF(bot);
	edict_t *me = NB_Self(bot);
	float now = NB_Time();
	if (t->primetime > 0)
	{
		/* throw: the grenade leaves on the first timer tick after the throw (>= 0.8 s after
		   priming) and explodes 3.8 s after priming; time it for the distance */
		float flight = dist / 650.0f;
		float held = now - t->primetime;
		if (held >= 0.85f && (held + flight >= 3.0f || held > 3.0f || !bot->enemy))
		{
			TF_Impulse(bot, tfi->throwgren);
			t->primetime = 0;
			t->nextgren = now + 4 + frandom() * 4;
		}
		return;
	}
	if (!bot->enemy || now < t->nextgren || dist < 150 || dist > 900)
		return;
	if (NB_GetFloat(me, "no_grenades_1") > 0 && frandom() < 0.5f)
	{
		TF_Impulse(bot, tfi->prime1);
		t->primetime = now;
		t->primeslot = 1;
	}
	else if (NB_GetFloat(me, "no_grenades_2") > 0)
	{
		int type = (int)NB_GetFloat(me, "tp_grenades_2");
		/* flares and caltrops aren't worth it in a fight */
		if (type == 6 || type == 10)
			return;
		TF_Impulse(bot, tfi->prime2);
		t->primetime = now;
		t->primeslot = 2;
	}
}

/* --- Fight -------------------------------------------------------------------------------------- */
static nbresult_t Fight_OnStart(nbbot_t *bot, nbaction_t *self, nbaction_t *prior)
{
	((tffight_t*)self)->strafedir = (rand() & 1) ? 1 : -1;
	return NB_Continue();
}

static nbresult_t Fight_Update(nbbot_t *bot, nbaction_t *self, float dt)
{
	tffight_t *f = (tffight_t*)self;
	tfbot_t *t = TF(bot);
	edict_t *me = NB_Self(bot), *e = bot->enemy;
	int pclass = TF_Class(me);
	float dist, now = NB_Time();
	vec3_t aim;
	if (!e || e->v->health <= 0 || ED_ISFREE(e) || !NBTF_IsEnemy(bot, e))
		return NB_Done("enemy gone");
	if (now - bot->enemyseen > 1.5f)
		return NB_Done("lost sight");
	dist = Distance(me->v->origin, e->v->origin);
	/* never swap a spinning assault cannon (FortressOne asserts on it) */
	if (!f->weapon || (now >= f->nextweapon && !(pclass == PC_HVYWEAP && (bot->cl->lastcmd.buttons & 1))))
	{
		const tfweapon_t *w = TF_ChooseWeapon(pclass, dist);
		if (w && w != f->weapon)
			TF_Impulse(bot, w->impulse);
		f->weapon = w;
		f->nextweapon = now + 1.0f;
	}
	if (!f->weapon)
		return NB_Done("no weapon");
	NB_LeadTarget(bot, e, f->weapon->speed, aim);
	if (f->weapon->speed == 600 && f->weapon->mindist >= 150)
		aim[2] += dist * 0.12f;   /* grenade launcher / incendiary arc */
	NBBody_AimAt(bot, aim);

	if (pclass == PC_SNIPER && f->weapon->impulse == 2)
	{
		/* sniper rifle: stand still, hold attack to charge, release when aimed and charged */
		bot->hasmovegoal = false;
		if (!t->sniperstart)
			t->sniperstart = now;
		if (now - t->sniperstart < 1.2f + bot->skill || !NBBody_IsAimedAt(bot, aim, 2.5f))
			bot->buttons |= 1;   /* charging */
		else
			t->sniperstart = 0;  /* release: fires */
	}
	else
	{
		t->sniperstart = 0;
		if (now - bot->enemyfirstseen > bot->reaction && NBBody_IsAimedAt(bot, aim, 7 + (1 - bot->skill) * 10)
			&& !(f->weapon->mindist >= 120 && dist < 130))
			bot->buttons |= 1;
		if (pclass == PC_HVYWEAP && f->weapon->impulse == 7)
			bot->hasmovegoal = false;   /* assault cannon: rooted while firing */
		else
			NB_CombatMove(bot, dist, f->weapon->mindist + 40, f->weapon->maxdist * 0.6f, &f->strafedir, &f->nextstrafe);
	}
	TF_Grenades(bot, dist);
	return NB_Continue();
}

static void Fight_OnEnd(nbbot_t *bot, nbaction_t *self, nbaction_t *next)
{
	tfbot_t *t = TF(bot);
	if (t->primetime > 0)
	{	/* don't blow up holding it */
		TF_Impulse(bot, tfi->throwgren);
		t->primetime = 0;
	}
	t->sniperstart = 0;
}
static const nbactionvt_t TF_FightVT = { "Fight", sizeof(tffight_t), Fight_OnStart, Fight_Update, Fight_OnEnd };

/* fight anything in sight before going on; carriers and VIPs only fight when cornered */
static qboolean TF_WantsFight(nbbot_t *bot)
{
	return bot->enemy && NB_Time() - bot->enemyseen < 0.2f && NBTF_IsEnemy(bot, bot->enemy);
}

/* --- Goto (fetch item / capture / objective / resupply / detpack spot) ------------------------ */
enum { GOTO_FETCH, GOTO_CAPTURE, GOTO_OBJECTIVE, GOTO_RESUPPLY, GOTO_FOLLOW };

static nbresult_t Goto_OnStart(nbbot_t *bot, nbaction_t *self, nbaction_t *prior)
{
	tfgoto_t *g = (tfgoto_t*)self;
	g->until = NB_Time() + 25;
	bot->path.valid = false;
	return NB_Continue();
}

static nbresult_t Goto_Update(nbbot_t *bot, nbaction_t *self, float dt)
{
	tfgoto_t *g = (tfgoto_t*)self;
	edict_t *me = NB_Self(bot);
	float d;
	if (NB_Time() > g->until)
	{
		if (g->ent)
			NB_MarkBadGoal(bot, g->ent, 20);
		return NB_Done("timeout");
	}
	if (g->kind == GOTO_FOLLOW && g->ent)
	{
		if (!NB_IsAlive(g->ent))
			return NB_Done("lost who we followed");
		VectorCopy(g->ent->v->origin, g->pos);
		if (Distance(me->v->origin, g->pos) < 160)
		{
			bot->hasmovegoal = false;
			return NB_Time() - self->started > 3 ? NB_Done("with them") : NB_Continue();
		}
	}
	else if (g->ent)
		NB_EdCenter(g->ent, g->pos);
	/* the fight comes first unless we're carrying the flag near home */
	if (TF_WantsFight(bot) && (g->kind != GOTO_CAPTURE || Distance(me->v->origin, g->pos) > 500) && TF_Class(me) != PC_CIVILIAN)
		return NB_SuspendFor(NBAction_New(&TF_FightVT), "enemy");
	if (g->kind == GOTO_FETCH && g->ent && (int)NB_GetFloat(g->ent, "goal_state") == 1)
		return NB_Done("someone took it");
	if (g->kind == GOTO_CAPTURE && !TF_CarriedItem(bot))
		return NB_Done("captured / dropped");
	d = Distance(me->v->origin, g->pos);
	if (d < 24 && NB_Time() - self->started > 1)
	{
		if (g->ent)
			NB_MarkBadGoal(bot, g->ent, 10);
		return NB_Done("arrived");
	}
	if (!NBLoco_PathTo(bot, g->pos, 2))
	{
		if (g->ent)
			NB_MarkBadGoal(bot, g->ent, 20);
		return NB_Done("no path");
	}
	return NB_Continue();
}
static nbresult_t Goto_OnStuck(nbbot_t *bot, nbaction_t *self) { bot->path.valid = false; return NB_Sustain("repath"); }
static const nbactionvt_t TF_GotoVT = { "Goto", sizeof(tfgoto_t), Goto_OnStart, Goto_Update, NULL, NULL, NULL, Goto_OnStuck };

static nbaction_t *TF_NewGoto(int kind, edict_t *ent, const vec3_t pos, const char *name)
{
	tfgoto_t *g = (tfgoto_t*)NBAction_New(&TF_GotoVT);
	g->kind = kind;
	g->ent = ent;
	if (ent)
		NB_EdCenter(ent, g->pos);
	else
		VectorCopy(pos, g->pos);
	return &g->base;
}

/* --- Defend ------------------------------------------------------------------------------------ */
static nbresult_t Defend_OnStart(nbbot_t *bot, nbaction_t *self, nbaction_t *prior)
{
	((tfdefend_t*)self)->until = NB_Time() + 30 + frandom() * 30;
	return NB_Continue();
}

static nbresult_t Defend_Update(nbbot_t *bot, nbaction_t *self, float dt)
{
	tfdefend_t *d = (tfdefend_t*)self;
	edict_t *me = NB_Self(bot), *carrier;
	vec3_t point;
	if (TF_WantsFight(bot))
		return NB_SuspendFor(NBAction_New(&TF_FightVT), "enemy");
	if (NB_Time() > d->until)
		return NB_Done("rotate");
	if (!TF_DefencePoint(bot, point, &carrier))
		return NB_Done("nothing to defend");
	if (carrier)
	{	/* the flag is out: chase the carrier */
		if (!NBLoco_PathTo(bot, carrier->v->origin, 1))
			return NB_Done("can't reach carrier");
		return NB_Continue();
	}
	if (NB_Time() >= d->nextspot || Distance(d->spot, point) > 700)
	{
		/* a spot near the defence point, on the mesh */
		int tries;
		VectorCopy(point, d->spot);
		for (tries = 0; tries < 8; tries++)
		{
			vec3_t p;
			int a;
			p[0] = point[0] + crandom() * 400;
			p[1] = point[1] + crandom() * 400;
			p[2] = point[2];
			a = NBNav_AreaAt(p, 200);
			if (a >= 0)
			{
				NBNav_RandomPoint(&nb_mesh.areas[a], d->spot);
				d->spot[2] += 24;
				break;
			}
		}
		d->nextspot = NB_Time() + 6 + frandom() * 8;
		bot->path.valid = false;
	}
	if (Distance(me->v->origin, d->spot) > 48)
		NBLoco_PathTo(bot, d->spot, 3);
	else
	{
		bot->hasmovegoal = false;
		/* look around */
		if (!bot->hasaim)
		{
			vec3_t look;
			VectorCopy(point, look);
			look[2] += 30;
			NBBody_AimAt(bot, look);
		}
	}
	return NB_Continue();
}
static const nbactionvt_t TF_DefendVT = { "Defend", sizeof(tfdefend_t), Defend_OnStart, Defend_Update };

/* --- Heal (medic) ------------------------------------------------------------------------------ */
static edict_t *TF_HurtTeammate(nbbot_t *bot, float maxdist)
{
	edict_t *me = NB_Self(bot), *best = NULL;
	int i, team = NB_TeamOf(me);
	float bestscore = 0;
	for (i = 0; i < sv.allocated_client_slots; i++)
	{
		edict_t *e = svs.clients[i].edict;
		float need, d;
		if (svs.clients[i].state != cs_spawned || !e || e == me || !NB_IsAlive(e))
			continue;
		if (!NBTF_Allied(NB_TeamOf(e), team))
			continue;
		need = e->v->max_health > 0 ? 1 - e->v->health / e->v->max_health : 0;
		if (need < 0.3f)
			continue;
		d = Distance(me->v->origin, e->v->origin);
		if (d > maxdist)
			continue;
		if (need / (d + 200) > bestscore)
		{
			bestscore = need / (d + 200);
			best = e;
		}
	}
	return best;
}

static nbresult_t Heal_OnStart(nbbot_t *bot, nbaction_t *self, nbaction_t *prior)
{
	((tfheal_t*)self)->until = NB_Time() + 12;
	TF_Impulse(bot, tfi->medikit);
	return NB_Continue();
}

static nbresult_t Heal_Update(nbbot_t *bot, nbaction_t *self, float dt)
{
	tfheal_t *h = (tfheal_t*)self;
	edict_t *me = NB_Self(bot), *p = h->patient;
	vec3_t aim;
	if (!p || !NB_IsAlive(p) || NB_Time() > h->until || p->v->health >= p->v->max_health)
		return NB_Done("healed / gone");
	if (TF_WantsFight(bot) && Distance(me->v->origin, bot->enemy->v->origin) < 500)
		return NB_ChangeTo(NBAction_New(&TF_FightVT), "attacked while healing");
	if (Distance(me->v->origin, p->v->origin) > 56)
	{
		NBLoco_PathTo(bot, p->v->origin, 0.5f);
		return NB_Continue();
	}
	VectorCopy(p->v->origin, aim);
	NBBody_AimAt(bot, aim);
	bot->hasmovegoal = false;
	if (NBBody_IsAimedAt(bot, aim, 20))
		bot->buttons |= 1;
	return NB_Continue();
}
static const nbactionvt_t TF_HealVT = { "Heal", sizeof(tfheal_t), Heal_OnStart, Heal_Update };

/* --- Build (engineer) -------------------------------------------------------------------------- */
static nbresult_t Build_OnStart(nbbot_t *bot, nbaction_t *self, nbaction_t *prior)
{
	tfbuild_t *b = (tfbuild_t*)self;
	edict_t *carrier;
	vec3_t point;
	b->until = NB_Time() + 40;
	if (!TF_DefencePoint(bot, point, &carrier) || carrier)
		VectorCopy(NB_Self(bot)->v->origin, point);
	/* a spot a little way from the point, toward the middle of the area it's in */
	{
		int a = NBNav_AreaAt(point, 200);
		if (a < 0)
			a = NBNav_NearestArea(point, 400);
		if (a < 0)
			return NB_Done("nowhere to build");
		NBNav_RandomPoint(&nb_mesh.areas[a], b->spot);
		b->spot[2] += 24;
	}
	return NB_Continue();
}

static nbresult_t Build_Update(nbbot_t *bot, nbaction_t *self, float dt)
{
	tfbuild_t *b = (tfbuild_t*)self;
	edict_t *me = NB_Self(bot);
	float now = NB_Time();
	if (now > b->until)
		return NB_Done("timeout");
	if (NB_GetFloat(me, "has_sentry") > 0 && b->step >= 2)
		return NB_Done("built");
	if (TF_WantsFight(bot) && b->step < 2)
		return NB_SuspendFor(NBAction_New(&TF_FightVT), "enemy");
	if (b->step == 0)
	{
		if (Distance(me->v->origin, b->spot) > 40)
		{
			if (!NBLoco_PathTo(bot, b->spot, 2))
				return NB_Done("no path to build spot");
			return NB_Continue();
		}
		bot->hasmovegoal = false;
		if (!((int)me->v->flags & FL_ONGROUND) || NB_GetFloat(me, "ammo_cells") < 130)
			return NB_Continue();
		if (tfi->buildsentry)
		{
			TF_Impulse(bot, tfi->buildsentry);
			b->step = 2;
		}
		else
		{
			TF_Impulse(bot, tfi->buildmenu);
			b->step = 1;
		}
		b->steptime = now;
		return NB_Continue();
	}
	if (b->step == 1)
	{
		bot->hasmovegoal = false;
		if (now - b->steptime > 0.3f)
		{
			TF_Impulse(bot, 2);   /* build menu: 2 = sentry gun */
			b->step = 2;
			b->steptime = now;
		}
		return NB_Continue();
	}
	/* building takes a few seconds; stay put */
	bot->hasmovegoal = false;
	if (now - b->steptime > 7)
		return NB_Done(NB_GetFloat(me, "has_sentry") > 0 ? "built" : "build failed");
	return NB_Continue();
}
static const nbactionvt_t TF_BuildVT = { "BuildSentry", sizeof(tfbuild_t), Build_OnStart, Build_Update };

/* engineer maintenance: whack the sentry with the spanner (menu 17: 2 upgrade, 3 repair) */
typedef struct { nbaction_t base; edict_t *sentry; float until; int step; float steptime; } tfmaint_t;
static nbresult_t Maint_OnStart(nbbot_t *bot, nbaction_t *self, nbaction_t *prior)
{
	((tfmaint_t*)self)->until = NB_Time() + 15;
	TF_Impulse(bot, 1);   /* melee: the spanner */
	return NB_Continue();
}
static nbresult_t Maint_Update(nbbot_t *bot, nbaction_t *self, float dt)
{
	tfmaint_t *m = (tfmaint_t*)self;
	edict_t *me = NB_Self(bot), *s = m->sentry;
	vec3_t c;
	float now = NB_Time();
	if (!s || ED_ISFREE(s) || s->v->health <= 0 || now > m->until)
		return NB_Done("done maintaining");
	if (TF_WantsFight(bot) && Distance(me->v->origin, bot->enemy->v->origin) < 300)
		return NB_ChangeTo(NBAction_New(&TF_FightVT), "defend the gun");
	NB_EdCenter(s, c);
	if (Distance(me->v->origin, c) > 56)
	{
		NBLoco_PathTo(bot, c, 1);
		return NB_Continue();
	}
	bot->hasmovegoal = false;
	NBBody_AimAt(bot, c);
	if ((int)NB_GetFloat(me, "current_menu") == 17)
	{	/* spanner menu on the sentry: upgrade if we can afford it, else repair, then leave */
		if (now - m->steptime > 0.3f)
		{
			TF_Impulse(bot, (s->v->weapon < 3 && NB_GetFloat(me, "ammo_cells") >= 130) ? 2 : 3);
			m->steptime = now;
			m->step++;
			if (m->step > 2)
				return NB_Done("maintained");
		}
		return NB_Continue();
	}
	if (NBBody_IsAimedAt(bot, c, 15))
		bot->buttons |= 1;
	return NB_Continue();
}
static const nbactionvt_t TF_MaintVT = { "MaintainSentry", sizeof(tfmaint_t), Maint_OnStart, Maint_Update };

static edict_t *TF_MySentry(nbbot_t *bot)
{
	edict_t *me = NB_Self(bot);
	int i;
	for (i = sv.allocated_client_slots + 1; i < sv.world.num_edicts; i++)
	{
		edict_t *e = EDICT_NUM_PB(svprogfuncs, i);
		if (!ED_ISFREE(e) && !strcmp(NB_Classname(e), "building_sentrygun") && NB_GetEntity(e, "real_owner") == me)
			return e;
	}
	return NULL;
}

/* --- Detpack (demoman) ------------------------------------------------------------------------- */
static nbresult_t Det_OnStart(nbbot_t *bot, nbaction_t *self, nbaction_t *prior)
{
	((tfbuild_t*)self)->until = NB_Time() + 40;
	return NB_Continue();
}
static nbresult_t Det_Update(nbbot_t *bot, nbaction_t *self, float dt)
{
	tfbuild_t *b = (tfbuild_t*)self;
	edict_t *me = NB_Self(bot);
	float now = NB_Time();
	if (now > b->until)
		return NB_Done("timeout");
	if (b->step == 0)
	{
		if (TF_WantsFight(bot))
			return NB_SuspendFor(NBAction_New(&TF_FightVT), "enemy");
		if (Distance(me->v->origin, b->spot) > 64)
		{
			if (!NBLoco_PathTo(bot, b->spot, 2))
				return NB_Done("no path");
			return NB_Continue();
		}
		bot->hasmovegoal = false;
		if (!((int)me->v->flags & FL_ONGROUND))
			return NB_Continue();
		TF_Impulse(bot, tfi->detpack);
		b->step = 1;
		b->steptime = now;
		return NB_Continue();
	}
	if (b->step == 1)
	{	/* setting takes 3 s, then run */
		bot->hasmovegoal = false;
		if (now - b->steptime > 3.5f)
		{
			vec3_t away;
			if (NB_PickRoamPoint(bot, away, 700))
				return NB_ChangeTo(TF_NewGoto(GOTO_OBJECTIVE, NULL, away, "flee detpack"), "run from the detpack");
			return NB_Done("set");
		}
	}
	return NB_Continue();
}
static const nbactionvt_t TF_DetpackVT = { "Detpack", sizeof(tfbuild_t), Det_OnStart, Det_Update };

/* --- Join -------------------------------------------------------------------------------------- */
static nbresult_t Join_Update(nbbot_t *bot, nbaction_t *self, float dt)
{
	tfbot_t *t = TF(bot);
	edict_t *me = NB_Self(bot);
	float now = NB_Time();
	int team = NB_TeamOf(me), pclass = TF_Class(me);
	bot->hasmovegoal = false;
	if (team > 0 && pclass > 0 && NB_IsAlive(me) && me->v->modelindex)
		return NB_Done("playing");
	if (now < t->nextjoin)
		return NB_Continue();
	t->nextjoin = now + 1.2f;
	if (team <= 0)
	{
		int want = bot->wantteam >= 1 && bot->wantteam <= tf_numteams ? bot->wantteam : TF_PickTeam(bot);
		/* close any menu first (impulse 1-10 go to menus) */
		TF_Impulse(bot, tfi->team1 + want - 1);
		t->team = want;
		return NB_Continue();
	}
	if (pclass <= 0)
	{
		if (tf_civilianteams & (1 << (team - 1)))
			return NB_Continue();   /* civilian teams get their class automatically */
		if (!t->pclass || t->classtries >= 2)
		{
			t->pclass = TF_PickClass(bot, team);
			if (t->classtries >= 2)
			{
				/* rejected (restriction): try anything legal */
				int c;
				for (c = 1; c <= 9; c++)
				{
					int cc = ((t->pclass + c) % 9) + 1;
					if (TF_ClassLegal(team, cc))
					{
						t->pclass = cc;
						break;
					}
				}
			}
			t->classtries = 0;
		}
		TF_Impulse(bot, tfi->class1 + t->pclass - 1);
		t->classtries++;
		return NB_Continue();
	}
	/* has team and class but dead: press to respawn */
	if (!NB_IsAlive(me))
		bot->buttons |= 1;
	return NB_Continue();
}
static const nbactionvt_t TF_JoinVT = { "Join", sizeof(nbaction_t), NULL, Join_Update };

/* --- spy disguise after spawning --------------------------------------------------------------- */
static void TF_SpyDisguise(nbbot_t *bot)
{
	tfbot_t *t = TF(bot);
	edict_t *me = NB_Self(bot);
	float now = NB_Time();
	int team = NB_TeamOf(me), enemy;
	if (TF_Class(me) != PC_SPY || NB_GetFloat(me, "is_undercover") > 0)
	{
		t->disguisestep = 0;
		return;
	}
	if (now < t->disguisestep_t)
		return;
	if (tfi->disguiseenemy)
	{	/* FortressOne: one impulse; is_undercover doesn't exist there, so just repeat now and then */
		TF_Impulse(bot, tfi->disguiseenemy);
		t->disguisestep_t = now + 30;
		return;
	}
	for (enemy = 1; enemy <= tf_numteams; enemy++)
		if (!NBTF_Allied(enemy, team))
			break;
	if (enemy > tf_numteams)
		return;
	switch (t->disguisestep)
	{
	case 0: TF_Impulse(bot, tfi->spymenu); t->disguisestep = 1; break;   /* spy menu */
	case 1: TF_Impulse(bot, 2); t->disguisestep = 2; break;              /* 2 = change team */
	case 2: TF_Impulse(bot, enemy); t->disguisestep = 3; break;          /* the enemy team */
	default: t->disguisestep = 0; t->disguisestep_t = now + 20; return;  /* retry later if it failed */
	}
	t->disguisestep_t = now + 0.4f;
}

/* --- RideVehicle (border1's truck) ------------------------------------------------------------- */
typedef struct { nbaction_t base; int vehicle; float nextjump; float until; } tfride_t;

static qboolean TF_InVehicle(edict_t *me, edict_t *door)
{
	return me->v->origin[0] > door->v->absmin[0] + 4 && me->v->origin[0] < door->v->absmax[0] - 4
		&& me->v->origin[1] > door->v->absmin[1] + 4 && me->v->origin[1] < door->v->absmax[1] - 4
		&& me->v->origin[2] - 24 >= door->v->absmin[2] + 8 && me->v->origin[2] - 24 <= door->v->absmax[2] + 4;
}

static nbresult_t Ride_OnStart(nbbot_t *bot, nbaction_t *self, nbaction_t *prior)
{
	((tfride_t*)self)->until = NB_Time() + 90;
	return NB_Continue();
}

static nbresult_t Ride_Update(nbbot_t *bot, nbaction_t *self, float dt)
{
	tfride_t *r = (tfride_t*)self;
	tfvehicle_t *v;
	edict_t *me = NB_Self(bot), *door;
	vec3_t trig, doorofs;
	qboolean atstart, inside;
	float now = NB_Time();
	if (r->vehicle >= tf_numvehicles || now > r->until)
		return NB_Done("no vehicle");
	v = &tf_vehicles[r->vehicle];
	door = v->door;
	VectorSubtract(door->v->origin, v->pos1, doorofs);
	atstart = VectorLength(doorofs) < 8;
	inside = TF_InVehicle(me, door);
	NB_EdCenter(v->trigger, trig);   /* static: only inside the vehicle while it's at its start */

	if (inside)
	{
		/* in the vehicle: stand on the start trigger until it goes, then hold still; shoot from it */
		if (atstart)
			NBLoco_SetGoal(bot, trig);
		else
			bot->hasmovegoal = false;
		bot->path.valid = false;
		if (bot->enemy && now - bot->enemyseen < 0.3f && NBTF_IsEnemy(bot, bot->enemy))
		{
			const tfweapon_t *w = TF_ChooseWeapon(TF_Class(me), Distance(me->v->origin, bot->enemy->v->origin));
			vec3_t aim;
			if (w)
			{
				NB_LeadTarget(bot, bot->enemy, w->speed, aim);
				NBBody_AimAt(bot, aim);
				if (NBBody_IsAimedAt(bot, aim, 10))
					bot->buttons |= 1;
			}
		}
		return NB_Continue();
	}
	if (!atstart)
	{
		/* it left without us: wait near where it starts */
		vec3_t home;
		VectorAdd(door->v->absmin, door->v->absmax, home);
		VectorScale(home, 0.5f, home);
		VectorSubtract(home, doorofs, home);
		home[0] -= (door->v->absmax[0] - door->v->absmin[0]) * 0.5f + 48;
		if (TF_WantsFight(bot))
			return NB_SuspendFor(NBAction_New(&TF_FightVT), "enemy while waiting for the vehicle");
		if (Distance(me->v->origin, home) > 96)
			NBLoco_PathTo(bot, home, 2);
		else
			bot->hasmovegoal = false;
		return NB_Continue();
	}
	/* get in: walk up to it on the mesh, then climb in directly with jumps */
	{
		float dx = max(door->v->absmin[0] - me->v->origin[0], me->v->origin[0] - door->v->absmax[0]);
		float dy = max(door->v->absmin[1] - me->v->origin[1], me->v->origin[1] - door->v->absmax[1]);
		float edge = max(dx, dy);
		if (edge > 80)
		{
			if (TF_WantsFight(bot))
				return NB_SuspendFor(NBAction_New(&TF_FightVT), "enemy on the way to the vehicle");
			if (!NBLoco_PathTo(bot, trig, 2))
			{
				NBLoco_SetGoal(bot, trig);
				bot->path.valid = false;
			}
			return NB_Continue();
		}
		NBLoco_SetGoal(bot, trig);
		bot->path.valid = false;
		if (((int)me->v->flags & FL_ONGROUND) && now >= r->nextjump)
		{
			bot->wantjump = true;
			r->nextjump = now + 0.6f;
		}
	}
	return NB_Continue();
}
static nbresult_t Ride_OnStuck(nbbot_t *bot, nbaction_t *self) { bot->wantjump = true; return NB_Sustain("hop"); }
static const nbactionvt_t TF_RideVT = { "RideVehicle", sizeof(tfride_t), Ride_OnStart, Ride_Update, NULL, NULL, NULL, Ride_OnStuck };

/* --- Root -------------------------------------------------------------------------------------- */
static nbresult_t Root_Update(nbbot_t *bot, nbaction_t *self, float dt)
{
	tfbot_t *t = TF(bot);
	edict_t *me = NB_Self(bot), *carrier;
	int team = NB_TeamOf(me), pclass = TF_Class(me);
	tfgoal_t *item, *g;
	vec3_t p;

	TF_MaybeCensus();
	if (team <= 0 || pclass <= 0 || !me->v->modelindex)
		return NB_SuspendFor(NBAction_New(&TF_JoinVT), "join");
	TF_SpyDisguise(bot);

	/* carrying an enemy item: bring it home */
	item = TF_CarriedItem(bot);
	if (item && (g = TF_CaptureFor(bot, item)))
		return NB_SuspendFor(TF_NewGoto(GOTO_CAPTURE, g->ed, NULL, "capture"), "carrying an item");

	/* an escape vehicle for this team (border1): get in it */
	{
		int vi;
		for (vi = 0; vi < tf_numvehicles; vi++)
			if (tf_vehicles[vi].team == team)
			{
				tfride_t *r = (tfride_t*)NBAction_New(&TF_RideVT);
				r->vehicle = vi;
				return NB_SuspendFor(&r->base, "escape vehicle");
			}
	}

	/* the VIP: head for the escape, but not miles ahead of the escort */
	if (pclass == PC_CIVILIAN)
	{
		g = TF_ObjectiveFor(bot);
		if (g)
			return NB_SuspendFor(TF_NewGoto(GOTO_OBJECTIVE, g->ed, NULL, "escape"), "VIP escape");
	}

	if (TF_WantsFight(bot))
		return NB_SuspendFor(NBAction_New(&TF_FightVT), "enemy");

	/* class jobs */
	if (pclass == PC_MEDIC)
	{
		edict_t *patient = TF_HurtTeammate(bot, 900);
		if (patient)
		{
			tfheal_t *h = (tfheal_t*)NBAction_New(&TF_HealVT);
			h->patient = patient;
			return NB_SuspendFor(&h->base, "heal");
		}
	}
	if (pclass == PC_ENGINEER)
	{
		edict_t *s = TF_MySentry(bot);
		if (!s && NB_GetFloat(me, "ammo_cells") >= 130 && NB_Time() >= t->nextspecial)
		{
			t->nextspecial = NB_Time() + 20;
			return NB_SuspendFor(NBAction_New(&TF_BuildVT), "build a sentry");
		}
		if (s && (s->v->health < s->v->max_health * 0.8f || s->v->weapon < 3) && NB_Time() >= t->nextspecial)
		{
			tfmaint_t *m = (tfmaint_t*)NBAction_New(&TF_MaintVT);
			m->sentry = s;
			t->nextspecial = NB_Time() + 12;
			return NB_SuspendFor(&m->base, "maintain the sentry");
		}
	}
	if (pclass == PC_DEMOMAN && NB_GetFloat(me, "ammo_detpack") > 0 && NB_GetFloat(me, "is_detpacking") <= 0)
	{
		int i;
		for (i = 0; i < tf_numgoals; i++)
		{
			tfgoal_t *dg = &tf_goals[i];
			if (dg->kind == TFG_DETPACK && TF_TeamMay(dg->team, team) && (int)NB_GetFloat(dg->ed, "goal_state") == 2 && !NB_IsBadGoal(bot, dg->ed))
			{
				tfbuild_t *b = (tfbuild_t*)NBAction_New(&TF_DetpackVT);
				NB_EdCenter(dg->ed, b->spot);
				NB_MarkBadGoal(bot, dg->ed, 60);
				return NB_SuspendFor(&b->base, "detpack goal");
			}
		}
	}

	/* bodyguards stay near their VIP */
	{
		edict_t *vip = TF_AlliedVIP(bot);
		if (vip && (Distance(vip->v->origin, me->v->origin) > 250 || frandom() < 0.5f))
		{
			tfgoto_t *g2 = (tfgoto_t*)TF_NewGoto(GOTO_FOLLOW, vip, NULL, "escort");
			return NB_SuspendFor(&g2->base, "escort the VIP");
		}
	}

	/* team objectives */
	g = TF_ObjectiveFor(bot);
	if (g && !NB_IsBadGoal(bot, g->ed) && (t->role == 0 || !TF_DefencePoint(bot, p, &carrier)))
		return NB_SuspendFor(TF_NewGoto(GOTO_OBJECTIVE, g->ed, NULL, "objective"), "objective");

	if (t->role == 0 || pclass == PC_SCOUT || pclass == PC_SPY)
	{
		edict_t *teamcarrier;
		item = TF_ItemToSteal(bot, &teamcarrier);
		if (item && teamcarrier && teamcarrier != me)
		{
			tfgoto_t *g2 = (tfgoto_t*)TF_NewGoto(GOTO_FOLLOW, teamcarrier, NULL, "escort carrier");
			return NB_SuspendFor(&g2->base, "escort our carrier");
		}
		if (item && !teamcarrier && !NB_IsBadGoal(bot, item->ed))
			return NB_SuspendFor(TF_NewGoto(GOTO_FETCH, item->ed, NULL, "fetch"), "steal the flag");
	}
	if (TF_DefencePoint(bot, p, &carrier))
		return NB_SuspendFor(NBAction_New(&TF_DefendVT), "defend");

	{	/* nothing to do: roam */
		extern nbaction_t *NBDM_NewRoam(void);
		return NB_SuspendFor(NBDM_NewRoam(), "roam");
	}
}

static nbresult_t Root_OnResume(nbbot_t *bot, nbaction_t *self, nbaction_t *interrupter)
{
	/* switch attack/defence now and then */
	if (frandom() < 0.1f)
		TF(bot)->role ^= 1;
	return NB_Continue();
}

static const nbactionvt_t TF_RootVT = { "TF", sizeof(tfroot_t), NULL, Root_Update, NULL, NULL, Root_OnResume };

nbaction_t *NBTF_RootAction(void)
{
	return NBAction_New(&TF_RootVT);
}

#endif

#ifndef CLIENTONLY
/* qw_bot_tfinfo: what the TF census found on this map */
static void TF_Info_f(void)
{
	static const char *kinds[] = { "item", "capture", "touch", "trigger", "detpack", "spanner", "resupply" };
	int i;
	TF_Census();
	Con_Printf("TF profile %s, %d teams, civilian teams %x, allies %x %x %x %x\n", tf_profile == TFP_FO ? "FortressOne" : "TF 2.8/2.9",
		tf_numteams, tf_civilianteams, tf_allies[1], tf_allies[2], tf_allies[3], tf_allies[4]);
	for (i = 0; i < tf_numgoals; i++)
	{
		tfgoal_t *g = &tf_goals[i];
		vec3_t c;
		NB_EdCenter(g->ed, c);
		Con_Printf("%3d %-8s %-24s team %d owned %d goal %d needs %d score %d/%d/%d/%d%s state %.0f at %.0f %.0f %.0f\n", i, kinds[g->kind],
			PR_GetString(svprogfuncs, g->ed->v->netname), g->team, g->ownedby, g->goalno, g->needitem,
			g->score[1], g->score[2], g->score[3], g->score[4], g->hurts ? " HURTS" : "",
			NB_GetFloat(g->ed, "goal_state"), c[0], c[1], c[2]);
	}
}

void NBTF_InitCommands(void)
{
	Cmd_AddCommandD("qw_bot_tfinfo", TF_Info_f, "List the TF goals the bots know about on this map");
	{
		extern void NBTF_InitCommands2(void);
		NBTF_InitCommands2();
	}
}
#endif

#ifndef CLIENTONLY
/* qw_bot_pathtest <slot> <goal#>: can this bot path to that TF goal? */
static void TF_PathTest_f(void)
{
	int slot = atoi(Cmd_Argv(1)), gi = atoi(Cmd_Argv(2));
	nbpath_t *path;
	vec3_t c;
	edict_t *me;
	int s, g, team;
	TF_Census();
	if (slot < 0 || slot >= NB_MAX_BOTS || !nb_bots[slot].inuse || gi < 0 || gi >= tf_numgoals)
	{
		Con_Printf("usage: qw_bot_pathtest <botslot> <goal# from qw_bot_tfinfo>\n");
		return;
	}
	me = nb_bots[slot].cl->edict;
	team = NB_TeamOf(me);
	NB_EdCenter(tf_goals[gi].ed, c);
	s = NBNav_AreaAt(me->v->origin, 96);
	g = NBNav_AreaAt(c, 160);
	path = Z_Malloc(sizeof(*path));
	Con_Printf("from area %d to area %d (nearest %d): ", s, g, NBNav_NearestArea(c, 400));
	if (NBPath_Compute(path, me->v->origin, c, team, NULL, NULL))
		Con_Printf("path %d segments, ends at %.0f %.0f %.0f\n", path->numsegs, path->seg[path->numsegs - 1].pos[0], path->seg[path->numsegs - 1].pos[1], path->seg[path->numsegs - 1].pos[2]);
	else if (NBPath_Compute(path, me->v->origin, c, 0, NULL, NULL))
		Con_Printf("NO path for team %d, but there is one ignoring team doors (%d segments)\n", team, path->numsegs);
	else
		Con_Printf("NO path at all\n");
	Z_Free(path);
}

void NBTF_InitCommands2(void)
{
	Cmd_AddCommandD("qw_bot_pathtest", TF_PathTest_f, "qw_bot_pathtest <botslot> <goal#>");
}
#endif
