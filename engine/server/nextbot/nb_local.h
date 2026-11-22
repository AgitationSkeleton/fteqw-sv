/*
 * NextBot-style bots for the FTE QuakeWorld server.
 *
 * Copyright (C) 2026 fteqw-sv contributors
 *
 * This program is free software; you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version. See the file LICENSE in the repository root.
 *
 * A clean-room implementation of the architecture described publicly for Valve's NextBot
 * (Michael Booth, "The AI Systems of Left 4 Dead", GDC 2009): a bot is a set of components
 * (locomotion, body/aim, vision, intention) and its intention is a stack of actions that respond to
 * events and return results (continue, change to, suspend for, done). No Valve code is used.
 */
#ifndef NB_LOCAL_H
#define NB_LOCAL_H

#include "quakedef.h"

#ifndef CLIENTONLY

#include "pr_common.h"

#ifndef Distance
#define Distance(a, b) NB_Distance(a, b)
static inline float NB_Distance(const vec3_t a, const vec3_t b)
{
	vec3_t d;
	VectorSubtract(a, b, d);
	return sqrt(DotProduct(d, d));
}
#endif

/* ------------------------------------------------------------------------------------------------
 * constants
 */
#define NB_MAX_BOTS            MAX_CLIENTS
#define NB_HULL_MINS_X         -16
#define NB_HULL_MINS_Z         -24
#define NB_HULL_MAXS_Z         32
#define NB_STEPSIZE            18
#define NB_JUMPHEIGHT          44      /* QW jump (270 up, 800 gravity) lifts ~45 units */
#define NB_MAXDROP             320     /* longest drop the nav mesh links (fall damage above ~ 500 in TF) */
#define NB_GEN_STEP            16      /* nav generation grid */

/* ------------------------------------------------------------------------------------------------
 * nav mesh
 */
enum
{
	NBAREA_WATER      = 1 << 0,    /* under water: swim */
	NBAREA_LAVA       = 1 << 1,
	NBAREA_SLIME      = 1 << 2,
	NBAREA_DOOR       = 1 << 3,    /* overlaps a door brush */
	NBAREA_PLAT       = 1 << 4,    /* overlaps a lift */
	NBAREA_HURT       = 1 << 5,    /* overlaps a trigger_hurt */
	NBAREA_SPAWN      = 1 << 6,    /* contains a spawn point */
	NBAREA_CROUCH     = 1 << 7,    /* unused in QW (no crouch); reserved */
};

enum
{
	NBLINK_WALK = 0,
	NBLINK_JUMP,          /* needs a jump to go up */
	NBLINK_DROP,          /* falls down */
	NBLINK_TELEPORT,      /* trigger_teleport to its destination */
	NBLINK_PLAT,          /* ride a lift */
	NBLINK_SWIM,          /* water surface / water column */
};

typedef struct nblink_s
{
	unsigned int to;       /* area index */
	unsigned char type;    /* NBLINK_* */
	unsigned short cost;   /* extra cost on top of distance */
} nblink_t;

typedef struct nbarea_s
{
	vec3_t mins, maxs;     /* x/y extent; z of the floor at mins/maxs corners is z[0..3] */
	float z[4];            /* floor height at (minx,miny) (maxx,miny) (maxx,maxy) (minx,maxy) */
	vec3_t center;
	unsigned int flags;    /* NBAREA_* */
	unsigned int firstlink;
	unsigned short numlinks;
	unsigned char teamblock; /* bit per team (1..4): area blocked for that team (team-only doors) */
	unsigned char pad;
	/* pathfinding scratch */
	float g, f;
	unsigned int parent;
	unsigned int openid, closedid;
	unsigned short parentlink;
	unsigned short heapindex;
} nbarea_t;

typedef struct nbmesh_s
{
	char mapname[MAX_QPATH];
	unsigned int bspchecksum;
	unsigned int numareas;
	nbarea_t *areas;
	unsigned int numlinks;
	nblink_t *links;
	/* coarse grid for point lookups */
	vec3_t gridmins;
	int gridw, gridh;
	float gridcell;
	unsigned int *gridstart;    /* gridw*gridh+1 offsets into gridareas */
	unsigned int *gridareas;
	unsigned int searchid;
	unsigned char *areatele;    /* runtime: per area, teams a trigger_teleport there teleports (bit team-1; 0x10 = everyone) */
	unsigned char *linkteams;   /* runtime: per link, teams that may NOT use it (team-only teleporters); NULL = none */
} nbmesh_t;

extern nbmesh_t nb_mesh;

