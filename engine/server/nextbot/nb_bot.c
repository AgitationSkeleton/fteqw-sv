/*
 * Bot manager: fake clients, quota, console commands, per-frame thinking and usercmds,
 * and the server features the bot build carries (mapcycle, intermission, Discord webhook).
 * Copyright (C) 2026 fteqw-sv contributors. GPLv2 or later; see nb_local.h.
 *
 * The qw_bot_* cvars and commands keep the names and meanings of the earlier fteqw-sv bot build,
 * so existing server.cfg files work unchanged.
 */
#include "nb_local.h"
#include "nb_public.h"

#ifndef CLIENTONLY

#ifdef WEBCLIENT
#include "../http/iweb.h"
#endif

nbbot_t nb_bots[NB_MAX_BOTS];
nbgame_t nb_game;

/* compatible with the earlier bot build */
cvar_t qw_bot_enable            = CVARD("qw_bot_enable", "1", "Enable the engine-side bots (0 kicks them all).");
cvar_t qw_bot_quota             = CVARD("qw_bot_quota", "0", "Desired number of bots.");
cvar_t qw_bot_quota_mode        = CVARD("qw_bot_quota_mode", "1", "0 = fixed bot count, 1 = fill up to qw_bot_quota total players.");
cvar_t qw_bot_quota_spawns      = CVARD("qw_bot_quota_spawns", "0", "1 = the quota is the number of player spawn points on the map (team spawns, else deathmatch spawns, else single-player starts) instead of qw_bot_quota. qw_bot_quota_mode still applies.");
cvar_t qw_bot_skins             = CVARD("qw_bot_skins", "base blue red yellow green", "Skin names for non-TF bots when the server has no skins/ folder.");
cvar_t qw_bot_random_colors     = CVARD("qw_bot_random_colors", "1", "Non-TF bots get random top/bottom colours.");
cvar_t qw_bot_meleeonly         = CVARD("qw_bot_meleeonly", "0", "Bots only use the axe.");
cvar_t qw_bot_force_class       = CVARD("qw_bot_force_class", "", "TF class for every bot: scout/sniper/soldier/demoman/medic/hwguy/pyro/spy/engineer/civilian or 1-9. Empty = varied.");
cvar_t qw_bot_forceclass        = CVARD("qw_bot_forceclass", "", "Alias for qw_bot_force_class.");
cvar_t qw_bot_stucktime         = CVARD("qw_bot_stucktime", "0", "Seconds a bot may go without scoring before it is killed and respawns. 0 = off.");
cvar_t qw_bot_verbose           = CVARD("qw_bot_verbose", "1", "Bot console messages: 0 off, 1 lifecycle, 2 more.");
cvar_t qw_bot_spawnloadouts     = CVARD("qw_bot_spawnloadouts", "0", "Non-TF: bots spawn with axe+shotgun plus a random extra weapon (or none).");
cvar_t qw_bot_spawnloadouts_time= CVARD("qw_bot_spawnloadouts_time", "0", "Seconds after map load before qw_bot_spawnloadouts gives the extra weapon.");
cvar_t qw_bot_spawnloadouts_ssg = CVARD("qw_bot_spawnloadouts_ssg", "1", "Super Shotgun is in the qw_bot_spawnloadouts pool.");
cvar_t qw_bot_spawnloadouts_ng  = CVARD("qw_bot_spawnloadouts_ng", "1", "Nailgun is in the qw_bot_spawnloadouts pool.");
cvar_t qw_bot_spawnloadouts_sng = CVARD("qw_bot_spawnloadouts_sng", "1", "Super Nailgun is in the qw_bot_spawnloadouts pool.");
cvar_t qw_bot_spawnloadouts_gl  = CVARD("qw_bot_spawnloadouts_gl", "1", "Grenade Launcher is in the qw_bot_spawnloadouts pool.");
cvar_t qw_bot_spawnloadouts_rl  = CVARD("qw_bot_spawnloadouts_rl", "1", "Rocket Launcher is in the qw_bot_spawnloadouts pool.");
cvar_t qw_bot_spawnloadouts_lg  = CVARD("qw_bot_spawnloadouts_lg", "1", "Lightning Gun is in the qw_bot_spawnloadouts pool.");
cvar_t discord_webhook_url      = CVARD("discord_webhook_url", "", "Discord webhook URL: real players' joins, leaves and chat are posted to it.");
/* new */
cvar_t qw_bot_skill             = CVARD("qw_bot_skill", "0.6", "Bot skill 0..1: aim speed, reaction time, accuracy.");
cvar_t qw_bot_debug             = CVARD("qw_bot_debug", "0", "Bot debugging output: 1 stuck/paths, 2 action changes.");
cvar_t qw_bot_nav_autogen       = CVARD("qw_bot_nav_autogen", "1", "Generate and save a nav mesh when a map has none.");
cvar_t qw_bot_mapcycle          = CVARD("qw_bot_mapcycle", "1", "Use <gamedir>/mapcycle.txt for map changes made by the game code.");

static float nb_nextquota;
static float nb_maploadtime;
static float nb_lastframe;

/* ------------------------------------------------------------------------------------------------
 * helpers
 */
float NB_Time(void)
{
	return sv.time;
}

edict_t *NB_Self(nbbot_t *bot)
{
	return bot->cl->edict;
}

qboolean NB_IsAlive(edict_t *ed)
{
	return ed && ed->v->health > 0 && ed->v->deadflag == 0 && ed->v->solid != SOLID_NOT;
}

const char *NB_Classname(edict_t *ed)
{
	return PR_GetString(svprogfuncs, ed->v->classname);
}

typedef struct { char name[32]; etype_t type; evalc_t cache; qboolean missing; } nbfieldcache_t;
static nbfieldcache_t nb_fields[96];
static int nb_numfields;

static eval_t *NB_Field(edict_t *ed, const char *name, etype_t type)
{
	int i;
	nbfieldcache_t *f = NULL;
	for (i = 0; i < nb_numfields; i++)
		if (nb_fields[i].type == type && !strcmp(nb_fields[i].name, name))
		{
			f = &nb_fields[i];
			break;
		}
	if (!f)
	{
		if (nb_numfields >= (int)countof(nb_fields))
			return svprogfuncs->GetEdictFieldValue(svprogfuncs, ed, name, type, NULL);
		f = &nb_fields[nb_numfields++];
		memset(f, 0, sizeof(*f));
		Q_strncpyz(f->name, name, sizeof(f->name));
		f->type = type;
	}
	if (f->missing)
		return NULL;
	{
		eval_t *v = svprogfuncs->GetEdictFieldValue(svprogfuncs, ed, name, type, &f->cache);
		if (!v)
			f->missing = true;
		return v;
	}
}

static void NB_ResetFieldCache(void)
{
	nb_numfields = 0;
}

float NB_GetFloat(edict_t *ed, const char *field)
{
	eval_t *v = NB_Field(ed, field, ev_float);
	return v ? v->_float : 0;
}

void NB_SetFloat(edict_t *ed, const char *field, float value)
{
	eval_t *v = NB_Field(ed, field, ev_float);
	if (v)
		v->_float = value;
}

qboolean NB_HasField(const char *field)
{
	edict_t *w = EDICT_NUM_PB(svprogfuncs, 0);
	return NB_Field(w, field, ev_float) != NULL || NB_Field(w, field, ev_vector) != NULL
		|| NB_Field(w, field, ev_entity) != NULL || NB_Field(w, field, ev_string) != NULL;
}

void NB_GetVector(edict_t *ed, const char *field, vec3_t out)
{
	eval_t *v = NB_Field(ed, field, ev_vector);
	if (v)
		VectorCopy(v->_vector, out);
	else
		VectorClear(out);
}

