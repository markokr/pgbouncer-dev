
#include "bouncer.h"



/*
 * global caches
 */

static struct AATree plan_body_tree;

static struct Slab *plan_body_store;
static struct Slab *server_plan_store;
static struct Slab *client_plan_store;

/*
 * global plan cache refcounting.
 */

static void inc_refcnt(struct PlanBody *plan)
{
	Assert(plan->refcnt >= 0);
	plan->refcnt++;
}

static void dec_refcnt(struct PlanBody *plan)
{
	Assert(plan->refcnt > 0);

	plan->refcnt--;
	if (plan->refcnt == 0)
		aatree_remove(&plan_body_tree, (uintptr_t)plan);
}

/*
 * tree comparision functions.
 */

/* args: char *, ClientPlan */
static int cmp_client_plan(uintptr_t value, struct AANode *node)
{
	const char *name = (const char *)value;
	struct ClientPlan *cplan = container_of(node, struct ClientPlan, head);
	return strcmp(name, cplan->name);
}

/* args: PlanBody, ServerPlan */
static int cmp_server_plan(uintptr_t value, struct AANode *node)
{
	ServerPlan *needle = (ServerPlan*)value;
	struct ServerPlan *splan = container_of(node, struct ServerPlan, head);

	//if (needle->ptr_hash != splan->ptr_hash)
	//	return (needle->ptr_hash > splan->ptr_hash) ? 1 : -1;
	if (needle->plan == splan->plan)
		return 0;
	else
		return (needle->plan > splan->plan) ? 1 : -1;
}

/* args: PlanBody, PlanBody */
static int cmp_plan_body(uintptr_t value, struct AANode *node)
{
	struct PlanBody *need_plan = (struct PlanBody *)value;
	struct PlanBody *cur_plan = container_of(node, struct PlanBody, head);

	/* first compare hashes */
	if (need_plan->bodyhash != cur_plan->bodyhash)
		return (need_plan->bodyhash > cur_plan->bodyhash) ? 1 : -1;

	/* then contents */
	return memcmp(need_plan->body, cur_plan->body, cur_plan->bodylen);
}

/*
 * Release of plan objects.
 */

static void drop_client_plan(struct AANode *node, void *arg)
{
	struct ClientPlan *cplan = container_of(node, struct ClientPlan, head);
	dec_refcnt(cplan->plan);
	cplan->name[0] = 0;
	slab_free(client_plan_store, cplan);
}

static void drop_server_plan(struct AANode *node, void *arg)
{
	struct ServerPlan *splan = container_of(node, struct ServerPlan, head);
	dec_refcnt(splan->plan);
	slab_free(server_plan_store, splan);
}

static void drop_plan_body(struct AANode *node, void *arg)
{
	struct PlanBody *plan = container_of(node, struct PlanBody, head);

	Assert(plan->refcnt == 0);

	if (plan->pkt)
		free(plan->pkt);
	slab_free(plan_body_store, plan);
}

/*
 * Per-socket tree init.
 */

void init_client_plan_cache(PgSocket *client)
{
	aatree_init(&client->plan_cache.cache, cmp_client_plan, drop_client_plan);
}

void init_server_plan_cache(PgSocket *server)
{
	aatree_init(&server->plan_cache.cache, cmp_server_plan, drop_server_plan);
}

/*
 * global plan cache init.
 */

void init_plan_cache(void)
{
	plan_body_store = slab_create("plan_body_store", sizeof(struct PlanBody), 0, NULL, USUAL_ALLOC);
	client_plan_store = slab_create("client_plan_store", sizeof(struct ClientPlan), 0, NULL, USUAL_ALLOC);
	server_plan_store = slab_create("server_plan_store", sizeof(struct ServerPlan), 0, NULL, USUAL_ALLOC);

	aatree_init(&plan_body_tree, cmp_plan_body, drop_plan_body);
}

/*
 * Lookup functions
 */

static struct PlanBody *plan_body_lookup(uint32_t hash, uint8_t *body, int bodylen)
{
	struct PlanBody tmp;
	struct AANode *node;

