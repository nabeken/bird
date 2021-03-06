/*
 *	BIRD -- The Border Gateway Protocol
 *
 *	(c) 2000 Martin Mares <mj@ucw.cz>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

/**
 * DOC: Border Gateway Protocol
 *
 * The BGP protocol is implemented in three parts: |bgp.c| which takes care of the
 * connection and most of the interface with BIRD core, |packets.c| handling
 * both incoming and outgoing BGP packets and |attrs.c| containing functions for
 * manipulation with BGP attribute lists.
 *
 * As opposed to the other existing routing daemons, BIRD has a sophisticated core
 * architecture which is able to keep all the information needed by BGP in the
 * primary routing table, therefore no complex data structures like a central
 * BGP table are needed. This increases memory footprint of a BGP router with
 * many connections, but not too much and, which is more important, it makes
 * BGP much easier to implement.
 *
 * Each instance of BGP (corresponding to a single BGP peer) is described by a &bgp_proto
 * structure to which are attached individual connections represented by &bgp_connection
 * (usually, there exists only one connection, but during BGP session setup, there
 * can be more of them). The connections are handled according to the BGP state machine
 * defined in the RFC with all the timers and all the parameters configurable.
 *
 * In incoming direction, we listen on the connection's socket and each time we receive
 * some input, we pass it to bgp_rx(). It decodes packet headers and the markers and
 * passes complete packets to bgp_rx_packet() which distributes the packet according
 * to its type.
 *
 * In outgoing direction, we gather all the routing updates and sort them to buckets
 * (&bgp_bucket) according to their attributes (we keep a hash table for fast comparison
 * of &rta's and a &fib which helps us to find if we already have another route for
 * the same destination queued for sending, so that we can replace it with the new one
 * immediately instead of sending both updates). There also exists a special bucket holding
 * all the route withdrawals which cannot be queued anywhere else as they don't have any
 * attributes. If we have any packet to send (due to either new routes or the connection
 * tracking code wanting to send a Open, Keepalive or Notification message), we call
 * bgp_schedule_packet() which sets the corresponding bit in a @packet_to_send
 * bit field in &bgp_conn and as soon as the transmit socket buffer becomes empty,
 * we call bgp_fire_tx(). It inspects state of all the packet type bits and calls
 * the corresponding bgp_create_xx() functions, eventually rescheduling the same packet
 * type if we have more data of the same type to send.
 *
 * The processing of attributes consists of two functions: bgp_decode_attrs() for checking
 * of the attribute blocks and translating them to the language of BIRD's extended attributes
 * and bgp_encode_attrs() which does the converse. Both functions are built around a
 * @bgp_attr_table array describing all important characteristics of all known attributes.
 * Unknown transitive attributes are attached to the route as %EAF_TYPE_OPAQUE byte streams.
 */

#undef LOCAL_DEBUG

#include "nest/bird.h"
#include "nest/iface.h"
#include "nest/protocol.h"
#include "nest/route.h"
#include "nest/locks.h"
#include "conf/conf.h"
#include "lib/socket.h"
#include "lib/resource.h"
#include "lib/string.h"

#include "bgp.h"

struct linpool *bgp_linpool;		/* Global temporary pool */
static sock *bgp_listen_sk;		/* Global listening socket */
static int bgp_counter;			/* Number of protocol instances using the listening socket */

static void bgp_close(struct bgp_proto *p, int apply_md5);
static void bgp_connect(struct bgp_proto *p);
static void bgp_active(struct bgp_proto *p);
static sock *bgp_setup_listen_sk(ip_addr addr, unsigned port, u32 flags);


/**
 * bgp_open - open a BGP instance
 * @p: BGP instance
 *
 * This function allocates and configures shared BGP resources.
 * Should be called as the last step during initialization
 * (when lock is acquired and neighbor is ready).
 * When error, state changed to PS_DOWN, -1 is returned and caller
 * should return immediately.
 */