edict_t *NB_GetEntity(edict_t *ed, const char *field)
{
	eval_t *v = NB_Field(ed, field, ev_entity);
	if (!v || !v->edict)
		return NULL;
	return ((edict_t*)PROG_TO_EDICT(svprogfuncs, v->edict));
}

const char *NB_GetString(edict_t *ed, const char *field)
{
	eval_t *v = NB_Field(ed, field, ev_string);
	return v ? PR_GetString(svprogfuncs, v->string) : "";
}

float NB_Global(const char *name)
{
	eval_t *v = svprogfuncs ? PR_FindGlobal(svprogfuncs, name, 0, NULL) : NULL;
	return v ? v->_float : 0;
}

nbbot_t *NB_BotForClient(client_t *cl)
{
	int i = cl - svs.clients;
	if (i < 0 || i >= NB_MAX_BOTS || !nb_bots[i].inuse || nb_bots[i].cl != cl)
		return NULL;
	return &nb_bots[i];
}

cvar_t qw_bot_teamghost = CVARD("qw_bot_teamghost", "1", "Bots move through bots of their own (or an allied) team, so they don't jam narrow corridors. Humans still collide with everyone.");

nbbot_t *NB_BotForEdict(edict_t *ed);
int NB_TeamOf(edict_t *ed);
/* SV_RunCmd's pmove: a bot doesn't collide with a teammate bot (hunted's long corridor) */
qboolean NB_PassThrough(edict_t *mover, edict_t *other)
{
	int a, b;
	if (!qw_bot_teamghost.ival || !NB_BotForEdict(mover) || !NB_BotForEdict(other))
		return false;
	a = NB_TeamOf(mover);
	b = NB_TeamOf(other);
	if (a <= 0 || b <= 0)
		return false;
	if (nb_game == NBGAME_TF)
	{
		extern qboolean NBTF_Allied(int a, int b);
		return NBTF_Allied(a, b);
	}
	return a == b;
}

nbbot_t *NB_BotForEdict(edict_t *ed)
{
	int n = NUM_FOR_EDICT(svprogfuncs, ed) - 1;
	if (n < 0 || n >= sv.allocated_client_slots)
		return NULL;
	return NB_BotForClient(&svs.clients[n]);
}

qboolean NB_OwnsClient(client_t *cl)
{
	return NB_BotForClient(cl) != NULL;
}

/* team number as the game sees it: TF team_no, DM teamplay team string, 0 = none */
int NB_TeamOf(edict_t *ed)
{
	if (nb_game == NBGAME_TF)
		return (int)NB_GetFloat(ed, "team_no");
	return 0;
}

/* run a client console command as if the bot had typed it ("cmd ..." on the server side) */
void NB_ClientCommand(nbbot_t *bot, const char *fmt, ...)
{
	va_list argptr;
	char text[256];
	client_t *oldhost = host_client;
	edict_t *oldplayer = sv_player;
	va_start(argptr, fmt);
	Q_vsnprintfz(text, sizeof(text), fmt, argptr);
	va_end(argptr);
	host_client = bot->cl;
	sv_player = bot->cl->edict;
	SV_ExecuteUserCommand(text, true);
	host_client = oldhost;
	sv_player = oldplayer;
}

/* ------------------------------------------------------------------------------------------------
 * names, skins, colours
 */
static const char *nb_names[] = {
	"Shambler", "Vore", "Fiend", "Ogre", "Scrag", "Knight", "Enforcer", "Chthon", "Zombie", "Rotfish",
	"Spawn", "Grunt", "Death Knight", "Shub", "Rottweiler", "Quad", "Pent", "Ranger", "Axe", "Thunderbolt",
	"Nailgun", "Slipgate", "Runekeeper", "Gib", "Ziggurat", "Necropolis", "Hellknight", "Lavaman", "Teleporter", "Megahealth",
	"Biosuit", "Eyes"
};

static qboolean NB_NameTaken(const char *name)
{
	int i;
	for (i = 0; i < sv.allocated_client_slots; i++)
		if (svs.clients[i].state >= cs_connected && svs.clients[i].name && !Q_strcasecmp(svs.clients[i].name, name))
			return true;
	return false;
}

static void NB_PickName(char *out, size_t size, const char *wanted)
{
	int tries, n = countof(nb_names);
	if (wanted && *wanted && !NB_NameTaken(wanted))
	{
		Q_strncpyz(out, wanted, size);
		return;
	}
	for (tries = 0; tries < 64; tries++)
	{
		const char *b = nb_names[rand() % n];
		if (!NB_NameTaken(b))
		{
			Q_strncpyz(out, b, size);
			return;
		}
	}
	for (tries = 2; tries < 100; tries++)
	{
		Q_snprintfz(out, size, "%s%d", nb_names[rand() % n], tries);
		if (!NB_NameTaken(out))
			return;
	}
}

static char nb_skinlist[64][32];
static int nb_numskins;

static int QDECL NB_SkinEnum(const char *name, qofs_t size, time_t mtime, void *parm, searchpathfuncs_t *spath)
{
	char base[64];
	if (nb_numskins >= (int)countof(nb_skinlist))
		return false;
	COM_FileBase(name, base, sizeof(base));
	if (!*base || !Q_strncasecmp(base, "tf_", 3))
		return true;
	Q_strncpyz(nb_skinlist[nb_numskins++], base, sizeof(nb_skinlist[0]));
	return true;
}

static void NB_LoadSkins(void)
{
	nb_numskins = 0;
	COM_EnumerateFiles("skins/*.pcx", NB_SkinEnum, NULL);
	if (!nb_numskins)
	{
		const char *s = qw_bot_skins.string;
		char tok[64];
		while ((s = COM_ParseOut(s, tok, sizeof(tok))) && nb_numskins < (int)countof(nb_skinlist))
		{
			char *c;
			for (c = tok; *c; c++)
				if (*c == ',') *c = 0;
			if (*tok && atoi(tok) == 0 && strcmp(tok, "0") && strcmp(tok, "1"))
				Q_strncpyz(nb_skinlist[nb_numskins++], tok, sizeof(nb_skinlist[0]));
		}
	}
}

/* ------------------------------------------------------------------------------------------------
 * spawning
 */
static void NB_BotUserinfo(nbbot_t *bot, const char *name)
{
	client_t *cl = bot->cl;
	char nm[32];
	NB_PickName(nm, sizeof(nm), name);
	InfoBuf_SetValueForKey(&cl->userinfo, "name", nm);
	InfoBuf_SetValueForKey(&cl->userinfo, "*bot", "1");
	InfoBuf_SetValueForKey(&cl->userinfo, "rate", "25000");
	InfoBuf_SetValueForKey(&cl->userinfo, "msg", "1");
	InfoBuf_SetValueForKey(&cl->userinfo, "noaim", "1");
	if (nb_game == NBGAME_DM)
	{
		if (nb_numskins)
			InfoBuf_SetValueForKey(&cl->userinfo, "skin", nb_skinlist[rand() % nb_numskins]);
		if (qw_bot_random_colors.ival)
		{
			InfoBuf_SetValueForKey(&cl->userinfo, "topcolor", va("%i", rand() % 14));
			InfoBuf_SetValueForKey(&cl->userinfo, "bottomcolor", va("%i", rand() % 14));
		}
	}
	else
	{
		InfoBuf_SetValueForKey(&cl->userinfo, "skin", "base");
		InfoBuf_SetValueForKey(&cl->userinfo, "topcolor", "0");
		InfoBuf_SetValueForKey(&cl->userinfo, "bottomcolor", "0");
	}
}

