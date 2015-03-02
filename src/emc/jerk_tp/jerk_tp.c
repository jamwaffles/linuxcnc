/********************************************************************
* Description: tp.c
*   Trajectory planner based on TC elements
*
*   Derived from a work by Fred Proctor & Will Shackleford
*
* Author:
* License: GPL Version 2
* System: Linux
*    
* Copyright (c) 2004 All rights reserved.
********************************************************************/

#include "rtapi.h"		/* rtapi_print_msg */
#include "rtapi_string.h"       /* NULL */
#include "posemath.h"
#include "jerk_tc.h"
#include "jerk_tp.h"
#include "rtapi_math.h"
#include "../motion/motion.h"
#include "hal.h"
#include "../motion/mot_priv.h"
#include "motion_debug.h"
#include "motion_types.h"

extern emcmot_status_t *emcmotStatus;
extern emcmot_debug_t *emcmotDebug;

#define SMLBLND

int output_chan = 0;
syncdio_t syncdio; //record tpSetDout's here

int tpCreate(TP_STRUCT * const tp, int _queueSize, TC_STRUCT * const tcSpace)
{
    if (0 == tp) {
	return -1;
    }

    if (_queueSize <= 0) {
	tp->queueSize = TP_DEFAULT_QUEUE_SIZE;
    } else {
	tp->queueSize = _queueSize;
    }

    /* create the queue */
    if (-1 == tcqCreate(&tp->queue, tp->queueSize, tcSpace)) {
	return -1;
    }

    /* init the rest of our data */
    return tpInit(tp);
}


// this clears any potential DIO toggles
// anychanged signals if any DIOs need to be changed
// dios[i] = 1, DIO needs to get turned on, -1 = off
int tpClearDIOs() {
    //XXX: All IO's will be flushed on next synced aio/dio! Is it ok?
    int i;
    syncdio.anychanged = 0;
    syncdio.dio_mask = 0;
    syncdio.aio_mask = 0;
    for (i = 0; i < num_dio; i++)
	syncdio.dios[i] = 0;
    for (i = 0; i < num_aio; i++)
	syncdio.aios[i] = 0;

    return 0;
}


/*
  tpClear() is a "soft init" in the sense that the TP_STRUCT configuration
  parameters (cycleTime, vMax, and aMax) are left alone, but the queue is
  cleared, and the flags are set to an empty, ready queue. The currentPos
  is left alone, and goalPos is set to this position.

  This function is intended to put the motion queue in the state it would
  be if all queued motions finished at the current position.
 */
int tpClear(TP_STRUCT * tp)
{
    tcqInit(&tp->queue);
    tp->queueSize = 0;
    tp->goalPos = tp->currentPos;
    tp->nextId = 0;
    tp->execId = 0;
    tp->motionType = 0;
    tp->termCond = TC_TERM_COND_BLEND;
    tp->tolerance = 0.0;
    tp->done = 1;
    tp->depth = tp->activeDepth = 0;
    tp->aborting = 0;
    tp->pausing = 0;
    tp->synchronized = 0;
    tp->velocity_mode = 0;
    tp->uu_per_rev = 0.0;
    emcmotStatus->spindleSync = 0;
    emcmotStatus->current_vel = 0.0;
    emcmotStatus->requested_vel = 0.0;
    emcmotStatus->distance_to_go = 0.0;
    ZERO_EMC_POSE(emcmotStatus->dtg);

    return tpClearDIOs();
}


int tpInit(TP_STRUCT * tp)
{
    tp->cycleTime = 0.0;
    tp->vLimit = 0.0;
    tp->aMax = 0.0;
    tp->vMax = 0.0;
    tp->ini_maxvel = 0.0;
    tp->wMax = 0.0;
    tp->wDotMax = 0.0;

    ZERO_EMC_POSE(tp->currentPos);
    
    return tpClear(tp);
}

int tpSetCycleTime(TP_STRUCT * tp, double secs)
{
    if (0 == tp || secs <= 0.0) {
	return -1;
    }

    tp->cycleTime = secs;

    return 0;
}

// This is called before adding lines or circles, specifying
// vMax (the velocity requested by the F word) and
// ini_maxvel, the max velocity possible before meeting
// a machine constraint caused by an AXIS's max velocity.
// (the TP is allowed to go up to this high when feed 
// override >100% is requested)  These settings apply to
// subsequent moves until changed.

int tpSetVmax(TP_STRUCT * tp, double vMax, double ini_maxvel)
{
    if (0 == tp || vMax <= 0.0 || ini_maxvel <= 0.0) {
	return -1;
    }

    tp->vMax = vMax;
    tp->ini_maxvel = ini_maxvel;

    return 0;
}

// I think this is the [TRAJ] max velocity.  This should
// be the max velocity of the TOOL TIP, not necessarily
// any particular axis.  This applies to subsequent moves
// until changed.

int tpSetVlimit(TP_STRUCT * tp, double vLimit)
{
    if (!tp) return -1;

    if (vLimit < 0.) 
        tp->vLimit = 0.;
    else
        tp->vLimit = vLimit;

    return 0;
}

// Set max accel

int tpSetAmax(TP_STRUCT * tp, double aMax)
{
    if (0 == tp || aMax <= 0.0) {
	return -1;
    }

    tp->aMax = aMax;

    return 0;
}

/*
  tpSetId() sets the id that will be used for the next appended motions.
  nextId is incremented so that the next time a motion is appended its id
  will be one more than the previous one, modulo a signed int. If
  you want your own ids for each motion, call this before each motion
  you append and stick what you want in here.
  */
int tpSetId(TP_STRUCT * tp, int id)
{

    if (!MOTION_ID_VALID(id)) {
	rtapi_print_msg(RTAPI_MSG_ERR, "tpSetId: invalid motion id %d\n", id);
	return -1;
    }
	
    if (0 == tp) {
	return -1;
    }

    tp->nextId = id;

    return 0;
}

/*
  tpGetExecId() returns the id of the last motion that is currently
  executing.
  */
int tpGetExecId(TP_STRUCT * tp)
{
    if (0 == tp) {
	return -1;
    }

    return tp->execId;
}

/*
  tpSetTermCond(tp, cond) sets the termination condition for all subsequent
  queued moves. If cond is TC_TERM_STOP, motion comes to a stop before
  a subsequent move begins. If cond is TC_TERM_BLEND, the following move
  is begun when the current move decelerates.
  */
int tpSetTermCond(TP_STRUCT * tp, int cond, double tolerance)
{
    if (0 == tp) {
	return -1;
    }

    if (cond != TC_TERM_COND_STOP && cond != TC_TERM_COND_BLEND) {
	return -1;
    }

    tp->termCond = cond;
    tp->tolerance = tolerance;
    rtapi_print("TERM %d %f\n", cond, tolerance);

    return 0;
}

// Used to tell the tp the initial position.  It sets
// the current position AND the goal position to be the same.  
// Used only at TP initialization and when switching modes.

int tpSetPos(TP_STRUCT * tp, EmcPose * pos)
{
    if (0 == tp) {
	return -1;
    }

    tp->currentPos = *pos;
    tp->goalPos = *pos;

    return 0;
}

