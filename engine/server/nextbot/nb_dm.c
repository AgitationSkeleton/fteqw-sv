/*
 * QuakeWorld deathmatch behaviour (and the shared combat helpers used by TF).
 * Copyright (C) 2026 fteqw-sv contributors. GPLv2 or later; see nb_local.h.
 *
 *   DMRoot ── Attack     (enemy in sight: pick a weapon, lead the target, strafe, keep range)
 *          ├─ Chase      (enemy just lost: go to where it was last seen)
 *          ├─ GetItem    (the most useful item: weapons, armour, health, ammo, powerups)
 *          └─ Roam       (nothing better: a random far area)
 */
#include "nb_local.h"

#ifndef CLIENTONLY

/* id QW item bits */
#define IT_SHOTGUN          1
#define IT_SUPER_SHOTGUN    2
#define IT_NAILGUN          4
#define IT_SUPER_NAILGUN    8
#define IT_GRENADE_LAUNCHER 16
#define IT_ROCKET_LAUNCHER  32
#define IT_LIGHTNING        64
#define IT_AXE              4096
#define IT_ARMOR1           8192
#define IT_ARMOR2           16384
#define IT_ARMOR3           32768
#define IT_QUAD             4194304
#define IT_INVULNERABILITY  1048576

extern cvar_t qw_bot_meleeonly, qw_bot_spawnloadouts, qw_bot_spawnloadouts_time;
extern cvar_t qw_bot_spawnloadouts_ssg, qw_bot_spawnloadouts_ng, qw_bot_spawnloadouts_sng;
extern cvar_t qw_bot_spawnloadouts_gl, qw_bot_spawnloadouts_rl, qw_bot_spawnloadouts_lg;
extern int NB_TeamOf(edict_t *ed);

qboolean NB_IsEnemy(nbbot_t *bot, edict_t *other);
qboolean NBTF_IsEnemy(nbbot_t *bot, edict_t *other);

qboolean NB_IsEnemy(nbbot_t *bot, edict_t *other)
{
	edict_t *self = NB_Self(bot);
	int n;
	if (nb_game == NBGAME_TF)
		return NBTF_IsEnemy(bot, other);
	n = NUM_FOR_EDICT(svprogfuncs, other);
	if (n < 1 || n > sv.allocated_client_slots)
		return false;   /* DM: only players */
	if (svs.clients[n - 1].state != cs_spawned || svs.clients[n - 1].spectator)
		return false;
	if (!NB_IsAlive(other))
		return false;
	if (teamplay.ival)
	{
		const char *a = InfoBuf_ValueForKey(&bot->cl->userinfo, "team");
		const char *b = InfoBuf_ValueForKey(&svs.clients[n - 1].userinfo, "team");
		if (*a && !strcmp(a, b))
			return false;
	}
	(void)self;
	return true;
}

/* ------------------------------------------------------------------------------------------------
 * items
 */
typedef struct
{
	const char *classname;
	int kind;
} nbitemdef_t;

enum { NBI_WEAPON, NBI_ARMOR, NBI_HEALTH, NBI_AMMO, NBI_POWERUP };

qboolean NB_IsBadGoal(nbbot_t *bot, edict_t *e)
{
	int i;
	for (i = 0; i < 4; i++)
		if (bot->badgoal[i] == e && NB_Time() < bot->badgoaluntil[i])
			return true;
	return false;
}

void NB_MarkBadGoal(nbbot_t *bot, edict_t *e, float seconds)
{
	int i, oldest = 0;
	for (i = 0; i < 4; i++)
	{
		if (bot->badgoal[i] == e || NB_Time() >= bot->badgoaluntil[i])
		{
			oldest = i;
			break;
		}
		if (bot->badgoaluntil[i] < bot->badgoaluntil[oldest])
			oldest = i;
	}
	bot->badgoal[oldest] = e;
	bot->badgoaluntil[oldest] = NB_Time() + seconds;
}

