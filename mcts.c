

#include <stdio.h>
#include <math.h>

#include "debug.h"
#include "mcts.h"

#include "alloc-inl.h"

#define mcts_key_cmp(n1, n2) (memcmp((n1).bits, (n2).bits, (MAP_SIZE >> 3)))

KBTREE_INIT(mcts, mcts_key_t, mcts_key_cmp)

extern double UR_1();


static inline u8 read_bit(u8* bits, u32 i){
  return (bits[i>>3] & (128 >> (i & 7))) ? 1 : 0 ;
}


static inline void write_bit(u8* bits, u32 i){
    bits[i >> 3] |= (128 >> (i & 7));
}


extern /*static*/ u64 get_cur_time_us(void);

static const u16 next_p2_lookup[4097] = {

  [0]             = 1,
  [1]             = 1,
  [2]             = 2,
  [3 ... 4]       = 4,
  [5 ... 8]       = 8,
  [9 ... 16]      = 16,
  [17 ... 32]     = 32,
  [33 ... 64]     = 64,
  [65 ... 128]    = 128,
  [129 ... 256]   = 256,
  [257 ... 512]   = 512,
  [513 ... 1024]  = 1024,
  [1025 ... 2048] = 2048,
  [2049 ... 4096] = 4096
};


static inline u64 next_p2(u64 val) {

//   u64 ret = 1;
//   while (val > ret) ret <<= 1;
//   return ret;

    u64 v = val;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    v++;

    v += (v == 0);

    return v;

} 


static inline u64 round_p2(u64 val, u32 bits){
    u64 ret;
    u64 limit = (1 << bits);
    if(val <= limit){
        // if(val <= 2048) ret = next_p2_lookup[val];
        // else ret = next_p2(val);
        ret = next_p2_lookup[val];
    }
    else{
        // ret = ((val - 1) >> bits);
        // ret = ((ret + 1) << bits);
        ret = (val & (~(limit - 1))) + limit;
    }
    return ret;
}

#define FF(_b)  (0xff << ((_b) << 3))
#define FFFF(_b)  (0xff << ((_b) << 4))




static u32 count_bits(u8* mem, u32 size) {

  u32* ptr = (u32*)mem;
  u32  i   = (size >> 2);
  u32  ret = 0;

  while (i--) {

    u32 v = *(ptr++);

    if (v == 0xffffffff) {
      ret += 32;
      continue;
    }

    v -= ((v >> 1) & 0x55555555);
    v = (v & 0x33333333) + ((v >> 2) & 0x33333333);
    ret += (((v + (v >> 4)) & 0xF0F0F0F) * 0x01010101) >> 24;

  }

  return ret;

}


static u32 count_bytes(u8* mem, u32 size) {

  u32* ptr = (u32*)mem;
  u32  i   = (size >> 2);
  u32  ret = 0;

  while (i--) {

    u32 v = *(ptr++);

    if (!v) continue;
    if (v & FF(0)) ret++;
    if (v & FF(1)) ret++;
    if (v & FF(2)) ret++;
    if (v & FF(3)) ret++;

  }

  return ret;

}


static u32 count_words(u16* mem, u32 size) {

  u32* ptr = (u32*)mem;
  u32  i   = (size >> 1);
  u32  ret = 0;

  while (i--) {

    u32 v = *(ptr++);

    if (!v) continue;
    if (v & FFFF(0)) ret++;
    if (v & FFFF(1)) ret++;

  }

  return ret;

}


static inline void minimize_bits(u8* dst, u8* src, u32 size){

    u64* ptr = (u64*)src;
    u32 i;
    u8 v;
    u64 s;
    for(i = 0; i < (size >> 3); i++){
        v = 0;
        s = ptr[i];
        if(afl_unlikely(s)){
            if(s & 0xff)               v |= 128;
            if(s & 0xff00)             v |= 64;
            if(s & 0xff0000)           v |= 32;
            if(s & 0xff000000)         v |= 16;
            if(s & 0xff00000000)       v |= 8;
            if(s & 0xff0000000000)     v |= 4;
            if(s & 0xff000000000000)   v |= 2;
            if(s & 0xff00000000000000) v |= 1;
        }
        dst[i] = v;
    }
}



// static inline void minimize_bits(u8* dst, u8* src, u32 size) {

//   u32 i = 0;

//   while (i < size) {

//     if (*(src++)) dst[i >> 3] |= (128 >> (i & 7));
//     i++;

//   }

// }


static const u8 sim_lookup[256] = { 

  [0]         = 0,
  [1 ... 255] = 1

};

