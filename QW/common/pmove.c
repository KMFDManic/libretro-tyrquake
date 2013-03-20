/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "pmove.h"

#ifdef SERVERONLY
#include "qwsvdef.h"
#else
#include "quakedef.h"
#endif

movevars_t movevars;
playermove_t pmove;

int onground;
int waterlevel;
int watertype;

static float frametime;

vec3_t player_mins = { -16, -16, -24 };
vec3_t player_maxs = { 16, 16, 32 };

// #define      PM_GRAVITY                      800
// #define      PM_STOPSPEED            100
// #define      PM_MAXSPEED                     320
// #define      PM_SPECTATORMAXSPEED    500
// #define      PM_ACCELERATE           10
// #define      PM_AIRACCELERATE        0.7
// #define      PM_WATERACCELERATE      10
// #define      PM_FRICTION                     6
// #define      PM_WATERFRICTION        1

#define	STEPSIZE	18
#define	BUTTON_JUMP	2


/*
==================
PM_ClipVelocity

Slide off of the impacting object
returns the blocked flags (1 = floor, 2 = step / wall)
==================
*/
#define	STOP_EPSILON	0.1

static int
PM_ClipVelocity(vec3_t in, vec3_t normal, vec3_t out, float overbounce)
{
    float backoff;
    float change;
    int i, blocked;

    blocked = 0;
    if (normal[2] > 0)
	blocked |= 1;		// floor
    if (!normal[2])
	blocked |= 2;		// step

    backoff = DotProduct(in, normal) * overbounce;

    for (i = 0; i < 3; i++) {
	change = normal[i] * backoff;
	out[i] = in[i] - change;
	if (out[i] > -STOP_EPSILON && out[i] < STOP_EPSILON)
	    out[i] = 0;
    }

    return blocked;
}


/*
============
PM_FlyMove

The basic solid body movement clip that slides along multiple planes
============
*/
#define MOVE_CLIP_NONE  0
#define MOVE_CLIP_FLOOR (1 << 0)
#define MOVE_CLIP_WALL  (1 << 1)
#define MOVE_CLIP_STOP  (1 << 2)

#define	MAX_CLIP_PLANES	5

static int
PM_FlyMove(void)
{
    int bumpcount, numbumps;
    vec3_t dir;
    float d;
    int numplanes;
    vec3_t planes[MAX_CLIP_PLANES];
    vec3_t primal_velocity, original_velocity;
    int i, j;
    trace_t trace;
    vec3_t end;
    float time_left;
    int blocked;
    int touchentity;

    numbumps = 4;

    blocked = MOVE_CLIP_NONE;
    VectorCopy(pmove.velocity, original_velocity);
    VectorCopy(pmove.velocity, primal_velocity);
    numplanes = 0;

    time_left = frametime;

    for (bumpcount = 0; bumpcount < numbumps; bumpcount++) {
	for (i = 0; i < 3; i++)
	    end[i] = pmove.origin[i] + time_left * pmove.velocity[i];

	touchentity = PM_PlayerMove(pmove.origin, end, &trace);
	if (trace.startsolid || trace.allsolid) {
	    /* entity is trapped in another solid */
	    VectorCopy(vec3_origin, pmove.velocity);
	    return MOVE_CLIP_FLOOR | MOVE_CLIP_WALL;
	}

	if (trace.fraction > 0) {
	    /* actually covered some distance */
	    VectorCopy(trace.endpos, pmove.origin);
	    numplanes = 0;
	}

	/* moved the entire distance */
	if (trace.fraction == 1)
	    break;

	/* save entity for contact */
	pmove.touchindex[pmove.numtouch] = touchentity;
	pmove.numtouch++;

	if (trace.plane.normal[2] > 0.7)
	    blocked |= MOVE_CLIP_FLOOR;
	if (!trace.plane.normal[2])
	    blocked |= MOVE_CLIP_WALL;

	time_left -= time_left * trace.fraction;

	/* cliped to another plane */
	if (numplanes >= MAX_CLIP_PLANES) {
	    /* this shouldn't really happen */
	    VectorCopy(vec3_origin, pmove.velocity);
	    break;
	}
	VectorCopy(trace.plane.normal, planes[numplanes]);
	numplanes++;

	/*
	 * modify original_velocity so it parallels all of the clip planes
	 */
	for (i = 0; i < numplanes; i++) {
	    PM_ClipVelocity(original_velocity, planes[i], pmove.velocity, 1);
	    for (j = 0; j < numplanes; j++)
		if (j != i) {
		    if (DotProduct(pmove.velocity, planes[j]) < 0)
			break;	/* not ok */
		}
	    if (j == numplanes)
		break;
	}

	if (i != numplanes) {
	    /* go along this plane */
	    /* ??? */
	} else {
	    /* go along the crease */
	    if (numplanes != 2) {
		VectorCopy(vec3_origin, pmove.velocity);
		break;
	    }
	    CrossProduct(planes[0], planes[1], dir);
	    d = DotProduct(dir, pmove.velocity);
	    VectorScale(dir, d, pmove.velocity);
	}

	/*
	 * If velocity is against the original velocity, stop dead
	 * to avoid tiny occilations in sloping corners
	 */
	if (DotProduct(pmove.velocity, primal_velocity) <= 0) {
	    VectorCopy(vec3_origin, pmove.velocity);
	    break;
	}
    }

    if (pmove.waterjumptime)
	VectorCopy(primal_velocity, pmove.velocity);

    return blocked;
}

