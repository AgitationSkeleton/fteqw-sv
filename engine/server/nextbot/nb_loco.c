/*
 * Locomotion (path following), body (aim) and vision for NextBot-style bots.
 * Copyright (C) 2026 fteqw-sv contributors. GPLv2 or later; see nb_local.h.
 */
#include "nb_local.h"

#ifndef CLIENTONLY

static const vec3_t nb_hmins = { -16, -16, -24 };
static const vec3_t nb_hmaxs = { 16, 16, 32 };

/* ------------------------------------------------------------------------------------------------
 * vision
 */
qboolean NBVision_LineClear(const vec3_t a, const vec3_t b, edict_t *ignore)
{
	trace_t tr = World_Move(&sv.world, (float*)a, vec3_origin, vec3_origin, (float*)b, MOVE_NOMONSTERS, (wedict_t*)ignore);
	return tr.fraction >= 1 && !tr.startsolid;
}

static void NB_Eye(edict_t *ed, vec3_t out)
{
	VectorAdd(ed->v->origin, ed->v->view_ofs, out);
	if (!ed->v->view_ofs[2])
		out[2] += 22;
}

qboolean NBVision_CanSee(nbbot_t *bot, edict_t *target)
{
	edict_t *self = NB_Self(bot);
	vec3_t eye, p;
	NB_Eye(self, eye);
	VectorCopy(target->v->origin, p);
	if (NBVision_LineClear(eye, p, self))
		return true;
	p[2] += 20;   /* head */
	if (NBVision_LineClear(eye, p, self))
		return true;
	p[2] -= 36;   /* feet */
	return NBVision_LineClear(eye, p, self);
}

/* ------------------------------------------------------------------------------------------------
 * body: aim
 */
static float NB_AngleMod(float a)
{
	while (a > 180) a -= 360;
	while (a < -180) a += 360;
	return a;
}

void NBBody_AimAt(nbbot_t *bot, const vec3_t pos)
{
	VectorCopy(pos, bot->aimtarget);
	bot->hasaim = true;
}

qboolean NBBody_IsAimedAt(nbbot_t *bot, const vec3_t pos, float tol)
{
	edict_t *self = NB_Self(bot);
	vec3_t eye, dir, ang;
	NB_Eye(self, eye);
	VectorSubtract(pos, eye, dir);
	VectorAngles(dir, NULL, ang, false);
	return fabs(NB_AngleMod(ang[YAW] - bot->viewangles[YAW])) <= tol && fabs(NB_AngleMod(ang[PITCH] - bot->viewangles[PITCH])) <= tol * 1.5f;
}

void NBBody_LookAlongPath(nbbot_t *bot)
{
	vec3_t p;
	if (!bot->hasmovegoal)
		return;
	VectorCopy(bot->movegoal, p);
	p[2] += 46;   /* eye height above the floor point */
	NBBody_AimAt(bot, p);
}

/* turn the view toward the aim target at the bot's aim speed */
void NBBody_Update(nbbot_t *bot, float dt)
{
	edict_t *self = NB_Self(bot);
	vec3_t eye, dir, want;
	float dy, dp, maxturn;
	if (!bot->hasaim)
		return;
	NB_Eye(self, eye);
	VectorSubtract(bot->aimtarget, eye, dir);
	if (VectorLength(dir) < 1)
		return;
	VectorAngles(dir, NULL, want, false);
	want[PITCH] = -want[PITCH];   /* VectorAngles gives model pitch; view pitch is inverted */
	dy = NB_AngleMod(want[YAW] - bot->viewangles[YAW]);
	dp = NB_AngleMod(want[PITCH] - bot->viewangles[PITCH]);
	maxturn = bot->aimspeed * dt;
	/* ease in: fast for large errors, precise when close */
	if (fabs(dy) > maxturn) dy = dy > 0 ? maxturn : -maxturn;
	if (fabs(dp) > maxturn) dp = dp > 0 ? maxturn : -maxturn;
	bot->viewangles[YAW] = anglemod(bot->viewangles[YAW] + dy);
	bot->viewangles[PITCH] = bound(-80, bot->viewangles[PITCH] + dp, 80);
	bot->viewangles[ROLL] = 0;
}

/* ------------------------------------------------------------------------------------------------
 * locomotion
 */