void prepareLimitedJerk(TC_STRUCT *tc) {
    double cycles_per_sec = 1.0/tc->cycle_time;
	double j = tc->jerk*cycles_per_sec*cycles_per_sec*cycles_per_sec;
	double a = tc->maxaccel*cycles_per_sec*cycles_per_sec;
	double v = tc->reqvel;
    double x = tc->target;
    double tj, ta, tv, tx, maxa, maxv1, maxv2, nj = j;

	rtapi_print("ID %d TYPE %d\n", tc->id, tc->motion_type);
	rtapi_print("cmd vel [unit/s] = %.4f | [unit/min] = %.4f\n", v, v*60.0);
	rtapi_print("cmd acc [unit/s^2] = %.4f\n", a);
	rtapi_print("cmd jerk [unit/s^3] = %.4f\n", j);
	rtapi_print("feed override = %.4f\n", tc->feed_override);
	rtapi_print("scaled cmd vel [unit/s] = %.4f | [unit/min] = %.4f\n", v*tc->feed_override, v*tc->feed_override*60.0);
	rtapi_print("target [unit] = %.4f\n", tc->target);
	v *= tc->feed_override;

    // maxa and maxv are 0
    tx = 4.0*pow(x/(2.0*nj), 1.0/3.0);
    tj = tx/4.0;
    //tj = ceil(tj*cycles_per_sec)/cycles_per_sec;

    rtapi_print("1: tj = %.12f\n", tj);
    // calc max vel with current tj
    maxv1 = nj*tj*tj;
    rtapi_print("2: maxv1 = %.12f\n", maxv1);
    // if its bigger than maxv recalcualte it from maxv
    if (maxv1 > v) {
		tj = sqrt(v/nj);
		rtapi_print("  tj = %.12f\n", tj);
		//tj = ceil(tj*cycles_per_sec)/cycles_per_sec;
		//rtapi_print("  ceil(tj) = %.12f\n", tj);
		nj = v/(tj*tj);
		rtapi_print("  j = %.12f\n", nj);
	}
	
	maxa = nj*tj;
	rtapi_print("3: maxa = %.12f\n", maxa);
	if (maxa > a) {
		tj = a/nj;
		rtapi_print("  tj = %.12f\n", tj);
		//tj = ceil(tj*cycles_per_sec)/cycles_per_sec;
		//rtapi_print("  ceil(tj) = %.12f\n", tj);
		nj = a/tj;
		rtapi_print("  j = %.12f\n", nj);
	}
	
	if (x > (2.0*nj*tj*tj*tj)) {
		ta = -1.5*tj + 0.5*sqrt(tj*tj + (4.0*x)/(nj*tj));
		rtapi_print("  ta = %.12f\n", ta);
		//ta = ceil(ta*cycles_per_sec)/cycles_per_sec;
		//rtapi_print("  ceil(ta) = %.12f\n", ta);
	} else {
		ta = 0.0;
		rtapi_print("  ta = %.12f\n", ta);
	}
	
	maxv2 = nj*tj*tj + nj*tj*ta;
	rtapi_print("4: maxv2 = %.12f\n", maxv2);
	if (maxv2 > v) {
		ta = v/a - tj;
		rtapi_print("  ta = %.12f\n", ta);
		//ta = ceil(ta*cycles_per_sec)/cycles_per_sec;
		//rtapi_print("  ceil(ta) = %.12f\n", ta);
	}

	if (ta <= 0.000001) ta = 0.0;

	tv = (x - 2.0*nj*tj*tj*tj - 3.0*nj*tj*tj*ta - nj*tj*ta*ta)/v;
	rtapi_print("5: tv = %.12f\n", tv);
	//tv = ceil(tv*cycles_per_sec)/cycles_per_sec;
	//rtapi_print("6: ceil(tv) = %.12f\n", tv);
	
	if (tv <= 0.000001) tv = 0.0;
	
	double ctj = ceil(tj*cycles_per_sec)/cycles_per_sec;
	double cta = ceil(ta*cycles_per_sec)/cycles_per_sec;
	double ctv = ceil(tv*cycles_per_sec)/cycles_per_sec;
	double est1 = 2.0*nj*tj*tj*tj + 3.0*nj*tj*tj*ta + nj*tj*ta*ta + tv*v;
	double est2 = 2.0*nj*ctj*ctj*ctj + 3.0*nj*ctj*ctj*cta + nj*ctj*cta*cta + ctv*v;
	double est3 = 2.0*nj*ctj*ctj*ctj + 3.0*nj*ctj*ctj*cta + nj*ctj*cta*cta + nj*ctj*ctj*ctv + nj*ctj*cta*ctv;
	double jerk = x/(2.0*ctj*ctj*ctj + 3.0*ctj*ctj*cta + ctj*cta*cta + ctj*ctj*ctv + ctj*cta*ctv);
	double est4 = jerk*(2.0*ctj*ctj*ctj + 3.0*ctj*ctj*cta + ctj*cta*cta + ctj*ctj*ctv + ctj*cta*ctv);
	nj = jerk;

	if (ta <= 0.000001) {
		tc->after_S0 = ACCEL_S2;
		tc->after_S4 = ACCEL_S6;
		tc->s1_cycles = 0;
	} else {
		tc->after_S0 = ACCEL_S1;
		tc->after_S4 = ACCEL_S5;
		tc->s1_cycles = ceil(ta*cycles_per_sec);
	}

	tc->s0_cycles = ceil(tj*cycles_per_sec);
	tc->jerk = nj*tc->cycle_time*tc->cycle_time*tc->cycle_time;
	if (tv > 0.000001) {
		tc->after_S2 = ACCEL_S3;
		tc->s3_cycles = ceil(tv*cycles_per_sec);
	} else {
		tc->after_S2 = ACCEL_S4;
		tc->s3_cycles = 0;
	}
	
    rtapi_print("================================\n");
	rtapi_print("tj = %.12f %.6f %d\n", tj, ctj, tc->s0_cycles);
	rtapi_print("ta = %.12f %.6f %d\n", ta, cta, tc->s1_cycles);
	rtapi_print("tv = %.12f %.6f %d\n", tv, ctv, tc->s3_cycles);
	rtapi_print("jerk = %.12f\n", jerk);
//	rtapi_print("est1 = %.12f\n", est1);
//	rtapi_print("est2 = %.12f\n", est2);
//	rtapi_print("est3 = %.12f\n", est3);
	rtapi_print("est4 = %.12f\n", est4);
    rtapi_print("================================\n");

	tc->cycles = 0;
}