static float NBDM_ItemValue(nbbot_t *bot, edict_t *item)
{
	edict_t *self = NB_Self(bot);
	const char *cn = NB_Classname(item);
	int items = (int)self->v->items;
	float hp = self->v->health, armor = self->v->armorvalue;

	if (!strncmp(cn, "weapon_", 7))
	{
		/* deathmatch 2/3/5: weapons stay and an owned weapon cannot be picked up again */
		qboolean stay = deathmatch.ival == 2 || deathmatch.ival == 3 || deathmatch.ival == 5;
		static const struct { const char *cn; int bit; } wbits[] = {
			{ "weapon_rocketlauncher", IT_ROCKET_LAUNCHER }, { "weapon_lightning", IT_LIGHTNING }, { "weapon_grenadelauncher", IT_GRENADE_LAUNCHER },
			{ "weapon_supernailgun", IT_SUPER_NAILGUN }, { "weapon_supershotgun", IT_SUPER_SHOTGUN }, { "weapon_nailgun", IT_NAILGUN } };
		unsigned int k;
		if (stay)
			for (k = 0; k < countof(wbits); k++)
				if (!strcmp(cn, wbits[k].cn) && (items & wbits[k].bit))
					return 0;
		if (!strcmp(cn, "weapon_rocketlauncher"))     return (items & IT_ROCKET_LAUNCHER) ? (self->v->ammo_rockets < 20 ? 25 : 5) : 100;
		if (!strcmp(cn, "weapon_lightning"))          return (items & IT_LIGHTNING) ? (self->v->ammo_cells < 40 ? 20 : 5) : 90;
		if (!strcmp(cn, "weapon_grenadelauncher"))    return (items & IT_GRENADE_LAUNCHER) ? 8 : 60;
		if (!strcmp(cn, "weapon_supernailgun"))       return (items & IT_SUPER_NAILGUN) ? 8 : 55;
		if (!strcmp(cn, "weapon_supershotgun"))       return (items & IT_SUPER_SHOTGUN) ? 6 : 45;
		if (!strcmp(cn, "weapon_nailgun"))            return (items & IT_NAILGUN) ? 5 : 30;
		return 5;
	}
	if (!strncmp(cn, "item_armor", 10))
	{
		float give = !strcmp(cn, "item_armorInv") ? 200 : (!strcmp(cn, "item_armor2") ? 150 : 100);
		if (armor >= give * 0.9f)
			return 0;
		return (give - armor) * 0.5f;
	}
	if (!strcmp(cn, "item_health"))
	{
		qboolean mega = ((int)item->v->spawnflags & 2) != 0;
		if (mega)
			return hp < 250 ? 70 : 0;
		if (hp >= 100)
			return 0;
		return (100 - hp) * 0.8f;
	}
	if (!strcmp(cn, "item_artifact_super_damage"))  return 120;
	if (!strcmp(cn, "item_artifact_invulnerability"))return 110;
	if (!strcmp(cn, "item_artifact_invisibility"))  return 60;
	if (!strcmp(cn, "item_artifact_envirosuit"))    return 5;
	if (!strcmp(cn, "item_shells"))  return self->v->ammo_shells < 50 ? 12 : 0;
	if (!strcmp(cn, "item_spikes"))  return (items & (IT_NAILGUN | IT_SUPER_NAILGUN)) && self->v->ammo_nails < 100 ? 12 : 0;
	if (!strcmp(cn, "item_rockets")) return (items & (IT_ROCKET_LAUNCHER | IT_GRENADE_LAUNCHER)) && self->v->ammo_rockets < 30 ? 22 : 0;
	if (!strcmp(cn, "item_cells"))   return (items & IT_LIGHTNING) && self->v->ammo_cells < 60 ? 15 : 0;
	if (!strcmp(cn, "item_backpack") || !strcmp(cn, "backpack")) return 18;
	return 0;
}

static qboolean NB_ItemAvailable(edict_t *item)
{
	const char *m;
	if (ED_ISFREE(item) || item->v->solid != SOLID_TRIGGER)
		return false;
	m = PR_GetString(svprogfuncs, item->v->model);
	return m && *m;
}