void NBLoco_SetGoal(nbbot_t *bot, const vec3_t pos)
{
	VectorCopy(pos, bot->movegoal);
	bot->hasmovegoal = true;
}

void NBLoco_ClearGoal(nbbot_t *bot)
{
	bot->hasmovegoal = false;
	bot->path.valid = false;
}

static qboolean NB_WalkableDirect(edict_t *self, const vec3_t floorpt)
{
	vec3_t end;
	trace_t tr;
	VectorCopy(floorpt, end);
	end[2] += 24 + 4;
	if (fabs(end[2] - self->v->origin[2]) > NB_STEPSIZE)
		return false;
	tr = World_Move(&sv.world, self->v->origin, (float*)nb_hmins, (float*)nb_hmaxs, end, MOVE_NOMONSTERS, (wedict_t*)self);
	if (tr.fraction < 1)
		return false;
	{	/* floor all the way, and no lava or slime: sample every 24 units */
		vec3_t d;
		float len, t;
		VectorSubtract(end, self->v->origin, d);
		len = VectorLength(d);
		for (t = 24; t < len; t += 24)
		{
			vec3_t p, down, feet;
			int c;
			VectorMA(self->v->origin, t / len, d, p);
			VectorCopy(p, down);
			down[2] -= 24 + NB_STEPSIZE + 8;
			tr = World_Move(&sv.world, p, (float*)nb_hmins, (float*)nb_hmaxs, down, MOVE_NOMONSTERS, (wedict_t*)self);
			if (tr.fraction >= 1)
				return false;
			VectorCopy(tr.endpos, feet);
			feet[2] -= 20;
			c = World_PointContentsWorldOnly(&sv.world, feet);
			if (c & (FTECONTENTS_LAVA | FTECONTENTS_SLIME))
				return false;
		}
	}
	return true;
}

/* in the air, steering at target: QW air control adds speed only while the velocity along the wish
   direction is under NB_AIRSPEED, so pushing toward a target ahead does nothing at speed. Push
   across the line of flight instead (as players strafe), toward the side the target is on; push
   straight at it when well off course (that brakes an overshoot) */
void NB_AirSteer(const vec3_t pos, const vec3_t vel, const vec3_t target, vec3_t wish)
{
	vec3_t d, v;
	float sp, along;
	VectorSet(d, target[0] - pos[0], target[1] - pos[1], 0);
	VectorNormalize(d);
	VectorSet(v, vel[0], vel[1], 0);
	sp = VectorNormalize(v);
	along = DotProduct(d, v);
	if (sp > 1 && along > 0.7f)
	{
		vec3_t perp;
		VectorMA(d, -along, v, perp);
		if (VectorNormalize(perp) > 0.02f)   /* on line (within a degree): nothing to correct */
		{
			VectorCopy(perp, wish);
			return;
		}
	}
	VectorCopy(d, wish);
}

/* running at a running jump's landing spot: is the floor about to end? (the edge is within one
   short movement step, NB_RJ_STEP, ahead) */
qboolean NBLoco_AtJumpEdge(nbbot_t *bot)
{
	edict_t *self = NB_Self(bot);
	nbpath_t *p = &bot->path;
	vec3_t u, ahead, down;
	trace_t tr;
	float look;
	if (!p->valid || p->cur >= p->numsegs || p->seg[p->cur].how != NBLINK_RUNJUMP || !((int)self->v->flags & FL_ONGROUND))
		return false;
	VectorSet(u, p->seg[p->cur].pos[0] - self->v->origin[0], p->seg[p->cur].pos[1] - self->v->origin[1], 0);
	if (VectorNormalize(u) < 24)
		return false;
	/* look where we're actually going (sliding along a wall, that's along the wall) */
	look = sqrt(self->v->velocity[0] * self->v->velocity[0] + self->v->velocity[1] * self->v->velocity[1]);
	if (look > 100)
		VectorSet(u, self->v->velocity[0] / look, self->v->velocity[1] / look, 0);
	look = look * (NB_RJ_STEP / 1000.0f) + 8;
	VectorSet(ahead, self->v->origin[0] + u[0] * look, self->v->origin[1] + u[1] * look, self->v->origin[2]);
	tr = World_Move(&sv.world, self->v->origin, (float*)nb_hmins, (float*)nb_hmaxs, ahead, MOVE_NOMONSTERS, (wedict_t*)self);
	VectorCopy(tr.endpos, ahead);
	VectorSet(down, ahead[0], ahead[1], ahead[2] - NB_STEPSIZE - 1);
	tr = World_Move(&sv.world, ahead, (float*)nb_hmins, (float*)nb_hmaxs, down, MOVE_NOMONSTERS, (wedict_t*)self);
	return !tr.startsolid && (tr.fraction >= 1 || tr.plane.normal[2] < 0.7f);
}