static int
bgp_open(struct bgp_proto *p)
{
  struct config *cfg = p->cf->c.global;
  bgp_counter++;

  if (!bgp_listen_sk)
    bgp_listen_sk = bgp_setup_listen_sk(cfg->listen_bgp_addr, cfg->listen_bgp_port, cfg->listen_bgp_flags);

  if (!bgp_linpool)
    bgp_linpool = lp_new(&root_pool, 4080);

  if (p->cf->password)
    {
      int rv = sk_set_md5_auth(bgp_listen_sk, p->cf->remote_ip, p->cf->password);
      if (rv < 0)
	{
	  bgp_close(p, 0);
	  p->p.disabled = 1;
	  bgp_store_error(p, NULL, BE_MISC, BEM_INVALID_MD5);
	  proto_notify_state(&p->p, PS_DOWN);
	  return -1;
	}
    }

  return 0;
}

static void
bgp_startup(struct bgp_proto *p)
{
  BGP_TRACE(D_EVENTS, "Started");
  p->start_state = p->cf->capabilities ? BSS_CONNECT : BSS_CONNECT_NOCAP;

  if (!p->cf->passive)
    bgp_active(p);
}

static void
bgp_startup_timeout(timer *t)
{
  bgp_startup(t->data);
}


static void
bgp_initiate(struct bgp_proto *p)
{
  if (p->startup_delay)
    {
      BGP_TRACE(D_EVENTS, "Startup delayed by %d seconds", p->startup_delay);
      bgp_start_timer(p->startup_timer, p->startup_delay);
    }
  else
    bgp_startup(p);
}

/**
 * bgp_close - close a BGP instance
 * @p: BGP instance
 * @apply_md5: 0 to disable unsetting MD5 auth
 *
 * This function frees and deconfigures shared BGP resources.
 * @apply_md5 is set to 0 when bgp_close is called as a cleanup
 * from failed bgp_open().
 */
static void
bgp_close(struct bgp_proto *p, int apply_md5)
{
  ASSERT(bgp_counter);
  bgp_counter--;

  if (p->cf->password && apply_md5)
    sk_set_md5_auth(bgp_listen_sk, p->cf->remote_ip, NULL);

  if (!bgp_counter)
    {
      rfree(bgp_listen_sk);
      bgp_listen_sk = NULL;
      rfree(bgp_linpool);
      bgp_linpool = NULL;
    }
}

/**
 * bgp_start_timer - start a BGP timer
 * @t: timer
 * @value: time to fire (0 to disable the timer)
 *
 * This functions calls tm_start() on @t with time @value and the
 * amount of randomization suggested by the BGP standard. Please use
 * it for all BGP timers.
 */
void
bgp_start_timer(timer *t, int value)
{
  if (value)
    {
      /* The randomization procedure is specified in RFC 1771: 9.2.3.3 */
      t->randomize = value / 4;
      tm_start(t, value - t->randomize);
    }
  else
    tm_stop(t);
}

/**
 * bgp_close_conn - close a BGP connection
 * @conn: connection to close
 *
 * This function takes a connection described by the &bgp_conn structure,
 * closes its socket and frees all resources associated with it.
 */
void
bgp_close_conn(struct bgp_conn *conn)
{
  // struct bgp_proto *p = conn->bgp;

  DBG("BGP: Closing connection\n");
  conn->packets_to_send = 0;
  rfree(conn->connect_retry_timer);
  conn->connect_retry_timer = NULL;
  rfree(conn->keepalive_timer);
  conn->keepalive_timer = NULL;
  rfree(conn->hold_timer);
  conn->hold_timer = NULL;
  rfree(conn->sk);
  conn->sk = NULL;
  rfree(conn->tx_ev);
  conn->tx_ev = NULL;
}


/**
 * bgp_update_startup_delay - update a startup delay
 * @p: BGP instance
 *
 * This function updates a startup delay that is used to postpone next BGP connect.
 * It also handles disable_after_error and might stop BGP instance when error
 * happened and disable_after_error is on.
 *
 * It should be called when BGP protocol error happened.
 */