int tpAddRigidTap(TP_STRUCT *tp, EmcPose end, double vel, double ini_maxvel, 
                  double acc, double jerk, unsigned char enables) {
    TC_STRUCT tc = {0};
    PmLine line_xyz;
    PmPose start_xyz, end_xyz;
    PmCartesian abc, uvw;
    PmQuaternion identity_quat = { 1.0, 0.0, 0.0, 0.0 };

    if (!tp) {
        rtapi_print_msg(RTAPI_MSG_ERR, "TP is null\n");
        return -1;
    }
    if (tp->aborting) {
        rtapi_print_msg(RTAPI_MSG_ERR, "TP is aborting\n");
	return -1;
    }

    start_xyz.tran = tp->goalPos.tran;
    end_xyz.tran = end.tran;

    start_xyz.rot = identity_quat;
    end_xyz.rot = identity_quat;
    
    // abc cannot move
    abc.x = tp->goalPos.a;
    abc.y = tp->goalPos.b;
    abc.z = tp->goalPos.c;

    uvw.x = tp->goalPos.u;
    uvw.y = tp->goalPos.v;
    uvw.z = tp->goalPos.w;

    pmLineInit(&line_xyz, &start_xyz, &end_xyz);

    tc.sync_accel = 0;
    tc.cycle_time = tp->cycleTime;
    tc.coords.rigidtap.reversal_target = line_xyz.tmag;

    // allow 10 turns of the spindle to stop - we don't want to just go on forever
    tc.target = line_xyz.tmag + 10. * tp->uu_per_rev;

    tc.progress = 0.0;
    tc.accel_state = ACCEL_S0;
    tc.reqvel = vel;
    tc.maxvel = ini_maxvel * tp->cycleTime;
    tc.maxaccel = acc * tp->cycleTime * tp->cycleTime;
    tc.jerk = jerk * tp->cycleTime * tp->cycleTime * tp->cycleTime;
    tc.feed_override = 0.0;
    tc.id = tp->nextId;
    tc.active = 0;
    tc.done = 0;
    tc.atspeed = 1;

    tc.cur_vel = 0.0;
    tc.cur_accel = 0.0;
    tc.blending = 0;
    tc.seamless_blend_mode = SMLBLND_INIT;

    tc.coords.rigidtap.xyz = line_xyz;
    tc.coords.rigidtap.abc = abc;
    tc.coords.rigidtap.uvw = uvw;
    tc.coords.rigidtap.state = TAPPING;
    tc.motion_type = TC_RIGIDTAP;
    tc.canon_motion_type = 0;
    tc.blend_with_next = 0;
    tc.tolerance = tp->tolerance;

    if(!tp->synchronized) {
        rtapi_print_msg(RTAPI_MSG_ERR, "Cannot add unsynchronized rigid tap move.\n");
        return -1;
    }
    tc.synchronized = tp->synchronized;
    
    tc.uu_per_rev = tp->uu_per_rev;
    tc.velocity_mode = tp->velocity_mode;
    tc.enables = enables;
    tc.indexrotary = -1;

    if (syncdio.anychanged != 0) {
	tc.syncdio = syncdio; //enqueue the list of DIOs that need toggling
	tpClearDIOs(); // clear out the list, in order to prepare for the next time we need to use it
    } else {
	tc.syncdio.anychanged = 0;
    }

    if (tcqPut(&tp->queue, tc) == -1) {
        rtapi_print_msg(RTAPI_MSG_ERR, "tcqPut failed.\n");
	return -1;
    }
    
    // do not change tp->goalPos here,
    // since this move will end just where it started

    tp->done = 0;
    tp->depth = tcqLen(&tp->queue);
    tp->nextId++;

    return 0;
}

// Add a straight line to the tc queue.  This is a coordinated
// move in any or all of the six axes.  it goes from the end
// of the previous move to the new end specified here at the
// currently-active accel and vel settings from the tp struct.

int tpAddLine(TP_STRUCT * tp, EmcPose end, int type, double vel, double ini_maxvel, double acc, double jerk,
              unsigned char enables, char atspeed, int indexrotary)
{
    TC_STRUCT tc = {0};
    PmLine line_xyz, line_uvw, line_abc;
    PmPose start_xyz, end_xyz;
    PmPose start_uvw, end_uvw;
    PmPose start_abc, end_abc;
    PmQuaternion identity_quat = { 1.0, 0.0, 0.0, 0.0 };

    if (!tp) {
        rtapi_print_msg(RTAPI_MSG_ERR, "TP is null\n");
        return -1;
    }
    if (tp->aborting) {
        rtapi_print_msg(RTAPI_MSG_ERR, "TP is aborting\n");
	return -1;
    }

    start_xyz.tran = tp->goalPos.tran;
    end_xyz.tran = end.tran;

    start_uvw.tran.x = tp->goalPos.u;
    start_uvw.tran.y = tp->goalPos.v;
    start_uvw.tran.z = tp->goalPos.w;
    end_uvw.tran.x = end.u;
    end_uvw.tran.y = end.v;
    end_uvw.tran.z = end.w;

    start_abc.tran.x = tp->goalPos.a;
    start_abc.tran.y = tp->goalPos.b;
    start_abc.tran.z = tp->goalPos.c;
    end_abc.tran.x = end.a;
    end_abc.tran.y = end.b;
    end_abc.tran.z = end.c;

    start_xyz.rot = identity_quat;
    end_xyz.rot = identity_quat;
    start_uvw.rot = identity_quat;
    end_uvw.rot = identity_quat;
    start_abc.rot = identity_quat;
    end_abc.rot = identity_quat;

    pmLineInit(&line_xyz, &start_xyz, &end_xyz);
    pmLineInit(&line_uvw, &start_uvw, &end_uvw);
    pmLineInit(&line_abc, &start_abc, &end_abc);

    tc.sync_accel = 0;
    tc.cycle_time = tp->cycleTime;

    if (!line_xyz.tmag_zero) 
        tc.target = line_xyz.tmag;
    else if (!line_uvw.tmag_zero)
        tc.target = line_uvw.tmag;
    else
        tc.target = line_abc.tmag;

    tc.progress = 0.0;
    tc.accel_state = ACCEL_S0;
    tc.reqvel = vel;
    tc.maxvel = ini_maxvel * tp->cycleTime;
    tc.maxaccel = acc * tp->cycleTime * tp->cycleTime;
    tc.jerk = jerk * tp->cycleTime * tp->cycleTime * tp->cycleTime;
    tc.feed_override = 0.0;
    tc.id = tp->nextId;
    tc.active = 0;
    tc.done = 0;
    tc.atspeed = atspeed;

    tc.cur_vel = 0.0;
    tc.cur_accel = 0.0;
    tc.blending = 0;
    tc.seamless_blend_mode = SMLBLND_INIT;

    tc.coords.line.xyz = line_xyz;
    tc.coords.line.uvw = line_uvw;
    tc.coords.line.abc = line_abc;
    tc.motion_type = TC_LINEAR;
    tc.canon_motion_type = type;
    tc.blend_with_next = tp->termCond == TC_TERM_COND_BLEND;
    tc.tolerance = tp->tolerance;

    tc.synchronized = tp->synchronized;
    tc.velocity_mode = tp->velocity_mode;
    tc.uu_per_rev = tp->uu_per_rev;
    tc.enables = enables;
    tc.indexrotary = indexrotary;

    if (syncdio.anychanged != 0) {
	tc.syncdio = syncdio; //enqueue the list of DIOs that need toggling
	tpClearDIOs(); // clear out the list, in order to prepare for the next time we need to use it
    } else {
	tc.syncdio.anychanged = 0;
    }

    tc.utvIn = line_xyz.uVec;
    tc.utvOut = line_xyz.uVec;

    if (tcqPut(&tp->queue, tc) == -1) {
        rtapi_print_msg(RTAPI_MSG_ERR, "tcqPut failed.\n");
	return -1;
    }

    tp->goalPos = end;      // remember the end of this move, as it's
                            // the start of the next one.
    tp->done = 0;
    tp->depth = tcqLen(&tp->queue);
    tp->nextId++;

    return 0;
}

// likewise, this adds a circular (circle, arc, helix) move from
// the end of the last move to this new position.  end is the
// xyzabc of the destination, center/normal/turn specify the arc
// in a way that makes sense to pmCircleInit (we don't care about
// the details here.)  Note that degenerate arcs/circles are not
// allowed; we are guaranteed to have a move in xyz so target is
// always the circle/arc/helical length.

