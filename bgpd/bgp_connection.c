/* BGP Connection Handling -- functions
 * Copyright (C) 2009 Chris Hall (GMCH), Highwayman
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
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

#include <zebra.h>
#include "bgpd/bgp.h"

#include "bgpd/bgpd.h"

#include "bgpd/bgp_fsm.h"
#include "bgpd/bgp_engine.h"
#include "bgpd/bgp_session.h"
#include "bgpd/bgp_connection.h"
#include "bgpd/bgp_notification.h"

#include "lib/memory.h"
#include "lib/mqueue.h"
#include "lib/symtab.h"
#include "lib/stream.h"

/*==============================================================================
 * BGP Connections.
 *
 * Each BGP Connection has its own:
 *
 *   * BGP Finite State Machine (FSM)
 *   * socket and related qpselect file
 *   * input/output buffers and I/O management
 *   * timers to support the above
 *
 * Each BGP Session is associated with at most two BGP Connections.  The second
 * connection exists only if a connect and a listen connection is made while
 * a session is starting up, and one will be dropped before either connection
 * reaches Established state.
 *
 * The bgp_connection structure is private to the BGP Engine, and is accessed
 * directly, without the need for any mutex.
 *
 * Each connection is closely tied to its parent bgp_session.  The bgp_session
 * is shared between the Routeing Engine and the BGP Engine, and therefore
 * access is subject to the bgp_session's mutex.
 *
 */

/*==============================================================================
 * The connection queue.
 *
 * When the connection's write buffer empties, the connection is placed on the
 * connection queue.
 *
 * The connection queue is processed as the highest priority action in the
 * BGP Engine, at which point as many of the items on the connection's
 * pending queue as possible will be processed.
 *
 * The connection_queue is managed as a circular list of connections.  The
 * connection_queue variable points at the next to be processed.
 *
 */

static bgp_connection bgp_connection_queue ;

/*==============================================================================
 * Managing bgp_connection stuctures.
 */
static const char* bgp_connection_tags[] =
  {
      [bgp_connection_primary]   = "(primary)",
      [bgp_connection_secondary] = "(secondary)",
  } ;

static void
bgp_connection_init_host(bgp_connection connection, const char* tag) ;

static void
bgp_write_buffer_init_new(bgp_wbuffer wb, size_t size) ;

/*------------------------------------------------------------------------------
 * Initialise connection structure -- allocate if required.
 *
 *
 *
 * NB: acquires and releases the session mutex.
 */
extern bgp_connection
bgp_connection_init_new(bgp_connection connection, bgp_session session,
                                               bgp_connection_ordinal_t ordinal)
{
  assert( (ordinal == bgp_connection_primary)
       || (ordinal == bgp_connection_secondary) ) ;
  assert(session->connections[ordinal] == NULL) ;

  BGP_SESSION_LOCK(session) ;   /*<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<*/

  if (connection == NULL)
    connection = XCALLOC(MTYPE_BGP_CONNECTION, sizeof(struct bgp_connection)) ;
  else
    memset(connection, 0, sizeof(struct bgp_connection)) ;

  /* Structure is zeroised, so the following are implictly initialised:
   *
   *   * state                    bgp_fsm_Initial
   *   * comatose                 not comatose
   *   * next                     NULL -- not on the connection queue
   *   * prev                     NULL -- not on the connection queue
   *   * post                     bgp_fsm_null_event
   *   * fsm_active               not active
   *   * stopped                  bgp_stopped_not
   *   * notification             NULL -- none received or sent
   *   * err                      no error, so far
   *   * su_local                 NULL -- no address, yet
   *   * su_remote                NULL -- no address, yet
   *   * hold_timer_interval      none -- set when connection is opened
   *   * keepalive_timer_interval none -- set when connection is opened
   *   * read_pending             nothing pending
   *   * read_header              not reading header
   *   * notification_pending     nothing pending
   *   * wbuff_full               not full
   *   * wbuff                    all pointers NULL -- empty buffer
   */

  confirm(bgp_fsm_Initial    == 0) ;
  confirm(bgp_fsm_null_event == 0) ;
  confirm(bgp_stopped_not    == 0) ;

  /* Link back to session, point at its mutex and point session here        */
  connection->session  = session ;
  connection->p_mutex  = &session->mutex ;

  connection->ordinal  = ordinal ;
  connection->accepted = (ordinal == bgp_connection_secondary) ;

  session->connections[ordinal] = connection ;

  /* qps_file structure                                                 */
  qps_file_init_new(&connection->qf, NULL) ;

  /* Initialise all the timers                                          */
  qtimer_init_new(&connection->hold_timer,      p_bgp_engine->pile,
                                                             NULL, connection) ;
  qtimer_init_new(&connection->keepalive_timer, p_bgp_engine->pile,
                                                             NULL, connection) ;

  /* Copy log destination and make host name + (primary)/(secondary)    */
  /* Makes complete copies so that connection may continue to run, even */
  /* after the session has stopped, and may have been destroyed.        */
  connection->log  = session->log ;
  bgp_connection_init_host(connection, bgp_connection_tags[ordinal]) ;

  /* Need two empty "stream" buffers                                    */
  connection->ibuf = stream_new(BGP_MAX_MSG_L) ;
  connection->obuf = stream_new(BGP_MAX_MSG_L) ;

  /* Ensure mqueue_local_queue is empty.                                */
  mqueue_local_init_new(&connection->pending_queue) ;

  BGP_SESSION_UNLOCK(session) ; /*>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>*/

  return connection ;
} ;