qboolean NBLoco_FollowPath(nbbot_t *bot)
{
	edict_t *self = NB_Self(bot);
	nbpath_t *p = &bot->path;
	nbpathseg_t *seg;
	vec3_t d;
	float dist2d;
	int look;

	if (!p->valid || p->cur >= p->numsegs)
		return false;

	/* advance past reached segments */
	while (p->cur < p->numsegs)
	{
		seg = &p->seg[p->cur];
		VectorSubtract(seg->pos, self->v->origin, d);
		dist2d = sqrt(d[0] * d[0] + d[1] * d[1]);
		if (dist2d < 20 && fabs(seg->pos[2] + 24 - self->v->origin[2]) < 48)
		{
			p->cur++;
			continue;
		}
		/* already standing in a later segment's area (dropped, got pushed): skip ahead */
		{
			int k, here = NBNav_AreaAt(self->v->origin, 64);
			qboolean skipped = false;
			for (k = p->numsegs - 1; k > p->cur && here >= 0; k--)
				if (p->seg[k].area == here && k + 1 < p->numsegs)
				{
					p->cur = k + 1;
					skipped = true;
					break;
				}
			if (skipped)
				continue;
		}
		break;
	}
	if (p->cur >= p->numsegs)
		return false;

	/* look ahead: walk straight to a later point when nothing is in the way */
	look = p->cur;
	while (look + 1 < p->numsegs && look < p->cur + 3)
	{
		nbpathseg_t *n = &p->seg[look + 1];
		if (n->how != NBLINK_WALK || p->seg[look].how != NBLINK_WALK)
			break;
		if (n->area < 0 || p->seg[look].area < 0)
			break;   /* a running jump's run-up point is walked to, not cut */
		if (!NB_WalkableDirect(self, n->pos))
			break;
		look++;
	}
	seg = &p->seg[look];
	NBLoco_SetGoal(bot, seg->pos);

	/* special movement for the segment being entered */
	switch (p->seg[p->cur].how)
	{
	case NBLINK_JUMP:
		VectorSubtract(p->seg[p->cur].pos, self->v->origin, d);
		if (sqrt(d[0] * d[0] + d[1] * d[1]) < 64 && ((int)self->v->flags & FL_ONGROUND))
			bot->wantjump = true;
		break;
	case NBLINK_RUNJUMP:
		/* run straight at the landing spot (the run-up lined us up) and jump where the floor ends.
		   A think is 0.1 s (32 units at run speed), too coarse to find the edge: while this is set,
		   the bot's movement runs in short steps that check for it (NB_RunBotCmd) */
		if ((int)self->v->flags & FL_ONGROUND)
		{
			bot->runjumping = true;
			if (NBLoco_AtJumpEdge(bot))
				bot->wantjump = true;
		}
		break;
	case NBLINK_PLAT:
		{	/* stand on the lift until it has carried us up */
			nbpathseg_t *s = &p->seg[p->cur];
			if (self->v->origin[2] < s->pos[2] + 24 - 20 && p->cur > 0)
			{
				int here = NBNav_AreaAt(self->v->origin, 2000);
				if (here >= 0 && (nb_mesh.areas[here].flags & NBAREA_PLAT))
				{
					VectorCopy(nb_mesh.areas[here].center, bot->movegoal);
					bot->movegoal[2] = self->v->origin[2] - 24;
				}
			}
		}
		break;
	default:
		break;
	}
	if (p->seg[p->cur].how != NBLINK_RUNJUMP)
	{	/* nearing a running jump's run-up point: slow down, so the turns are tight and the run
		   starts straight. At full speed we'd circle the point, or skid off the edge the jump
		   starts from (run-ups are often on ledges) */
		int k;
		for (k = p->cur; k < p->numsegs && k <= p->cur + 3; k++)
			if (p->seg[k].area < 0)
			{
				VectorSubtract(p->seg[k].pos, self->v->origin, d);
				if (sqrt(d[0] * d[0] + d[1] * d[1]) < 160)
					bot->wantslow = true;
				break;
			}
	}
	return true;
}