static inline void simplify_bits(u64* dst, u64* src, u32 size){

    u32 i = size >> 3;

    while (i--) {

        /* Optimize for sparse bitmaps. */

        if (afl_unlikely(*src)) {

        u8* src8 = (u8*)src;
        u8* dst8 = (u8*)dst;

        dst8[0] = sim_lookup[src8[0]];
        dst8[1] = sim_lookup[src8[1]];
        dst8[2] = sim_lookup[src8[2]];
        dst8[3] = sim_lookup[src8[3]];
        dst8[4] = sim_lookup[src8[4]];
        dst8[5] = sim_lookup[src8[5]];
        dst8[6] = sim_lookup[src8[6]];
        dst8[7] = sim_lookup[src8[7]];

        } else *dst = 0x0ULL;

        dst++;
        src++;

    }
}





void update_trace_map(mcts_tree_t* tree, u8* trace_bits){
 
    u32 i;
    u8* se_map = tree->trace_map->step_exec_map;
    // u16* e_map = tree->trace_map->exec_map;
    u8* bytes = trace_bits;
    for(i = 0; i < MAP_SIZE * (N_INFI - 1); i++){
        if(bytes[i]){
            if(se_map[i] < 255) se_map[i] += 1;
            // if(e_map[i] < 100000) e_map[i] += 1;                
        }
    }
}



void reset_trace_map(mcts_tree_t* tree){
    memset(tree->trace_map, 0, sizeof(trace_map_t));
}




static queue_cluster_t* new_queue_cluster(mcts_tree_t* tree){
    queue_cluster_t* new = (queue_cluster_t*)malloc(sizeof(queue_cluster_t));
    memset(new, 0, sizeof(queue_cluster_t));
    new->id = tree->n_tnodes[N_LAST];
    return new;
}



static mcts_node_t* new_node(mcts_tree_t* tree, n_gram_t n, mcts_node_t* parent){
    mcts_node_t* nx = (mcts_node_t*) malloc(sizeof(mcts_node_t));
    memset(nx, 0, sizeof(mcts_node_t));

    nx->n_cov_level = n;
    nx->parent = parent;
    nx->id = tree->n_tnodes[0];  
    nx->fuzz_level = 1;  

    if(nx->n_cov_level == N_LAST){
        nx->queue_cluster = new_queue_cluster(tree);
    }else{
        nx->children  = (void*) kb_init(mcts, KB_DEFAULT_SIZE);
    }    
    
    tree->n_tnodes[0]++;
    if(nx->n_cov_level > N_NULL && nx->n_cov_level < N_INFI){
        tree->n_tnodes[nx->n_cov_level]++;
        u8* buf = (u8*)malloc(MAP_SIZE);
        memset(buf, 0, MAP_SIZE);
        nx->fuzz_trace = buf;
    }

    tree->new_node = 1;
    
    return nx;
}



static void delete_node(mcts_node_t* node){

    if(node->queue_cluster){
        free(node->queue_cluster);
        // node->queue_cluster = NULL;
    }
    free(node->fuzz_trace);

    kbtree_t(mcts) *map; 
    if(node->children){
        kbitr_t itr;
        mcts_key_t *p;
        map = (kbtree_t(mcts) *)node->children;
        kb_itr_first(mcts, map, &itr);
        for(; kb_itr_valid(&itr); kb_itr_next(mcts, map, &itr)){
            p = &kb_itr_key(mcts_key_t, &itr);
            free(p->bits);
            delete_node(p->node);
        }
        kb_destroy(mcts, map);
    }

    // if(!node->parent){
    free(node);
    // }
}



mcts_tree_t* new_tree(void){
    mcts_tree_t* tree = (mcts_tree_t*)malloc(sizeof(mcts_tree_t));

    memset(tree, 0, sizeof(mcts_tree_t));

    tree->_rlevel = RLEVEL;

    tree->trace_map = (trace_map_t*)malloc(sizeof(trace_map_t));
    reset_trace_map(tree);

    mcts_node_t* root = new_node(tree, N_NULL, NULL);
    tree->root = root;

    return tree;
}



static void report_tree(mcts_tree_t* tree){
    u32 i;
    char* tags[N_LAST + 1] = {" total", "   ctx", /*"    n0",*/ "    n1", /*"    n2", "   n4", "   n8"*/ "    ma"};
    printf("final tree (#nodes, #bytes):\n");
    printf("  %s:\t %u(%u)\n", tags[0], tree->n_tnodes[0], tree->n_fuzzed_tnodes[0]);
    for(i=N_NULL+1; i <= N_LAST; i++){
        printf("  %s:\t %u(%u)\t %llu(%llu)\n", tags[i], tree->n_tnodes[i], tree->n_fuzzed_tnodes[i], tree->b[(i-1)*2], tree->b[i*2-1]);
    }

    // _report_tree(tree);
}