int tpAddCircle(TP_STRUCT * tp, EmcPose end,
		PmCartesian center, PmCartesian normal, int turn, int type,
                double vel, double ini_maxvel, double acc, double jerk, unsigned char enables, char atspeed)
{
    TC_STRUCT tc = {0};
    PmLine line_uvw, line_abc;
    PmPose start_uvw, end_uvw;
    PmPose start_abc, end_abc;
    PmQuaternion identity_quat = { 1.0, 0.0, 0.0, 0.0 };

    if (!tp || tp->aborting) 
	return -1;

    start_abc.tran.x = tp->goalPos.a;
    start_abc.tran.y = tp->goalPos.b;
    start_abc.tran.z = tp->goalPos.c;
    end_abc.tran.x = end.a;
    end_abc.tran.y = end.b;
    end_abc.tran.z = end.c;

    start_uvw.tran.x = tp->goalPos.u;
    start_uvw.tran.y = tp->goalPos.v;
    start_uvw.tran.z = tp->goalPos.w;
    end_uvw.tran.x = end.u;
    end_uvw.tran.y = end.v;
    end_uvw.tran.z = end.w;

    start_uvw.rot = identity_quat;
    end_uvw.rot = identity_quat;
    start_abc.rot = identity_quat;
    end_abc.rot = identity_quat;

    pmCircle9Init(&tc.coords.circle,
            &tp->goalPos,
            &end,
            &center,
            &normal,
            turn);
    pmLineInit(&line_uvw, &start_uvw, &end_uvw);
    pmLineInit(&line_abc, &start_abc, &end_abc);

    tc.sync_accel = 0;
    tc.cycle_time = tp->cycleTime;
    tc.target = pmCircle9Target(&tc.coords.circle);
    tc.progress = 0.0;
    tc.accel_state = ACCEL_S0;
    tc.reqvel = vel;
    tc.maxvel = ini_maxvel * tp->cycleTime;
    tc.maxaccel = acc * tp->cycleTime * tp->cycleTime;
    tc.jerk = jerk * tp->cycleTime * tp->cycleTime * tp->cycleTime;
    tc.feed_override = 0.0;
    tc.id = tp->nextId;
    tc.active = 0;
    tc.done = 0;
    tc.atspeed = atspeed;

    tc.cur_vel = 0.0;
    tc.cur_accel = 0.0;
    tc.blending = 0;
    tc.seamless_blend_mode = SMLBLND_INIT;

    tc.motion_type = TC_CIRCULAR;
    tc.canon_motion_type = type;
    tc.blend_with_next = tp->termCond == TC_TERM_COND_BLEND;
    tc.tolerance = tp->tolerance;

    tc.synchronized = tp->synchronized;
    tc.velocity_mode = tp->velocity_mode;
    tc.uu_per_rev = tp->uu_per_rev;
    tc.enables = enables;
    tc.indexrotary = -1;
    
    if (syncdio.anychanged != 0) {
	tc.syncdio = syncdio; //enqueue the list of DIOs that need toggling
	tpClearDIOs(); // clear out the list, in order to prepare for the next time we need to use it
    } else {
	tc.syncdio.anychanged = 0;
    }

    tc.utvIn = tc.coords.circle.xyz.utvIn;
    tc.utvOut = tc.coords.circle.xyz.utvOut;

    if (tcqPut(&tp->queue, tc) == -1) {
	return -1;
    }

    tp->goalPos = end;
    tp->done = 0;
    tp->depth = tcqLen(&tp->queue);
    tp->nextId++;

    return 0;
}

int debug1 = 0;

#define DEBUG_AVP(state) \
  do { \
   if (debug1) {\
     double ca = tc->cur_accel/tc->cycle_time/tc->cycle_time; \
     double cv = tc->cur_vel/tc->cycle_time; \
     double cp = tc->progress; \
     rtapi_print("ID %d | STATE %s | %d | %+.4f | %+.4f | %+.6f\n", tc->id, state, tc->cycles, \
     ca, cv, cp); \
    } \
 } while (0);
#define DEBUG_AVP2(state) \
  do { \
   if (debug1) {\
     double ca = tc->cur_accel/tc->cycle_time/tc->cycle_time; \
     double cv = tc->cur_vel/tc->cycle_time; \
     double cp = tc->progress; \
     double oa = last_acc/tc->cycle_time/tc->cycle_time; \
     double ov = last_vel/tc->cycle_time; \
     double op = last_pos; \
     rtapi_print("ID %d | STATE %s | %d | %+.1f%+.1f=%+.1f | %+.3f%+.3f+0.5*%+.3f=%+.3f | %+.6f%+.6f+0.5*%+.4f+0.166*%+.6f=%+.6f \n", tc->id, state, tc->cycles, \
     oa, tc->jerk/tc->cycle_time/tc->cycle_time, ca,  ov, ca*tc->cycle_time, tc->jerk/tc->cycle_time, cv,  op, cv*tc->cycle_time, ca*tc->cycle_time*tc->cycle_time, tc->jerk, cp); \
    } \
 } while (0);

#define SHOW_AVP(txt) \
  do { \
   if (debug1) {\
     double ca = tc->cur_accel/tc->cycle_time/tc->cycle_time; \
     double cv = tc->cur_vel/tc->cycle_time; \
     double cp = tc->progress; \
     rtapi_print("ID %d | %s | ACC %-.13f | VEL %-.13f | POS %-.13f\n", tc->id, txt, ca, cv, cp); \
    } \
 } while (0);

static double last_vel = 0, last_acc = 0, last_pos = 0;