static void NB_CallQC(client_t *cl, func_t f)
{
	client_t *oldhost = host_client;
	edict_t *oldplayer = sv_player;
	if (!f)
		return;
	host_client = cl;
	sv_player = cl->edict;
	pr_global_struct->time = sv.world.physicstime;
	pr_global_struct->self = EDICT_TO_PROG(svprogfuncs, cl->edict);
	PR_ExecuteProgram(svprogfuncs, f);
	host_client = oldhost;
	sv_player = oldplayer;
}

static nbbot_t *NB_SpawnBot(int team, int pclass, const char *name)
{
	extern int nextuserid;
	int i;
	client_t *cl;
	nbbot_t *bot;

	if (!svprogfuncs || sv.state != ss_active)
		return NULL;
	for (i = 0; i < sv.allocated_client_slots && i < NB_MAX_BOTS; i++)
		if (svs.clients[i].state == cs_free && !svs.clients[i].protocol)
			break;
	if (i >= sv.allocated_client_slots || i >= NB_MAX_BOTS)
		return NULL;

	cl = &svs.clients[i];
	bot = &nb_bots[i];
	memset(bot, 0, sizeof(*bot));
	bot->inuse = true;
	bot->cl = cl;
	bot->clientnum = i;
	bot->wantteam = team;
	bot->wantclass = pclass;
	bot->skill = bound(0, qw_bot_skill.value + (frandom() - 0.5f) * 0.2f, 1);
	bot->aimspeed = 300 + bot->skill * 700;
	bot->reaction = 0.45f - bot->skill * 0.35f;

	if (!cl->name)
		cl->name = cl->namebuf;
	if (!cl->team)
		cl->team = cl->teambuf;
	InfoBuf_Clear(&cl->userinfo, true);
	cl->userinfo.ChangeCB = svs.info.ChangeCB;
	cl->userinfo.ChangeCTX = &cl->userinfo;
	cl->num_backbuf = 0;
	cl->controller = NULL;
	cl->controlled = NULL;
	cl->seat = 0;
	cl->drop = false;
	cl->spectator = false;
	cl->spawned = false;
	cl->userid = ++nextuserid;
	cl->protocol = SCP_BAD;
	cl->state = cs_spawned;
	cl->prespawn_stage = PRESPAWN_COMPLETED;
	cl->connection_started = realtime;
	cl->netchan.remote_address.type = NA_INVALID;
	cl->netchan.netprim = svs.netprim;
	cl->netchan.message.data = cl->netchan.message_buf;
	cl->netchan.message.prim = cl->netchan.netprim;
	cl->netchan.message.allowoverflow = true;
	cl->netchan.message.maxsize = sizeof(cl->netchan.message_buf);
	cl->datagram.prim = cl->netchan.netprim;
	cl->datagram.data = cl->datagram_buf;
	cl->datagram.allowoverflow = true;
	cl->datagram.maxsize = sizeof(cl->datagram_buf);
	cl->backbuf.prim = cl->netchan.netprim;
	cl->backbuf.data = cl->backbuf_data[0];
	cl->edict = EDICT_NUM_PB(svprogfuncs, i + 1);

	NB_BotUserinfo(bot, name);
	SV_SetUpClientEdict(cl, cl->edict);
	SV_ExtractFromUserinfo(cl, false);

	/* join like a real client: new spawn parms, ClientConnect, PutClientInServer */
	SV_GetNewSpawnParms(cl);
	SV_SpawnParmsToQC(cl);
	cl->spawned = true;
	sv.spawned_client_slots++;
	/* FortressOne's ClientConnect drops clients without CSQC; bots have nothing to run it with,
	   but claim it so they can play (harmless for other game code) */
	cl->csqcactive = (nb_game == NBGAME_TF);
	sv.skipbprintclient = cl;
	NB_CallQC(cl, pr_global_ptrs->ClientConnect ? *pr_global_ptrs->ClientConnect : 0);
	sv.skipbprintclient = NULL;
	NB_CallQC(cl, pr_global_ptrs->PutClientInServer ? *pr_global_ptrs->PutClientInServer : 0);
	VectorCopy(cl->edict->v->angles, bot->viewangles);
	bot->viewangles[PITCH] = 0;
	VectorCopy(cl->edict->v->origin, cl->edict->v->oldorigin);

	SV_BroadcastUserinfoChange(cl, true, "name", NULL);
	SV_BroadcastUserinfoChange(cl, true, "skin", NULL);
	SV_BroadcastUserinfoChange(cl, true, "*bothcolours", NULL);
	SV_BroadcastTPrintf(PRINT_HIGH, "%s entered the game\n", cl->name);

	bot->spawntime = sv.time;
	bot->lastthink = sv.time;
	if (nb_game == NBGAME_TF)
	{
		NBTF_ClientJoin(bot);
		NBIntention_Init(bot, NBTF_RootAction());
	}
	else
		NBIntention_Init(bot, NBDM_RootAction());
	if (qw_bot_verbose.ival)
		Con_Printf("bot %s added (slot %d, skill %.2f)\n", cl->name, i, bot->skill);
	return bot;
}

static void NB_RemoveBot(nbbot_t *bot, qboolean runqc)
{
	client_t *cl = bot->cl;
	NBIntention_Free(bot);
	if (bot->gamedata)
		Z_Free(bot->gamedata);
	bot->gamedata = NULL;
	bot->inuse = false;
	if (!runqc)
		cl->spawned = false;   /* SV_DropClient then skips ClientDisconnect */
	SV_DropClient(cl);
	cl->protocol = SCP_BAD;
	cl->state = cs_free;
	cl->protocol = 0;
	cl->name[0] = 0;
}

static void NB_CountPlayers(int *humans, int *bots)
{
	int i;
	*humans = *bots = 0;
	for (i = 0; i < sv.allocated_client_slots; i++)
	{
		client_t *cl = &svs.clients[i];
		if (cl->state < cs_connected)
			continue;
		if (NB_BotForClient(cl))
			(*bots)++;
		else if (cl->protocol != SCP_BAD && !cl->spectator)
			(*humans)++;
	}
}

static nbbot_t *NB_PickBotToKick(void)
{
	int i, team, bestteam = -1, bestcount = -1;
	int count[8] = { 0 };
	/* kick from the team with the most players */
	if (nb_game == NBGAME_TF)
	{
		for (i = 0; i < sv.allocated_client_slots; i++)
			if (svs.clients[i].state >= cs_connected && svs.clients[i].edict)
			{
				team = NB_TeamOf(svs.clients[i].edict);
				if (team >= 1 && team <= 4)
					count[team]++;
			}
		for (team = 1; team <= 4; team++)
			if (count[team] > bestcount)
			{
				bestcount = count[team];
				bestteam = team;
			}
	}
	for (i = sv.allocated_client_slots - 1; i >= 0; i--)
	{
		nbbot_t *b = NB_BotForClient(&svs.clients[i]);
		if (b && (bestteam < 0 || NB_TeamOf(b->cl->edict) == bestteam))
			return b;
	}
	for (i = sv.allocated_client_slots - 1; i >= 0; i--)
	{
		nbbot_t *b = NB_BotForClient(&svs.clients[i]);
		if (b)
			return b;
	}
	return NULL;
}