/*
=============
PM_GroundMove

Player is on ground, with no upwards velocity
=============
*/
static void
PM_GroundMove(void)
{
    vec3_t dest;
    trace_t trace;
    vec3_t original, originalvel, down, up, downvel;
    float downdist, updist;

    pmove.velocity[2] = 0;
    if (!pmove.velocity[0] && !pmove.velocity[1] && !pmove.velocity[2])
	return;

    // first try just moving to the destination
    dest[0] = pmove.origin[0] + pmove.velocity[0] * frametime;
    dest[1] = pmove.origin[1] + pmove.velocity[1] * frametime;
    dest[2] = pmove.origin[2];

    // first try moving directly to the next spot
    PM_PlayerMove(pmove.origin, dest, &trace);
    if (trace.fraction == 1) {
	VectorCopy(trace.endpos, pmove.origin);
	return;
    }
    // try sliding forward both on ground and up 16 pixels
    // take the move that goes farthest
    VectorCopy(pmove.origin, original);
    VectorCopy(pmove.velocity, originalvel);

    // slide move
    PM_FlyMove();

    VectorCopy(pmove.origin, down);
    VectorCopy(pmove.velocity, downvel);

    VectorCopy(original, pmove.origin);
    VectorCopy(originalvel, pmove.velocity);

// move up a stair height
    VectorCopy(pmove.origin, dest);
    dest[2] += STEPSIZE;
    PM_PlayerMove(pmove.origin, dest, &trace);
    if (!trace.startsolid && !trace.allsolid) {
	VectorCopy(trace.endpos, pmove.origin);
    }
// slide move
    PM_FlyMove();

// press down the stepheight
    VectorCopy(pmove.origin, dest);
    dest[2] -= STEPSIZE;
    PM_PlayerMove(pmove.origin, dest, &trace);
    if (trace.plane.normal[2] < 0.7)
	goto usedown;
    if (!trace.startsolid && !trace.allsolid) {
	VectorCopy(trace.endpos, pmove.origin);
    }
    VectorCopy(pmove.origin, up);

    // decide which one went farther
    downdist = (down[0] - original[0]) * (down[0] - original[0])
	+ (down[1] - original[1]) * (down[1] - original[1]);
    updist = (up[0] - original[0]) * (up[0] - original[0])
	+ (up[1] - original[1]) * (up[1] - original[1]);

    if (downdist > updist) {
      usedown:
	VectorCopy(down, pmove.origin);
	VectorCopy(downvel, pmove.velocity);
    } else			// copy z value from slide move
	pmove.velocity[2] = downvel[2];

// if at a dead stop, retry the move with nudges to get around lips

}