qboolean NBLoco_PathTo(nbbot_t *bot, const vec3_t goal, float repath)
{
	edict_t *self = NB_Self(bot);
	int team = 0;
	extern int NB_TeamOf(edict_t *ed);
	if (!bot->path.valid || NB_Time() - bot->path.computed > repath || Distance(goal, bot->path.goal) > 64)
	{
		team = NB_TeamOf(self);
		if (!NBPath_Compute(&bot->path, self->v->origin, goal, team, NULL, NULL))
		{
			NBIntention_OnPathFailed(bot);
			return false;
		}
	}
	return NBLoco_FollowPath(bot);
}

/* stuck detection: called each think */
void NBLoco_UpdateStuck(nbbot_t *bot)
{
	edict_t *self = NB_Self(bot);
	float now = NB_Time();
	if (!bot->hasmovegoal || !NB_IsAlive(self)
		|| (fabs(bot->movegoal[0] - self->v->origin[0]) < 40 && fabs(bot->movegoal[1] - self->v->origin[1]) < 40))
	{	/* no goal, or holding position on purpose: not stuck */
		bot->stucktime = 0;
		VectorCopy(self->v->origin, bot->stuckpos);
		bot->laststuckcheck = now;
		return;
	}
	if (now - bot->laststuckcheck < 0.5f)
		return;
	if (Distance(self->v->origin, bot->stuckpos) < 24)
		bot->stucktime += now - bot->laststuckcheck;
	else
	{
		bot->stucktime = 0;
		VectorCopy(self->v->origin, bot->stuckpos);
	}
	bot->laststuckcheck = now;
	if (bot->stucktime > 1.0f && ((int)self->v->flags & FL_ONGROUND))
		bot->wantjump = true;   /* hop over small obstacles / other players */
	if (bot->stucktime > 3.0f)
	{
		bot->stucktime = 0;
		bot->stuckevents++;
		VectorCopy(self->v->origin, bot->stuckpos);
		bot->path.valid = false;
		if (qw_bot_debug.ival)
			Con_Printf("[nb] %s stuck at %.0f %.0f %.0f\n", bot->cl->name, self->v->origin[0], self->v->origin[1], self->v->origin[2]);
		NBIntention_OnStuck(bot);
	}
}