u8* report_queue_cluster(queue_cluster_t* qc){
    char* res = "";
    struct queue_entry* q = qc->queue;
    while(q){
        res = alloc_printf("%s %u,", res, q->id);
        q = q->local_next;
    }
    return res;
}



u8* report_node(mcts_node_t* node){
    mcts_key_t *p;
    mcts_node_t* nx;
    kbitr_t itr;
    char* res = "{ ";    
    kbtree_t(mcts) *map; 
    map = (kbtree_t(mcts) *)node->children;
    if(map){
        kb_itr_first(mcts, map, &itr);
        int i = 0;       
        for(; kb_itr_valid(&itr); kb_itr_next(mcts, map, &itr)){
            p = &kb_itr_key(mcts_key_t, &itr);
            nx = p->node;
            // printf("%u, ", nx->id);
            u8* nstr = report_node(nx);
            res = alloc_printf("%s [%u](%u) %s", res, i, nx->id, nstr);
            i++;
        }
    }
    else{
        res = alloc_printf("%s <%s>", res, report_queue_cluster(node->queue_cluster));
    }
    res = alloc_printf("%s }", res);
    return res;
}



void _report_tree(mcts_tree_t* tree){
    printf("%s\n", report_node(tree->root));
}



void delete_tree(mcts_tree_t* tree){
    report_tree(tree);
    delete_node(tree->root);
    free(tree->trace_map);
    free(tree);
}








static inline void calc_base_score(mcts_tree_t* tree, mcts_node_t* node){
    trace_map_t* trace_map = tree->trace_map;
    u8* etrace = node->exec_trace;

    u8* step_emap = trace_map->step_exec_map + MAP_SIZE * (node->n_cov_level - 1);
    u32* emap = trace_map->exec_map + MAP_SIZE * (node->n_cov_level - 1);

    double score = 0; 

    u32 i, j, k;
    u8 val;
    u64 cnt;
    u64 num = 0;


    for(i = 0; i < (MAP_SIZE >> 3); i++){
        val = etrace[i];
        for(j = 0; j < 8; j++){
            k = i * 8 + j;
            // cnt = round_p2(emap[k] + val1, 10);
            // cnt = cnt / factor + 1;
            // cnt = next_p2(emap[k] + val1);
            if(val & (128 >> j)){
                cnt = emap[k] + step_emap[k];
                score += ((double) 1.0) / pow(cnt, 2);
                num++;
            }


        }
    }

    score = tree->root->fuzz_level * sqrt(score / num);

    //
    // if(node->n_cov_level == N_LAST) score = 1.0;
    //
    node->base_score = score;

    node->fuzz_score = 1.0;

}




static inline void calc_score(mcts_tree_t* tree){

    // const double w = 0.50;

    if(!tree->leaf_cur) return;

    trace_map_t* trace_map = tree->trace_map;
    u8 *etrace, *step_map;
    u32 *emap;


    double score1;
    double score2 = 1.0;
    double score2x;

    u32 i, j, k;
    u8 val, val1;
    u64 num1, num3;
    u64 num2 = 0;

    u64 cnt, min_cnt, cut_off;

    u64 factor = tree->root->fuzz_level;

    mcts_node_t* nx;

    for(nx = tree->leaf_cur; nx->n_cov_level < N_INFI && nx->n_cov_level > N_NULL; nx = nx->parent){
        etrace = nx->exec_trace;
        step_map = trace_map->step_exec_map + MAP_SIZE * (nx->n_cov_level - 1);
        emap = trace_map->exec_map + MAP_SIZE * (nx->n_cov_level - 1);

        score1 = 0;
        score2x = 0;
        min_cnt = (u64)-1;
        cut_off = (u64)-1;
        num1 = 0;
        // num3 = 0;

        for(i = 0; i < (MAP_SIZE >> 3); i++){
            val = etrace[i];
            for(j = 0; j < 8; j++){
                k = i * 8 + j;

                val1 = step_map[k];
                cnt = emap[k] + val1;

                if(val & (128 >> j)){
                    score1 += ((double) 1.0) / pow(cnt, 2);
                    num1++;
                }

                if(val1){
                    // if(cnt <= cut_off){
                    //     if(cnt * 2 <= cut_off){
                    //         cut_off = next_p2(cnt);
                    //         num3 = 0;
                    //     }
                    //     num3++;
                    // }
                    min_cnt = MIN(min_cnt, cnt);
                }
            }
        }

        score1 = factor * sqrt(score1 / num1);

        score2x = ((double)1.0) / next_p2(min_cnt);
        // score2x = (1+ log(num3)) / cut_off;

        // score2 *= pow(score2x, N_INFI - nx->n_cov_level);
        // num2 += (N_INFI - nx->n_cov_level);
        
        //
        // if(nx->n_cov_level == N_LAST) { 
        //     score1 = 1.0;
        //     score2x = 1.0;
        // }
        //
        score2 *= score2x;
        // score2 = score2x;
        num2 += 1;

        nx->base_score = score1;

        // nx->fuzz_score = pow(score2, 1.0 / num2);

        // double new_acc_w = nx->acc_w * w;

        // nx->fuzz_score = (nx->fuzz_score * new_acc_w + pow(score2, 1.0 / num2)) / (1 + new_acc_w);
        // nx->acc_w = 1 + new_acc_w;

        nx->fuzz_score = (nx->fuzz_score + pow(score2, 1.0 / num2)) / 2;
        
        // fprintf(stderr, ">>>> #%d score1: %f, score2x: %f, score2: %f\n", nx->id, score1, score2x, nx->fuzz_score);

    }
    // score2 = pow(score2, 1.0 / num2);
    // fprintf(stderr, ">>>> score2: %f\n", score2);

    // for(nx = tree->leaf_cur; nx->n_cov_level < N_INFI && nx->n_cov_level > N_NULL; nx = nx->parent){
        // nx->fuzz_score = score2;
    // }

}

