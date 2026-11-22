/*
 * NextBot-style intention: a stack of actions.
 * Copyright (C) 2026 fteqw-sv contributors. GPLv2 or later; see nb_local.h.
 *
 * Semantics:
 *   Update of the top action returns
 *     CONTINUE    keep running it
 *     CHANGETO    end it and start the new action in its place
 *     SUSPENDFOR  suspend it and start the new action on top; when that one is DONE, resume it
 *     DONE        end it and resume the action below (the bottom action restarts if it ends)
 *   Events go to the top action first; an action that does not handle an event (NULL handler or
 *   CONTINUE) passes it to the action below. The first non-CONTINUE result is applied to the
 *   action that returned it (SUSTAIN means "handled, do nothing").
 */
#include "nb_local.h"

#ifndef CLIENTONLY

nbresult_t NB_Continue(void)                         { nbresult_t r = { NBR_CONTINUE, NULL, NULL }; return r; }
nbresult_t NB_ChangeTo(nbaction_t *a, const char *w)  { nbresult_t r = { NBR_CHANGETO, a, w }; return r; }
nbresult_t NB_SuspendFor(nbaction_t *a, const char *w){ nbresult_t r = { NBR_SUSPENDFOR, a, w }; return r; }
nbresult_t NB_Done(const char *w)                     { nbresult_t r = { NBR_DONE, NULL, w }; return r; }
nbresult_t NB_Sustain(const char *w)                  { nbresult_t r = { NBR_SUSTAIN, NULL, w }; return r; }

nbaction_t *NBAction_New(const nbactionvt_t *vt)
{
	nbaction_t *a = Z_Malloc(vt->size);
	a->vt = vt;
	return a;
}

static void NBAction_Free(nbaction_t *a)
{
	Z_Free(a);
}

static void NB_DebugTransition(nbbot_t *bot, const char *what, nbaction_t *from, nbaction_t *to, const char *reason)
{
	if (qw_bot_debug.ival >= 2)
		Con_Printf("^3[nb] %s: %s %s -> %s (%s)\n", bot->cl ? bot->cl->name : "?", what,
			from ? from->vt->name : "-", to ? to->vt->name : "-", reason ? reason : "");
}

/* start action a on top of the stack (its 'below' already set). Follows OnStart results. */
static void NBIntention_Start(nbbot_t *bot, nbaction_t *a, nbaction_t *prior, int depth);

static void NBIntention_Apply(nbbot_t *bot, nbaction_t *self, nbresult_t r, int depth)
{
	nbaction_t *below;
	if (depth > 16)
	{	/* a result loop: drop what we were given */
		if (r.action)
			NBAction_Free(r.action);
		return;
	}
	switch (r.type)
	{
	case NBR_CONTINUE:
	case NBR_SUSTAIN:
		if (r.action)
			NBAction_Free(r.action);
		return;
	case NBR_CHANGETO:
		/* self must be the top: remove self and everything above it */
		while (bot->top && bot->top != self)
		{
			nbaction_t *t = bot->top;
			bot->top = t->below;
			if (t->vt->OnEnd)
				t->vt->OnEnd(bot, t, NULL);
			NBAction_Free(t);
		}
		NB_DebugTransition(bot, "change", self, r.action, r.reason);
		below = self->below;
		bot->top = below;
		if (self->vt->OnEnd)
			self->vt->OnEnd(bot, self, r.action);
		r.action->below = below;
		NBIntention_Start(bot, r.action, self, depth + 1);
		NBAction_Free(self);
		return;
	case NBR_SUSPENDFOR:
		while (bot->top && bot->top != self)
		{
			nbaction_t *t = bot->top;
			bot->top = t->below;
			if (t->vt->OnEnd)
				t->vt->OnEnd(bot, t, NULL);
			NBAction_Free(t);
		}
		NB_DebugTransition(bot, "suspend", self, r.action, r.reason);
		if (self->vt->OnSuspend)
		{
			nbresult_t s = self->vt->OnSuspend(bot, self, r.action);
			if (s.type == NBR_DONE)
			{	/* the suspended action gives up instead: replace it */
				below = self->below;
				bot->top = below;
				if (self->vt->OnEnd)
					self->vt->OnEnd(bot, self, r.action);
				NBAction_Free(self);
				r.action->below = below;
				NBIntention_Start(bot, r.action, NULL, depth + 1);
				return;
			}
		}
		r.action->below = self;
		NBIntention_Start(bot, r.action, self, depth + 1);
		return;
	case NBR_DONE:
		while (bot->top && bot->top != self)
		{
			nbaction_t *t = bot->top;
			bot->top = t->below;
			if (t->vt->OnEnd)
				t->vt->OnEnd(bot, t, NULL);
			NBAction_Free(t);
		}
		below = self->below;
		NB_DebugTransition(bot, "done", self, below, r.reason);
		bot->top = below;
		if (self->vt->OnEnd)
			self->vt->OnEnd(bot, self, below);
		if (!below)
		{	/* the root finished: restart a fresh root of the same kind */
			nbaction_t *root = NBAction_New(self->vt);
			NBAction_Free(self);
			root->below = NULL;
			NBIntention_Start(bot, root, NULL, depth + 1);
			return;
		}
		NBAction_Free(self);
		if (below->vt->OnResume)
			NBIntention_Apply(bot, below, below->vt->OnResume(bot, below, NULL), depth + 1);
		return;
	}
}