void
bgp_update_startup_delay(struct bgp_proto *p)
{
  struct bgp_config *cf = p->cf;

  DBG("BGP: Updating startup delay\n");

  if (p->last_proto_error && ((now - p->last_proto_error) >= (int) cf->error_amnesia_time))
    p->startup_delay = 0;

  p->last_proto_error = now;

  if (cf->disable_after_error)
    {
      p->startup_delay = 0;
      p->p.disabled = 1;
      return;
    }

  if (!p->startup_delay)
    p->startup_delay = cf->error_delay_time_min;
  else
    p->startup_delay = MIN(2 * p->startup_delay, cf->error_delay_time_max);
}

static void
bgp_graceful_close_conn(struct bgp_conn *conn, unsigned subcode)
{
  switch (conn->state)
    {
    case BS_IDLE:
    case BS_CLOSE:
      return;
    case BS_CONNECT:
    case BS_ACTIVE:
      bgp_conn_enter_idle_state(conn);
      return;
    case BS_OPENSENT:
    case BS_OPENCONFIRM:
    case BS_ESTABLISHED:
      bgp_error(conn, 6, subcode, NULL, 0);
      return;
    default:
      bug("bgp_graceful_close_conn: Unknown state %d", conn->state);
    }
}

static void
bgp_down(struct bgp_proto *p)
{
  if (p->start_state > BSS_PREPARE)
    bgp_close(p, 1);

  BGP_TRACE(D_EVENTS, "Down");
  proto_notify_state(&p->p, PS_DOWN);
}

static void
bgp_decision(void *vp)
{
  struct bgp_proto *p = vp;

  DBG("BGP: Decision start\n");
  if ((p->p.proto_state == PS_START)
      && (p->outgoing_conn.state == BS_IDLE)
      && (!p->cf->passive))
    bgp_active(p);

  if ((p->p.proto_state == PS_STOP)
      && (p->outgoing_conn.state == BS_IDLE)
      && (p->incoming_conn.state == BS_IDLE))
    bgp_down(p);
}

void
bgp_stop(struct bgp_proto *p, unsigned subcode)
{
  proto_notify_state(&p->p, PS_STOP);
  bgp_graceful_close_conn(&p->outgoing_conn, subcode);
  bgp_graceful_close_conn(&p->incoming_conn, subcode);
  ev_schedule(p->event);
}

static inline void
bgp_conn_set_state(struct bgp_conn *conn, unsigned new_state)
{
  if (conn->bgp->p.mrtdump & MD_STATES)
    mrt_dump_bgp_state_change(conn, conn->state, new_state);

  conn->state = new_state;
}

void
bgp_conn_enter_openconfirm_state(struct bgp_conn *conn)
{
  /* Really, most of the work is done in bgp_rx_open(). */
  bgp_conn_set_state(conn, BS_OPENCONFIRM);
}

void
bgp_conn_enter_established_state(struct bgp_conn *conn)
{
  struct bgp_proto *p = conn->bgp;
 
  BGP_TRACE(D_EVENTS, "BGP session established");
  DBG("BGP: UP!!!\n");

  p->conn = conn;
  p->last_error_class = 0;
  p->last_error_code = 0;
  bgp_attr_init(conn->bgp);
  bgp_conn_set_state(conn, BS_ESTABLISHED);
  proto_notify_state(&p->p, PS_UP);
}

static void
bgp_conn_leave_established_state(struct bgp_proto *p)
{
  BGP_TRACE(D_EVENTS, "BGP session closed");
  p->conn = NULL;

  if (p->p.proto_state == PS_UP)
    bgp_stop(p, 0);
}

void
bgp_conn_enter_close_state(struct bgp_conn *conn)
{
  struct bgp_proto *p = conn->bgp;
  int os = conn->state;

  bgp_conn_set_state(conn, BS_CLOSE);
  tm_stop(conn->hold_timer);
  tm_stop(conn->keepalive_timer);
  conn->sk->rx_hook = NULL;

  if (os == BS_ESTABLISHED)
    bgp_conn_leave_established_state(p);
}

void
bgp_conn_enter_idle_state(struct bgp_conn *conn)
{
  struct bgp_proto *p = conn->bgp;
  int os = conn->state;

  bgp_close_conn(conn);
  bgp_conn_set_state(conn, BS_IDLE);
  ev_schedule(p->event);

  if (os == BS_ESTABLISHED)
    bgp_conn_leave_established_state(p);
}

