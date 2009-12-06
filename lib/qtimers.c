/* Quagga timers support -- header
 * Copyright (C) 2009 Chris Hall (GMCH), Highwayman
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2, or (at your
 * option) any later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <stddef.h>
#include <string.h>

#include "zassert.h"
#include "qtimers.h"
#include "memory.h"
#include "heap.h"

/*==============================================================================
 * Quagga Timers -- qtimer_xxxx
 *
 * Here and in qtimers.h is a data structure for managing multiple timers
 * each with an action to be executed when the timer expires.
 *
 * The qtime_pile structure manages a "pile" of qtimer structures which are
 * waiting for the right time to go off.
 *
 * NB: it is ASSUMED that a qtime_pile will be private to the thread in which
 *     it is created and used.
 *
 *     There is NO mutex handling here.
 *
 * Timers are triggered by calling qtimer_dispatch_next().  This is given the
 * current qtimer time (see below), and it dispatches the first timer whose
 * time has come (or been passed).  Dispatching a timer means calling its
 * action function (see below).  Each call of qtimer_dispatch_next triggers at
 * most one timer.
 *
 * Time Base
 * ---------
 *
 * The time base for qtimers is the monotonic time provided in qtime.c/.h.
 *
 * The qtimer_time_now(), qtimer_time_future(), timer_time_from_realtime(),
 * qtimer_time_from_timeofday() functions return qtimer times.
 *
 * Action Functions
 * ----------------
 *
 * There is a separate action function for each timer.
 *
 * When the action function is called it is passed the qtimer structure, the
 * timer_info pointer from that structure and the time which triggered the
 * timer (which may, or may not, be the current qtimer time).
 *
 * During an action function timers may be set/unset, actions changed, and so
 * on... there are no restrictions.
 */

static int
qtimer_cmp(qtimer* a, qtimer* b)        /* the heap discipline  */
{
  if ((**a).time < (**b).time)
    return -1 ;
  if ((**a).time > (**b).time)
    return +1 ;
  return 0 ;
} ;

/*==============================================================================
 * qtimer_pile handling
 */

/* Initialise a timer pile -- allocating it if required.
 *
 * Returns the qtimer_pile.
 */
qtimer_pile
qtimer_pile_init_new(qtimer_pile qtp)
{
  if (qtp == NULL)
    qtp = XCALLOC(MTYPE_QTIMER_PILE, sizeof(struct qtimer_pile)) ;
  else
    memset(qtp, 0,  sizeof(struct qtimer_pile)) ;

  /* Zeroising has initialised:
   *
   *   timers        -- invalid heap -- need to properly initialise
   *
   *   unset_pending -- NULL -- nothing pending
   */

  /* Eclipse flags offsetof(struct qtimer, backlink) as a syntax error :-(  */
  typedef struct qtimer qtimer_t ;

  heap_init_new_backlinked(&qtp->timers, 0, (heap_cmp*)qtimer_cmp,
                                                 offsetof(qtimer_t, backlink)) ;
  return qtp ;
} ;

/* Dispatch the next timer whose time is <= the given "upto" time.
 *
 * The upto time must be a qtimer time (!) -- see qtimer_time_now().
 *
 * The upto argument allows the caller to get a qtimer_time_now() value, and
 * then process all timers upto that time.
 *
 * Returns true  <=> dispatched a timer, and there may be more to do.
 *         false <=> nothing to do (and nothing done).
 */
int
qtimer_pile_dispatch_next(qtimer_pile qtp, qtime_t upto)
{
  qtimer   qtr ;

  if (qtp->unset_pending != NULL)
    qtimer_unset(qtp->unset_pending) ;  /* just in case recurse through here  */

  qtr = heap_top_item(&qtp->timers) ;
  if ((qtr != NULL) && (qtr->time <= upto))
    {
      qtp->unset_pending = qtr ;        /* delay unset of top item, pro tem...*/

      qtr->action(qtr, qtr->timer_info, upto) ;

      if (qtp->unset_pending != NULL)   /* ...now must unset if not yet done  */
        qtimer_unset(qtp->unset_pending) ;

      return 1 ;
    }
  else
    return 0 ;
} ;