extern cvar_t maxclients;
/* qw_bot_quota_spawns: how many players the map has spawn points for */
static int nb_spawncounts[3];   /* team, deathmatch, single-player (for qw_bot_status) */
static int NB_MapSpawnCount(void)
{
	static int cachedcount = -1;
	static char cachedmap[sizeof(svs.name)];
	static unsigned int cachededicts;
	int team = 0, dm = 0, start = 0;
	unsigned int i;
	if (cachedcount >= 0 && !strcmp(cachedmap, svs.name) && cachededicts == (unsigned int)sv.world.num_edicts)
		return cachedcount;
	for (i = 1; i < (unsigned int)sv.world.num_edicts; i++)
	{
		edict_t *e = EDICT_NUM_PB(svprogfuncs, i);
		const char *cn;
		if (ED_ISFREE(e))
			continue;
		cn = PR_GetString(svprogfuncs, e->v->classname);
		if (!strcmp(cn, "info_player_teamspawn"))
			team++;
		else if (!strcmp(cn, "info_player_deathmatch"))
			dm++;
		else if (!strcmp(cn, "info_player_start") || !strcmp(cn, "info_player_coop"))
			start++;
	}
	cachedcount = team ? team : (dm ? dm : start);
	nb_spawncounts[0] = team; nb_spawncounts[1] = dm; nb_spawncounts[2] = start;
	Q_strncpyz(cachedmap, svs.name, sizeof(cachedmap));
	cachededicts = sv.world.num_edicts;
	return cachedcount;
}

static void NB_UpdateQuota(void)
{
	int humans, bots, target;
	NB_CountPlayers(&humans, &bots);
	if (!qw_bot_enable.ival)
	{
		while (bots-- > 0)
		{
			nbbot_t *b = NB_PickBotToKick();
			if (!b)
				break;
			NB_RemoveBot(b, true);
		}
		return;
	}
	target = max(0, qw_bot_quota_spawns.ival ? NB_MapSpawnCount() : qw_bot_quota.ival);
	if (qw_bot_quota_mode.ival == 1)
		target = max(0, target - humans);
	target = min(target, sv.allocated_client_slots - humans);
	if (maxclients.ival > 0)
		target = min(target, maxclients.ival - humans);
	if (bots < target)
		NB_SpawnBot(0, 0, NULL);    /* one per second, so TF spawns spread out */
	else if (bots > target)
	{
		nbbot_t *b = NB_PickBotToKick();
		if (b)
			NB_RemoveBot(b, true);
	}
}

/* qw_bot_stucktime: kill a bot that has not scored for that long (it may be wedged somewhere) */
static void NB_CheckNoScore(nbbot_t *bot)
{
	edict_t *ed = NB_Self(bot);
	static float lastscore[NB_MAX_BOTS];
	static int lastfrags[NB_MAX_BOTS];
	static vec3_t lastpos[NB_MAX_BOTS];
	int i = bot->clientnum;
	if (qw_bot_stucktime.value <= 0 || sv.time < 60)
		return;
	if (!NB_IsAlive(ed))
	{
		lastscore[i] = 0;
		return;
	}
	/* only for bots that are wedged: scoring, or getting around the map (a TF VIP running for the
	   exit, whose suicide would hand the other team a win), resets the clock */
	if (lastscore[i] <= 0 || ed->v->frags > lastfrags[i] || Distance(ed->v->origin, lastpos[i]) > 512)
	{
		lastscore[i] = sv.time;
		lastfrags[i] = (int)ed->v->frags;
		VectorCopy(ed->v->origin, lastpos[i]);
		return;
	}
	if (sv.time - lastscore[i] >= qw_bot_stucktime.value)
	{
		if (qw_bot_verbose.ival)
			Con_Printf("bot %s: no score for %.0f s, respawning\n", bot->cl->name, sv.time - lastscore[i]);
		NB_ClientCommand(bot, "kill");
		lastscore[i] = sv.time;
	}
}

/* ------------------------------------------------------------------------------------------------
 * per-frame
 */
static void NB_RunBotCmd(nbbot_t *bot, float dt)
{
	usercmd_t cmd;
	client_t *oldhost = host_client;
	edict_t *oldplayer = sv_player;
	extern void NBLoco_BuildCmd(nbbot_t *bot, usercmd_t *cmd, float dt);

	NBLoco_BuildCmd(bot, &cmd, dt);
	host_client = bot->cl;
	sv_player = bot->cl->edict;
	/* SV_RunCmd reloads a bot client's movement and view angles from these fields after
	   PlayerPreThink (the DP_SV_BOTCLIENT convention), so they must hold the same values */
	sv_player->xv->movement[0] = cmd.forwardmove;
	sv_player->xv->movement[1] = cmd.sidemove;
	sv_player->xv->movement[2] = cmd.upmove;
	sv_player->v->v_angle[0] = SHORT2ANGLE(cmd.angles[0]);
	sv_player->v->v_angle[1] = SHORT2ANGLE(cmd.angles[1]);
	sv_player->v->v_angle[2] = SHORT2ANGLE(cmd.angles[2]);
	host_client->isindependant = true;
	host_client->localtime = sv.time;
	if (bot->runjumping && !(cmd.buttons & 2) && cmd.msec > NB_RJ_STEP)
	{	/* running at a running jump's edge: move in short steps (as a player's client does) and
		   jump on the step where the floor ends */
		int left = cmd.msec;
		usercmd_t step = cmd;
		while (left > 0)
		{
			step.msec = min(left, NB_RJ_STEP);
			left -= step.msec;
			if (!(step.buttons & 2) && NBLoco_AtJumpEdge(bot))
				step.buttons |= 2;
			SV_PreRunCmd();
			if (host_client->msecs < step.msec)
				host_client->msecs = step.msec;
			SV_RunCmd(&step, false);
			SV_PostRunCmd();
		}
	}
	else
	{
		SV_PreRunCmd();
		if (host_client->msecs < cmd.msec)
			host_client->msecs = cmd.msec;   /* bots send no packets: fund the speed-cheat budget */
		SV_RunCmd(&cmd, false);
		SV_PostRunCmd();
	}
	bot->runjumping = false;
	host_client->lastcmd = cmd;
	host_client->msecs = 0;
	host_client->lastruncmd = (unsigned int)(sv.time * 1000);
	host_client = oldhost;
	sv_player = oldplayer;
}