/* the best item to fetch, scored by value over travel distance */
static edict_t *NBDM_PickItem(nbbot_t *bot, float maxdist, float *outvalue)
{
	edict_t *self = NB_Self(bot);
	edict_t *best = NULL;
	float bestscore = 0;
	int i;
	for (i = sv.allocated_client_slots + 1; i < sv.world.num_edicts; i++)
	{
		edict_t *e = EDICT_NUM_PB(svprogfuncs, i);
		const char *cn;
		float value, dist, score;
		vec3_t c;
		if (ED_ISFREE(e) || !NB_ItemAvailable(e))
			continue;
		cn = NB_Classname(e);
		if (strncmp(cn, "item_", 5) && strncmp(cn, "weapon_", 7) && strcmp(cn, "backpack"))
			continue;
		if (NB_IsBadGoal(bot, e))
			continue;
		value = NBDM_ItemValue(bot, e);
		if (value <= 0)
			continue;
		VectorAdd(e->v->absmin, e->v->absmax, c);
		VectorScale(c, 0.5f, c);
		dist = Distance(self->v->origin, c);
		if (dist > maxdist)
			continue;
		score = value / (dist + 300);
		if (score > bestscore)
		{
			/* reachable? (a travel estimate on the mesh) */
			float travel = NBPath_TravelDistance(self->v->origin, c, NB_TeamOf(self), maxdist * 2);
			if (travel < 0)
				continue;
			score = value / (travel + 300);
			if (score > bestscore)
			{
				bestscore = score;
				best = e;
			}
		}
	}
	if (outvalue)
		*outvalue = bestscore;
	return best;
}

/* ------------------------------------------------------------------------------------------------
 * weapons (DM)
 */
typedef struct { int impulse, item; float speed; float mindist, maxdist; float pref; int ammo; } nbweapon_t;
enum { AM_NONE, AM_SHELLS, AM_NAILS, AM_ROCKETS, AM_CELLS };

static const nbweapon_t nb_dmweapons[] = {
	{ 7, IT_ROCKET_LAUNCHER,  1000, 120, 1600, 100, AM_ROCKETS },
	{ 8, IT_LIGHTNING,           0,   0,  600,  95, AM_CELLS },
	{ 5, IT_SUPER_NAILGUN,    1000,   0, 1000,  70, AM_NAILS },
	{ 6, IT_GRENADE_LAUNCHER,  600, 150,  700,  55, AM_ROCKETS },
	{ 3, IT_SUPER_SHOTGUN,       0,   0,  500,  60, AM_SHELLS },
	{ 4, IT_NAILGUN,          1000,   0,  900,  40, AM_NAILS },
	{ 2, IT_SHOTGUN,             0,   0, 2000,  20, AM_SHELLS },
	{ 1, IT_AXE,                 0,   0,   64,   5, AM_NONE },
};

static int NB_Ammo(edict_t *e, int am)
{
	switch (am)
	{
	case AM_SHELLS:  return (int)e->v->ammo_shells;
	case AM_NAILS:   return (int)e->v->ammo_nails;
	case AM_ROCKETS: return (int)e->v->ammo_rockets;
	case AM_CELLS:   return (int)e->v->ammo_cells;
	}
	return 1;
}

static const nbweapon_t *NBDM_ChooseWeapon(nbbot_t *bot, float dist)
{
	edict_t *self = NB_Self(bot);
	const nbweapon_t *best = &nb_dmweapons[countof(nb_dmweapons) - 1];
	float bestscore = -1;
	unsigned int i;
	if (qw_bot_meleeonly.ival)
		return best;
	for (i = 0; i < countof(nb_dmweapons); i++)
	{
		const nbweapon_t *w = &nb_dmweapons[i];
		float s = w->pref;
		if (!((int)self->v->items & w->item) || NB_Ammo(self, w->ammo) <= 0)
			continue;
		if (dist < w->mindist)
			s *= 0.3f;   /* self-damage / too close */
		if (dist > w->maxdist)
			s *= 0.4f;
		if (w->item == IT_LIGHTNING && self->v->waterlevel >= 2)
			continue;   /* discharge */
		if (s > bestscore)
		{
			bestscore = s;
			best = w;
		}
	}
	return best;
}