void tcRunCycle(TP_STRUCT *tp, TC_STRUCT *tc) {
    int immediate_state;
    
    last_acc = tc->cur_accel;
    last_vel = tc->cur_vel;
    last_pos = tc->progress;

    immediate_state = 0;
    do {
        switch (tc->accel_state) {
        case ACCEL_S0:
			// acc > 0 
			// jerk > 0 
			// AT = AT + JT
			// VT = VT + AT + 1/2JT
			// PT = PT + VT + 1/2AT + 1/6JT
			tc->cur_accel += tc->jerk;
			tc->cur_vel += tc->cur_accel + 0.5 * tc->jerk;
            tc->progress += tc->cur_vel + 0.5 * tc->cur_accel + 1.0/6.0 * tc->jerk;

            tc->cycles++;
			DEBUG_AVP("S0");
            if (tc->cycles >= tc->s0_cycles) {
//			DEBUG_AVP("S0");
                rtapi_print("ID %d | STATE S0->S%d | CYCLES %d | ACC %.6f | VEL %.6f | PROGRESS %.6f\n", tc->id, tc->after_S0, tc->cycles, \
                    tc->cur_accel/tc->cycle_time/tc->cycle_time, tc->cur_vel/tc->cycle_time, tc->progress);
				tc->cycles = 0;
                tc->accel_state = tc->after_S0;
                break;
			}
            
            break;
        
        case ACCEL_S1:
			// acc = max 
			// jerk = 0 
			// VT = VT + AT + 1/2JT
			// PT = PT + VT + 1/2AT + 1/6JT
            tc->cur_vel += tc->cur_accel;
            tc->progress += tc->cur_vel + 0.5 * tc->cur_accel;

            tc->cycles++;
			DEBUG_AVP("S1");
            if (tc->cycles >= tc->s1_cycles) {
				//DEBUG_AVP("S1");
                rtapi_print("ID %d | STATE S1->S2 | CYCLES %d | ACC %.6f | VEL %.6f | PROGRESS %.6f\n", tc->id, tc->cycles, \
                    tc->cur_accel/tc->cycle_time/tc->cycle_time, tc->cur_vel/tc->cycle_time, tc->progress);
				tc->cycles = 0;
                tc->accel_state = ACCEL_S2;
                break;
			}

            break;
 
        case ACCEL_S2: 
			// acc > 0 
			// jerk < 0 
			// AT = AT + JT
			// VT = VT + AT + 1/2JT
			// PT = PT + VT + 1/2AT + 1/6JT
            tc->cur_accel += -tc->jerk;
			tc->cur_vel += tc->cur_accel + 0.5 * (-tc->jerk);
			tc->progress += tc->cur_vel + 0.5 * tc->cur_accel + 1.0/6.0 * (-tc->jerk);

            tc->cycles++;
			DEBUG_AVP("S2");
            if ((tc->cycles >= tc->s0_cycles)) {
                rtapi_print("ID %d | STATE S2->S%d | CYCLES %d | ACC %.6f | VEL %.6f | PROGRESS %.6f\n", tc->id, tc->after_S2, tc->cycles, \
                    tc->cur_accel/tc->cycle_time/tc->cycle_time, tc->cur_vel/tc->cycle_time, tc->progress);
                rtapi_print("ID %d CURR VEL %.6f | REQ VEL %.6f | DIFF %.6f\n", tc->id, \
                    tc->cur_vel/tc->cycle_time*60.0, tc->reqvel*60.0, (tc->reqvel - tc->cur_vel/tc->cycle_time)*60.0);
				//DEBUG_AVP("S2");
				tc->cur_accel = 0;
				tc->cycles = 0;
				tc->accel_state = tc->after_S2;
                break;
			}

            break;
        
        case ACCEL_S3:
			// acc = 0 
			// jerk = 0 
			// PT = PT + VT + 1/2AT + 1/6JT
            tc->progress += tc->cur_vel;

            tc->cycles++;
			DEBUG_AVP("S3");
            if (tc->cycles >= tc->s3_cycles) {
				//DEBUG_AVP("S3");
                rtapi_print("ID %d | STATE S3->S4 | CYCLES %d | ACC %.6f | VEL %.6f | PROGRESS %.6f\n", tc->id, tc->cycles, \
                    tc->cur_accel/tc->cycle_time/tc->cycle_time, tc->cur_vel/tc->cycle_time, tc->progress);
				tc->cycles = 0;
                tc->accel_state = ACCEL_S4;
                break;
			}

            break;

        case ACCEL_S4:
			// acc < 0 
			// jerk < 0 
			// AT = AT + JT
			// VT = VT + AT + 1/2JT
			// PT = PT + VT + 1/2AT + 1/6JT
            tc->cur_accel += -tc->jerk;
            tc->cur_vel += tc->cur_accel + 0.5 * (-tc->jerk);
            tc->progress += tc->cur_vel + 0.5 * tc->cur_accel + 1.0/6.0 * (-tc->jerk);

            tc->cycles++;
			DEBUG_AVP("S4");
            if (tc->cycles >= tc->s0_cycles) {
			//DEBUG_AVP("S4");
                rtapi_print("ID %d | STATE S4->S%d | CYCLES %d | ACC %.6f | VEL %.6f | PROGRESS %.6f\n", tc->id, tc->after_S4, tc->cycles, \
                    tc->cur_accel/tc->cycle_time/tc->cycle_time, tc->cur_vel/tc->cycle_time, tc->progress);
				tc->cycles = 0;
                tc->accel_state = tc->after_S4;
                break;
			}
            break;
        
        case ACCEL_S5:
			// acc < 0 
			// jerk = 0 
			// VT = VT + AT + 1/2JT
			// PT = PT + VT + 1/2AT + 1/6JT
            tc->cur_vel += tc->cur_accel;
            tc->progress += tc->cur_vel + 0.5 * tc->cur_accel;

            tc->cycles++;
			DEBUG_AVP("S5");
            if (tc->cycles >= tc->s1_cycles) {
				//DEBUG_AVP("S5");
                rtapi_print("ID %d | STATE S5->S6 | CYCLES %d | ACC %.6f | VEL %.6f | PROGRESS %.6f\n", tc->id, tc->cycles, \
                    tc->cur_accel/tc->cycle_time/tc->cycle_time, tc->cur_vel/tc->cycle_time, tc->progress);
				tc->cycles = 0;
                tc->accel_state = ACCEL_S6;
                break;
			}
            break;
        
        case ACCEL_S6:
			// acc < 0 
			// jerk > 0 
			// AT = AT + JT
			// VT = VT + AT + 1/2JT
			// PT = PT + VT + 1/2AT + 1/6JT
            tc->cur_accel += tc->jerk;
            tc->cur_vel += tc->cur_accel + 0.5 * tc->jerk;
            tc->progress += tc->cur_vel + 0.5 * tc->cur_accel + 1.0/6.0 * tc->jerk;
            
            tc->cycles++;
			DEBUG_AVP("S6");
            if (tc->cycles >= tc->s0_cycles) {
                rtapi_print("ID %d | STATE S6->END | CYCLES %d | ACC %.6f | VEL %.6f | PROGRESS %.6f\n", tc->id, tc->cycles, \
                    tc->cur_accel/tc->cycle_time/tc->cycle_time, tc->cur_vel/tc->cycle_time, tc->progress);
                rtapi_print("ID %d | END | TARGET %.12f | PROGRESS %.12f | OVERSHOOT %.12f\n", tc->id, tc->target, \
                    tc->progress, tc->target - tc->progress);
				//DEBUG_AVP("S6");
				tc->cur_accel = 0;
				tc->cur_vel = 0;
				tc->progress = tc->target;
				tc->done = 1;
                break;
			}
            break;
        

        default:
         break;
        } // switch (tc->accel_state)
    } while (immediate_state);

    tc->distance_to_go = tc->target - tc->progress;
    emcmot_hal_data->debug_s32_0 = tc->accel_state;
}

void tpToggleDIOs(TC_STRUCT * tc) {
    int i=0;
    if (tc->syncdio.anychanged != 0) { // we have DIO's to turn on or off
	for (i=0; i < num_dio; i++) {
            if (!(tc->syncdio.dio_mask & (1 << i))) continue;
	    if (tc->syncdio.dios[i] > 0) emcmotDioWrite(i, 1); // turn DIO[i] on
	    if (tc->syncdio.dios[i] < 0) emcmotDioWrite(i, 0); // turn DIO[i] off
	}
	for (i=0; i < num_aio; i++) {
            if (!(tc->syncdio.aio_mask & (1 << i))) continue;
	    emcmotAioWrite(i, tc->syncdio.aios[i]); // set AIO[i]
        }
	tc->syncdio.anychanged = 0; //we have turned them all on/off, nothing else to do for this TC the next time
    }
}

static void tpSetRotaryUnlock(int axis, int unlock) {
    emcmotSetRotaryUnlock(axis, unlock);
}

static int tpGetRotaryIsUnlocked(int axis) {
    return emcmotGetRotaryIsUnlocked(axis);
}