static void NB_Think(nbbot_t *bot, float dt)
{
	edict_t *ed = NB_Self(bot);
	extern void NBLoco_UpdateStuck(nbbot_t *bot);
	qboolean alive = NB_IsAlive(ed);

	if (alive && !bot->wasalive)
	{
		bot->spawntime = sv.time;
		VectorCopy(ed->v->angles, bot->viewangles);
		bot->viewangles[PITCH] = 0;
		bot->path.valid = false;
		bot->enemy = NULL;
	}
	if (!alive && bot->wasalive)
	{
		edict_t *killer = ed->v->enemy ? ((edict_t*)PROG_TO_EDICT(svprogfuncs, ed->v->enemy)) : NULL;
		bot->deathtime = sv.time;
		bot->deaths++;
		if (!killer || killer == ed || killer == (edict_t*)sv.world.edicts)
			bot->suicides++;
		Q_snprintfz(bot->lastdeath, sizeof(bot->lastdeath), "at %.0f %.0f %.0f killer %s wl %.0f wt %.0f action %s",
			ed->v->origin[0], ed->v->origin[1], ed->v->origin[2], killer ? NB_Classname(killer) : "none",
			ed->v->waterlevel, ed->v->watertype, bot->top ? bot->top->vt->name : "-");
		if (qw_bot_debug.ival)
			Con_Printf("[nb] %s died at %.0f %.0f %.0f: killer %s, waterlevel %.0f watertype %.0f contents %d, action %s\n", bot->cl->name,
				ed->v->origin[0], ed->v->origin[1], ed->v->origin[2], killer ? NB_Classname(killer) : "none",
				ed->v->waterlevel, ed->v->watertype, World_PointContentsWorldOnly(&sv.world, ed->v->origin), bot->top ? bot->top->vt->name : "-");
	}
	bot->wasalive = alive;

	if (alive && ed->v->health < bot->lasthealth)
	{
		edict_t *att = ed->v->enemy ? ((edict_t*)PROG_TO_EDICT(svprogfuncs, ed->v->enemy)) : NULL;
		bot->lastpain = sv.time;
		bot->lastattacker = att;
		NBIntention_OnInjured(bot, att);
	}
	bot->lasthealth = (int)ed->v->health;

	bot->hasaim = false;
	if (nb_game == NBGAME_TF && NBTF_PreThink(bot))
		;	/* TF took the frame (a feigning spy getting up) */
	else if (alive)
	{
		NBVision_Update(bot);
		NBIntention_Update(bot, dt);
		/* in lava or slime: get out before anything else */
		if (ed->v->waterlevel >= 1 && (ed->v->watertype == Q1CONTENTS_LAVA || ed->v->watertype == Q1CONTENTS_SLIME))
		{
			int safe = NBNav_NearestArea(ed->v->origin, 600);
			if (safe >= 0)
			{
				NBLoco_SetGoal(bot, nb_mesh.areas[safe].center);
				bot->path.valid = false;
			}
			bot->wantjump = true;
		}
		NBLoco_UpdateStuck(bot);
		/* fallen somewhere with no way out (a sealed pit): respawn rather than swim about for ever */
		{
			int here = NBNav_AreaAt(ed->v->origin, ed->v->waterlevel >= 2 ? 600 : 96);
			if (here >= 0 && (nb_mesh.areas[here].flags & NBAREA_TRAP))
			{
				if (!bot->trapsince)
					bot->trapsince = sv.time;
				else if (sv.time - bot->trapsince > 5)
				{
					bot->trapsince = 0;
					if (qw_bot_debug.ival)
						Con_Printf("[nb] %s is trapped at %.0f %.0f %.0f, respawning\n", bot->cl->name, ed->v->origin[0], ed->v->origin[1], ed->v->origin[2]);
					NB_ClientCommand(bot, "kill");
				}
			}
			else
				bot->trapsince = 0;
		}
		if (!bot->hasaim)
			NBBody_LookAlongPath(bot);
	}
	else if (nb_game == NBGAME_TF && (NB_TeamOf(ed) <= 0 || NB_GetFloat(ed, "playerclass") <= 0))
	{
		/* TF: not on a team / no class yet (an observer): the intention runs the join */
		bot->hasmovegoal = false;
		NBIntention_Update(bot, dt);
	}
	else
	{
		/* respawn: press attack/jump a moment after death (QW needs a fresh press) */
		bot->hasmovegoal = false;
		if (sv.time - bot->deathtime > 1.2f + frandom() * 0.5f && ((int)(sv.time * 10) & 1))
			bot->buttons |= 1;
	}
	NB_CheckNoScore(bot);
}

/* intermission: QW/TF wait for a button press to leave it, and FortressOne's GotoNextMap restarts
   the same map. Make sure the map moves on even with nobody pressing anything. */
static void NB_Intermission(void)
{
	float running = NB_Global("intermission_running");
	float exittime = NB_Global("intermission_exittime");
	int i;
	if (running <= 0)
		return;
	for (i = 0; i < NB_MAX_BOTS; i++)
		if (nb_bots[i].inuse && sv.time >= exittime + 1 && ((int)(sv.time * 4) & 1))
			nb_bots[i].buttons |= 1;
	if (exittime > 0 && sv.time > exittime + 8)
	{
		char next[MAX_QPATH];
		eval_t *v = PR_FindGlobal(svprogfuncs, "intermission_running", 0, NULL);
		Q_strncpyz(next, svs.name, sizeof(next));
		if (!NB_MapcycleNext(next, sizeof(next)) && !Q_strcasecmp(next, svs.name))
			return;   /* no cycle: let the game do what it does */
		if (v)
			v->_float = 0;
		Cbuf_AddText(va("\nchangelevel %s\n", next), RESTRICT_LOCAL);
	}
}

void NB_Frame(void)
{
	int i;
	float dt;
	extern void NBHeat_Frame(void);
	if (sv.state != ss_active || !svprogfuncs)
		return;
	NBHeat_Frame();
	dt = sv.time - nb_lastframe;
	if (dt <= 0)
		return;
	if (dt > 0.25f)
		dt = 0.25f;
	nb_lastframe = sv.time;

	if (sv.time >= nb_nextquota && sv.time - nb_maploadtime > 2)
	{
		NB_UpdateQuota();
		nb_nextquota = sv.time + 1;
	}
	if (nb_game == NBGAME_TF || NB_Global("intermission_running") > 0)
		NB_Intermission();

	for (i = 0; i < NB_MAX_BOTS; i++)
	{
		nbbot_t *bot = &nb_bots[i];
		if (!bot->inuse)
			continue;
		if (bot->cl->state < cs_spawned || !bot->cl->spawned || !bot->cl->edict)
			continue;
		NB_Think(bot, dt);
		NB_RunBotCmd(bot, dt);
	}
}

/* ------------------------------------------------------------------------------------------------
 * map change
 */
static qboolean NB_IsFortress(void)
{
	/* Team Fortress game code (any gamedir name): it has TF's player fields */
	const char *g = FS_GetGamedir(true);
	if (NB_HasField("playerclass") && NB_HasField("team_no"))
		return true;
	return !Q_strncasecmp(g, "fortress", 8) || !Q_strncasecmp(g, "customtf", 8);
}

void NB_PreMapChange(void)
{
	int i;
	extern void NBHeat_MapChange(void);
	NBHeat_MapChange();
	for (i = 0; i < NB_MAX_BOTS; i++)
		if (nb_bots[i].inuse)
			NB_RemoveBot(&nb_bots[i], false);
}

void NB_MapLoaded(void)
{
	int i;
	for (i = 0; i < NB_MAX_BOTS; i++)
		if (nb_bots[i].inuse)
			NB_RemoveBot(&nb_bots[i], false);
	NB_ResetFieldCache();
	nb_game = NB_IsFortress() ? NBGAME_TF : NBGAME_DM;
	nb_maploadtime = sv.time;
	nb_nextquota = sv.time + 2;
	nb_lastframe = sv.time;
	NB_LoadSkins();
	NB_LoadMapcycle();
	if (!NBNav_Load(svs.name))
	{
		if (qw_bot_nav_autogen.ival && NBNav_Generate())
			NBNav_Save();
	}
	else if (qw_bot_verbose.ival)
		Con_Printf("nav: loaded %s (%u areas)\n", svs.name, nb_mesh.numareas);
	NBNav_MarkTeleporters();
	if (nb_game == NBGAME_TF)
		NBTF_MapStart();
}

/* ------------------------------------------------------------------------------------------------
 * mapcycle (<gamedir>/mapcycle.txt, one map per line, // and # comments)
 */
static char nb_cycle[128][64];
static int nb_numcycle;

void NB_LoadMapcycle(void)
{
	size_t size;
	char *buf = FS_LoadMallocFile("mapcycle.txt", &size), *p, *end;
	nb_numcycle = 0;
	if (!buf)
		return;
	p = buf;
	end = buf + size;
	while (p < end && nb_numcycle < (int)countof(nb_cycle))
	{
		char *line;
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
			p++;
		if (p >= end)
			break;
		if ((p[0] == '/' && p + 1 < end && p[1] == '/') || p[0] == '#')
		{
			while (p < end && *p != '\n')
				p++;
			continue;
		}
		line = p;
		while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
			p++;
		if (p - line > 0 && p - line < 64)
		{
			memcpy(nb_cycle[nb_numcycle], line, p - line);
			nb_cycle[nb_numcycle][p - line] = 0;
			nb_numcycle++;
		}
		while (p < end && *p != '\n')
			p++;
	}
	BZ_Free(buf);
}