/* aim point with lead for projectiles */
void NB_LeadTarget(nbbot_t *bot, edict_t *enemy, float speed, vec3_t out)
{
	edict_t *self = NB_Self(bot);
	vec3_t eye;
	float t;
	VectorCopy(self->v->origin, eye);
	eye[2] += 22;
	VectorCopy(enemy->v->origin, out);
	if (speed > 0)
	{
		t = Distance(eye, out) / speed;
		t = min(t, 1.2f);
		VectorMA(out, t * (0.4f + bot->skill * 0.6f), enemy->v->velocity, out);
		/* rockets: aim at the feet so splash catches them, but not up close (self-damage) */
		if (speed >= 900 && Distance(eye, out) > 250)
			out[2] -= 18;
	}
	/* skill-based wobble */
	{
		float err = (1 - bot->skill) * 40;
		out[0] += crandom() * err;
		out[1] += crandom() * err;
		out[2] += crandom() * err * 0.5f;
	}
}

/* ------------------------------------------------------------------------------------------------
 * actions
 */
typedef struct { nbaction_t base; float nextweapon; int strafedir; float nextstrafe; const nbweapon_t *weapon; float checkat, checkdist; } nbdmattack_t;
typedef struct { nbaction_t base; vec3_t where; float until; } nbchase_t;
typedef struct { nbaction_t base; edict_t *item; float until; vec3_t pos; } nbgetitem_t;
typedef struct { nbaction_t base; vec3_t where; float until; } nbroam_t;
typedef struct { nbaction_t base; float nextitemcheck; edict_t *ignoreenemy; float ignoreuntil; } nbdmroot_t;

static const nbactionvt_t NBDM_AttackVT, NBDM_ChaseVT, NBDM_GetItemVT, NBDM_RoamVT, NBDM_RootVT;

/* is an action of this kind already suspended below 'self'? (then resume it instead of stacking another) */
static qboolean NB_Below(nbaction_t *self, const nbactionvt_t *vt)
{
	nbaction_t *a;
	for (a = self->below; a; a = a->below)
		if (a->vt == vt)
			return true;
	return false;
}

/* --- Attack ------------------------------------------------------------------------------------ */
static nbresult_t Attack_OnStart(nbbot_t *bot, nbaction_t *self, nbaction_t *prior)
{
	nbdmattack_t *a = (nbdmattack_t*)self;
	a->strafedir = (rand() & 1) ? 1 : -1;
	return NB_Continue();
}

/* off the mesh (a ledge, lava) or into a lift shaft */
static qboolean NB_StrafeUnsafe(const vec3_t goal, const vec3_t mid)
{
	int a = NBNav_AreaAt(goal, 24);
	return a < 0 || NBNav_AreaAt(mid, 24) < 0 || (nb_mesh.areas[a].flags & (NBAREA_PLAT | NBAREA_TRAP));
}

void NB_CombatMove(nbbot_t *bot, float dist, float wantmin, float wantmax, int *strafedir, float *nextstrafe)
{
	edict_t *self = NB_Self(bot);
	edict_t *e = bot->enemy;
	vec3_t to, side, goal;
	float now = NB_Time();
	VectorSubtract(e->v->origin, self->v->origin, to);
	to[2] = 0;
	VectorNormalize(to);
	VectorSet(side, -to[1], to[0], 0);
	if (now > *nextstrafe)
	{
		*strafedir = -*strafedir;
		*nextstrafe = now + 0.4f + frandom() * 0.8f;
	}
	VectorMA(self->v->origin, 96 * *strafedir, side, goal);
	if (dist > wantmax)
		VectorMA(goal, 96, to, goal);
	else if (dist < wantmin)
		VectorMA(goal, -96, to, goal);
	/* don't strafe into a wall */
	{
		static const vec3_t hmins = { -16, -16, -24 }, hmaxs = { 16, 16, 32 };
		trace_t tr = World_Move(&sv.world, self->v->origin, (float*)hmins, (float*)hmaxs, goal, MOVE_NOMONSTERS, (wedict_t*)self);
		if (tr.fraction < 0.5f)
		{
			*strafedir = -*strafedir;
			*nextstrafe = now + 0.4f + frandom() * 0.8f;
			VectorMA(self->v->origin, 96 * *strafedir, side, goal);
			if (dist > wantmax)
				VectorMA(goal, 96, to, goal);
			else if (dist < wantmin)
				VectorMA(goal, -96, to, goal);
		}
	}
	/* don't strafe off a ledge (into lava, a pit): stay on floor at about the same height */
	{
		vec3_t mid;
		VectorAdd(self->v->origin, goal, mid);
		VectorScale(mid, 0.5f, mid);
		if (NB_StrafeUnsafe(goal, mid))
		{
			*strafedir = -*strafedir;
			VectorMA(self->v->origin, 96 * *strafedir, side, goal);
			VectorAdd(self->v->origin, goal, mid);
			VectorScale(mid, 0.5f, mid);
			if (NB_StrafeUnsafe(goal, mid))
				VectorCopy(self->v->origin, goal);   /* nowhere safe to strafe: hold */
		}
	}
	goal[2] = self->v->origin[2] - 24;
	NBLoco_SetGoal(bot, goal);
	bot->path.valid = false;
	if (bot->skill > 0.5f && frandom() < 0.01f * bot->skill && ((int)self->v->flags & FL_ONGROUND))
		bot->wantjump = true;
}