n_gram_t add_to_tree(mcts_tree_t* tree, struct queue_entry* queue, u8* trace_bits){

    // printf("add #%d to_tree\n", queue->id);
    mcts_node_t* cur = tree->root;
    kbtree_t(mcts)* map;
    mcts_key_t k, *k_p = &k, *p;
    mcts_node_t *nx;
    n_gram_t i, ret=N_INFI;
    u32 size = (MAP_SIZE >> 3);
    cur->n_seeds++;
    for(i = N_NULL + 1; i <= N_LAST; i++){
        u8* bits = (u8*)malloc(size);
        memset(bits, 0, size);
        minimize_bits(bits, trace_bits + MAP_SIZE * (i - 1), MAP_SIZE);
        k_p->bits = bits;
        map = (kbtree_t(mcts) *)cur->children;
        p = kb_getp(mcts, map, k_p);
        if(!p){
            ret = MIN(ret, i);
            nx = new_node(tree, i, cur);
            nx->exec_trace = bits;
            // simplify_bits((u64*)(nx->fuzz_trace), (u64*)(trace_bits + MAP_SIZE * (i - 1)), MAP_SIZE);
            calc_base_score(tree, nx);

            int j;
            u32 base = MAP_SIZE * (i - 1);
            u8* fuzz_mark = tree->trace_map->fuzz_mark;
            u8* fuzz_trace = trace_bits + MAP_SIZE * (i - 1);
            for(j = 0; j < MAP_SIZE; j++){
                if(fuzz_trace[j] && fuzz_mark[j + base] == 0){
                    fuzz_mark[j + base] = 1;
                    tree->b[2 * (i - 1)] += 1;
                }
            }

            k_p->node = nx;
            p = kb_putp(mcts, map, k_p);
        }
        else free(bits);
        cur = p->node;
        cur->n_seeds ++;
        // printf("  add to %d\n", cur->id);

    }
    // assert(cur->n_cov_level == N_LAST)
    queue_cluster_t* cur_cluster = cur->queue_cluster;

    if(cur_cluster->queue_top){
        cur_cluster->queue_top->local_next = queue;
        cur_cluster->queue_top = queue;
    }
    else{
        cur_cluster->queue = cur_cluster->queue_top = queue;
    }
    queue->local_next = NULL;

    cur_cluster->volumn++;

    return ret;
}