/*------------------------------------------------------------------------------
 * Set the host field for the connection to session->host + given tag.
 *
 * NB: requires the session to be LOCKED.
 */
static void
bgp_connection_init_host(bgp_connection connection, const char* tag)
{
  const char* host = connection->session->host ;

  connection->host = XMALLOC(MTYPE_BGP_PEER_HOST, strlen(host)
                                                + strlen(tag) + 1) ;
  strcpy(connection->host, host) ;
  strcat(connection->host, tag) ;
} ;

/*------------------------------------------------------------------------------
 * Get sibling (if any) for given connection.
 *
 * NB: requires the session to be LOCKED.
 */
extern bgp_connection
bgp_connection_get_sibling(bgp_connection connection)
{
  bgp_session session = connection->session ;

  if (session == NULL)
    return NULL ;               /* no sibling if no session             */

  confirm(bgp_connection_primary   == (bgp_connection_secondary ^ 1)) ;
  confirm(bgp_connection_secondary == (bgp_connection_primary   ^ 1)) ;

  return session->connections[connection->ordinal ^ 1] ;
} ;

/*------------------------------------------------------------------------------
 * Make given connection the primary.
 *
 * Expects the given connection to be the only remaining connection.
 *
 * NB: requires the session to be LOCKED.
 */
extern void
bgp_connection_make_primary(bgp_connection connection)
{
  bgp_session session = connection->session ;

  /* Deal with the connection ordinal.                          */
  if (connection->ordinal != bgp_connection_primary)
    {
      connection->ordinal = bgp_connection_primary ;
      session->connections[bgp_connection_primary] = connection ;
    } ;

  session->connections[bgp_connection_secondary] = NULL ;

  /* Move the open_state to the session.
   * Change the connection host to drop the primary/secondary distinction.
   * Copy the negotiated hold_timer_interval and keepalive_timer_interval
   * Copy the su_local and su_remote
   */

  session->open_recv = connection->open_recv ;
  connection->open_recv = NULL ;        /* no longer interested in this  */

  XFREE(MTYPE_BGP_PEER_HOST, connection->host) ;
  bgp_connection_init_host(connection, "") ;

  session->hold_timer_interval        = connection->hold_timer_interval ;
  session->keepalive_timer_interval   = session->keepalive_timer_interval ;

  session->su_local  = connection->su_local ;
  connection->su_local  = NULL ;
  session->su_remote = connection->su_remote ;
  connection->su_remote = NULL ;
} ;

/*------------------------------------------------------------------------------
 * Free connection.
 *
 * Connection must be Stopping -- no longer attached to a session.
 *
 * The FSM will have
 *
 *
 */
extern bgp_connection
bgp_connection_free(bgp_connection connection)
{

  return connection ;
} ;

