/*
 * FortressOne Team Fortress behaviour.
 * Copyright (C) 2026 fteqw-sv contributors. GPLv2 or later; see nb_local.h.
 * (Initial stub: teams by team_no; full TF behaviour follows.)
 */
#include "nb_local.h"

#ifndef CLIENTONLY

extern int NB_TeamOf(edict_t *ed);
nbaction_t *NBDM_RootAction(void);

qboolean NBTF_IsEnemy(nbbot_t *bot, edict_t *other)
{
	int n = NUM_FOR_EDICT(svprogfuncs, other);
	int mine = NB_TeamOf(NB_Self(bot)), theirs;
	if (n < 1 || n > sv.allocated_client_slots)
		return false;
	if (svs.clients[n - 1].state != cs_spawned || !NB_IsAlive(other))
		return false;
	theirs = NB_TeamOf(other);
	if (theirs <= 0 || mine <= 0)
		return false;
	return theirs != mine;
}

nbaction_t *NBTF_RootAction(void)
{
	return NBDM_RootAction();
}

void NBTF_MapStart(void)
{
}

void NBTF_ClientJoin(nbbot_t *bot)
{
}

int NBTF_TeamBlockBits(edict_t *door)
{
	return 0;
}

#endif