qboolean NB_MapcycleNext(char *level, int size)
{
	int i, cur = -1;
	if (!qw_bot_mapcycle.ival || nb_numcycle < 2)
		return false;
	for (i = 0; i < nb_numcycle; i++)
		if (!Q_strcasecmp(svs.name, nb_cycle[i]))
		{
			cur = i;
			break;
		}
	Q_strncpyz(level, nb_cycle[(cur + 1) % nb_numcycle], size);
	return true;
}

/* map changes requested by the game code follow the cycle; console map/changelevel do not */
void NB_MapcycleOverride(char *level, int size)
{
	char next[64];
	if (NB_MapcycleNext(next, sizeof(next)))
		Q_strncpyz(level, next, size);
}

/* ------------------------------------------------------------------------------------------------
 * Discord webhook
 */
#ifdef WEBCLIENT
static void NB_DiscordDone(struct dl_download *dl) { (void)dl; }
#endif
void NB_DiscordPost(const char *msg)
{
#ifdef WEBCLIENT
	char json[640], esc[480], *d = esc;
	const char *s;
	if (!*discord_webhook_url.string)
		return;
	for (s = msg; *s && d < esc + sizeof(esc) - 8; s++)
	{
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') { *d++ = '\\'; *d++ = c; }
		else if (c == '\n') { *d++ = '\\'; *d++ = 'n'; }
		else if (c == '\r') { *d++ = '\\'; *d++ = 'r'; }
		else if (c == '\t') { *d++ = '\\'; *d++ = 't'; }
		else if (c < 0x20) { Q_snprintfz(d, 7, "\\u%04x", c); d += 6; }
		else *d++ = c;
	}
	*d = 0;
	Q_snprintfz(json, sizeof(json), "{\"content\":\"%s\"}", esc);
	HTTP_CL_Put(discord_webhook_url.string, "application/json", json, strlen(json), NB_DiscordDone);
#else
	(void)msg;
#endif
}

/* ------------------------------------------------------------------------------------------------
 * commands
 */
static int NB_ParseClass(const char *s)
{
	static const char *names[] = { "", "scout", "sniper", "soldier", "demoman", "medic", "hwguy", "pyro", "spy", "engineer", "random", "civilian" };
	int i;
	if (!s || !*s)
		return 0;
	if (atoi(s) >= 1 && atoi(s) <= 11)
		return atoi(s);
	for (i = 1; i < (int)countof(names); i++)
		if (!Q_strcasecmp(s, names[i]))
			return i;
	if (!Q_strcasecmp(s, "demo")) return 4;
	if (!Q_strcasecmp(s, "heavy") || !Q_strcasecmp(s, "hw")) return 6;
	if (!Q_strcasecmp(s, "engy") || !Q_strcasecmp(s, "engi")) return 9;
	if (!Q_strcasecmp(s, "civ")) return 11;
	return 0;
}

int NB_ForcedClass(void)
{
	return NB_ParseClass(*qw_bot_force_class.string ? qw_bot_force_class.string : qw_bot_forceclass.string);
}

static void NB_Add_f(void)
{
	int count = Cmd_Argc() > 1 ? atoi(Cmd_Argv(1)) : 1, team = 0, pclass = 0, i;
	const char *name = NULL;
	if (sv.state != ss_active)
	{
		Con_Printf("no map running\n");
		return;
	}
	if (count < 1)
		count = 1;
	if (nb_game == NBGAME_TF)
	{	/* qw_bot_add [count] [team] [class] [difficulty] [name] */
		if (Cmd_Argc() > 2) team = atoi(Cmd_Argv(2));
		if (Cmd_Argc() > 3) pclass = NB_ParseClass(Cmd_Argv(3));
		if (Cmd_Argc() > 5) name = Cmd_Argv(5);
	}
	else
	{	/* qw_bot_add [count] [difficulty] [name] */
		if (Cmd_Argc() > 3) name = Cmd_Argv(3);
	}
	for (i = 0; i < count; i++)
		if (!NB_SpawnBot(team, pclass, i == 0 ? name : NULL))
		{
			Con_Printf("no free player slots\n");
			break;
		}
}

static void NB_Kick_f(void)
{
	const char *what = Cmd_Argc() > 1 ? Cmd_Argv(1) : "";
	int i, team = 0, n = 0;
	if (!*what)
	{
		Con_Printf("Usage: qw_bot_kick <name|team|all|N>\n");
		return;
	}
	if (!Q_strcasecmp(what, "blue")) team = 1;
	else if (!Q_strcasecmp(what, "red")) team = 2;
	else if (!Q_strcasecmp(what, "yellow")) team = 3;
	else if (!Q_strcasecmp(what, "green")) team = 4;
	for (i = NB_MAX_BOTS - 1; i >= 0; i--)
	{
		nbbot_t *b = &nb_bots[i];
		if (!b->inuse)
			continue;
		if (!Q_strcasecmp(what, "all")
			|| (team && NB_TeamOf(b->cl->edict) == team)
			|| (atoi(what) > 0 && n < atoi(what))
			|| (!team && atoi(what) <= 0 && Q_strcasestr(b->cl->name, what)))
		{
			NB_RemoveBot(b, true);
			n++;
		}
	}
	Con_Printf("%d bot(s) kicked\n", n);
}

static void NB_Status_f(void)
{
	int i;
	char buf[256];
	for (i = 0; i < NB_MAX_BOTS; i++)
	{
		nbbot_t *b = &nb_bots[i];
		edict_t *ed;
		if (!b->inuse)
			continue;
		ed = b->cl->edict;
		Con_Printf("%2d %-16s team %d cls %d hp %3.0f fr %3.0f at %5.0f %5.0f %5.0f stuck %d d%d s%d %s [%s]\n", i, b->cl->name, NB_TeamOf(ed),
			nb_game == NBGAME_TF ? (int)NB_GetFloat(ed, "playerclass") : 0, ed->v->health, ed->v->frags,
			ed->v->origin[0], ed->v->origin[1], ed->v->origin[2], b->stuckevents, b->deaths, b->suicides,
			b->enemy ? va("enemy %s", PR_GetString(svprogfuncs, b->enemy->v->netname)) : "",
			NBIntention_Describe(b, buf, sizeof(buf)));
	}
	Con_Printf("nav: %s %u areas, %u links\n", nb_mesh.mapname, nb_mesh.numareas, nb_mesh.numlinks);
	if (qw_bot_quota_spawns.ival)
		Con_Printf("quota from spawns: %d (team %d, deathmatch %d, start %d)\n", NB_MapSpawnCount(),
			nb_spawncounts[0], nb_spawncounts[1], nb_spawncounts[2]);
}

static void NB_NavGenerate_f(void)
{
	if (NBNav_Generate())
		NBNav_Save();
	NBNav_MarkTeleporters();
	if (nb_game == NBGAME_TF)
		NBTF_MapStart();   /* team blocks live on the areas: redo them for the new mesh */
}