/*------------------------------------------------------------------------------
 * Full if not enough room for a maximum size BGP message.
 */
static inline int
bgp_write_buffer_full(bgp_wbuffer wb)
{
  return ((wb->limit - wb->p_in) < BGP_MAX_MSG_L) ;
} ;

/*------------------------------------------------------------------------------
 * Empty if in and out pointers are equal (but may need to be reset !)
 */
static inline int
bgp_write_buffer_empty(bgp_wbuffer wb)
{
  return (wb->p_out == wb->p_in) ;
} ;

/*------------------------------------------------------------------------------
 * Allocate new write buffer and initialise pointers
 *
 * NB: assumes structure has been zeroised by the initialisation of the
 *     enclosing connection.
 */
static void
bgp_write_buffer_init_new(bgp_wbuffer wb, size_t size)
{
  assert(wb->base == NULL) ;

  wb->base  = XMALLOC(MTYPE_STREAM_DATA, size) ;
  wb->limit = wb->base + size ;

  wb->p_in  = wb->p_out = wb->base ;
  wb->full  = bgp_write_buffer_full(wb) ;

  assert(!wb->full) ;
} ;

/*==============================================================================
 * Connection queue management.
 *
 * Connections appear on this queue when their write buffer becomes empty, or
 * they are finally stopped.
 */

/*------------------------------------------------------------------------------
 * Add connection to connection queue -- if not already on it
 */
extern void
bgp_connection_queue_add(bgp_connection connection)
{
  if (connection->next == NULL)
    {
      if (bgp_connection_queue == NULL)
        {
          /* adding to empty queue              */
          bgp_connection_queue = connection ;
          connection->next = connection ;
          connection->prev = connection ;
        }
      else
        {
          /* add behind the current entry       */
          connection->next       = bgp_connection_queue ;
          connection->prev       = bgp_connection_queue->prev ;

          connection->next->prev = connection ;
          connection->prev->next = connection ;
        } ;
    } ;
} ;

/*------------------------------------------------------------------------------
 * Delete connection from connection queue -- if on it
 */
extern void
bgp_connection_queue_del(bgp_connection connection)
{
  if (connection->next != NULL)
    {
      if (connection == connection->next)
        {
          /* deleting the only item on the queue                */
          assert((connection == connection->prev)
                                      && (connection == bgp_connection_queue)) ;
          bgp_connection_queue = NULL ;
        }
      else
        {
          if (connection == bgp_connection_queue)
            bgp_connection_queue = connection->next ;

          connection->next->prev = connection->prev ;
          connection->prev->next = connection->next ;
        } ;

      connection->next = connection->prev = NULL ;
    } ;
} ;

/*------------------------------------------------------------------------------
 * Process the connection queue until it becomes empty.
 *
 * Process each item until its pending queue becomes empty, or its write
 * buffer becomes full, or it is stopped.
 *
 */
extern void
bgp_connection_queue_process(void)
{
  mqueue_block mqb ;

  while (bgp_connection_queue != NULL)
    {
      /* select the first in the queue, and step to the next    */
      bgp_connection connection = bgp_connection_queue ;
      bgp_connection_queue = connection->next ;

      /* Reap the connection if it is now stopped.              */
      if (connection->state == bgp_fsm_Stopping)
        {
          bgp_connection_reset(connection) ;
        } ;

      /* .....                                                  */


      /* .....                                                  */


    } ;


} ;

/*==============================================================================
 * Opening and closing Connections
 */

/*------------------------------------------------------------------------------
 * Open connection.
 *
 * Expects connection to either be newly created or recently closed.
 *
 * Sets:
 *
 *   * if accept() clears the session accept flag
 *   * sets the qfile and fd ready for use
 *   * clears err and stopped
 *   * discards any open_state and notification
 *   * copies hold_timer_interval and keep_alive_timer_interval from session
 *
 * Expects:
 *
 *   * links to/from session to be set up (including ordinal)
 *   * timers to be initialised and unset
 *   * log and host to be set up
 *   * buffers to exist and all buffering to be set empty
 *   * pending queue to be empty
 *
 * Does not touch:
 *
 *   * state of the connection
 *
 * NB: requires the session to be LOCKED.
 */
