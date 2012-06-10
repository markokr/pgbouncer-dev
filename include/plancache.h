
#define PLAN_NAME_SIZE 28

/* lookup by name */
struct ClientPlan {
	struct AANode head;
	struct PlanBody *plan;
	char name[PLAN_NAME_SIZE];
};

/* lookup by ptr value */
struct ServerPlan {
	struct AANode head;
	//uint32_t ptr_hash;
	struct PlanBody *plan;
};

/* lookup by hash + body */
struct PlanBody {
	struct AANode head;
	int refcnt;

	/* ptrs to inside pkt */
	const char *name;
	uint8_t *body;
	uint32_t bodyhash;
	uint32_t bodylen;

	uint8_t *pkt;
	/* final pkt len, during loading - allocated buf size */
	uint32_t pktlen;

#define tmp_plan_write_pos	bodyhash
};

enum LastPlanState {
	NO_PLAN,
	CL_PARSE_RECV,	/* receiving plan from client, body not in global cache */
	CL_PARSE_SEND,	/* body in global cache, client_plan not in client->plan_cache */
	CL_PARSE_OK,	/* cache of last parse */
	SV_PARSE_SEND,	/* new plan for server, send result to client */
	SV_PARSE_OK,	/* cache of last parse */
};

typedef struct ClientPlan ClientPlan;
typedef struct ServerPlan ServerPlan;
typedef struct PlanBody PlanBody;
typedef struct PlanCache PlanCache;

struct PlanCache {
	struct AATree cache;	/* already accepted, working plans */


	/* remember in-progress plans */
	enum LastPlanState last_state; 
	
	/* if parse is in progres, the plan does not actually
	 * exist in client and server trees, ->last.
	 * is the only reference.
	 *
	 * It will be added to trees when ParseComplete arrives.
	 */
	int parse_in_progress;
	union {
		ClientPlan *client_plan;
		ServerPlan *server_plan;
	} last;
};

struct ClientPlan * client_plan_lookup(struct PgSocket *client, const char *name);
struct ServerPlan * server_plan_lookup(struct PgSocket *server, struct PlanBody *plan);

/*
struct ClientPlan * register_client_plan(struct PgSocket *client, const char *name, const uint8_t *body, int bodylen)  _MUSTCHECK;
struct ServerPlan * register_server_plan(struct PgSocket *server, struct PlanBody *plan)  _MUSTCHECK;
*/

bool client_deallocate(struct PgSocket *client, const char *name)  _MUSTCHECK;
void client_deallocate_all(struct PgSocket *client);
void server_deallocate_all(struct PgSocket *server);

void init_client_plan_cache(struct PgSocket *client);
void init_server_plan_cache(struct PgSocket *client);

void init_plan_cache(void);

bool plan_load_start(PgSocket *client, uint32_t pkt_len, const char *plan_name)  _MUSTCHECK;
bool plan_load_part(PgSocket *client, struct MBuf *pkt_data) _MUSTCHECK;