/* fill a usercmd from the bot's goals */
void NBLoco_BuildCmd(nbbot_t *bot, usercmd_t *cmd, float dt)
{
	edict_t *self = NB_Self(bot);
	vec3_t dir, fwd, right;
	float speed = 400;
	qboolean slow = bot->wantslow;

	bot->wantslow = false;
	memset(cmd, 0, sizeof(*cmd));
	NBBody_Update(bot, dt);
	cmd->angles[0] = ANGLE2SHORT(bot->viewangles[0]);
	cmd->angles[1] = ANGLE2SHORT(bot->viewangles[1]);
	cmd->angles[2] = 0;

	if (bot->hasmovegoal && NB_IsAlive(self))
	{
		vec3_t flat;
		VectorSubtract(bot->movegoal, self->v->origin, dir);
		VectorSet(flat, dir[0], dir[1], 0);
		if (!((int)self->v->flags & FL_ONGROUND) && bot->path.valid && bot->path.cur < bot->path.numsegs
			&& bot->path.seg[bot->path.cur].how == NBLINK_RUNJUMP)
		{	/* in the air on a running jump: steer at the landing spot */
			vec3_t target, wish;
			VectorCopy(bot->path.seg[bot->path.cur].pos, target);
			NB_AirSteer(self->v->origin, self->v->velocity, target, wish);
			VectorScale(wish, 100, flat);
		}
		else if (((int)self->v->flags & FL_ONGROUND) && VectorLength(flat) > 4 && (slow || (bot->path.valid
			&& bot->path.cur < bot->path.numsegs && bot->path.seg[bot->path.cur].how == NBLINK_RUNJUMP)))
		{	/* lining up a running jump: control the velocity, not just the heading (heading for the
			   goal leaves sideways momentum to bleed off by friction alone, which carries a bot off
			   ledges and spoils the run's line). Ask for the velocity change that's needed: in one
			   0.1 s step, pmove's acceleration delivers about the wish speed */
			vec3_t err;
			float e;
			VectorNormalize(flat);
			VectorScale(flat, slow ? 100 : NB_RUNSPEED, err);
			err[0] -= self->v->velocity[0];
			err[1] -= self->v->velocity[1];
			e = VectorNormalize(err);
			if (e > 30)
			{
				speed = bound(30, e + DotProduct(self->v->velocity, err), 400);
				VectorCopy(err, flat);
			}
			else if (slow)
				speed = 100;
			VectorScale(flat, 100, flat);   /* (a direction: the length test below wants more than a unit) */
		}
		if (VectorLength(flat) > 4)
		{
			vec3_t yawonly;
			VectorNormalize(flat);
			VectorSet(yawonly, 0, bot->viewangles[YAW], 0);
			AngleVectors(yawonly, fwd, right, NULL);
			cmd->forwardmove = DotProduct(flat, fwd) * speed;
			cmd->sidemove = DotProduct(flat, right) * speed;
		}
		/* swimming: go up toward a higher goal, or for air */
		if (self->v->waterlevel >= 2)
		{
			if (dir[2] > 16 || self->v->waterlevel >= 3)
				cmd->upmove = 400;
			else if (dir[2] < -32)
				cmd->upmove = -400;
		}
	}
	if (bot->wantjump)
	{
		cmd->buttons |= 2;
		bot->wantjump = false;
	}
	cmd->buttons |= bot->buttons;
	cmd->impulse = bot->impulse;
	bot->buttons = 0;
	bot->impulse = 0;
	cmd->msec = bound(1, (int)(dt * 1000), 250);
}

#endif

#ifndef CLIENTONLY
/* ------------------------------------------------------------------------------------------------
 * vision: pick the enemy to fight
 */
extern int NB_TeamOf(edict_t *ed);
qboolean NB_IsEnemy(nbbot_t *bot, edict_t *other);

void NBVision_Update(nbbot_t *bot)
{
	edict_t *self = NB_Self(bot);
	edict_t *best = NULL;
	float bestscore = 1e9, now = NB_Time();
	vec3_t fwd, yawonly;
	int i;

	if (now - bot->lastthink < 0.1f && bot->enemy)
		return;   /* 10 Hz */
	bot->lastthink = now;
	VectorSet(yawonly, 0, bot->viewangles[YAW], 0);
	AngleVectors(yawonly, fwd, NULL, NULL);

	for (i = 1; i < sv.world.num_edicts; i++)
	{
		edict_t *e = EDICT_NUM_PB(svprogfuncs, i);
		vec3_t d;
		float dist, score;
		if (ED_ISFREE(e) || e == self || e->v->takedamage == DAMAGE_NO || e->v->health <= 0)
			continue;
		if (!NB_IsEnemy(bot, e))
			continue;
		VectorSubtract(e->v->origin, self->v->origin, d);
		dist = VectorLength(d);
		if (dist > 3000)
			continue;
		/* field of view, unless it is the current enemy or it just hurt us */
		if (e != bot->enemy && !(e == bot->lastattacker && now - bot->lastpain < 2))
		{
			vec3_t n;
			VectorScale(d, 1.0f / max(dist, 1), n);
			if (DotProduct(n, fwd) < -0.2f && dist > 200)
				continue;
		}
		if (!NBVision_CanSee(bot, e))
			continue;
		score = dist;
		if (e == bot->enemy)
			score *= 0.6f;    /* stick with the current target */
		if (e->v->health < 50)
			score *= 0.8f;
		if (score < bestscore)
		{
			bestscore = score;
			best = e;
		}
	}
	if (best)
	{
		if (best != bot->enemy)
		{
			bot->enemyfirstseen = now;
			NBIntention_OnSight(bot, best);
		}
		bot->enemy = best;
		bot->enemyseen = now;
		VectorCopy(best->v->origin, bot->enemylastpos);
	}
	else if (bot->enemy && (now - bot->enemyseen > 4 || bot->enemy->v->health <= 0 || ED_ISFREE(bot->enemy)))
		bot->enemy = NULL;
}
#endif