void NB_Init(void)
{
	const char *grp = "Bots";
	Cvar_Register(&qw_bot_enable, grp);
	Cvar_Register(&qw_bot_quota, grp);
	Cvar_Register(&qw_bot_quota_mode, grp);
	Cvar_Register(&qw_bot_quota_spawns, grp);
	Cvar_Register(&qw_bot_skins, grp);
	Cvar_Register(&qw_bot_random_colors, grp);
	Cvar_Register(&qw_bot_meleeonly, grp);
	Cvar_Register(&qw_bot_force_class, grp);
	Cvar_Register(&qw_bot_forceclass, grp);
	Cvar_Register(&qw_bot_stucktime, grp);
	Cvar_Register(&qw_bot_verbose, grp);
	Cvar_Register(&qw_bot_spawnloadouts, grp);
	Cvar_Register(&qw_bot_spawnloadouts_time, grp);
	Cvar_Register(&qw_bot_spawnloadouts_ssg, grp);
	Cvar_Register(&qw_bot_spawnloadouts_ng, grp);
	Cvar_Register(&qw_bot_spawnloadouts_sng, grp);
	Cvar_Register(&qw_bot_spawnloadouts_gl, grp);
	Cvar_Register(&qw_bot_spawnloadouts_rl, grp);
	Cvar_Register(&qw_bot_spawnloadouts_lg, grp);
	Cvar_Register(&discord_webhook_url, grp);
	Cvar_Register(&qw_bot_skill, grp);
	Cvar_Register(&qw_bot_teamghost, grp);
	Cvar_Register(&qw_bot_debug, grp);
	Cvar_Register(&qw_bot_nav_autogen, grp);
	Cvar_Register(&qw_bot_mapcycle, grp);
	{
		extern void NBHeat_Init(void);
		NBHeat_Init();
	}
	Cmd_AddCommandD("qw_bot_add", NB_Add_f, "qw_bot_add [count] [team] [class] [difficulty] [name] (TF) / qw_bot_add [count] [difficulty] [name]");
	Cmd_AddCommandD("qw_bot_kick", NB_Kick_f, "qw_bot_kick <name|team|all|N>");
	Cmd_AddCommandD("qw_bot_status", NB_Status_f, "List the bots and what they are doing");
	Cmd_AddCommandD("qw_bot_nav_generate", NB_NavGenerate_f, "Generate and save the nav mesh for the current map");
	{
		extern void NB_InitDebugCommands(void);
		extern void NBTF_InitCommands(void);
		NB_InitDebugCommands();
		NBTF_InitCommands();
	}
}

#endif

#ifndef CLIENTONLY
/* ------------------------------------------------------------------------------------------------
 * stuffcmd: a real client would run these; bots apply the ones that change their userinfo, which
 * TF's team-cheat check verifies (bottomcolor, skin and team must match the player's team/class)
 */
static void NB_StuffLine(nbbot_t *bot, char *line)
{
	char *argv[8];
	int argc = 0;
	char *p = line;
	while (*p && argc < 8)
	{
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		if (*p == '"')
		{
			argv[argc++] = ++p;
			while (*p && *p != '"')
				p++;
		}
		else
		{
			argv[argc++] = p;
			while (*p && *p != ' ' && *p != '\t')
				p++;
		}
		if (*p)
			*p++ = 0;
	}
	if (!argc)
		return;
	if (!Q_strcasecmp(argv[0], "color") && argc >= 2)
	{
		NB_ClientCommand(bot, "setinfo topcolor %s", argv[1]);
		NB_ClientCommand(bot, "setinfo bottomcolor %s", argc >= 3 ? argv[2] : argv[1]);
	}
	else if ((!Q_strcasecmp(argv[0], "topcolor") || !Q_strcasecmp(argv[0], "bottomcolor")
		|| !Q_strcasecmp(argv[0], "skin") || !Q_strcasecmp(argv[0], "team")) && argc >= 2)
		NB_ClientCommand(bot, "setinfo %s \"%s\"", argv[0], argv[1]);
	else if (!Q_strcasecmp(argv[0], "setinfo") && argc >= 3)
		NB_ClientCommand(bot, "setinfo %s \"%s\"", argv[1], argv[2]);
	else if (!Q_strcasecmp(argv[0], "cmd") && argc >= 2)
	{	/* forwarded command, e.g. FortressOne's "cmd ..." stuffs */
		char rest[256];
		int i;
		rest[0] = 0;
		for (i = 1; i < argc; i++)
		{
			if (i > 1)
				Q_strncatz(rest, " ", sizeof(rest));
			Q_strncatz(rest, argv[i], sizeof(rest));
		}
		NB_ClientCommand(bot, "%s", rest);
	}
}

void NB_StuffCmd(client_t *cl, const char *text)
{
	nbbot_t *bot = NB_BotForClient(cl);
	if (!bot)
		return;
	for (; *text; text++)
	{
		if (*text == '\n' || *text == ';')
		{
			bot->stuffbuf[bot->stufflen] = 0;
			if (bot->stufflen)
				NB_StuffLine(bot, bot->stuffbuf);
			bot->stufflen = 0;
			continue;
		}
		if (bot->stufflen < (int)sizeof(bot->stuffbuf) - 1)
			bot->stuffbuf[bot->stufflen++] = *text;
	}
}
#endif

#ifndef CLIENTONLY
/* qw_bot_navinfo <slot>: the nav area under a bot and its links (debugging) */
static void NB_NavInfo_f(void)
{
	int slot = atoi(Cmd_Argv(1)), a;
	unsigned int l;
	edict_t *ed;
	if (slot < 0 || slot >= NB_MAX_BOTS || !nb_bots[slot].inuse)
	{
		Con_Printf("no bot in slot %d\n", slot);
		return;
	}
	ed = nb_bots[slot].cl->edict;
	a = NBNav_AreaAt(ed->v->origin, 128);
	Con_Printf("%s at %.0f %.0f %.0f: area %d, path %s seg %d/%d goal %.0f %.0f %.0f\n", nb_bots[slot].cl->name,
		ed->v->origin[0], ed->v->origin[1], ed->v->origin[2], a,
		nb_bots[slot].path.valid ? "valid" : "none", nb_bots[slot].path.cur, nb_bots[slot].path.numsegs,
		nb_bots[slot].movegoal[0], nb_bots[slot].movegoal[1], nb_bots[slot].movegoal[2]);
	Con_Printf("  last death: %s\n", nb_bots[slot].lastdeath);
	if (a < 0)
		return;
	{
		nbarea_t *ar = &nb_mesh.areas[a];
		Con_Printf("  area %.0f..%.0f x %.0f..%.0f z %.0f/%.0f/%.0f/%.0f flags %x links %u\n", ar->mins[0], ar->maxs[0], ar->mins[1], ar->maxs[1],
			ar->z[0], ar->z[1], ar->z[2], ar->z[3], ar->flags, ar->numlinks);
		for (l = 0; l < ar->numlinks; l++)
		{
			nblink_t *lk = &nb_mesh.links[ar->firstlink + l];
			Con_Printf("   -> %u type %d (center %.0f %.0f %.0f)\n", lk->to, lk->type, nb_mesh.areas[lk->to].center[0], nb_mesh.areas[lk->to].center[1], nb_mesh.areas[lk->to].center[2]);
		}
	}
}

void NB_InitDebugCommands(void)
{
	Cmd_AddCommandD("qw_bot_navinfo", NB_NavInfo_f, "qw_bot_navinfo <slot>: nav area under a bot");
	{
		extern void NB_InitDebugCommands2(void);
		extern void NB_InitDebugCommands3(void);
		NB_InitDebugCommands2();
		NB_InitDebugCommands3();
		{
			extern void NB_InitDebugCommands4(void);
			NB_InitDebugCommands4();
		}
	}
}
#endif

#ifndef CLIENTONLY
/* qw_bot_navreach <x> <y> <z>: how much of the mesh is reachable from that point (debugging) */
static void NB_NavReach_f(void)
{
	vec3_t p;
	int start, n = 0, head = 0, tail = 0;
	unsigned int *queue, i, l;
	unsigned char *seen;
	float zmin = 1e9, zmax = -1e9;
	p[0] = atof(Cmd_Argv(1)); p[1] = atof(Cmd_Argv(2)); p[2] = atof(Cmd_Argv(3));
	start = NBNav_AreaAt(p, 200);
	if (start < 0)
		start = NBNav_NearestArea(p, 400);
	if (start < 0)
	{
		Con_Printf("no area near that point\n");
		return;
	}
	queue = Z_Malloc(sizeof(*queue) * nb_mesh.numareas);
	seen = Z_Malloc(nb_mesh.numareas);
	queue[tail++] = start;
	seen[start] = 1;
	while (head < tail)
	{
		nbarea_t *a = &nb_mesh.areas[queue[head++]];
		n++;
		zmin = min(zmin, a->center[2]);
		zmax = max(zmax, a->center[2]);
		for (l = 0; l < a->numlinks; l++)
		{
			unsigned int to = nb_mesh.links[a->firstlink + l].to;
			if (!seen[to])
			{
				seen[to] = 1;
				queue[tail++] = to;
			}
		}
	}
	Con_Printf("from area %d: %d of %u areas reachable, z %.0f..%.0f\n", start, n, nb_mesh.numareas, zmin, zmax);
	/* the reachable area closest to an unreachable one: where the mesh breaks */
	{
		float best = 1e9;
		unsigned int ba = 0, bu = 0;
		for (i = 0; i < nb_mesh.numareas && n < (int)nb_mesh.numareas; i++)
		{
			unsigned int j;
			if (!seen[i])
				continue;
			for (j = 0; j < nb_mesh.numareas; j += 1)
			{
				float d;
				if (seen[j])
					continue;
				d = Distance(nb_mesh.areas[i].center, nb_mesh.areas[j].center);
				if (d < best)
				{
					best = d;
					ba = i;
					bu = j;
				}
			}
			if (n > 2000)
				break;   /* big component: don't spend forever */
		}
		if (best < 1e9)
			Con_Printf("closest gap: area %u (%.0f %.0f %.0f) -> unreachable area %u (%.0f %.0f %.0f), %.0f apart\n", ba,
				nb_mesh.areas[ba].center[0], nb_mesh.areas[ba].center[1], nb_mesh.areas[ba].center[2], bu,
				nb_mesh.areas[bu].center[0], nb_mesh.areas[bu].center[1], nb_mesh.areas[bu].center[2], best);
	}
	Z_Free(queue);
	Z_Free(seen);
}

void NB_InitDebugCommands2(void)
{
	Cmd_AddCommandD("qw_bot_navreach", NB_NavReach_f, "qw_bot_navreach <x> <y> <z>: reachable mesh from a point");
}
#endif

#ifndef CLIENTONLY
/* qw_bot_pathpts x1 y1 z1 x2 y2 z2: print a path's segments (debugging) */
static void NB_PathPts_f(void)
{
	vec3_t a, b;
	nbpath_t *p = Z_Malloc(sizeof(*p));
	int i;
	a[0] = atof(Cmd_Argv(1)); a[1] = atof(Cmd_Argv(2)); a[2] = atof(Cmd_Argv(3));
	b[0] = atof(Cmd_Argv(4)); b[1] = atof(Cmd_Argv(5)); b[2] = atof(Cmd_Argv(6));
	if (!NBPath_Compute(p, a, b, 0, NULL, NULL))
		Con_Printf("no path\n");
	else
		for (i = 0; i < p->numsegs; i++)
			Con_Printf("%3d area %5d how %d  %.0f %.0f %.0f\n", i, p->seg[i].area, p->seg[i].how, p->seg[i].pos[0], p->seg[i].pos[1], p->seg[i].pos[2]);
	Z_Free(p);
}
void NB_InitDebugCommands3(void)
{
	Cmd_AddCommandD("qw_bot_pathpts", NB_PathPts_f, "qw_bot_pathpts x1 y1 z1 x2 y2 z2");
}
#endif

#ifndef CLIENTONLY
/* qw_bot_navcheck: reachability report for the current map (batch validation) */
static void NB_NavCheck_f(void)
{
	unsigned int *queue, l;
	unsigned char *seen;
	int head = 0, tail = 0, i, n = 0, items = 0, itemsok = 0, goals = 0, goalsok = 0, spawns = 0;
	char missing[512];
	missing[0] = 0;
	if (!nb_mesh.numareas)
	{
		Con_Printf("navcheck %s: NO MESH\n", svs.name);
		return;
	}
	queue = Z_Malloc(sizeof(*queue) * nb_mesh.numareas);
	seen = Z_Malloc(nb_mesh.numareas);
	for (i = 1; i < sv.world.num_edicts; i++)
	{
		edict_t *e = EDICT_NUM_PB(svprogfuncs, i);
		int a;
		if (ED_ISFREE(e) || strncmp(NB_Classname(e), "info_player", 11))
			continue;
		a = NBNav_AreaAt(e->v->origin, 128);
		if (a < 0)
			a = NBNav_NearestArea(e->v->origin, 128);
		if (a >= 0 && !seen[a])
		{
			seen[a] = 1;
			queue[tail++] = a;
			spawns++;
		}
	}
	while (head < tail)
	{
		nbarea_t *ar = &nb_mesh.areas[queue[head++]];
		n++;
		for (l = 0; l < ar->numlinks; l++)
		{
			unsigned int to = nb_mesh.links[ar->firstlink + l].to;
			if (!seen[to])
			{
				seen[to] = 1;
				queue[tail++] = to;
			}
		}
	}
	for (i = 1; i < sv.world.num_edicts; i++)
	{
		edict_t *e = EDICT_NUM_PB(svprogfuncs, i);
		const char *cn;
		vec3_t c;
		int a, isgoal;
		if (ED_ISFREE(e))
			continue;
		cn = NB_Classname(e);
		isgoal = !strcmp(cn, "item_tfgoal") || ((!strcmp(cn, "info_tfgoal") || !strcmp(cn, "i_t_g")) && ((int)NB_GetFloat(e, "goal_activation") & 1));
		if (!isgoal && strncmp(cn, "item_", 5) && strncmp(cn, "weapon_", 7))
			continue;
		if (isgoal) goals++; else items++;
		VectorAdd(e->v->absmin, e->v->absmax, c);
		VectorScale(c, 0.5f, c);
		a = NBNav_AreaAt(c, 160);
		if (a < 0)
			a = NBNav_NearestArea(c, 96);
		if (a >= 0 && seen[a])
		{
			if (isgoal) goalsok++; else itemsok++;
		}
		else if (strlen(missing) < sizeof(missing) - 64)
			Q_strncatz(missing, va(" %s@%.0f,%.0f,%.0f", cn, c[0], c[1], c[2]), sizeof(missing));
	}
	Con_Printf("navcheck %s: areas %u reach %d%% (from %d spawn areas) items %d/%d goals %d/%d%s%s\n", svs.name, nb_mesh.numareas,
		(int)(100.0f * n / nb_mesh.numareas), spawns, itemsok, items, goalsok, goals, missing[0] ? " missing:" : "", missing);
	Z_Free(queue);
	Z_Free(seen);
}
void NB_InitDebugCommands4(void)
{
	Cmd_AddCommandD("qw_bot_navcheck", NB_NavCheck_f, "Reachability report for the current map's nav mesh");
}
#endif