static void
bgp_send_open(struct bgp_conn *conn)
{
  conn->start_state = conn->bgp->start_state;
  conn->want_as4_support = conn->bgp->cf->enable_as4 && (conn->start_state != BSS_CONNECT_NOCAP);
  conn->peer_as4_support = 0;	// Default value, possibly changed by receiving capability.
  conn->advertised_as = 0;

  DBG("BGP: Sending open\n");
  conn->sk->rx_hook = bgp_rx;
  conn->sk->tx_hook = bgp_tx;
  tm_stop(conn->connect_retry_timer);
  bgp_schedule_packet(conn, PKT_OPEN);
  bgp_conn_set_state(conn, BS_OPENSENT);
  bgp_start_timer(conn->hold_timer, conn->bgp->cf->initial_hold_time);
}

static void
bgp_connected(sock *sk)
{
  struct bgp_conn *conn = sk->data;
  struct bgp_proto *p = conn->bgp;

  BGP_TRACE(D_EVENTS, "Connected");
  bgp_send_open(conn);
}

static void
bgp_connect_timeout(timer *t)
{
  struct bgp_conn *conn = t->data;
  struct bgp_proto *p = conn->bgp;

  DBG("BGP: connect_timeout\n");
  if (p->p.proto_state == PS_START)
    {
      bgp_close_conn(conn);
      bgp_connect(p);
    }
  else
    bgp_conn_enter_idle_state(conn);
}

static void
bgp_sock_err(sock *sk, int err)
{
  struct bgp_conn *conn = sk->data;
  struct bgp_proto *p = conn->bgp;

  bgp_store_error(p, conn, BE_SOCKET, err);

  if (err)
    BGP_TRACE(D_EVENTS, "Connection lost (%M)", err);
  else
    BGP_TRACE(D_EVENTS, "Connection closed");

  bgp_conn_enter_idle_state(conn);
}

static void
bgp_hold_timeout(timer *t)
{
  struct bgp_conn *conn = t->data;

  DBG("BGP: Hold timeout\n");

  /* If there is something in input queue, we are probably congested
     and perhaps just not processed BGP packets in time. */

  if (sk_rx_ready(conn->sk) > 0)
    bgp_start_timer(conn->hold_timer, 10);
  else
    bgp_error(conn, 4, 0, NULL, 0);
}

static void
bgp_keepalive_timeout(timer *t)
{
  struct bgp_conn *conn = t->data;

  DBG("BGP: Keepalive timer\n");
  bgp_schedule_packet(conn, PKT_KEEPALIVE);
}

static void
bgp_setup_conn(struct bgp_proto *p, struct bgp_conn *conn)
{
  timer *t;

  conn->sk = NULL;
  conn->bgp = p;
  conn->packets_to_send = 0;

  t = conn->connect_retry_timer = tm_new(p->p.pool);
  t->hook = bgp_connect_timeout;
  t->data = conn;
  t = conn->hold_timer = tm_new(p->p.pool);
  t->hook = bgp_hold_timeout;
  t->data = conn;
  t = conn->keepalive_timer = tm_new(p->p.pool);
  t->hook = bgp_keepalive_timeout;
  t->data = conn;
  conn->tx_ev = ev_new(p->p.pool);
  conn->tx_ev->hook = bgp_kick_tx;
  conn->tx_ev->data = conn;
}

static void
bgp_setup_sk(struct bgp_conn *conn, sock *s)
{
  s->data = conn;
  s->err_hook = bgp_sock_err;
  conn->sk = s;
}

static void
bgp_active(struct bgp_proto *p)
{
  int delay = MAX(1, p->cf->start_delay_time);
  struct bgp_conn *conn = &p->outgoing_conn;

  BGP_TRACE(D_EVENTS, "Connect delayed by %d seconds", delay);
  bgp_setup_conn(p, conn);
  bgp_conn_set_state(conn, BS_ACTIVE);
  bgp_start_timer(conn->connect_retry_timer, delay);
}