static nbresult_t Attack_Update(nbbot_t *bot, nbaction_t *self, float dt)
{
	nbdmattack_t *a = (nbdmattack_t*)self;
	edict_t *me = NB_Self(bot);
	edict_t *e = bot->enemy;
	vec3_t aim;
	float dist, now = NB_Time();
	if (!e || !NB_IsAlive(e))
		return NB_Done("enemy dead");
	if (now - bot->enemyseen > 0.6f)
	{
		nbchase_t *c = (nbchase_t*)NBAction_New(&NBDM_ChaseVT);
		VectorCopy(bot->enemylastpos, c->where);
		return NB_ChangeTo(&c->base, "lost sight");
	}
	dist = Distance(me->v->origin, e->v->origin);
	if (now >= a->nextweapon || !a->weapon)
	{
		a->weapon = NBDM_ChooseWeapon(bot, dist);
		if ((int)me->v->weapon != a->weapon->item)
			bot->impulse = a->weapon->impulse;
		a->nextweapon = now + 0.8f;
	}
	NB_LeadTarget(bot, e, a->weapon->speed, aim);
	NBBody_AimAt(bot, aim);
	if (now - bot->enemyfirstseen > bot->reaction && NBBody_IsAimedAt(bot, aim, 6 + (1 - bot->skill) * 10)
		&& !((a->weapon->item == IT_ROCKET_LAUNCHER || a->weapon->item == IT_GRENADE_LAUNCHER) && dist < 140))
		bot->buttons |= 1;
	/* a stand-off (across a gap, ledge to ledge) that isn't getting closer: leave this enemy alone for a
	   while and go on (before this both bots stood shooting at each other's ledge for minutes) */
	if (!a->checkat)
	{
		a->checkat = now + 5;
		a->checkdist = dist;
	}
	else if (now >= a->checkat)
	{
		if (dist > 350 && dist > a->checkdist - 96 && (dist > 600 || now - bot->lastpain > 1.0f))
		{
			nbaction_t *r;
			for (r = self; r; r = r->below)
				if (r->vt == &NBDM_RootVT)
				{
					((nbdmroot_t*)r)->ignoreenemy = e;
					((nbdmroot_t*)r)->ignoreuntil = now + 8 + frandom() * 4;
					return NB_Done("stand-off");
				}
		}
		a->checkat = now + 5;
		a->checkdist = dist;
	}
	if (dist > a->weapon->maxdist * 0.6f + 150 && NBLoco_PathTo(bot, e->v->origin, 1))
		;	/* too far for this weapon: walk the mesh toward them instead of strafing at a ledge */
	else
		NB_CombatMove(bot, dist, a->weapon->mindist + 50, a->weapon == &nb_dmweapons[countof(nb_dmweapons) - 1] ? 48 : a->weapon->maxdist * 0.6f, &a->strafedir, &a->nextstrafe);
	/* very hurt with a strong enemy: go get health */
	if (me->v->health < 30 && e->v->health > 60)
	{
		float v;
		edict_t *item = NBDM_PickItem(bot, 1200, &v);
		if (item && !strncmp(NB_Classname(item), "item_health", 11) && !NB_Below(self, &NBDM_GetItemVT))
		{
			nbgetitem_t *g = (nbgetitem_t*)NBAction_New(&NBDM_GetItemVT);
			g->item = item;
			return NB_SuspendFor(&g->base, "retreat to health");
		}
	}
	return NB_Continue();
}