static void NBIntention_Start(nbbot_t *bot, nbaction_t *a, nbaction_t *prior, int depth)
{
	bot->top = a;
	a->started = NB_Time();
	a->started_ok = true;
	if (a->vt->OnStart)
	{	/* events raised while starting are queued like those raised during an Update */
		qboolean was = bot->inupdate;
		nbresult_t r;
		bot->inupdate = true;
		r = a->vt->OnStart(bot, a, prior);
		bot->inupdate = was;
		NBIntention_Apply(bot, a, r, depth);
	}
}

void NBIntention_Init(nbbot_t *bot, nbaction_t *root)
{
	NBIntention_Free(bot);
	root->below = NULL;
	NBIntention_Start(bot, root, NULL, 0);
}

void NBIntention_Free(nbbot_t *bot)
{
	if (bot->pendingact && bot->pendingres.action)
		NBAction_Free(bot->pendingres.action);
	bot->pendingact = NULL;
	while (bot->top)
	{
		nbaction_t *t = bot->top;
		bot->top = t->below;
		if (t->vt->OnEnd)
			t->vt->OnEnd(bot, t, NULL);
		NBAction_Free(t);
	}
}

static qboolean NBIntention_OnStack(nbbot_t *bot, nbaction_t *a)
{
	nbaction_t *t;
	for (t = bot->top; t; t = t->below)
		if (t == a)
			return true;
	return false;
}

void NBIntention_Update(nbbot_t *bot, float dt)
{
	nbaction_t *top = bot->top;
	nbresult_t r;
	if (!top || !top->vt->Update)
		return;
	bot->inupdate = true;
	r = top->vt->Update(bot, top, dt);
	bot->inupdate = false;
	if (r.type != NBR_CONTINUE || !bot->pendingact)
	{
		/* the update's own result wins over an event raised during it */
		if (bot->pendingact && bot->pendingres.action)
			NBAction_Free(bot->pendingres.action);
		bot->pendingact = NULL;
		NBIntention_Apply(bot, top, r, 0);
		return;
	}
	{
		nbaction_t *a = bot->pendingact;
		nbresult_t pr = bot->pendingres;
		bot->pendingact = NULL;
		if (NBIntention_OnStack(bot, a))
			NBIntention_Apply(bot, a, pr, 0);
		else if (pr.action)
			NBAction_Free(pr.action);
	}
}

/* deliver an event from the top down: the first action that does not CONTINUE handles it */
enum { EV_STUCK, EV_INJURED, EV_SIGHT, EV_PATHFAILED };

static void NBIntention_Event(nbbot_t *bot, int ev, edict_t *who)
{
	nbaction_t *a;
	for (a = bot->top; a; a = a->below)
	{
		nbresult_t r = NB_Continue();
		switch (ev)
		{
		case EV_STUCK:      if (a->vt->OnStuck) r = a->vt->OnStuck(bot, a); break;
		case EV_INJURED:    if (a->vt->OnInjured) r = a->vt->OnInjured(bot, a, who); break;
		case EV_SIGHT:      if (a->vt->OnSight) r = a->vt->OnSight(bot, a, who); break;
		case EV_PATHFAILED: if (a->vt->OnPathFailed) r = a->vt->OnPathFailed(bot, a); break;
		}
		if (r.type != NBR_CONTINUE)
		{
			if (bot->inupdate)
			{	/* raised from inside an Update: apply once it has returned */
				if (bot->pendingact && bot->pendingres.action)
					NBAction_Free(bot->pendingres.action);
				bot->pendingact = a;
				bot->pendingres = r;
				return;
			}
			NBIntention_Apply(bot, a, r, 0);
			return;
		}
	}
}

void NBIntention_OnStuck(nbbot_t *bot)                      { NBIntention_Event(bot, EV_STUCK, NULL); }
void NBIntention_OnInjured(nbbot_t *bot, edict_t *attacker) { NBIntention_Event(bot, EV_INJURED, attacker); }
void NBIntention_OnSight(nbbot_t *bot, edict_t *who)        { NBIntention_Event(bot, EV_SIGHT, who); }
void NBIntention_OnPathFailed(nbbot_t *bot)                 { NBIntention_Event(bot, EV_PATHFAILED, NULL); }

int NBIntention_ShouldAttack(nbbot_t *bot, edict_t *enemy)
{
	nbaction_t *a;
	for (a = bot->top; a; a = a->below)
	{
		if (a->vt->ShouldAttack)
		{
			int v = a->vt->ShouldAttack(bot, a, enemy);
			if (v >= 0)
				return v;
		}
	}
	return 1;
}

const char *NBIntention_Describe(nbbot_t *bot, char *buf, size_t size)
{
	nbaction_t *a;
	buf[0] = 0;
	for (a = bot->top; a; a = a->below)
	{
		if (buf[0])
			Q_strncatz(buf, " < ", size);
		Q_strncatz(buf, a->vt->name, size);
	}
	return buf;
}

#endif