extern void
bgp_connection_open(bgp_connection connection, int fd)
{
  bgp_session session = connection->session ;

  /* If this is the secondary connection, do not accept any more.       */
  if (connection->ordinal == bgp_connection_secondary)
    session->accept = 0 ;

  /* Set the file going                                                 */
  qps_add_file(p_bgp_engine->selection, &connection->qf, fd, connection) ;

  /* Clear sundry state is clear                                        */
  connection->post    = bgp_fsm_null_event ;    /* no post event event  */
  connection->err     = 0 ;                     /* so far, so good      */
  connection->stopped = bgp_stopped_not ;       /* up and running       */

  /* These accept NULL arguments                                        */
  connection->open_recv    = bgp_open_state_free(connection->open_recv) ;
  connection->notification = bgp_notify_free(connection->notification) ;

  /* Copy the original hold_timer_interval and keepalive_timer_interval
   * Assume these have sensible initial values.
   *
   * These may be changed during the exchange of BGP OPEN messages.
   */
  connection->hold_timer_interval      = session->hold_timer_interval ;
  connection->keepalive_timer_interval = session->keepalive_timer_interval ;
} ;

/*------------------------------------------------------------------------------
 * Close connection.
 *
 *   * if there is an fd, close it
 *   * if qfile is active, remove it
 *   * forget any addresses
 *   * unset any timers
 *   * reset all buffering to empty
 *   * empties the pending queue -- destroying all messages
 *
 * The following remain:
 *
 *   * state of the connection
 *   * links to and from the session
 *   * the timers remain initialised (but unset)
 *   * the buffers remain (but reset)
 *   * logging and host string
 *   * any open_state that has been received
 *   * any notification sent/received
 *   * the stopped cause (if any)
 *
 * Once closed, the only further possible actions are:
 *
 *   * bgp_connection_open()     -- to retry connection
 *
 *   * bgp_connection_free()     -- to finally discard
 *
 *   * bgp_connection_close()    -- can do this again
 *
 */
extern void
bgp_connection_close(bgp_connection connection)
{
  int fd ;

  /* close the qfile and any associate file descriptor                  */
  qps_remove_file(&connection->qf) ;
  fd = qps_file_unset_fd(&connection->qf) ;
  if (fd != fd_undef)
    shutdown(fd, SHUT_RDWR) ;

  /* forget any addresses                                               */
  sockunion_clear(&connection->su_local) ;
  sockunion_clear(&connection->su_remote) ;

  /* Unset all the timers                                               */
  qtimer_unset(&connection->hold_timer) ;
  qtimer_unset(&connection->keepalive_timer) ;

  /* Reset all buffering empty.                                         */
  stream_reset(connection->ibuf) ;
  stream_reset(connection->obuf) ;

  connection->read_pending  = 0 ;
  connection->read_header   = 0 ;
  connection->notification_pending = 0 ;

  connection->wbuff.p_in  = connection->wbuff.base ;
  connection->wbuff.p_out = connection->wbuff.base ;
  connection->wbuff.full  = 0 ;

  /* Empty out the pending queue                                        */
  mqueue_local_reset_keep(&connection->pending_queue) ;
} ;

/*------------------------------------------------------------------------------
 * Close connection for reading and purge the write buffers.
 *
 * This is done when the connection is about to be fully closed, but need to
 * send a NOTIFICATION message before finally closing.
 *
 *   * if there is an fd, shutdown(, SHUT_RD) and disable the qfile for reading
 *   * reset all read buffering to empty
 *   * discard all output except any partially written message
 *   * empty the pending queue
 *
 * Can do this because the write buffer contains only complete BGP messages.
 *
 * This ensures the write buffer is not full, so NOTIFICATION message can
 * be written (at least as far as the write buffer).
 *
 * Everything else is left untouched.
 */