static const nbactionvt_t NBDM_AttackVT = { "Attack", sizeof(nbdmattack_t), Attack_OnStart, Attack_Update };

/* --- Chase ------------------------------------------------------------------------------------- */
static nbresult_t Chase_OnStart(nbbot_t *bot, nbaction_t *self, nbaction_t *prior)
{
	if (NBNav_AreaAt(((nbchase_t*)self)->where, 48) < 0)
		return NB_Done("last seen off the mesh (fell, lava)");
	((nbchase_t*)self)->until = NB_Time() + 5;
	return NB_Continue();
}
static nbresult_t Chase_Update(nbbot_t *bot, nbaction_t *self, float dt)
{
	nbchase_t *c = (nbchase_t*)self;
	edict_t *me = NB_Self(bot);
	if (bot->enemy && NB_Time() - bot->enemyseen < 0.2f)
		return NB_ChangeTo(NBAction_New(&NBDM_AttackVT), "found again");
	if (NB_Time() > c->until || Distance(me->v->origin, c->where) < 48)
		return NB_Done("gave up chase");
	if (!NBLoco_PathTo(bot, c->where, 1.5f))
		return NB_Done("no path");
	return NB_Continue();
}
static const nbactionvt_t NBDM_ChaseVT = { "Chase", sizeof(nbchase_t), Chase_OnStart, Chase_Update };

/* --- GetItem ----------------------------------------------------------------------------------- */
static nbresult_t GetItem_OnStart(nbbot_t *bot, nbaction_t *self, nbaction_t *prior)
{
	nbgetitem_t *g = (nbgetitem_t*)self;
	edict_t *me = NB_Self(bot);
	VectorAdd(g->item->v->absmin, g->item->v->absmax, g->pos);
	VectorScale(g->pos, 0.5f, g->pos);
	{	/* time allowed: by the path's length, which can be far more than the straight line (an
		   item on a ledge right above us may be a long way round) */
		float d = Distance(me->v->origin, g->pos);
		float travel = NBPath_TravelDistance(me->v->origin, g->pos, NB_TeamOf(me), 0);
		if (travel > d)
			d = travel;
		g->until = NB_Time() + 4 + d / 150;
	}
	bot->path.valid = false;
	return NB_Continue();
}
static nbresult_t GetItem_Update(nbbot_t *bot, nbaction_t *self, float dt)
{
	nbgetitem_t *g = (nbgetitem_t*)self;
	if (!NB_ItemAvailable(g->item))
		return NB_Done("got it (or someone did)");
	if (NB_Time() > g->until)
	{
		NB_MarkBadGoal(bot, g->item, 30);
		return NB_Done("took too long");
	}
	if (bot->enemy && NB_Time() - bot->enemyseen < 0.2f && NBIntention_ShouldAttack(bot, bot->enemy))
	{
		if (NB_Below(self, &NBDM_AttackVT))
		{
			if (Distance(NB_Self(bot)->v->origin, g->pos) > 300)
				return NB_Done("back to the fight");
		}
		else
			return NB_SuspendFor(NBAction_New(&NBDM_AttackVT), "enemy while fetching");
	}
	if (!NBLoco_PathTo(bot, g->pos, 2))
	{
		NB_MarkBadGoal(bot, g->item, 30);
		return NB_Done("no path");
	}
	/* standing on it without picking it up (can't be taken, or out of reach above/below) */
	if (Distance(NB_Self(bot)->v->origin, g->pos) < 40 && NB_Time() - self->started > 2)
	{
		NB_MarkBadGoal(bot, g->item, 30);
		return NB_Done("at the item but it stays");
	}
	return NB_Continue();
}
static nbresult_t GetItem_OnStuck(nbbot_t *bot, nbaction_t *self)
{
	NB_MarkBadGoal(bot, ((nbgetitem_t*)self)->item, 20);
	return NB_Done("stuck");
}
static const nbactionvt_t NBDM_GetItemVT = { "GetItem", sizeof(nbgetitem_t), GetItem_OnStart, GetItem_Update, NULL, NULL, NULL, GetItem_OnStuck };