	tmp.bodyhash = hash;
	tmp.body = body;
	tmp.bodylen = bodylen;
	node = aatree_search(&plan_body_tree, (uintptr_t)&tmp);
	return node ? container_of(node, struct PlanBody, head) : NULL;
}

struct ClientPlan *client_plan_lookup(struct PgSocket *client, const char *name)
{
	struct AANode *node;
	ClientPlan *cplan = client->plan_cache.last.client_plan;
	if (cplan && !strcmp(cplan->name, name))
		return cplan;
	if (cplan)
		slog_warning(client, "stale plan");
	node = aatree_search(&client->plan_cache.cache, (uintptr_t)name);
	return node ? container_of(node, struct ClientPlan, head) : NULL;
}

struct ServerPlan *server_plan_lookup(struct PgSocket *server, struct PlanBody *plan)
{
	struct AANode *node;
	ServerPlan *splan = server->plan_cache.last.server_plan;
	ServerPlan tmp;
	if (splan && splan->plan == plan)
		return splan;
	if (splan)
		slog_warning(server, "stale plan");
	tmp.plan = plan;
	//tmp.ptr_hash = ptr_hash32(plan);
	//tmp.ptr_hash = (uintptr_t)(plan);
	node = aatree_search(&server->plan_cache.cache, (uintptr_t)&tmp);
	return node ? container_of(node, struct ServerPlan, head) : NULL;
}

/*
 * PLan deallocation
 */

bool client_deallocate(struct PgSocket *client, const char *name)
{
	/* fixme: check of plan exists? */
	aatree_remove(&client->plan_cache.cache, (long)name);
	return true;
}

void client_deallocate_all(struct PgSocket *client)
{
	aatree_destroy(&client->plan_cache.cache);
}

void server_deallocate_all(struct PgSocket *server)
{
	aatree_destroy(&server->plan_cache.cache);
}

/*
 * plan creation
 */

static const char *gen_new_server_plan_name(void)
{
	static char namebuf[32];
	static int plan_nr_cntr = 0;
	snprintf(namebuf, sizeof(namebuf), "P%08d", plan_nr_cntr++);
	return namebuf;
}

static inline void register_server_plan(PgSocket *server, ServerPlan *splan)
{
	splan->head.right = NULL; /* hack */
	aatree_insert(&server->plan_cache.cache, (long)splan, &splan->head);
	Assert(splan->head.right);
}

static void register_global_plan(PlanBody *plan)
{
	plan->head.right = NULL; /* hack */
	aatree_insert(&plan_body_tree, (long)plan, &plan->head);
	Assert(plan->head.right);
}

static void register_client_plan(PgSocket *client, ClientPlan *cplan)
{
	cplan->head.right = NULL; /* hack */
	aatree_insert(&client->plan_cache.cache, (long)cplan->name, &cplan->head);
	Assert(cplan->head.right);
}


bool plan_load_start(PgSocket *client, uint32_t pkt_len, const char *plan_name)
{
	/* fixme: free canceled plan */
	client->plan_cache.parse_in_progress = true;
	client->plan_cache.last.client_plan = NULL;
	slog_warning(client, "plan_load_start: tagged client for loading");
	return true;
}

static void load_plan_data(PgSocket *client, struct MBuf *buf)
{
	PlanCache *cache = &client->plan_cache;
	unsigned len;
	const uint8_t *data = NULL;
	uint8_t *dst;
	PlanBody *body;

	body = cache->last.client_plan->plan;
	dst = body->body + body->tmp_plan_write_pos;
	len = mbuf_avail_for_read(buf);
	if (!mbuf_get_bytes(buf, len, &data)) {}
	memcpy(dst, data, len);

	slog_warning(client, "load_plan_data: len=%d, remain=%d",
		     len, body->bodylen - body->tmp_plan_write_pos);

	body->tmp_plan_write_pos += len;
}