extern void
bgp_connection_part_close(bgp_connection connection)
{
  bgp_wbuffer wb = &connection->wbuff ;
  int         fd ;
  uint8_t*    p ;
  bgp_size_t  mlen ;

  /* close the qfile and any associate file descriptor                  */
  fd = qps_file_fd(&connection->qf) ;
  if (fd != fd_undef)
    {
      shutdown(fd, SHUT_RD) ;
      qps_disable_modes(&connection->qf, qps_read_mbit) ;
    } ;

  /* Reset all input buffering.                                        */
  stream_reset(connection->ibuf) ;

  connection->read_pending  = 0 ;
  connection->read_header   = 0 ;

  /* Reset obuf and purge wbuff.                                        */
  stream_reset(connection->obuf) ;

  connection->notification_pending = 0 ;

  if (wb->p_in != wb->p_out)    /* will be equal if buffer is empty     */
    {
      mlen = 0 ;
      p    = wb->base ;
      do                        /* Advance p until p + mlen > wb->p_out */
        {
          p += mlen ;
          mlen = bgp_msg_get_mlen(p) ;
        } while ((p + mlen) <= wb->p_out) ;

      if (p == wb->p_out)
        mlen = 0 ;              /* wb->p_out points at start of message */
      else
        memcpy(wb->base, p, mlen) ;

      wb->p_out = wb->base + (wb->p_out - p) ;
      wb->p_in  = wb->base + mlen ;
    }
  else
    wb->p_in = wb->p_out = wb->base ;

  wb->full = bgp_write_buffer_full(wb) ;
  assert(!wb->full) ;

  /* Empty out the pending queue                                        */
  mqueue_local_reset_keep(&connection->pending_queue) ;
} ;

/*==============================================================================
 * Writing to BGP connection.
 *
 * All writing is done by preparing a BGP message in the "obuf" buffer,
 * and then calling bgp_connection_write().
 *
 * If possible, that is written away immediately.  If not, then no further
 * messages may be prepared until the buffer has been cleared.
 *
 * Write the contents of the "work" buffer.
 *
 * Returns true <=> able to write the entire buffer without blocking.
 */

static int bgp_connection_write_direct(bgp_connection connection) ;
static void bgp_connection_write_action(qps_file qf, void* file_info) ;

/*------------------------------------------------------------------------------
 * Write the contents of the obuf -- MUST not be here if wbuff is full !
 *
 * Returns: 1 => all written     -- obuf and wbuff are empty
 *          0 => written         -- obuf now empty
 *         -1 => failed          -- error event generated
 */
extern int
bgp_connection_write(bgp_connection connection)
{
  bgp_wbuffer wb = &connection->wbuff ;

  if (bgp_write_buffer_empty(wb))
    {
      /* write buffer is empty -- attempt to write directly     */
      return bgp_connection_write_direct(connection) ;
    } ;

  /* Transfer the obuf contents to the staging buffer.                  */
  wb->p_in = stream_transfer(wb->p_in, connection->obuf, wb->limit) ;

  return 1 ;
} ;

/*------------------------------------------------------------------------------
 * The write buffer is empty -- so try to write obuf directly.
 *
 * If cannot empty the obuf directly to the TCP buffers, transfer it to to the
 * write buffer, and enable the qpselect action.
 * (This is where the write buffer is allocated, if it hasn't yet been.)
 *
 * Either way, the obuf is cleared and can be reused (unless failed).
 *
 * Returns:  1 => written obuf to TCP buffer -- all buffers empty
 *           0 => written obuf to wbuff      -- obuf empty
 *          -1 => failed                     -- stopping, dead
 */
enum { bgp_wbuff_size = BGP_MAX_MSG_L * 10 } ;

static int
bgp_connection_write_direct(bgp_connection connection)
{
  int ret ;

  ret = stream_flush_try(connection->obuf, qps_file_fd(&connection->qf)) ;

  if (ret == 0)
    return 1 ;          /* Done: wbuff and obuf are empty       */

  else if (ret > 0)
    {
      bgp_wbuffer wb = &connection->wbuff ;

      /* Partial write -- set up buffering, if required.            */
      if (wb->base == NULL)
        bgp_write_buffer_init_new(wb, bgp_wbuff_size) ;

      /* Transfer *entire* message to staging buffer                */
      wb->p_in = stream_transfer(wb->base, connection->obuf, wb->limit) ;

      wb->p_out = wb->p_in - ret ;          /* output from here     */

      /* Must now be enabled to write                               */
      qps_enable_mode(&connection->qf, qps_write_mnum,
                                        bgp_connection_write_action) ;

      return 0 ;        /* Done: obuf is empty, but wbuff is not    */
    } ;

  /* write failed -- signal error and return failed                 */
  bgp_fsm_io_error(connection, errno) ;

  return -1 ;
} ;