/*
==================
PM_Friction

Handles both ground friction and water friction
==================
*/
static void
PM_Friction(void)
{
    float *vel;
    float speed, newspeed, control;
    float friction;
    float drop;
    vec3_t start, stop;
    trace_t trace;

    if (pmove.waterjumptime)
	return;

    vel = pmove.velocity;

    speed = sqrt(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
    if (speed < 1) {
	vel[0] = 0;
	vel[1] = 0;
	return;
    }

    friction = movevars.friction;

    /* if the leading edge is over a dropoff, increase friction */
    if (onground != -1) {
	start[0] = stop[0] = pmove.origin[0] + vel[0] / speed * 16;
	start[1] = stop[1] = pmove.origin[1] + vel[1] / speed * 16;
	start[2] = pmove.origin[2] + player_mins[2];
	stop[2] = start[2] - 34;

	PM_PlayerMove(start, stop, &trace);

	if (trace.fraction == 1) {
	    friction *= 2;
	}
    }

    drop = 0;

    if (waterlevel >= 2) {
	/* apply water friction */
	drop += speed * movevars.waterfriction * waterlevel * frametime;
    } else if (onground != -1) {
	/* apply ground friction */
	control = speed < movevars.stopspeed ? movevars.stopspeed : speed;
	drop += control * friction * frametime;
    }

    /* scale the velocity */
    newspeed = speed - drop;
    if (newspeed < 0)
	newspeed = 0;
    newspeed /= speed;

    vel[0] = vel[0] * newspeed;
    vel[1] = vel[1] * newspeed;
    vel[2] = vel[2] * newspeed;
}


/*
==============
PM_Accelerate
==============
*/
static void
PM_Accelerate(vec3_t wishdir, float wishspeed, float accel)
{
    int i;
    float addspeed, accelspeed, currentspeed;

    if (pmove.dead)
	return;
    if (pmove.waterjumptime)
	return;

    currentspeed = DotProduct(pmove.velocity, wishdir);
    addspeed = wishspeed - currentspeed;
    if (addspeed <= 0)
	return;
    accelspeed = accel * frametime * wishspeed;
    if (accelspeed > addspeed)
	accelspeed = addspeed;

    for (i = 0; i < 3; i++)
	pmove.velocity[i] += accelspeed * wishdir[i];
}

static void
PM_AirAccelerate(vec3_t wishdir, float wishspeed, float accel)
{
    int i;
    float addspeed, accelspeed, currentspeed, wishspd = wishspeed;

    if (pmove.dead)
	return;
    if (pmove.waterjumptime)
	return;

    if (wishspd > 30)
	wishspd = 30;
    currentspeed = DotProduct(pmove.velocity, wishdir);
    addspeed = wishspd - currentspeed;
    if (addspeed <= 0)
	return;
    accelspeed = accel * wishspeed * frametime;
    if (accelspeed > addspeed)
	accelspeed = addspeed;

    for (i = 0; i < 3; i++)
	pmove.velocity[i] += accelspeed * wishdir[i];
}



/*
===================
PM_WaterMove

===================
*/
static void
PM_WaterMove(void)
{
    vec3_t wishvel, wishdir, start, dest;
    vec3_t forward, right, up;
    vec_t wishspeed;
    trace_t trace;

    /*
     * user intentions
     */
    AngleVectors(pmove.angles, forward, right, up);
    VectorScale(forward, pmove.cmd.forwardmove, forward);
    VectorScale(right, pmove.cmd.sidemove, right);
    VectorAdd(forward, right, wishvel);

    if (!pmove.cmd.forwardmove && !pmove.cmd.sidemove && !pmove.cmd.upmove)
	wishvel[2] -= 60;	/* drift towards bottom */
    else
	wishvel[2] += pmove.cmd.upmove;

    VectorCopy(wishvel, wishdir);
    wishspeed = VectorNormalize(wishdir);

    if (wishspeed > movevars.maxspeed) {
	VectorScale(wishvel, movevars.maxspeed / wishspeed, wishvel);
	wishspeed = movevars.maxspeed;
    }
    wishspeed *= 0.7;

    /*
     * water acceleration
     */
    PM_Accelerate(wishdir, wishspeed, movevars.wateraccelerate);

    /*
     * assume it is a stair or a slope, so press down from stepheight above
     */
    VectorMA(pmove.origin, frametime, pmove.velocity, dest);
    VectorCopy(dest, start);
    start[2] += STEPSIZE + 1;
    PM_PlayerMove(start, dest, &trace);
    if (!trace.startsolid && !trace.allsolid) {
	/* walked up the step */
	/* FIXME: check steep slope? */
	VectorCopy(trace.endpos, pmove.origin);
	return;
    }

    PM_FlyMove();
}


/*
===================
PM_AirMove

===================
*/
static void
PM_AirMove(void)
{
    int i;
    vec3_t wishvel, wishdir;
    vec3_t forward, right, up;
    vec_t wishspeed;
    const vec_t fmove = pmove.cmd.forwardmove;
    const vec_t smove = pmove.cmd.sidemove;

    AngleVectors(pmove.angles, forward, right, up);

    forward[2] = 0;
    right[2] = 0;
    VectorNormalize(forward);
    VectorNormalize(right);

    for (i = 0; i < 2; i++)
	wishvel[i] = forward[i] * fmove + right[i] * smove;
    wishvel[2] = 0;

    VectorCopy(wishvel, wishdir);
    wishspeed = VectorNormalize(wishdir);

    /*
     * clamp to server defined max speed
     */
    if (wishspeed > movevars.maxspeed) {
	VectorScale(wishvel, movevars.maxspeed / wishspeed, wishvel);
	wishspeed = movevars.maxspeed;
    }

    if (onground != -1) {
	pmove.velocity[2] = 0;
	PM_Accelerate(wishdir, wishspeed, movevars.accelerate);
	pmove.velocity[2] -=
	    movevars.entgravity * movevars.gravity * frametime;
	PM_GroundMove();
    } else {
	/* not on ground, so little effect on velocity */
	PM_AirAccelerate(wishdir, wishspeed, movevars.accelerate);

	/* add gravity */
	pmove.velocity[2] -=
	    movevars.entgravity * movevars.gravity * frametime;

	PM_FlyMove();
    }
}



/*
=============
PM_CatagorizePosition
=============
*/
static void
PM_CatagorizePosition(void)
{
    vec3_t point;
    int cont;
    trace_t trace;
    int groundentity;

// if the player hull point one unit down is solid, the player
// is on ground

// see if standing on something solid
    point[0] = pmove.origin[0];
    point[1] = pmove.origin[1];
    point[2] = pmove.origin[2] - 1;
    if (pmove.velocity[2] > 180) {
	onground = -1;
    } else {
	groundentity = PM_PlayerMove(pmove.origin, point, &trace);
	if (trace.plane.normal[2] < 0.7)
	    onground = -1;	// too steep
	else
	    onground = groundentity;
	if (onground != -1) {
	    pmove.waterjumptime = 0;
	    if (!trace.startsolid && !trace.allsolid)
		VectorCopy(trace.endpos, pmove.origin);
	}
	// standing on an entity other than the world
	if (groundentity > 0) {
	    pmove.touchindex[pmove.numtouch] = groundentity;
	    pmove.numtouch++;
	}
    }

//
// get waterlevel
//
    waterlevel = 0;
    watertype = CONTENTS_EMPTY;

    point[2] = pmove.origin[2] + player_mins[2] + 1;
    cont = PM_PointContents(point);

    if (cont <= CONTENTS_WATER) {
	watertype = cont;
	waterlevel = 1;
	point[2] = pmove.origin[2] + (player_mins[2] + player_maxs[2]) * 0.5;
	cont = PM_PointContents(point);
	if (cont <= CONTENTS_WATER) {
	    waterlevel = 2;
	    point[2] = pmove.origin[2] + 22;
	    cont = PM_PointContents(point);
	    if (cont <= CONTENTS_WATER)
		waterlevel = 3;
	}
    }
}


/*
=============
JumpButton
=============
*/
static void
JumpButton(void)
{
    if (pmove.dead) {
	/* don't jump again until released */
	pmove.oldbuttons |= BUTTON_JUMP;
	return;
    }

    if (pmove.waterjumptime) {
	pmove.waterjumptime -= frametime;
	if (pmove.waterjumptime < 0)
	    pmove.waterjumptime = 0;
	return;
    }

    if (waterlevel >= 2) {
	/* swimming, not jumping */
	onground = -1;
	if (watertype == CONTENTS_WATER)
	    pmove.velocity[2] = 100;
	else if (watertype == CONTENTS_SLIME)
	    pmove.velocity[2] = 80;
	else
	    pmove.velocity[2] = 50;
	return;
    }

    /* no effect if in air */
    if (onground == -1)
	return;

    /* don't pogo stick */
    if (pmove.oldbuttons & BUTTON_JUMP)
	return;

    onground = -1;
    pmove.velocity[2] += 270;

    /* don't jump again until released */
    pmove.oldbuttons |= BUTTON_JUMP;
}

/*
=============
CheckWaterJump
=============
*/
static void
CheckWaterJump(void)
{
    vec3_t spot;
    int cont;
    vec3_t forward, right, up;

    if (pmove.waterjumptime)
	return;

    // ZOID, don't hop out if we just jumped in
    if (pmove.velocity[2] < -180)
	return;			// only hop out if we are moving up

    // see if near an edge
    AngleVectors(pmove.angles, forward, right, up);
    forward[2] = 0;
    VectorNormalize(forward);

    VectorMA(pmove.origin, 24, forward, spot);
    spot[2] += 8;
    cont = PM_PointContents(spot);
    if (cont != CONTENTS_SOLID)
	return;
    spot[2] += 24;
    cont = PM_PointContents(spot);
    if (cont != CONTENTS_EMPTY)
	return;
    // jump out of water
    VectorScale(forward, 50, pmove.velocity);
    pmove.velocity[2] = 310;
    pmove.waterjumptime = 2;	// safety net
    pmove.oldbuttons |= BUTTON_JUMP;	// don't jump again until released
}

/*
=================
NudgePosition

If pmove.origin is in a solid position,
try nudging slightly on all axis to
allow for the cut precision of the net coordinates
=================
*/
static void
NudgePosition(void)
{
    vec3_t base;
    int x, y, z;
    int i;
    static int sign[3] = { 0, -1, 1 };

    VectorCopy(pmove.origin, base);

    for (i = 0; i < 3; i++)
	pmove.origin[i] = ((int)(pmove.origin[i] * 8)) * 0.125;
//      pmove.origin[2] += 0.124;

//      if (pmove.dead)
//              return;         // might be a squished point, so don'y bother
//      if (PM_TestPlayerPosition (pmove.origin) )
//              return;

    for (z = 0; z <= 2; z++) {
	for (x = 0; x <= 2; x++) {
	    for (y = 0; y <= 2; y++) {
		pmove.origin[0] = base[0] + (sign[x] * 1.0 / 8);
		pmove.origin[1] = base[1] + (sign[y] * 1.0 / 8);
		pmove.origin[2] = base[2] + (sign[z] * 1.0 / 8);
		if (PM_TestPlayerPosition(pmove.origin))
		    return;
	    }
	}
    }
    VectorCopy(base, pmove.origin);
//      Con_DPrintf ("NudgePosition: stuck\n");
}

/*
===============
SpectatorMove
===============
*/
static void
SpectatorMove(void)
{
    int i;
    vec_t speed, drop, friction, control, newspeed;
    vec_t currentspeed, addspeed, accelspeed, wishspeed;
    vec3_t wishvel, wishdir;
    vec3_t forward, right, up;

    const vec_t fmove = pmove.cmd.forwardmove;
    const vec_t smove = pmove.cmd.sidemove;

    /* Friction */
    speed = Length(pmove.velocity);
    if (speed < 1) {
	VectorCopy(vec3_origin, pmove.velocity);
    } else {
	drop = 0;

	friction = movevars.friction * 1.5;	/* extra friction */
	control = speed < movevars.stopspeed ? movevars.stopspeed : speed;
	drop += control * friction * frametime;

	/* scale the velocity */
	newspeed = speed - drop;
	if (newspeed < 0)
	    newspeed = 0;
	newspeed /= speed;

	VectorScale(pmove.velocity, newspeed, pmove.velocity);
    }

    /* Acceleration */
    AngleVectors(pmove.angles, forward, right, up);
    VectorNormalize(forward);
    VectorNormalize(right);

    for (i = 0; i < 3; i++)
	wishvel[i] = forward[i] * fmove + right[i] * smove;
    wishvel[2] += pmove.cmd.upmove;

    VectorCopy(wishvel, wishdir);
    wishspeed = VectorNormalize(wishdir);

    /*
     * clamp to server defined max speed
     */
    if (wishspeed > movevars.spectatormaxspeed) {
	VectorScale(wishvel, movevars.spectatormaxspeed / wishspeed, wishvel);
	wishspeed = movevars.spectatormaxspeed;
    }

    currentspeed = DotProduct(pmove.velocity, wishdir);
    addspeed = wishspeed - currentspeed;
    if (addspeed <= 0)
	return;
    accelspeed = movevars.accelerate * frametime * wishspeed;
    if (accelspeed > addspeed)
	accelspeed = addspeed;

    VectorMA(pmove.velocity, accelspeed, wishdir, pmove.velocity);

    /* Move */
    VectorMA(pmove.origin, frametime, pmove.velocity, pmove.origin);
}

/*
=============
PlayerMove

Returns with origin, angles, and velocity modified in place.

Numtouch and touchindex[] will be set if any of the physents
were contacted during the move.
=============
*/
void
PlayerMove(void)
{
    frametime = pmove.cmd.msec * 0.001;
    pmove.numtouch = 0;

    if (pmove.spectator) {
	SpectatorMove();
	return;
    }

    NudgePosition();

    /* take angles directly from command */
    VectorCopy(pmove.cmd.angles, pmove.angles);

    /* set onground, watertype, and waterlevel */
    PM_CatagorizePosition();

    if (waterlevel == 2)
	CheckWaterJump();

    if (pmove.velocity[2] < 0)
	pmove.waterjumptime = 0;

    if (pmove.cmd.buttons & BUTTON_JUMP)
	JumpButton();
    else
	pmove.oldbuttons &= ~BUTTON_JUMP;

    PM_Friction();

    if (waterlevel >= 2)
	PM_WaterMove();
    else
	PM_AirMove();

    /* set onground, watertype, and waterlevel for final spot */
    PM_CatagorizePosition();
}