qboolean NBNav_Load(const char *mapname);
void NBNav_Free(void);
qboolean NBNav_Generate(void);
qboolean NBNav_Save(void);
int NBNav_AreaAt(const vec3_t pos, float maxdrop);       /* area under pos (within maxdrop), or -1 */
int NBNav_NearestArea(const vec3_t pos, float maxdist);   /* closest area, or -1 */
float NBNav_FloorZ(const nbarea_t *a, float x, float y);
void NBNav_RandomPoint(const nbarea_t *a, vec3_t out);
qboolean NBNav_Contains(const nbarea_t *a, const vec3_t pos, float ztol);
void NBNav_MarkTeleporters(void);

/* ------------------------------------------------------------------------------------------------
 * paths
 */
#define NB_MAX_PATH 1024

typedef struct nbpathseg_s
{
	vec3_t pos;           /* point to walk to */
	int area;
	unsigned char how;    /* NBLINK_* used to enter this segment */
} nbpathseg_t;

typedef struct nbpath_s
{
	nbpathseg_t seg[NB_MAX_PATH];
	int numsegs;
	int cur;              /* segment being walked to */
	vec3_t goal;
	float computed;       /* sv time */
	qboolean valid;
} nbpath_t;

typedef float (*nbcostfn_t)(void *ctx, const nbarea_t *from, const nbarea_t *to, const nblink_t *link);
qboolean NBPath_Compute(nbpath_t *path, const vec3_t start, const vec3_t goal, int team, nbcostfn_t cost, void *ctx);
float NBPath_TravelDistance(const vec3_t start, const vec3_t goal, int team, float maxdist);

/* ------------------------------------------------------------------------------------------------
 * actions (intention stack)
 */
typedef struct nbbot_s nbbot_t;
typedef struct nbaction_s nbaction_t;

typedef enum
{
	NBR_CONTINUE = 0,
	NBR_CHANGETO,
	NBR_SUSPENDFOR,
	NBR_DONE,
	NBR_SUSTAIN          /* event results only: keep the current action, ignore lower ones */
} nbresulttype_t;

typedef struct nbresult_s
{
	nbresulttype_t type;
	nbaction_t *action;
	const char *reason;
} nbresult_t;

typedef struct nbactionvt_s
{
	const char *name;
	size_t size;                                                       /* sizeof the concrete struct */
	nbresult_t (*OnStart)(nbbot_t *bot, nbaction_t *self, nbaction_t *prior);
	nbresult_t (*Update)(nbbot_t *bot, nbaction_t *self, float dt);
	void (*OnEnd)(nbbot_t *bot, nbaction_t *self, nbaction_t *next);
	nbresult_t (*OnSuspend)(nbbot_t *bot, nbaction_t *self, nbaction_t *interrupter);
	nbresult_t (*OnResume)(nbbot_t *bot, nbaction_t *self, nbaction_t *interrupter);
	/* events (NULL = not handled, passes to the action below) */
	nbresult_t (*OnStuck)(nbbot_t *bot, nbaction_t *self);
	nbresult_t (*OnInjured)(nbbot_t *bot, nbaction_t *self, edict_t *attacker);
	nbresult_t (*OnSight)(nbbot_t *bot, nbaction_t *self, edict_t *who);
	nbresult_t (*OnPathFailed)(nbbot_t *bot, nbaction_t *self);
	/* queries: -1 = no opinion */
	int (*ShouldAttack)(nbbot_t *bot, nbaction_t *self, edict_t *enemy);
	int (*ShouldRetreat)(nbbot_t *bot, nbaction_t *self);
} nbactionvt_t;

struct nbaction_s
{
	const nbactionvt_t *vt;
	nbaction_t *below;     /* the action this one suspended */
	nbaction_t *child;     /* top-level behaviours may run a child stack: unused for now */
	float started;
	qboolean started_ok;
};

nbaction_t *NBAction_New(const nbactionvt_t *vt);
nbresult_t NB_Continue(void);
nbresult_t NB_ChangeTo(nbaction_t *a, const char *reason);
nbresult_t NB_SuspendFor(nbaction_t *a, const char *reason);
nbresult_t NB_Done(const char *reason);
nbresult_t NB_Sustain(const char *reason);

void NBIntention_Init(nbbot_t *bot, nbaction_t *root);
void NBIntention_Update(nbbot_t *bot, float dt);
void NBIntention_Free(nbbot_t *bot);
void NBIntention_OnStuck(nbbot_t *bot);
void NBIntention_OnInjured(nbbot_t *bot, edict_t *attacker);
void NBIntention_OnSight(nbbot_t *bot, edict_t *who);
void NBIntention_OnPathFailed(nbbot_t *bot);
int NBIntention_ShouldAttack(nbbot_t *bot, edict_t *enemy);
const char *NBIntention_Describe(nbbot_t *bot, char *buf, size_t size);

/* ------------------------------------------------------------------------------------------------
 * the bot
 */
typedef enum { NBGAME_DM, NBGAME_TF } nbgame_t;

struct nbbot_s
{
	qboolean inuse;
	client_t *cl;
	int clientnum;
	float skill;              /* 0 .. 1 */
	int wantteam, wantclass;  /* TF: requested team/class (0 = auto) */

	/* intention */
	nbaction_t *top;          /* top of the action stack */
	qboolean inupdate;        /* an action's Update is running: events are queued */
	nbaction_t *pendingact;   /* event result waiting for the Update to return */
	nbresult_t pendingres;

	/* locomotion */
	nbpath_t path;
	vec3_t movegoal;          /* where locomotion is heading this frame */
	qboolean hasmovegoal;
	qboolean wantjump, wantrun;
	float stucktime, laststuckcheck;
	vec3_t stuckpos;
	float jumptime;
	int stuckevents;
	edict_t *badgoal[4];      /* items/goals that failed recently */
	float badgoaluntil[4];
	int deaths, suicides;
	char lastdeath[160];          /* OnStuck events since joining (diagnostics) */

	/* body */
	vec3_t viewangles;        /* current aim */
	vec3_t aimtarget;         /* where we want to look */
	qboolean hasaim;
	float aimspeed;           /* degrees/s */
	int buttons;              /* buttons for this frame (BUTTON_ATTACK, BUTTON_JUMP) */
	int impulse;              /* impulse for this frame */
	float nextimpulse;

	/* vision */
	edict_t *enemy;
	float enemyseen;          /* sv time last seen */
	vec3_t enemylastpos;
	float reaction;           /* reaction delay (s) */
	float enemyfirstseen;

	/* misc */
	float lastthink;
	float spawntime;
	int lasthealth;
	qboolean wasalive;
	float nextcommand;        /* rate limit for console commands */
	char cmdqueue[8][64];
	int cmdhead, cmdtail;
	float deathtime;
	float lastpain;
	edict_t *lastattacker;

	/* stuffcmd text from the game code, collected until a newline */
	char stuffbuf[512];
	int stufflen;

	/* game specific scratch */
	void *gamedata;
};

extern nbbot_t nb_bots[NB_MAX_BOTS];
extern nbgame_t nb_game;

nbbot_t *NB_BotForClient(client_t *cl);
nbbot_t *NB_BotForEdict(edict_t *ed);
void NB_ClientCommand(nbbot_t *bot, const char *fmt, ...) LIKEPRINTF(2);
edict_t *NB_Self(nbbot_t *bot);
qboolean NB_IsAlive(edict_t *ed);
float NB_Time(void);

/* locomotion / body / vision */
void NBLoco_SetGoal(nbbot_t *bot, const vec3_t pos);
void NBLoco_ClearGoal(nbbot_t *bot);
qboolean NBLoco_FollowPath(nbbot_t *bot);     /* steer along bot->path; false when done/failed */
qboolean NBLoco_PathTo(nbbot_t *bot, const vec3_t goal, float repathinterval);
void NBBody_AimAt(nbbot_t *bot, const vec3_t pos);
void NBBody_LookAlongPath(nbbot_t *bot);
qboolean NBBody_IsAimedAt(nbbot_t *bot, const vec3_t pos, float tolerance_deg);
qboolean NBVision_CanSee(nbbot_t *bot, edict_t *target);
qboolean NBVision_LineClear(const vec3_t a, const vec3_t b, edict_t *ignore);
void NBVision_Update(nbbot_t *bot);

/* field access by name (game code fields that are not in the engine's entvars) */
float NB_GetFloat(edict_t *ed, const char *field);
qboolean NB_HasField(const char *field);
void NB_GetVector(edict_t *ed, const char *field, vec3_t out);
edict_t *NB_GetEntity(edict_t *ed, const char *field);
const char *NB_GetString(edict_t *ed, const char *field);
const char *NB_Classname(edict_t *ed);

qboolean NB_IsBadGoal(nbbot_t *bot, edict_t *e);
void NB_MarkBadGoal(nbbot_t *bot, edict_t *e, float seconds);

/* game modules */
nbaction_t *NBDM_RootAction(void);
nbaction_t *NBTF_RootAction(void);
void NBTF_MapStart(void);
void NBTF_ClientJoin(nbbot_t *bot);
int NBTF_TeamBlockBits(edict_t *door);

/* cvars shared by modules */
extern cvar_t qw_bot_debug;

#endif /* CLIENTONLY */
#endif /* NB_LOCAL_H */