/* Ream out (another) item from qtimer_pile.
 *
 * If pile is empty, release the qtimer_pile structure, if required.
 *
 * Useful for emptying out and discarding a pile of timers:
 *
 *     while ((p_qtr = qtimer_pile_ream(qtp, 1)))
 *       ... do what's required to release the item p_qtr
 *
 * Returns NULL when timer pile is empty (and has been released, if required).
 *
 * If the timer pile is not released, it may be reused without reinitialisation.
 *
 * NB: once reaming has started, the timer pile MUST NOT be used for anything,
 *     and the process MUST be run to completion.
 */
qtimer
qtimer_pile_ream(qtimer_pile qtp, int free_structure)
{
  qtimer qtr ;

  qtr = heap_ream_keep(&qtp->timers) ;  /* ream, keeping the heap structure   */
  if (qtr != NULL)
    qtr->active = 0 ;                   /* already removed from pile          */
  else
    {
      if (free_structure)
        XFREE(MTYPE_QTIMER_PILE, qtp) ;
      else
        qtp->unset_pending = NULL ;   /* heap is empty, so this is last thing */
                                      /* to be tidied up.                     */
    } ;

  return qtr ;
} ;

/*==============================================================================
 * qtimer handling
 */

/* Initialise qtimer structure -- allocating one if required.
 *
 * Associates qtimer with the given pile of timers, and sets up the action and
 * the timer_info ready for use.
 *
 * Once initialised, the timer may be set.
 *
 * Returns the qtimer.
 */
qtimer
qtimer_init_new(qtimer qtr, qtimer_pile qtp,
                                        qtimer_action* action, void* timer_info)
{
  if (qtr == NULL)
    qtr = XCALLOC(MTYPE_QTIMER, sizeof(struct qtimer)) ;
  else
    memset(qtr, 0,  sizeof(struct qtimer)) ;

  /* Zeroising has initialised:
   *
   *   pile        -- NULL -- not in any pile (yet)
   *   backlink    -- unset
   *
   *   active      -- not active
   *
   *   time        -- unset
   *   action      -- NULL -- no action set (yet)
   *   timer_info  -- NULL -- no timer info set (yet)
   */

  qtr->pile       = qtp ;
  qtr->action     = action ;
  qtr->timer_info = timer_info ;

  return qtr ;
} ;

/* Free given timer.
 *
 * Unsets it first if it is active.
 */
void
qtimer_free(qtimer qtr)
{
  if (qtr->active)
    qtimer_unset(qtr) ;

  XFREE(MTYPE_QTIMER, qtr) ;
} ;

/* Set pile in which given timer belongs.
 *
 * Unsets the timer if active in another pile.
 * (Does nothing if active in the "new" pile.)
 */
void
qtimer_set_pile(qtimer qtr, qtimer_pile qtp)
{
  if ((qtr->active) && (qtr->pile != qtp))
    qtimer_unset(qtr) ;
  qtr->pile = qtp ;
}

/* Set action for given timer.
 */
void
qtimer_set_action(qtimer qtr, qtimer_action* action)
{
  qtr->action = action ;
} ;

/* Set timer_info for given timer.
 */
void
qtimer_set_info(qtimer qtr, void* timer_info)
{
  qtr->timer_info = timer_info ;
} ;

/* Set given timer.
 *
 * Setting a -ve time => qtimer_unset.
 *
 * If the timer is already active, sets the new time & updates pile.
 *
 * Otherwise, sets the time and adds to pile -- making timer active.
 */
void
qtimer_set(qtimer qtr, qtime_t when)
{
  qtimer_pile qtp ;

  if (when < 0)
    return qtimer_unset(qtr) ;

  qtp = qtr->pile ;
  dassert(qtp != NULL) ;

  qtr->time = when ;

  if (qtr->active)
    {
      heap_update_item(&qtp->timers, qtr) ;     /* update in heap       */
      if (qtr == qtp->unset_pending)
        qtp->unset_pending = NULL ;             /* dealt with.          */
    }
  else
    {
      heap_push_item(&qtp->timers, qtr) ;       /* add to heap          */
      qtr->active = 1 ;
    } ;
} ;

/* Unset given timer
 *
 * If the timer is active, removes from pile and sets inactive.
 */
void
qtimer_unset(qtimer qtr)
{
  if (qtr->active)
    {
      qtimer_pile qtp = qtr->pile ;
      dassert(qtp != NULL) ;

      heap_delete_item(&qtp->timers, qtr) ;
      if (qtr == qtp->unset_pending)
        qtp->unset_pending = NULL ;

      qtr->active = 0 ;
    } ;
} ;