/*------------------------------------------------------------------------------
 * Write Action for bgp connection.
 *
 * Empty the write buffer if we can.
 *
 * If empties that, empty the obuf if there is anything pending, and....
 *
 * If empty out everything, disable write mode.
 *
 * If encounter an error, generate TCP_fatal_error event.
 */
static void
bgp_connection_write_action(qps_file qf, void* file_info)
{
  bgp_connection connection = file_info ;
  bgp_wbuffer wb = &connection->wbuff ;
  int have ;
  int ret ;

  /* Try to empty the write buffer.                                     */
  have = wb->p_out - wb->p_in ;
  while (have != 0)
    {
      ret = write(qps_file_fd(qf), wb->p_out, have) ;
      if      (ret > 0)
        {
          wb->p_out += ret ;
          have      -= ret ;
        }
      else if (ret < 0)
        {
          ret = errno ;
          if (ret == EINTR)
            continue ;

          if ((ret != EAGAIN) && (ret != EWOULDBLOCK))
            bgp_fsm_io_error(connection, errno) ;

          return ;
        } ;
    } ;

  /* Buffer is empty -- reset it and disable write mode                 */
  wb->p_out = wb->p_in = wb->base ;
  wb->full = 0 ;

  qps_disable_modes(&connection->qf, qps_write_mbit) ;

  /* If waiting to send NOTIFICATION, just did it.                      */
  /* Otherwise: is writable again -- so add to connection_queue         */
  if (connection->notification_pending)
    bgp_fsm_event(connection, bgp_fsm_Sent_NOTIFICATION_message) ;
  else
    bgp_connection_queue_add(connection) ;
} ;

/*==============================================================================
 * Read Action for bgp connection.
 *
 * Don't directly read -- all reading is done in response to the socket
 * becoming readable.
 *
 * Reads one BGP message into the ibuf and dispatches it.
 *
 * Performs the checks on the BGP message header:
 *
 *   * Marker is all '1's
 *   * Length is <= BGP_MAX_MSG_L
 *   * Type   is OPEN/UPDATE/NOTIFICATION/KEEPALIVE
 *
 */
static void
bgp_connection_read_action(qps_file qf, void* file_info)
{
  bgp_connection connection = file_info ;
  int want ;
  int ret ;

  /* If nothing pending for partial packet, start reading new one.      */

  want = connection->read_pending ;
  if (want == 0)
    {
      want = BGP_MH_HEAD_L ;
      stream_reset(connection->ibuf) ;
      connection->read_header = 1 ;
    } ;

  /* Loop to read entire BGP message into ibuf.
   *
   * On error or "EOF", raises suitable FSM events and returns.
   *
   * If cannot read entire message, sets new pending count and returns.
   *
   * Exits loop iff completes a BGP message.
   */
  while (1)
    {
      ret = stream_read_unblock(connection->ibuf, qps_file_fd(&connection->qf),
                                                                         want) ;
      if (ret >= 0)
        {
          want -= ret ;
          if (want != 0)
            {
              connection->read_pending = want ;
              return ;                  /* must wait for the rest       */
            } ;

          if (!connection->read_header)
            break ;                     /* got complete message         */

          connection->read_header = 0 ; /* got complete header          */

          want = bgp_msg_check_header(connection->ibuf) ;
                                        /* returns balance of message   */
          if (want < 0)
            return ;                    /* failed in header check       */
        }
      else
        {
          bgp_fsm_io_error(connection, (ret == -1) ? errno : 0) ;
          return ;
        } ;
    } ;

  /* Deal with the BGP message.  MUST remove from ibuf before returns ! */
  bgp_msg_dispatch(connection) ;

  /* Ready to read another message                                      */
  connection->read_pending = 0 ;
} ;