static bool init_plan_loading(PgSocket *client, struct MBuf *pkt_data)
{
	PlanCache *cache = &client->plan_cache;
	unsigned alloc, remain;
	uint8_t *buf = NULL;
	ClientPlan *cplan = NULL;
	PlanBody *body = NULL;
	PktHdr pkt;
	const char *name;

	slog_warning(client, "init_plan_loading: preparing");

	if (!get_header(pkt_data, &pkt))
		fatal("second header parse failed?");
	if (!mbuf_get_string(&pkt.data, &name))
		fatal("second packet parse failed?");

	remain = pkt.len - mbuf_consumed(&pkt.data);
	alloc = NEW_HEADER_LEN + PLAN_NAME_SIZE + 1 + remain;

	slog_warning(client, "init_plan_loading: name=%s, remain=%u, alloc=%u, inbuf=%u",
		     name, remain, alloc, mbuf_avail_for_read(&pkt.data));

	buf = malloc(alloc);
	body = slab_alloc(plan_body_store);
	cplan = slab_alloc(client_plan_store);
	if (!body || !cplan || !buf)
		goto nomem;

	slog_warning(client, "init_plan_loading: initializing");
	body->pkt = buf;
	body->pktlen = alloc;
	body->body = buf + alloc - remain;
	body->bodylen = remain;
	body->tmp_plan_write_pos = 0;

	safe_strcpy(cplan->name, name, sizeof(cplan->name));
	cplan->plan = body;
	cache->last.client_plan = cplan;

	slog_warning(client, "init_plan_loading: done");

	/* pkt already aquired all data */
	load_plan_data(client, &pkt.data);
	
	return true;

nomem:
	if (buf)
		free(buf);
	if (cplan)
		slab_free(client_plan_store, cplan);
	if (body)
		slab_free(plan_body_store, body);
	disconnect_client(client, true, "No memory for plan");
	return false;
}

static bool finish_plan_loading(PgSocket *client)
{
	PlanCache *cache = &client->plan_cache;
	ClientPlan *cplan = cache->last.client_plan;
	ServerPlan *splan = NULL;
	PlanBody *body = cplan->plan;
	PktBuf pkt;
	uint32_t hash = hash_lookup3(body->body, body->bodylen);
	const char *srv_name;
	bool ok;

	slog_warning(client, "finish_plan_loading: start");

	body = plan_body_lookup(hash, body->body, body->bodylen);
	if (body) {
		/* free temp one */
		free(cplan->plan->pkt);
		slab_free(plan_body_store, cplan->plan);

		splan = server_plan_lookup(client->link, body);
	} else {
		body = cplan->plan;
		srv_name = gen_new_server_plan_name();
		pktbuf_static(&pkt, body->pkt, body->pktlen);
		pktbuf_start_packet(&pkt, 'P');
		pktbuf_put_string(&pkt, srv_name);
		pktbuf_move_bytes(&pkt, body->body, body->bodylen);
		pktbuf_finish_packet(&pkt);

		body->pktlen = pktbuf_written(&pkt);
		body->bodyhash = hash;
		body->name = (char *)body->pkt + NEW_HEADER_LEN;
		body->refcnt = 1;

		register_global_plan(body);
		splan = NULL;
	}
	inc_refcnt(body);
	cplan->plan = body;

	if (splan) {
		/* FIXME: answer */
		ok = sbuf_answer(&client->sbuf, "1\0\0\0\4", 5);
		if (!ok)
			disconnect_client(client, true, "packet send failed");
		else
			register_client_plan(client, cplan);
		return ok;
	} else {
		/* FIXME: answer */
		ok = sbuf_answer(&client->link->sbuf, cplan->plan->pkt, cplan->plan->pktlen);
		if (!ok)
			disconnect_client(client, true, "packet send failed");
		return ok;
	}
}

bool plan_load_part(PgSocket *client, struct MBuf *pkt_data)
{
	PlanCache *cache = &client->plan_cache;
	PlanBody *body;
	bool ok;

	if (cache->last_state == NO_PLAN) {
		ok = init_plan_loading(client, pkt_data);
		if (!ok)
			return ok;
	} else
		load_plan_data(client, pkt_data);

	body = cache->last.client_plan->plan;

	if (body->tmp_plan_write_pos == body->bodylen)
		return finish_plan_loading(client);

	slog_warning(client, "plan_load_part: %d", __LINE__ );
	return true;
}