int
bgp_apply_limits(struct bgp_proto *p)
{
  if (p->cf->route_limit && (p->p.stats.imp_routes > p->cf->route_limit))
    {
      log(L_WARN "%s: Route limit exceeded, shutting down", p->p.name);
      bgp_store_error(p, NULL, BE_AUTO_DOWN, BEA_ROUTE_LIMIT_EXCEEDED);
      bgp_update_startup_delay(p);
      bgp_stop(p, 1); // Errcode 6, 1 - max number of prefixes reached
      return -1;
    }

  return 0;
}


/**
 * bgp_connect - initiate an outgoing connection
 * @p: BGP instance
 *
 * The bgp_connect() function creates a new &bgp_conn and initiates
 * a TCP connection to the peer. The rest of connection setup is governed
 * by the BGP state machine as described in the standard.
 */
static void
bgp_connect(struct bgp_proto *p)	/* Enter Connect state and start establishing connection */
{
  sock *s;
  struct bgp_conn *conn = &p->outgoing_conn;

  DBG("BGP: Connecting\n");
  s = sk_new(p->p.pool);
  s->type = SK_TCP_ACTIVE;
  s->saddr = p->source_addr;
  s->daddr = p->cf->remote_ip;
  s->dport = BGP_PORT;
  s->ttl = p->cf->multihop ? : 1;
  s->rbsize = BGP_RX_BUFFER_SIZE;
  s->tbsize = BGP_TX_BUFFER_SIZE;
  s->tos = IP_PREC_INTERNET_CONTROL;
  s->password = p->cf->password;
  s->tx_hook = bgp_connected;
  BGP_TRACE(D_EVENTS, "Connecting to %I from local address %I", s->daddr, s->saddr);
  bgp_setup_conn(p, conn);
  bgp_setup_sk(conn, s);
  bgp_conn_set_state(conn, BS_CONNECT);
  s->iface = p->neigh->iface;

  if (sk_open(s))
    {
      bgp_sock_err(s, 0);
      return;
    }
  DBG("BGP: Waiting for connect success\n");
  bgp_start_timer(conn->connect_retry_timer, p->cf->connect_retry_time);
}

/**
 * bgp_incoming_connection - handle an incoming connection
 * @sk: TCP socket
 * @dummy: unused
 *
 * This function serves as a socket hook for accepting of new BGP
 * connections. It searches a BGP instance corresponding to the peer
 * which has connected and if such an instance exists, it creates a
 * &bgp_conn structure, attaches it to the instance and either sends
 * an Open message or (if there already is an active connection) it
 * closes the new connection by sending a Notification message.
 */
static int
bgp_incoming_connection(sock *sk, int dummy UNUSED)
{
  struct proto_config *pc;

  DBG("BGP: Incoming connection from %I port %d\n", sk->daddr, sk->dport);
  WALK_LIST(pc, config->protos)
    if (pc->protocol == &proto_bgp && pc->proto)
      {
	struct bgp_proto *p = (struct bgp_proto *) pc->proto;
	if (ipa_equal(p->cf->remote_ip, sk->daddr))
	  {
	    /* We are in proper state and there is no other incoming connection */
	    int acc = (p->p.proto_state == PS_START || p->p.proto_state == PS_UP) &&
	      (p->start_state >= BSS_CONNECT) && (!p->incoming_conn.sk);

	    BGP_TRACE(D_EVENTS, "Incoming connection from %I (port %d) %s",
		      sk->daddr, sk->dport, acc ? "accepted" : "rejected");

	    if (!acc)
	      goto err;

	    bgp_setup_conn(p, &p->incoming_conn);
	    bgp_setup_sk(&p->incoming_conn, sk);
	    sk_set_ttl(sk, p->cf->multihop ? : 1);
	    bgp_send_open(&p->incoming_conn);
	    return 0;
	  }
      }

  log(L_WARN "BGP: Unexpected connect from unknown address %I (port %d)", sk->daddr, sk->dport);
 err:
  rfree(sk);
  return 0;
}

static void
bgp_listen_sock_err(sock *sk UNUSED, int err)
{
  if (err == ECONNABORTED)
    log(L_WARN "BGP: Incoming connection aborted");
  else
    log(L_ERR "BGP: Error on incoming socket: %M", err);
}