// This is the brains of the operation.  It's called every TRAJ period
// and is expected to set tp->currentPos to the new machine position.
// Lots of other tp fields (depth, done, etc) have to be twiddled to
// communicate the status; I think those are spelled out here correctly
// and I can't clean it up without breaking the API that the TP presents
// to motion.  It's not THAT bad and in the interest of not touching
// stuff outside this directory, I'm going to leave it for now.

int tpRunCycle(TP_STRUCT * tp, long period)
{
    // vel = (new position - old position) / cycle time
    // (two position points required)
    //
    // acc = (new vel - old vel) / cycle time
    // (three position points required)

    TC_STRUCT *tc, *nexttc;
    EmcPose primary_before, primary_after;
    EmcPose secondary_before, secondary_after;
    EmcPose primary_displacement, secondary_displacement;
    static double spindleoffset;
    static int waiting_for_index = MOTION_INVALID_ID;
    static int waiting_for_atspeed = MOTION_INVALID_ID;
    static double revs;
    EmcPose target;

    emcmotStatus->tcqlen = tcqLen(&tp->queue);
    emcmotStatus->requested_vel = 0.0;
    tc = tcqItem(&tp->queue, 0, period);
    if(!tc) {
        // this means the motion queue is empty.  This can represent
        // the end of the program OR QUEUE STARVATION.  In either case,
        // I want to stop.  Some may not agree that's what it should do.
        tcqInit(&tp->queue);
        tp->goalPos = tp->currentPos;
        tp->done = 1;
        tp->depth = tp->activeDepth = 0;
        tp->aborting = 0;
        tp->execId = 0;
        tp->motionType = 0;
        tpResume(tp);
	// when not executing a move, use the current enable flags
	emcmotStatus->enables_queued = emcmotStatus->enables_new;
        return 0;
    }

    if (tc->done == 1 && waiting_for_atspeed != tc->id) {
        // if we're synced, and this move is ending, save the
        // spindle position so the next synced move can be in
        // the right place.
        if(tc->synchronized)
            spindleoffset += tc->target/tc->uu_per_rev;
        else
            spindleoffset = 0.0;

        if(tc->indexrotary != -1) {
            // this was an indexing move, so before we remove it we must
            // relock the axis
            tpSetRotaryUnlock(tc->indexrotary, 0);
            // if it is now locked, fall through and remove the finished move.
            // otherwise, just come back later and check again
            if(tpGetRotaryIsUnlocked(tc->indexrotary))
                return 0;
        }

        // done with this move
        tcqRemove(&tp->queue, 1);

        // so get next move
        tc = tcqItem(&tp->queue, 0, period);
        if(!tc) return 0;
    }

    // now we have the active tc.  get the upcoming one, if there is one.
    // it's not an error if there isn't another one - we just don't
    // do blending.  This happens in MDI for instance.
    if(!emcmotDebug->stepping && tc->blend_with_next) 
        nexttc = tcqItem(&tp->queue, 1, period);
    else
        nexttc = NULL;

    {
	int this_synch_pos = tc->synchronized && !tc->velocity_mode;
	int next_synch_pos = nexttc && nexttc->synchronized && !nexttc->velocity_mode;
	if(!this_synch_pos && next_synch_pos) {
	    // we'll have to wait for spindle sync; might as well
	    // stop at the right place (don't blend)
	    tc->blend_with_next = 0;
	    nexttc = NULL;
	}
    }

    if(nexttc && nexttc->atspeed) {
        // we'll have to wait for the spindle to be at-speed; might as well
        // stop at the right place (don't blend), like above
        tc->blend_with_next = 0;
        nexttc = NULL;
    }

    if(tp->aborting) {
        // an abort message has come
        if( MOTION_ID_VALID(waiting_for_index) ||
	    MOTION_ID_VALID(waiting_for_atspeed) ||
            (tc->cur_vel == 0.0 && !nexttc) || 
            (tc->cur_vel == 0.0 && nexttc && nexttc->cur_vel == 0.0) ) {
            tcqInit(&tp->queue);
            tp->goalPos = tp->currentPos;
            tp->done = 1;
            tp->depth = tp->activeDepth = 0;
            tp->aborting = 0;
            tp->execId = 0;
            tp->motionType = 0;
            tp->synchronized = 0;
            waiting_for_index = MOTION_INVALID_ID;
            waiting_for_atspeed = MOTION_INVALID_ID;
            emcmotStatus->spindleSync = 0;
            tpResume(tp);
            return 0;
        } else {
            tc->reqvel = 0.0;
            if(nexttc) nexttc->reqvel = 0.0;
        }
    }

    // this is no longer the segment we were waiting_for_index for
    if (MOTION_ID_VALID(waiting_for_index) && waiting_for_index != tc->id) 
    {
        rtapi_print_msg(RTAPI_MSG_ERR,
                "Was waiting for index on motion id %d, but reached id %d\n",
                waiting_for_index, tc->id);
        waiting_for_index = MOTION_INVALID_ID;
    }
    if (MOTION_ID_VALID(waiting_for_atspeed) && waiting_for_atspeed != tc->id)  
    {

        rtapi_print_msg(RTAPI_MSG_ERR,
                "Was waiting for atspeed on motion id %d, but reached id %d\n",
                waiting_for_atspeed, tc->id);
        waiting_for_atspeed = MOTION_INVALID_ID;
    }

    // check for at-speed before marking the tc active
    if (MOTION_ID_VALID(waiting_for_atspeed)) {
        if(!emcmotStatus->spindle_is_atspeed) {
            /* spindle is still not at the right speed: wait */
            return 0;
        } else {
            waiting_for_atspeed = MOTION_INVALID_ID;
        }
    }

    if(tc->active == 0) {
        // this means this tc is being read for the first time.

        // wait for atspeed, if motion requested it.  also, force
        // atspeed check for the start of all spindle synchronized
        // moves.
        if((tc->atspeed || (tc->synchronized && !tc->velocity_mode && !emcmotStatus->spindleSync)) && 
           !emcmotStatus->spindle_is_atspeed) {
            waiting_for_atspeed = tc->id;
            return 0;
        }

        if (tc->indexrotary != -1) {
            // request that the axis unlock
            tpSetRotaryUnlock(tc->indexrotary, 1);
            // if it is unlocked, fall through and start the move.
            // otherwise, just come back later and check again
            if (!tpGetRotaryIsUnlocked(tc->indexrotary))
                return 0;
        }

        tc->active = 1;
        tc->cur_vel = 0;
        tp->depth = tp->activeDepth = 1;
        tp->motionType = tc->canon_motion_type;
        tc->blending = 0;

        tc->feed_override = emcmotStatus->net_feed_scale;
        prepareLimitedJerk(tc);

        if(tc->synchronized) {
            if(!tc->velocity_mode && !emcmotStatus->spindleSync) {
                // if we aren't already synced, wait
                waiting_for_index = tc->id;
                // ask for an index reset
                emcmotStatus->spindle_index_enable = 1;
                spindleoffset = 0.0;
                // don't move: wait
                return 0;
            }
        }
    }

    if (MOTION_ID_VALID(waiting_for_index)) {
        if(emcmotStatus->spindle_index_enable) {
            /* haven't passed index yet */
            return 0;
        } else {
            /* passed index, start the move */
            emcmotStatus->spindleSync = 1;
            waiting_for_index = MOTION_INVALID_ID;
            tc->sync_accel=1;
            revs=0;
        }
    }

    if (tc->motion_type == TC_RIGIDTAP) {
        static double old_spindlepos;
        double new_spindlepos = emcmotStatus->spindleRevs;
        if (emcmotStatus->spindle.direction < 0) new_spindlepos = -new_spindlepos;

        switch (tc->coords.rigidtap.state) {
        case TAPPING:
            if (tc->progress >= tc->coords.rigidtap.reversal_target) {
                // command reversal
                emcmotStatus->spindle.speed *= -1;
                tc->coords.rigidtap.state = REVERSING;
            }
            break;
        case REVERSING:
            if (new_spindlepos < old_spindlepos) {
                PmPose start, end;
                PmLine *aux = &tc->coords.rigidtap.aux_xyz;
                // we've stopped, so set a new target at the original position
                tc->coords.rigidtap.spindlerevs_at_reversal = new_spindlepos + spindleoffset;
                
                pmLinePoint(&tc->coords.rigidtap.xyz, tc->progress, &start);
                end = tc->coords.rigidtap.xyz.start;
                pmLineInit(aux, &start, &end);
                tc->coords.rigidtap.reversal_target = aux->tmag;
                tc->target = aux->tmag + 10. * tc->uu_per_rev;
                tc->progress = 0.0;

                tc->coords.rigidtap.state = RETRACTION;
            }
            old_spindlepos = new_spindlepos;
            break;
        case RETRACTION:
            if (tc->progress >= tc->coords.rigidtap.reversal_target) {
                emcmotStatus->spindle.speed *= -1;
                tc->coords.rigidtap.state = FINAL_REVERSAL;
            }
            break;
        case FINAL_REVERSAL:
            if (new_spindlepos > old_spindlepos) {
                PmPose start, end;
                PmLine *aux = &tc->coords.rigidtap.aux_xyz;
                pmLinePoint(aux, tc->progress, &start);
                end = tc->coords.rigidtap.xyz.start;
                pmLineInit(aux, &start, &end);
                tc->target = aux->tmag;
                tc->progress = 0.0;
                tc->synchronized = 0;
                tc->reqvel = tc->maxvel;
                
                tc->coords.rigidtap.state = FINAL_PLACEMENT;
            }
            old_spindlepos = new_spindlepos;
            break;
        case FINAL_PLACEMENT:
            // this is a regular move now, it'll stop at target above.
            break;
        }
    }


    if(!tc->synchronized) emcmotStatus->spindleSync = 0;


    if(nexttc && nexttc->active == 0) {
        // this means this tc is being read for the first time.

        nexttc->cur_vel = 0;
        tp->depth = tp->activeDepth = 1;
        nexttc->active = 1;
        nexttc->blending = 0;

        nexttc->feed_override = emcmotStatus->net_feed_scale;
        prepareLimitedJerk(nexttc);
    }


    if(tc->synchronized) {
        double pos_error;
        double oldrevs = revs;

        if(tc->velocity_mode) {
            pos_error = fabs(emcmotStatus->spindleSpeedIn) * tc->uu_per_rev;
            if(nexttc) pos_error -= nexttc->progress; /* ?? */
            if(!tp->aborting) {
                tc->feed_override = emcmotStatus->net_feed_scale;
                tc->reqvel = pos_error;
            }
        } else {
            double spindle_vel, target_vel;
            double new_spindlepos = emcmotStatus->spindleRevs;
            if (emcmotStatus->spindle.direction < 0) new_spindlepos = -new_spindlepos;

            if(tc->motion_type == TC_RIGIDTAP && 
               (tc->coords.rigidtap.state == RETRACTION || 
                tc->coords.rigidtap.state == FINAL_REVERSAL))
                revs = tc->coords.rigidtap.spindlerevs_at_reversal - 
                    new_spindlepos;
            else
                revs = new_spindlepos;

            pos_error = (revs - spindleoffset) * tc->uu_per_rev - tc->progress;
            if(nexttc) pos_error -= nexttc->progress;

            if(tc->sync_accel) {
                // detect when velocities match, and move the target accordingly.
                // acceleration will abruptly stop and we will be on our new target.
                spindle_vel = revs/(tc->cycle_time * tc->sync_accel++);
                target_vel = spindle_vel * tc->uu_per_rev;
                if(tc->cur_vel >= target_vel) {
                    // move target so as to drive pos_error to 0 next cycle
                    spindleoffset = revs - tc->progress/tc->uu_per_rev;
                    tc->sync_accel = 0;
                    tc->reqvel = target_vel;
                } else {
                    // beginning of move and we are behind: accel as fast as we can
                    tc->reqvel = tc->maxvel;
                }
            } else {
                // we have synced the beginning of the move as best we can -
                // track position (minimize pos_error).
                double errorvel;
                spindle_vel = (revs - oldrevs) / tc->cycle_time;
                target_vel = spindle_vel * tc->uu_per_rev;
                errorvel = pmSqrt(fabs(pos_error) * tc->maxaccel);
                if(pos_error<0) errorvel = -errorvel;
                tc->reqvel = target_vel + errorvel;
            }
            tc->feed_override = 1.0;
        }
        if(tc->reqvel < 0.0) tc->reqvel = 0.0;
        if(nexttc) {
	    if (nexttc->synchronized) {
		nexttc->reqvel = tc->reqvel;
		nexttc->feed_override = 1.0;
		if(nexttc->reqvel < 0.0) nexttc->reqvel = 0.0;
	    } else {
		nexttc->feed_override = emcmotStatus->net_feed_scale;
	    }
	}
    } else {
        tc->feed_override = emcmotStatus->net_feed_scale;
        if(nexttc) {
	    nexttc->feed_override = emcmotStatus->net_feed_scale;
	}
    }
    /* handle pausing */
    if(tp->pausing && (!tc->synchronized || tc->velocity_mode)) {
        tc->feed_override = 0.0;
        if(nexttc) {
        nexttc->feed_override = 0.0;
        }
    }

        // calculate the approximate peak velocity the nexttc will hit.
        // we know to start blending it in when the current tc goes below
        // this velocity...
        
    if(nexttc && nexttc->maxaccel) {
    #ifdef SMLBLND
        /**
         * TODO: 
         * G64 Q[01] seamless blending:
         * where Q1 means "enable seamless blending";
         *       Q0 means "disable seamless blending"
         **/
        if (tc->seamless_blend_mode == SMLBLND_INIT) {
            double dot;
            double k;   /* curvature */
            double ca;  /* centripetal acceleration */
            double rv;  /* request velocity per cycleTime */
            rv = tc->reqvel * tp->cycleTime;
            if (rv > tc->maxvel) {
                rv = tc->maxvel;
            }
            // pmCartCartDisp(tc->utvOut, nexttc->utvIn, &k);
            pmCartCartDot(&tc->utvOut, &nexttc->utvIn, &dot);
            k = acos(dot)/rv;
            ca = k * rv * rv;
            rtapi_print("BLEND %.6f %.6f | %.6f %.6f | DOT %.6f | k = %.6f | ca = %.6f | maxacc = %.6f\n", \
                tc->utvOut.x, tc->utvOut.y, nexttc->utvIn.x, nexttc->utvIn.y, dot, k, ca, tc->maxaccel);
            // SMLBLND is for XYZ motion only
            if ((ca < tc->maxaccel) && (!tc->coords.line.xyz.tmag_zero) && (!nexttc->coords.line.xyz.tmag_zero)) {
                // allow seamless blending, SMLBLND
                // also, (nexttc->atspeed == 0)
                tc->seamless_blend_mode = SMLBLND_ENABLE;
                tc->nexttc_target = nexttc->target;
            } else {
                tc->seamless_blend_mode = SMLBLND_DISABLE;
            }
        }
    #endif // SMLBLND
    }

    primary_before = tcGetPos(tc);
    
    tcRunCycle(tp, tc);
    
    primary_after = tcGetPos(tc);
    pmCartCartSub(&primary_after.tran, &primary_before.tran, 
            &primary_displacement.tran);
    primary_displacement.a = primary_after.a - primary_before.a;
    primary_displacement.b = primary_after.b - primary_before.b;
    primary_displacement.c = primary_after.c - primary_before.c;

    primary_displacement.u = primary_after.u - primary_before.u;
    primary_displacement.v = primary_after.v - primary_before.v;
    primary_displacement.w = primary_after.w - primary_before.w;

    // blend criteria
    if( (tc->blending && nexttc) || 
            (nexttc &&
                    (tc->seamless_blend_mode == SMLBLND_ENABLE) &&
                    (tc->distance_to_go <= tc->tolerance) &&
                    (nexttc->target >= (tc->distance_to_go * 2)))) {

        // make sure we continue to blend this segment even when its 
        // accel reaches 0 (at the very end)
        tc->blending = 1;

        // hack to show blends in axis
        // tp->motionType = 0;

        if(tc->cur_vel > nexttc->cur_vel) {
            target = tcGetEndpoint(tc);
            tp->motionType = tc->canon_motion_type;
	    emcmotStatus->distance_to_go = tc->target - tc->progress;
	    emcmotStatus->enables_queued = tc->enables;
	    // report our line number to the guis
	    tp->execId = tc->id;
            emcmotStatus->requested_vel = tc->reqvel;
        } else {
	    tpToggleDIOs(nexttc); //check and do DIO changes
            target = tcGetEndpoint(nexttc);
            tp->motionType = nexttc->canon_motion_type;
	    emcmotStatus->distance_to_go = nexttc->target - nexttc->progress;
	    emcmotStatus->enables_queued = nexttc->enables;
	    // report our line number to the guis
	    tp->execId = nexttc->id;
            emcmotStatus->requested_vel = nexttc->reqvel;
        }

        emcmotStatus->current_vel = (tc->cur_vel + nexttc->cur_vel) / tc->cycle_time;

        secondary_before = tcGetPos(nexttc);
        tcRunCycle(tp, nexttc);

        secondary_after = tcGetPos(nexttc);
        pmCartCartSub(&secondary_after.tran, &secondary_before.tran, 
                &secondary_displacement.tran);
        secondary_displacement.a = secondary_after.a - secondary_before.a;
        secondary_displacement.b = secondary_after.b - secondary_before.b;
        secondary_displacement.c = secondary_after.c - secondary_before.c;

        secondary_displacement.u = secondary_after.u - secondary_before.u;
        secondary_displacement.v = secondary_after.v - secondary_before.v;
        secondary_displacement.w = secondary_after.w - secondary_before.w;

        pmCartCartAdd(&tp->currentPos.tran, &primary_displacement.tran, 
                &tp->currentPos.tran);
        pmCartCartAdd(&tp->currentPos.tran, &secondary_displacement.tran, 
                &tp->currentPos.tran);
        tp->currentPos.a += primary_displacement.a + secondary_displacement.a;
        tp->currentPos.b += primary_displacement.b + secondary_displacement.b;
        tp->currentPos.c += primary_displacement.c + secondary_displacement.c;

        tp->currentPos.u += primary_displacement.u + secondary_displacement.u;
        tp->currentPos.v += primary_displacement.v + secondary_displacement.v;
        tp->currentPos.w += primary_displacement.w + secondary_displacement.w;
    } else {
	tpToggleDIOs(tc); //check and do DIO changes
        target = tcGetEndpoint(tc);
        tp->motionType = tc->canon_motion_type;
	emcmotStatus->distance_to_go = tc->target - tc->progress;
        tp->currentPos = primary_after;
        emcmotStatus->current_vel = tc->cur_vel / tc->cycle_time;
        emcmotStatus->requested_vel = tc->reqvel;
	emcmotStatus->enables_queued = tc->enables;
	// report our line number to the guis
	tp->execId = tc->id;
    }

    emcmotStatus->dtg.tran.x = target.tran.x - tp->currentPos.tran.x;
    emcmotStatus->dtg.tran.y = target.tran.y - tp->currentPos.tran.y;
    emcmotStatus->dtg.tran.z = target.tran.z - tp->currentPos.tran.z;
    emcmotStatus->dtg.a = target.a - tp->currentPos.a;
    emcmotStatus->dtg.b = target.b - tp->currentPos.b;
    emcmotStatus->dtg.c = target.c - tp->currentPos.c;
    emcmotStatus->dtg.u = target.u - tp->currentPos.u;
    emcmotStatus->dtg.v = target.v - tp->currentPos.v;
    emcmotStatus->dtg.w = target.w - tp->currentPos.w;

    return 0;
}

