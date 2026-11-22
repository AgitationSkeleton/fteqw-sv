/*
 * Engine-facing entry points of the NextBot-style bots.
 * Copyright (C) 2026 fteqw-sv contributors. GPLv2 or later; see nb_local.h.
 */
#ifndef NB_PUBLIC_H
#define NB_PUBLIC_H
#ifndef CLIENTONLY
struct client_s;
void NB_Init(void);                                  /* SV_Init */
void NB_Frame(void);                                 /* SV_Physics, before the bot usercmd loop */
qboolean NB_OwnsClient(struct client_s *cl);         /* the stock DP_SV_BOTCLIENT loop skips these */
void NB_PreMapChange(void);                          /* SV_Map_f, before SV_SaveSpawnparms */
void NB_MapLoaded(void);                             /* SV_SpawnServer, after the map has spawned */
void NB_MapcycleOverride(char *level, int size);     /* game-code changelevel */
qboolean NB_MapcycleNext(char *level, int size);
void NB_LoadMapcycle(void);
void NB_DiscordPost(const char *msg);                /* real players' join/leave/chat */
void NB_StuffCmd(struct client_s *cl, const char *text); /* stuffcmd text sent to a bot */
#endif
#endif
