/* Global time scaling.
 *
 * ACX_FrameTimer_Update_DeltaProducer computes one float per frame, the
 * elapsed time in 60 Hz ticks at g_ft_delta_60HzTicks, and every integrator
 * multiplies by it -- Player_SpeedIntegrator reads it six times, as do the
 * aircraft and object updates and ACX_Frame_Update.  Scaling it after the
 * producer has run gives a global time control that touches no object:
 *
 *     1.0   normal
 *     0.25  quarter speed
 *     0.0   frozen; rendering, input and the UI carry on
 *
 * Only the delta at +0 is scaled.  The producer also derives a floored copy
 * at +4 and its reciprocal at +8; those are left alone so anything reading a
 * rate rather than an interval stays defined at zero.
 */

#ifndef PSPRECO_TIMECTL_H
#define PSPRECO_TIMECTL_H

void  timectl_init(void);
void  timectl_set_scale(float s);   /* clamped to 0 .. 8 */
float timectl_scale(void);
int   timectl_frozen(void);         /* scale == 0 */

#endif