static sock *
bgp_setup_listen_sk(ip_addr addr, unsigned port, u32 flags)
{
  sock *s = sk_new(&root_pool);
  DBG("BGP: Creating incoming socket\n");
  s->type = SK_TCP_PASSIVE;
  s->saddr = addr;
  s->sport = port ? port : BGP_PORT;
  s->flags = flags;
  s->tos = IP_PREC_INTERNET_CONTROL;
  s->rbsize = BGP_RX_BUFFER_SIZE;
  s->tbsize = BGP_TX_BUFFER_SIZE;
  s->rx_hook = bgp_incoming_connection;
  s->err_hook = bgp_listen_sock_err;
  if (sk_open(s))
    {
      log(L_ERR "BGP: Unable to open incoming socket");
      rfree(s);
      return NULL;
    }
  else
    return s;
}

static void
bgp_start_neighbor(struct bgp_proto *p)
{
  p->local_addr = p->neigh->iface->addr->ip;
  p->source_addr = ipa_nonzero(p->cf->source_addr) ? p->cf->source_addr : p->local_addr;

  DBG("BGP: local=%I remote=%I\n", p->source_addr, p->next_hop);
#ifdef IPV6
  {
    struct ifa *a;
    p->local_link = IPA_NONE;
    WALK_LIST(a, p->neigh->iface->addrs)
      if (a->scope == SCOPE_LINK)
        {
	  p->local_link = a->ip;
	  break;
	}

    if (! ipa_nonzero(p->local_link))
      log(L_WARN "%s: Missing link local address on interface %s", p->p.name,  p->neigh->iface->name);

    DBG("BGP: Selected link-level address %I\n", p->local_link);
  }
#endif

  int rv = bgp_open(p);
  if (rv < 0)
    return;

  bgp_initiate(p);
}

static void
bgp_neigh_notify(neighbor *n)
{
  struct bgp_proto *p = (struct bgp_proto *) n->proto;

  if (n->iface)
    {
      if ((p->p.proto_state == PS_START) && (p->start_state == BSS_PREPARE))
	{
	  BGP_TRACE(D_EVENTS, "Neighbor found");
	  bgp_start_neighbor(p);
	}
    }
  else
    {
      if ((p->p.proto_state == PS_START) || (p->p.proto_state == PS_UP))
	{
	  BGP_TRACE(D_EVENTS, "Neighbor lost");
	  bgp_store_error(p, NULL, BE_MISC, BEM_NEIGHBOR_LOST);
	  bgp_stop(p, 0);
	}
    }
}

static int
bgp_reload_routes(struct proto *P)
{
  struct bgp_proto *p = (struct bgp_proto *) P;
  if (!p->conn || !p->conn->peer_refresh_support)
    return 0;

  bgp_schedule_packet(p->conn, PKT_ROUTE_REFRESH);
  return 1;
}

static void
bgp_start_locked(struct object_lock *lock)
{
  struct bgp_proto *p = lock->data;
  struct bgp_config *cf = p->cf;

  if (p->p.proto_state != PS_START)
    {
      DBG("BGP: Got lock in different state %d\n", p->p.proto_state);
    return;
    }

  DBG("BGP: Got lock\n");
  p->local_id = proto_get_router_id(&cf->c);
  p->next_hop = cf->multihop ? cf->multihop_via : cf->remote_ip;
  p->neigh = neigh_find(&p->p, &p->next_hop, NEF_STICKY);

  if (cf->rr_client)
    {
      p->rr_cluster_id = cf->rr_cluster_id ? cf->rr_cluster_id : p->local_id;
      p->rr_client = cf->rr_client;
    }

  p->rs_client = cf->rs_client;

  if (!p->neigh)
    {
      log(L_ERR "%s: Invalid next hop %I", p->p.name, p->next_hop);
      /* As we do not start yet, we can just disable protocol */
      p->p.disabled = 1;
      bgp_store_error(p, NULL, BE_MISC, BEM_INVALID_NEXT_HOP);
      proto_notify_state(&p->p, PS_DOWN);
      return;
    }
  
  if (p->neigh->iface)
    {
      if (cf->ifname)
        p->neigh->iface = if_find_by_name(cf->ifname);

      bgp_start_neighbor(p);
    }
  else
    BGP_TRACE(D_EVENTS, "Waiting for %I to become my neighbor", p->next_hop);
}

static int
bgp_start(struct proto *P)
{
  struct bgp_proto *p = (struct bgp_proto *) P;
  struct object_lock *lock;

  DBG("BGP: Startup.\n");
  p->start_state = BSS_PREPARE;
  p->outgoing_conn.state = BS_IDLE;
  p->incoming_conn.state = BS_IDLE;
  p->neigh = NULL;

  p->event = ev_new(p->p.pool);
  p->event->hook = bgp_decision;
  p->event->data = p;

  p->startup_timer = tm_new(p->p.pool);
  p->startup_timer->hook = bgp_startup_timeout;
  p->startup_timer->data = p;

  /*
   *  Before attempting to create the connection, we need to lock the
   *  port, so that are sure we're the only instance attempting to talk
   *  with that neighbor.
   */

  lock = p->lock = olock_new(P->pool);
  lock->addr = p->cf->remote_ip;
  lock->type = OBJLOCK_TCP;
  lock->port = BGP_PORT;
  lock->iface = NULL;
  lock->hook = bgp_start_locked;
  lock->data = p;
  olock_acquire(lock);

  return PS_START;
}

static int
bgp_shutdown(struct proto *P)
{
  struct bgp_proto *p = (struct bgp_proto *) P;
  unsigned subcode;

  BGP_TRACE(D_EVENTS, "Shutdown requested");
  bgp_store_error(p, NULL, BE_MAN_DOWN, 0);

  if (P->reconfiguring)
    {
      if (P->cf_new)
	subcode = 6; // Errcode 6, 6 - other configuration change
      else
	subcode = 3; // Errcode 6, 3 - peer de-configured
    }
  else
    subcode = 2; // Errcode 6, 2 - administrative shutdown

  p->startup_delay = 0;
  bgp_stop(p, subcode);

  return p->p.proto_state;
}

static struct proto *
bgp_init(struct proto_config *C)
{
  struct bgp_config *c = (struct bgp_config *) C;
  struct proto *P = proto_new(C, sizeof(struct bgp_proto));
  struct bgp_proto *p = (struct bgp_proto *) P;

  P->accept_ra_types = RA_OPTIMAL;
  P->rt_notify = bgp_rt_notify;
  P->rte_better = bgp_rte_better;
  P->import_control = bgp_import_control;
  P->neigh_notify = bgp_neigh_notify;
  P->reload_routes = bgp_reload_routes;
  p->cf = c;
  p->local_as = c->local_as;
  p->remote_as = c->remote_as;
  p->is_internal = (c->local_as == c->remote_as);
  return P;
}

/**
 * bgp_error - report a protocol error
 * @c: connection
 * @code: error code (according to the RFC)
 * @subcode: error sub-code
 * @data: data to be passed in the Notification message
 * @len: length of the data
 *
 * bgp_error() sends a notification packet to tell the other side that a protocol
 * error has occurred (including the data considered erroneous if possible) and
 * closes the connection.
 */
void
bgp_error(struct bgp_conn *c, unsigned code, unsigned subcode, byte *data, int len)
{
  struct bgp_proto *p = c->bgp;

  if (c->state == BS_CLOSE)
    return;

  bgp_log_error(p, BE_BGP_TX, "Error", code, subcode, data, (len > 0) ? len : -len);
  bgp_store_error(p, c, BE_BGP_TX, (code << 16) | subcode);
  bgp_conn_enter_close_state(c);

  c->notify_code = code;
  c->notify_subcode = subcode;
  c->notify_data = data;
  c->notify_size = (len > 0) ? len : 0;
  bgp_schedule_packet(c, PKT_NOTIFICATION);

  if (code != 6)
    {
      bgp_update_startup_delay(p);
      bgp_stop(p, 0);
    }
}

/**
 * bgp_store_error - store last error for status report
 * @p: BGP instance
 * @c: connection
 * @class: error class (BE_xxx constants)
 * @code: error code (class specific)
 *
 * bgp_store_error() decides whether given error is interesting enough
 * and store that error to last_error variables of @p
 */
void
bgp_store_error(struct bgp_proto *p, struct bgp_conn *c, u8 class, u32 code)
{
  /* During PS_UP, we ignore errors on secondary connection */
  if ((p->p.proto_state == PS_UP) && c && (c != p->conn))
    return;

  /* During PS_STOP, we ignore any errors, as we want to report
   * the error that caused transition to PS_STOP
   */
  if (p->p.proto_state == PS_STOP)
    return;

  p->last_error_class = class;
  p->last_error_code = code;
}

void
bgp_check(struct bgp_config *c)
{
  if (!c->local_as)
    cf_error("Local AS number must be set");

  if (!c->remote_as)
    cf_error("Neighbor must be configured");

  if (!(c->capabilities && c->enable_as4) && (c->remote_as > 0xFFFF))
    cf_error("Neighbor AS number out of range (AS4 not available)");

  if ((c->local_as != c->remote_as) && (c->rr_client))
    cf_error("Only internal neighbor can be RR client");

  if ((c->local_as == c->remote_as) && (c->rs_client))
    cf_error("Only external neighbor can be RS client");

  /* Different default based on rs_client */
  if (c->missing_lladdr == 0)
    c->missing_lladdr = c->rs_client ? MLL_DROP : MLL_SELF;
}

static char *bgp_state_names[] = { "Idle", "Connect", "Active", "OpenSent", "OpenConfirm", "Established", "Close" };
static char *bgp_err_classes[] = { "", "Error: ", "Socket: ", "Received: ", "BGP Error: ", "Automatic shutdown: ", ""};
static char *bgp_misc_errors[] = { "", "Neighbor lost", "Invalid next hop", "Kernel MD5 auth failed" };
static char *bgp_auto_errors[] = { "", "Route limit exceeded"};


static void
bgp_get_status(struct proto *P, byte *buf)
{
  struct bgp_proto *p = (struct bgp_proto *) P;

  const byte *err1 = bgp_err_classes[p->last_error_class];
  const byte *err2 = "";
  byte errbuf[32];

  switch (p->last_error_class)
    {
    case BE_MISC:
      err2 = bgp_misc_errors[p->last_error_code];
      break;
    case BE_SOCKET:
      err2 = (p->last_error_code == 0) ? "Connection closed" : strerror(p->last_error_code);
      break;
    case BE_BGP_RX:
    case BE_BGP_TX:
      err2 = bgp_error_dsc(errbuf, p->last_error_code >> 16, p->last_error_code & 0xFF);
      break;
    case BE_AUTO_DOWN:
      err2 = bgp_auto_errors[p->last_error_code];
      break;
    }

  if (P->proto_state == PS_DOWN)
    bsprintf(buf, "%s%s", err1, err2);
  else
    bsprintf(buf, "%-14s%s%s",
	     bgp_state_names[MAX(p->incoming_conn.state, p->outgoing_conn.state)],
	     err1, err2);
}

static int
bgp_reconfigure(struct proto *P, struct proto_config *C)
{
  struct bgp_config *new = (struct bgp_config *) C;
  struct bgp_proto *p = (struct bgp_proto *) P;
  struct bgp_config *old = p->cf;

  int same = !memcmp(((byte *) old) + sizeof(struct proto_config),
		     ((byte *) new) + sizeof(struct proto_config),
		     // password item is last and must be checked separately
		     OFFSETOF(struct bgp_config, password) - sizeof(struct proto_config))
    && ((!old->password && !new->password)
	|| (old->password && new->password && !strcmp(old->password, new->password)));

  /* We should update our copy of configuration ptr as old configuration will be freed */
  if (same)
    p->cf = new;

  return same;
}

struct protocol proto_bgp = {
  name:			"BGP",
  template:		"bgp%d",
  attr_class:		EAP_BGP,
  init:			bgp_init,
  start:		bgp_start,
  shutdown:		bgp_shutdown,
  get_status:		bgp_get_status,
  get_attr:		bgp_get_attr,
  reconfigure:		bgp_reconfigure,
  get_route_info:	bgp_get_route_info,
};