/* --- Roam -------------------------------------------------------------------------------------- */
qboolean NB_PickRoamPoint(nbbot_t *bot, vec3_t out, float mindist)
{
	edict_t *me = NB_Self(bot);
	int tries;
	if (!nb_mesh.numareas)
		return false;
	for (tries = 0; tries < 24; tries++)
	{
		nbarea_t *a = &nb_mesh.areas[rand() % nb_mesh.numareas];
		if (a->flags & (NBAREA_LAVA | NBAREA_SLIME | NBAREA_HURT))
			continue;
		if (Distance(a->center, me->v->origin) < mindist)
			continue;
		NBNav_RandomPoint(a, out);
		out[2] += 24;
		return true;
	}
	return false;
}

static nbresult_t Roam_OnStart(nbbot_t *bot, nbaction_t *self, nbaction_t *prior)
{
	nbroam_t *r = (nbroam_t*)self;
	if (!NB_PickRoamPoint(bot, r->where, 600))
		return NB_Done("no nav mesh");
	r->until = NB_Time() + 20;
	bot->path.valid = false;
	return NB_Continue();
}
static nbresult_t Roam_Update(nbbot_t *bot, nbaction_t *self, float dt)
{
	nbroam_t *r = (nbroam_t*)self;
	edict_t *me = NB_Self(bot);
	if (NB_Time() > r->until || Distance(me->v->origin, r->where) < 64)
		return NB_Done("arrived");
	if (bot->enemy && NB_Time() - bot->enemyseen < 0.2f && NBIntention_ShouldAttack(bot, bot->enemy))
	{
		if (nb_game == NBGAME_TF)
			return NB_Done("enemy: let TF decide");
		return NB_SuspendFor(NBAction_New(&NBDM_AttackVT), "enemy while roaming");
	}
	if (nb_game == NBGAME_TF && NB_Time() - self->started > 8)
		return NB_Done("TF: look for objectives again");
	if (NB_Time() - self->started > 2 && nb_game == NBGAME_DM && ((int)(NB_Time() * 2) % 4) == 0 && NBDM_PickItem(bot, 1500, NULL))
		return NB_Done("an item is worth fetching");
	if (!NBLoco_PathTo(bot, r->where, 3))
		return NB_Done("no path");
	return NB_Continue();
}
static nbresult_t Roam_OnStuck(nbbot_t *bot, nbaction_t *self) { return NB_Done("stuck"); }
static nbresult_t Roam_OnPathFailed(nbbot_t *bot, nbaction_t *self) { return NB_Done("unreachable"); }
static const nbactionvt_t NBDM_RoamVT = { "Roam", sizeof(nbroam_t), Roam_OnStart, Roam_Update, NULL, NULL, NULL, Roam_OnStuck, NULL, NULL, Roam_OnPathFailed };

/* --- Root -------------------------------------------------------------------------------------- */
static void NBDM_SpawnLoadout(nbbot_t *bot)
{
	/* qw_bot_spawnloadouts: axe + shotgun and a random extra weapon (or none) */
	static const struct { cvar_t *cv; int item, impulse; } pool[] = {
		{ &qw_bot_spawnloadouts_ssg, IT_SUPER_SHOTGUN, 3 }, { &qw_bot_spawnloadouts_ng, IT_NAILGUN, 4 },
		{ &qw_bot_spawnloadouts_sng, IT_SUPER_NAILGUN, 5 }, { &qw_bot_spawnloadouts_gl, IT_GRENADE_LAUNCHER, 6 },
		{ &qw_bot_spawnloadouts_rl, IT_ROCKET_LAUNCHER, 7 }, { &qw_bot_spawnloadouts_lg, IT_LIGHTNING, 8 },
	};
	edict_t *me = NB_Self(bot);
	int choices[8], n = 0;
	unsigned int i;
	if (!qw_bot_spawnloadouts.ival || NB_Time() < qw_bot_spawnloadouts_time.value)
		return;
	for (i = 0; i < countof(pool); i++)
		if (pool[i].cv->ival)
			choices[n++] = i;
	choices[n++] = -1;   /* none */
	i = choices[rand() % n];
	if ((int)i < 0)
		return;
	me->v->items = (int)me->v->items | pool[i].item;
	switch (pool[i].item)
	{
	case IT_SUPER_SHOTGUN: me->v->ammo_shells = max(me->v->ammo_shells, 25); break;
	case IT_NAILGUN: case IT_SUPER_NAILGUN: me->v->ammo_nails = max(me->v->ammo_nails, 50); break;
	case IT_GRENADE_LAUNCHER: case IT_ROCKET_LAUNCHER: me->v->ammo_rockets = max(me->v->ammo_rockets, 10); break;
	case IT_LIGHTNING: me->v->ammo_cells = max(me->v->ammo_cells, 50); break;
	}
	bot->impulse = pool[i].impulse;
}

static nbresult_t Root_OnStart(nbbot_t *bot, nbaction_t *self, nbaction_t *prior)
{
	return NB_Continue();
}

static nbresult_t Root_Update(nbbot_t *bot, nbaction_t *self, float dt)
{
	nbdmroot_t *r = (nbdmroot_t*)self;
	float now = NB_Time(), value;
	edict_t *item;
	if (now - bot->spawntime < 0.2f && bot->spawntime > 0)
	{
		NBDM_SpawnLoadout(bot);
		bot->spawntime = -1;
	}
	if (bot->enemy && now - bot->enemyseen < 0.2f && NBIntention_ShouldAttack(bot, bot->enemy))
		return NB_SuspendFor(NBAction_New(&NBDM_AttackVT), "enemy");
	if (now >= r->nextitemcheck)
	{
		r->nextitemcheck = now + 1;
		item = NBDM_PickItem(bot, 2500, &value);
		if (item)
		{
			nbgetitem_t *g = (nbgetitem_t*)NBAction_New(&NBDM_GetItemVT);
			g->item = item;
			return NB_SuspendFor(&g->base, "item");
		}
	}
	return NB_SuspendFor(NBAction_New(&NBDM_RoamVT), "roam");
}

static nbresult_t Root_OnResume(nbbot_t *bot, nbaction_t *self, nbaction_t *interrupter)
{
	((nbdmroot_t*)self)->nextitemcheck = 0;
	return NB_Continue();
}

static nbresult_t Root_OnSight(nbbot_t *bot, nbaction_t *self, edict_t *who)
{
	/* an enemy appeared while roaming / fetching: fight unless the top action says otherwise */
	if (bot->top && (bot->top->vt == &NBDM_RoamVT))
		return NB_Continue();
	return NB_Continue();
}

static int Root_ShouldAttack(nbbot_t *bot, nbaction_t *self, edict_t *enemy)
{
	nbdmroot_t *r = (nbdmroot_t*)self;
	if (enemy == r->ignoreenemy && NB_Time() < r->ignoreuntil && !(bot->lastattacker == enemy && NB_Time() - bot->lastpain < 2))
		return 0;   /* a stand-off with them went nowhere (unless they hit us since) */
	return -1;
}

static const nbactionvt_t NBDM_RootVT = { "DM", sizeof(nbdmroot_t), Root_OnStart, Root_Update, NULL, NULL, Root_OnResume, NULL, NULL, Root_OnSight, NULL, Root_ShouldAttack };

/* roam also breaks off for enemies and items */
nbaction_t *NBDM_RootAction(void)
{
	return NBAction_New(&NBDM_RootVT);
}

/* shared with TF */
nbaction_t *NBDM_NewAttack(void) { return NBAction_New(&NBDM_AttackVT); }
nbaction_t *NBDM_NewRoam(void) { return NBAction_New(&NBDM_RoamVT); }
nbaction_t *NBDM_NewGetItem(edict_t *item) { nbgetitem_t *g = (nbgetitem_t*)NBAction_New(&NBDM_GetItemVT); g->item = item; return &g->base; }
edict_t *NBDM_PickItemPub(nbbot_t *bot, float maxdist) { return NBDM_PickItem(bot, maxdist, NULL); }

#endif
