/* Quagga pselect support -- header
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

#ifndef _ZEBRA_QPSELECT_H
#define _ZEBRA_QPSELECT_H

#include <sys/select.h>
#include <errno.h>

#include "zassert.h"
#include "qtime.h"
#include "vector.h"

#ifndef Inline
#define Inline static inline
#endif

/*==============================================================================
 * Quagga pselect -- qps_xxxx
 *
 * Here and in qpselect.c is a data structure for managing multiple file
 * descriptors and running pselect to wait for I/O activity and to multiplex
 * between the file descriptors.
 */

enum qps_mnum                   /* "mode" numbers: error/read/write     */
{
  qps_mnum_first = 0,

  qps_error_mnum = 0,
  qps_read_mnum  = 1,
  qps_write_mnum = 2,

  qps_mnum_count = 3
} ;

typedef enum qps_mnum qps_mnum_t ;

#define qps_mbit(mnum) (1 << mnum)

enum qps_mbits                  /* "mode" bits: error/read/write        */
{
  qps_error_mbit  = qps_mbit(qps_error_mnum),
  qps_read_mbit   = qps_mbit(qps_read_mnum),
  qps_write_mbit  = qps_mbit(qps_write_mnum),

  qps_all_mbits   = qps_mbit(qps_mnum_count) - 1
};

typedef enum qps_mbits qps_mbit_t ;

/* Forward references   */
typedef struct qps_selection* qps_selection ;
typedef struct qps_file*      qps_file ;

/*==============================================================================
 * fd_super_set.
 *
 * To speed up scanning of large fd_set's this structure overlays a 32-bit
 * word and a byte array over the (assumed) fd_set bit vector.
 *
 * There is no guarantee that FD_SETSIZE is a multiple of 32 (or of 8, for
 * that matter) -- so some care must be taken.
 */

typedef uint32_t fd_word_t ;

#define FD_WORD_BITS  32
#define FD_WORD_BYTES (FD_WORD_BITS / 8)

CONFIRM(FD_WORD_BITS == (FD_WORD_BYTES * 8)) ;  /* for completeness     */

#define FD_SUPER_SET_WORD_SIZE ((FD_SETSIZE + FD_WORD_BITS - 1) / FD_WORD_BITS)
#define FD_SUPER_SET_BYTE_SIZE ((FD_SETSIZE + FD_WORD_BITS - 1) / 8)

/* Make sure that the overlay is at least as big as the fd_set !        */
CONFIRM(FD_SUPER_SET_BYTE_SIZE >= sizeof(fd_set)) ;

typedef union           /* see qps_make_super_set_map()         */
{
  fd_word_t words[FD_SUPER_SET_WORD_SIZE] ;
  uint8_t   bytes[FD_SUPER_SET_BYTE_SIZE] ;
  fd_set    fdset ;
} fd_super_set ;

/*==============================================================================
 * Action function.
 *
 * Each file has three action functions, to be called in qps_dispatch_next()
 * when pselect() has reported error/read/write for the file.
 *
 * For further discussion, see: qps_file_init.
 */

typedef void qps_action(qps_file qf, void* file_info) ;

/*==============================================================================
 * Data Structures.
 */

typedef fd_super_set fd_full_set[qps_mnum_count] ;

struct qps_selection
{
  int   fd_count ;      /* number of fds we are looking after             */
  int   fd_direct ;     /* direct lookup in vector or not                 */

  struct vector files ; /* mapping fd to qps_file                         */

  int   fd_last ;       /* highest numbered fd we are looking after       */
  int   enabled_count[qps_mnum_count] ;  /* no. enabled fds in each mode  */
  fd_full_set enabled ; /* bit vectors for select enabled stuff           */

  int   tried_fd_last ; /* highest numbered fd on last pselect            */
  int   tried_count[qps_mnum_count] ;    /* enabled_count on last pselect */
  fd_full_set results ; /* last set of results from pselect               */

  int         pend_count ;  /* results pending               (if any)     */
  qps_mnum_t  pend_mnum ;   /* error/read/write mode pending (if any)     */
  int         pend_fd ;     /* fd pending                    (if any)     */

  int         signum ;  /* signal that sigmask is enabling -- 0 => none   */
  sigset_t    sigmask ; /* sigmask to use for duration of pselect         */
} ;

struct qps_file
{
  qps_selection selection ;

  void*   file_info ;
  int     fd ;

  qps_mbit_t    enabled_bits ;

  qps_action*   actions[qps_mnum_count] ;
} ;

/*==============================================================================
 * qps_selection handling
 */

qps_selection
qps_selection_init_new(qps_selection qps) ;

void
qps_add_file(qps_selection qps, qps_file qf, int fd, void* file_info) ;

void
qps_remove_file(qps_file qf) ;

void
qps_set_signal(qps_selection qps, int signum, sigset_t sigmask) ;

int
qps_pselect(qps_selection qps, qtime_t timeout) ;

int
qps_dispatch_next(qps_selection qps) ;

/*==============================================================================
 * qps_file structure handling
 */

qps_file
qps_file_init_new(qps_file qf, qps_file template) ;

void
qps_file_free(qps_file qf) ;

void
qps_enable_mode(qps_file qf, qps_mnum_t mnum, qps_action* action) ;

void
qps_set_action(qps_file qf, qps_mnum_t mnum, qps_action* action) ;

void
qps_disable_modes(qps_file qf, qps_mbit_t mbits) ;

#endif /* _ZEBRA_QPSELECT_H */