int tpSetSpindleSync(TP_STRUCT * tp, double sync, int mode) {
    if(sync) {
        tp->synchronized = 1;
        tp->uu_per_rev = sync;
        tp->velocity_mode = mode;
    } else
        tp->synchronized = 0;

    return 0;
}

int tpPause(TP_STRUCT * tp)
{
    if (0 == tp) {
	return -1;
    }
    tp->pausing = 1;
    return 0;
}

int tpResume(TP_STRUCT * tp)
{
    if (0 == tp) {
	return -1;
    }
    tp->pausing = 0;
    return 0;
}

int tpAbort(TP_STRUCT * tp)
{
    if (0 == tp) {
	return -1;
    }

    if (!tp->aborting) {
	/* to abort, signal a pause and set our abort flag */
	tpPause(tp);
	tp->aborting = 1;
    }
    return tpClearDIOs(); //clears out any already cached DIOs
}

int tpGetMotionType(TP_STRUCT * tp)
{
    return tp->motionType;
}

int tpGetPos(TP_STRUCT * tp, EmcPose * pos)
{
    if (0 == tp) {
        ZERO_EMC_POSE((*pos));
        return -1;
    }
    *pos = tp->currentPos;

    return 0;
}

int tpIsDone(TP_STRUCT * tp)
{
    if (0 == tp) {
	return 0;
    }

    return tp->done;
}

int tpQueueDepth(TP_STRUCT * tp)
{
    if (0 == tp) {
	return 0;
    }

    return tp->depth;
}

int tpActiveDepth(TP_STRUCT * tp)
{
    if (0 == tp) {
	return 0;
    }

    return tp->activeDepth;
}

int tpSetAout(TP_STRUCT *tp, unsigned char index, double start, double end) {
    if (0 == tp) {
	return -1;
    }
    syncdio.anychanged = 1; //something has changed
    syncdio.aio_mask |= (1 << index);
    syncdio.aios[index] = start;
    return 0;
}

int tpSetDout(TP_STRUCT *tp, int index, unsigned char start, unsigned char end) {
    if (0 == tp) {
	return -1;
    }
    syncdio.anychanged = 1; //something has changed
    syncdio.dio_mask |= (1 << index);
    if (start > 0)
	syncdio.dios[index] = 1; // the end value can't be set from canon currently, and has the same value as start
    else 
	syncdio.dios[index] = -1;
    return 0;    
}


// vim:sw=4:sts=4:et: