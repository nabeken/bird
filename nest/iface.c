/*
 *	BIRD -- Management of Interfaces and Neighbor Cache
 *
 *	(c) 1998 Martin Mares <mj@ucw.cz>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

#define LOCAL_DEBUG

#include "nest/bird.h"
#include "nest/iface.h"
#include "nest/protocol.h"
#include "lib/resource.h"

static pool *if_pool;

/*
 *	Neighbor Cache
 *
 *	FIXME: Use hashing to get some real speed.
 *	FIXME: Cleanup when a protocol goes down.
 */

static slab *neigh_slab;
static list neigh_list;

static int
if_connected(ip_addr *a, struct iface *i) /* -1=error, 1=match, 0=no match */
{
  if (i->flags & (IF_ADMIN_DOWN | IF_IGNORE))
    return 0;
  if ((i->flags & IF_UNNUMBERED) && ipa_equal(*a, i->opposite))
    return 1;
  if (!ipa_in_net(*a, i->prefix, i->pxlen))
    return 0;
  if (ipa_equal(*a, i->prefix) ||	/* Network address */
      ipa_equal(*a, i->brd) ||		/* Broadcast */
      ipa_equal(*a, i->ip))		/* Our own address */
    return -1;
  if (!(i->flags & IF_UP))
    return 0;
  return 1;
}

neighbor *
neigh_find(struct proto *p, ip_addr *a, unsigned flags)
{
  neighbor *n;
  int class;
  struct iface *i, *j;

  WALK_LIST(n, neigh_list)
    if (n->proto == p && ipa_equal(*a, n->addr))
      return n;

  class = ipa_classify(*a);
  if (class < 0)			/* Invalid address */
    return NULL;
  if ((class & IADDR_SCOPE_MASK) < SCOPE_SITE ||
      !(class & IADDR_HOST))
    return NULL;			/* Bad scope or a somecast */

  j = NULL;
  WALK_LIST(i, iface_list)
    switch (if_connected(a, i))
      {
      case -1:
	return NULL;
      case 1:
	if (!j || j->pxlen > i->pxlen)
	  j = i;
	/* Fall-thru */
      }
  if (!j && !(flags & NEF_STICKY))
    return NULL;

  n = sl_alloc(neigh_slab);
  n->addr = *a;
  n->iface = j;
  add_tail(&neigh_list, &n->n);
  if (j)
    {
      n->sibling = j->neigh;
      j->neigh = n;
    }
  else
    n->sibling = NULL;
  n->proto = p;
  n->data = NULL;
  n->flags = flags;
  return n;
}

void
neigh_dump(neighbor *n)
{
  debug("%p %08x ", n, _I(n->addr));
  if (n->iface)
    debug("%s ", n->iface->name);
  else
    debug("[] ");
  debug("%s %p", n->proto->name, n->data);
  if (n->flags & NEF_STICKY)
    debug("STICKY");
  debug("\n");
}

void
neigh_dump_all(void)
{
  neighbor *n;

  debug("Known neighbors:\n");
  WALK_LIST(n, neigh_list)
    neigh_dump(n);
  debug("\n");
}

static void
neigh_if_up(struct iface *i)
{
  neighbor *n;

  WALK_LIST(n, neigh_list)
    if (!n->iface &&
	if_connected(&n->addr, i) > 0)
      {
	n->iface = i;
	n->sibling = i->neigh;
	i->neigh = n;
	DBG("Waking up sticky neighbor %08x\n", _I(n->addr));
	n->proto->neigh_notify(n);
      }
}

static void
neigh_if_down(struct iface *i)
{
  neighbor *n, *m;

  for(m=i->neigh; n = m;)
    {
      m = n->sibling;
      DBG("Flushing neighbor %08x on %s\n", _I(n->addr), n->iface->name);
      n->iface = NULL;
      n->proto->neigh_notify(n);
      if (!(n->flags & NEF_STICKY))
	{
	  rem_node(&n->n);
	  sl_free(neigh_slab, n);
	}
      i->neigh = NULL;
    }
}

/*
 *	The Interface List
 */

list iface_list;

void
if_dump(struct iface *i)
{
  debug("IF%d: %s", i->index, i->name);
  if (i->flags & IF_ADMIN_DOWN)
    debug(" ADMIN-DOWN");
  if (i->flags & IF_UP)
    debug(" UP");
  if (i->flags & IF_MULTIACCESS)
    debug(" MA");
  if (i->flags & IF_UNNUMBERED)
    debug(" UNNUM");
  if (i->flags & IF_BROADCAST)
    debug(" BC");
  if (i->flags & IF_MULTICAST)
    debug(" MC");
  if (i->flags & IF_TUNNEL)
    debug(" TUNL");
  if (i->flags & IF_LOOPBACK)
    debug(" LOOP");
  if (i->flags & IF_IGNORE)
    debug(" IGN");
  debug(" MTU=%d\n", i->mtu);
  debug("\t%08x, net %08x/%-2d bc %08x -> %08x\n", _I(i->ip), _I(i->prefix), i->pxlen, _I(i->brd), _I(i->opposite));
}

void
if_dump_all(void)
{
  struct iface *i;

  debug("Known network interfaces:\n\n");
  WALK_LIST(i, iface_list)
    if_dump(i);
  debug("\n");
}

static inline int
if_change_too_big_p(struct iface *i, struct iface *j)
{
  if (!ipa_equal(i->ip, j->ip) ||
      !ipa_equal(i->prefix, j->prefix) ||
      i->pxlen != j->pxlen ||
      !ipa_equal(i->brd, j->brd) ||
      !ipa_equal(i->opposite, j->opposite))
    return 1;				/* Changed addresses */
  if ((i->flags ^ j->flags) & ~(IF_UP | IF_ADMIN_DOWN | IF_UPDATED))
    return 1;
  return 0;
}

static inline void
if_copy(struct iface *to, struct iface *from)
{
  to->flags = from->flags;
  to->mtu = from->mtu;
  to->index = from->index;
}

static unsigned
if_changed(struct iface *i, struct iface *j)
{
  unsigned f = 0;

  if (i->mtu != j->mtu)
    f |= IF_CHANGE_MTU;
  if ((i->flags ^ j->flags) & ~IF_UPDATED)
    {
      f |= IF_CHANGE_FLAGS;
      if ((i->flags ^ j->flags) & IF_UP)
	if (i->flags & IF_UP)
	  f |= IF_CHANGE_DOWN;
	else
	  f |= IF_CHANGE_UP;
    }
  return f;
}

static void
if_notify_change(unsigned c, struct iface *old, struct iface *new)
{
  debug("Interface change notification (%x) for %s\n", c, new->name);
  if (old)
    if_dump(old);
  if (new)
    if_dump(new);

  if (c & IF_CHANGE_UP)
    neigh_if_up(new);

  /* FIXME: Notify protocols here */

  if (c & IF_CHANGE_DOWN)
    neigh_if_down(old);
}

void
if_update(struct iface *new)
{
  struct iface *i;
  unsigned c;

  WALK_LIST(i, iface_list)
    if (!strcmp(new->name, i->name))
      {
	if (if_change_too_big_p(i, new)) /* Changed a lot, convert it to down/up */
	  {
	    DBG("Interface %s changed too much -- forcing down/up transition\n", i->name);
	    i->flags &= ~IF_UP;
	    if_notify_change(IF_CHANGE_DOWN | IF_CHANGE_FLAGS, i, NULL);
	    rem_node(&i->n);
	    goto newif;
	  }
	c = if_changed(i, new);
	if_copy(i, new);		/* Even if c==0 as we might need to update i->index et al. */
	i->flags |= IF_UPDATED;
	if (c)
	  if_notify_change(c, i, new);
	return;
      }

  i = mb_alloc(if_pool, sizeof(struct iface));
newif:
  memcpy(i, new, sizeof(*i));
  i->flags |= IF_UPDATED;
  add_tail(&iface_list, &i->n);
  if_notify_change(IF_CHANGE_UP | IF_CHANGE_FLAGS | IF_CHANGE_MTU, NULL, i);
}

void
if_end_update(void)
{
  struct iface *i, j;

  WALK_LIST(i, iface_list)
    if (i->flags & IF_UPDATED)
      i->flags &= ~IF_UPDATED;
    else
      {
	memcpy(&j, i, sizeof(struct iface));
	i->flags = (i->flags & ~IF_UP) | IF_ADMIN_DOWN;
	if_notify_change(IF_CHANGE_DOWN | IF_CHANGE_FLAGS, &j, i);
      }
}

void
if_init(void)
{
  if_pool = rp_new(&root_pool, "Interfaces");
  init_list(&iface_list);
  neigh_slab = sl_new(if_pool, sizeof(neighbor));
  init_list(&neigh_list);
}