static mcts_node_t* best_child(mcts_tree_t* tree, mcts_node_t* node, double c_param){
    // printf("best child of %llx\n", (u64)node);
    double score, cur_best_score = -1;
    mcts_node_t *cur=NULL, *nx;
    mcts_key_t *p;
    kbitr_t itr;
    kbtree_t(mcts) *map; 
    map = (kbtree_t(mcts) *)node->children;
    kb_itr_first(mcts, map, &itr);

    // u64 factor = tree->root->fuzz_level;
    // u64 factor = 1;
    // double r;
    for(; kb_itr_valid(&itr); kb_itr_next(mcts, map, &itr)){
        p = &kb_itr_key(mcts_key_t, &itr);
        nx = p->node;
        double s1 = nx->base_score;

        double s2 = nx->fuzz_score;

        // u8 min_val = tree->trace_map->fuzz_map[MAP_SIZE * (nx->n_cov_level - 1) + nx->min_fuzz_byte];

        // double s3 = c_param * sqrt(nx->v_score) / sqrt(log((double)node->fuzz_level) / nx->fuzz_level);
        // double s3 = c_param * sqrt(nx->v_score) / sqrt(nx->fuzz_level);

        // double s3 = c_param * sqrt((double)nx->n_seeds / node->n_seeds) * sqrt(log((double)node->fuzz_level) / nx->fuzz_level);
        double s3 = c_param * sqrt(log((double)node->fuzz_level) / nx->fuzz_level);


        // double s4 = nx->v_score;
        // double s3 = c_param * sqrt( MAX(s4 - s2 * s2, 0) ) * sqrt(log((double)node->fuzz_level) / nx->fuzz_level);


        // score = (s1 + s2 + s3) * factor * r * 256 / next_p2_lookup[min_val];
        // score = s1 + s2 + s3; 
        score = s1 * (s2 + s3);


        // fprintf(stderr, "     %.20f, %.20f: %.20f - %.20f %.20f\n", s4 - s2 * s2, sqrt(s4 - s2 * s2), s4, s2 * s2, s2);
        // fprintf(stderr, "    #%d score: %f (%f, %f, %f) (%llu %llu) (%llu, %llu)\n", nx->id, score, s1, s2, s3, nx->n_seeds, node->n_seeds, nx->fuzz_level, node->fuzz_level);
        if( (score > cur_best_score) || (score == cur_best_score && nx->fuzz_level < cur->fuzz_level)){
            cur_best_score = score;
            cur = nx;
            // fprintf(stderr, "  cur_best_score: %f (%f, %f, %f), fuzz_level: %llu/%llu, visit_times: %llu / %llu\n", score, s1, s2, s3, nx->fuzz_level , tree->root->fuzz_level , node->visit_times, nx->visit_times);
            // fprintf(stderr, "  cur best child: %d, %f\n", cur->id, cur_best_score);
        }
        
    }

    // fprintf(stderr, "best child: %d, best score: %f\n", cur->id, cur_best_score);
    return cur;
}







static mcts_node_t* best_leaf_node(mcts_tree_t* tree, double c_param){
    
    mcts_node_t* cur = tree->root;
    while(cur->n_cov_level != N_LAST){
        cur = best_child(tree, cur, c_param);
    }
    return cur;
}


 

static void update_reward_0(mcts_tree_t* tree){

    u64 t1 = get_cur_time_us();

    u32 i;
    u32* bytes = tree->trace_map->exec_map;
    u8* step_bytes = tree->trace_map->step_exec_map;
    u8 val;


    mcts_node_t *nx = tree->leaf_cur;
    u64 mutation_times = tree->mutation_times;

    while(nx){
        if(nx->fuzz_level == 1){
            tree->n_fuzzed_tnodes[nx->n_cov_level]++;
            tree->n_fuzzed_tnodes[0]++;
        }
        nx->fuzz_level++;
        nx->visit_times += mutation_times;

        nx = nx->parent;
    }

    calc_score(tree);
    
    for(i = 0; i < MAP_SIZE * (N_INFI - 1); i++){
        val = step_bytes[i];
        if(val) bytes[i] += val; // = MIN(100000, bytes[i] + val);
    }

    memset(step_bytes, 0, MAP_SIZE * (N_INFI - 1));
    tree->mutation_times = 0;

    tree->t[2] += (get_cur_time_us() - t1);
    
}




struct queue_entry* next_queue_cur(mcts_tree_t* tree, double c_param){

    static u32 num = 0;

    // if(num){
    
    update_reward_0(tree);
    
    if(tree->new_node){
        report_tree(tree);
        tree->new_node = 0;
    }
    
    // if(num){
    //     exit(0);
    // }    
    mcts_node_t* nx = best_leaf_node(tree, c_param);
    tree->leaf_cur = nx;

    queue_cluster_t* qc = nx->queue_cluster;    
    struct queue_entry* q = qc->queue_cur;
    if(!q){
        q = qc->queue;
        qc->queue_cycle++;
    }
    qc->queue_cur = q->local_next;
    
    // printf("%u' entering cycle %llu of queue cluster %u\n", num, qc->queue_cycle, qc->id+1);
    // fprintf(stderr, "%u' test case #%d\n\n", num, q->id);
    num++;

    return q;
}




