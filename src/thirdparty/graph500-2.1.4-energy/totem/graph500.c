/* -*- mode: C; mode: folding; fill-column: 70; -*- */
/* Copyright 2010,  Georgia Institute of Technology, USA. */
/* See COPYING for license. */
#include "compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

#include <assert.h>

#include <alloca.h> /* Portable enough... */
#include <fcntl.h>
/* getopt should be in unistd.h */
#include <unistd.h>

#if !defined(__MTA__)
#include <getopt.h>
#endif

#include "graph500.h"
#include "rmat.h"
#include "kronecker.h"
#include "verify.h"
#include "prng.h"
#include "timer.h"
#include "xalloc.h"
#include "options.h"
#include "generator/splittable_mrg.h"
#include "generator/graph_generator.h"
#include "generator/make_graph.h"

static int64_t nvtx_scale;

static int64_t bfs_root[NBFS_max];
static int64_t max_bfsvtx[NBFS_max];

static double generation_time;
static double construction_time;
static double bfs_time[NBFS_max];
static int64_t bfs_nedge[NBFS_max];

static packed_edge * restrict IJ;
static int64_t nedge;

static void get_roots();
static void create_graph();
static void run_bfs(void);
void verify_all();
static void output_results(const int64_t SCALE, int64_t nvtx_scale,
                           int64_t edgefactor,
                           const double A, const double B,
                           const double C, const double D,
                           const double generation_time,
                           const double construction_time,
                           const int NBFS,
                           const double *bfs_time, const int64_t *bfs_nedge);

extern char* tmp_dump_path;

static const char* get_edgelist_dumpname() {
  if (dumpname) {
    return dumpname;
  }
  static char edgelist_dumpname[200];
  sprintf(edgelist_dumpname, "%s/edgelist%lld", tmp_dump_path, SCALE);
  return edgelist_dumpname;
}

static void dump_edgelist() {
  if (dumpname) {
    if (VERBOSE) fprintf(stderr, "Dumping edgelist to %s, already exists and "
                         "nothing to do\n", get_edgelist_dumpname());
    return;
  }

  if (VERBOSE) fprintf(stderr, "Dumping edgelist to %s...",
                       get_edgelist_dumpname());
  FILE* p_file = fopen(get_edgelist_dumpname(), "wb");
  if(p_file == NULL) {
    fprintf(stderr, "Cannot open edgelist file for write : %s\n",
            get_edgelist_dumpname());
    abort();
  }

  size_t edgelist_size = nedge * sizeof(*IJ);
  if (edgelist_size != fwrite(IJ, 1, edgelist_size, p_file)) {
    fprintf(stderr, "Error dumping edgelist file: %s",
            get_edgelist_dumpname());
    abort();
  }
  fclose(p_file);
  if (VERBOSE) fprintf(stderr, "done.\n");
}

static void load_edgelist() {
  FILE* p_file = fopen(get_edgelist_dumpname(), "rb");
  if(p_file == NULL) {
    fprintf(stderr, "Cannot open input file : %s\n", get_edgelist_dumpname());
    abort();
  }

  if (VERBOSE) fprintf(stderr, "Loading edgelist: %s\n",
                       get_edgelist_dumpname());
  if (VERBOSE) fprintf(stderr, "Figuring out graph size...\n");
  fseek(p_file, 0 , SEEK_END);
  size_t file_size = ftell(p_file);
  rewind(p_file);
  IJ = xmalloc_large_ext(file_size);
  nedge = file_size / sizeof(*IJ);
  if (VERBOSE) fprintf(stderr, "done: %llu edges\n", nedge);

  if (VERBOSE) fprintf(stderr, "Reading edge list from %s...",
                       get_edgelist_dumpname());
  if (file_size != fread(IJ, 1, file_size, p_file)) {
    perror("Error reading input graph file");
    abort();
  }
  fclose(p_file);
  if (VERBOSE) fprintf(stderr, " done.\n");
}

static void get_edgelist(int64_t desired_nedge) {
  if (!dumpname) {
    if (VERBOSE) fprintf(stderr, "Generating edge list...\n");
    if (use_RMAT) {
      nedge = desired_nedge;
      IJ = xmalloc_large_ext(nedge * sizeof(*IJ));
      TIME(generation_time, rmat_edgelist(IJ, nedge, SCALE, A, B, C));
    } else {
      TIME(generation_time, make_graph(SCALE, desired_nedge, userseed,
                                       userseed, &nedge,
                                       (packed_edge**)(&IJ)));
    }
    if (VERBOSE) fprintf(stderr, " done.\n");
  } else {
    load_edgelist();
  }
}

int main(int argc, char **argv) {
  if (sizeof(int64_t) < 8) {
    fprintf(stderr, "No 64-bit support.\n");
    return EXIT_FAILURE;
  }
  if (argc > 1) {
    get_options(argc, argv);
  }
  init_random();

  nvtx_scale = ((int64_t)1) << SCALE;
  int64_t desired_nedge = nvtx_scale * edgefactor - 1;
  assert(desired_nedge >= nvtx_scale);
  assert(desired_nedge >= edgefactor);

  get_edgelist(desired_nedge);
  dump_edgelist();
  get_roots();
  create_graph(); // Will free IJ.
  run_bfs();
  destroy_graph();
  load_edgelist();
  verify_all();
  xfree_large(IJ);
  output_results(SCALE, nvtx_scale, edgefactor, A, B, C, D, generation_time,
                 construction_time, NBFS, bfs_time, bfs_nedge);

  return EXIT_SUCCESS;
}

static void create_graph() {
  if (VERBOSE) fprintf(stderr, "Creating graph...\n");
  int err;
  TIME(construction_time, err = create_graph_from_edgelist(IJ, nedge));
  if (VERBOSE) fprintf(stderr, " done.\n");
  if (err) {
    fprintf(stderr, "Failure creating graph.\n");
    exit(EXIT_FAILURE);
  }
}

static void get_roots() {
  // If running the benchmark under an architecture simulator, replace
  // the following if () {} else {} with a statement pointing bfs_root
  // to wherever the BFS roots are mapped into the simulator's memory.
  if (!rootname) {
    int* restrict has_adj = xmalloc_large(nvtx_scale * sizeof(*has_adj));
    OMP("omp parallel") {
      OMP("omp for")
      for (int64_t k = 0; k < nvtx_scale; ++k)
        has_adj[k] = 0;
      MTA("mta assert nodep") OMP("omp for")
      for (int64_t k = 0; k < nedge; ++k) {
        const int64_t i = get_v0_from_edge(&IJ[k]);
        const int64_t j = get_v1_from_edge(&IJ[k]);
        if (i != j)
          has_adj[i] = has_adj[j] = 1;
      }
    }

    /* Sample from {0, ..., nvtx_scale-1} without replacement. */
    int m = 0;
    int64_t t = 0;
    while (m < NBFS && t < nvtx_scale) {
      double R = mrg_get_double_orig(prng_state);
      if (!has_adj[t] || (nvtx_scale - t)*R > NBFS - m) ++t;
      else bfs_root[m++] = t++;
    }
    if (t >= nvtx_scale && m < NBFS) {
      if (m > 0) {
        fprintf(stderr, "Cannot find %d sample roots of non-self degree"
                " > 0, using %d.\n",
                 NBFS, m);
        NBFS = m;
      } else {
        fprintf(stderr, "Cannot find any sample roots of non-self "
                "degree > 0.\n");
        exit(EXIT_FAILURE);
      }
    }
    xfree_large(has_adj);
  } else {
    int fd;
    ssize_t sz;
    if ((fd = open(rootname, O_RDONLY)) < 0) {
      perror("Cannot open input BFS root file");
      exit(EXIT_FAILURE);
    }
    sz = NBFS * sizeof(*bfs_root);
    if (sz != read(fd, bfs_root, sz)) {
      perror("Error reading input BFS root file");
      exit(EXIT_FAILURE);
    }
    close(fd);
  }
}

const char* get_tree_dumpname(int64_t root) {
  static char tree_file_name[200];
  sprintf(tree_file_name, "%s/scale%lld-root%lld", tmp_dump_path, SCALE,
          root);
  return tree_file_name;
}

static void dump_tree(int64_t root, tree_t* tree) {
  if (VERBOSE) fprintf(stderr, "dumping tree: %s\n",
                       get_tree_dumpname(root));
  FILE* p_file = fopen(get_tree_dumpname(root), "wb");
  assert(p_file);
  size_t tree_size = sizeof(*tree) * nvtx_scale;
  if (tree_size != fwrite(tree, 1, tree_size, p_file)) {
    fprintf(stderr, "Error dumping bfs tree file: %s",
            get_tree_dumpname(root));
    abort();
  }
  fclose(p_file);
}

static void load_tree(int64_t root, tree_t* tree) {
  if (VERBOSE) fprintf(stderr, "loading tree: %s\n",
                       get_tree_dumpname(root));
  size_t tree_size = sizeof(*tree) * nvtx_scale;
  FILE* p_file = fopen(get_tree_dumpname(root), "rb");
  if (tree_size != fread(tree, 1, tree_size, p_file)) {
    fprintf(stderr, "Error reading bfs tree file: %s",
            get_tree_dumpname(root));
    abort();
  }
  fclose(p_file);
}

static void convert_tree(tree_t* tree, int64_t* converted_tree) {
  OMP("omp for")
  for (int64_t v = 0; v < nvtx_scale; v++) {
    converted_tree[v] = (int64_t)tree[v];
    if (tree[v] == (tree_t)-1) { converted_tree[v] = (int64_t)-1; }
  }
}

void verify_all() {
  if (no_verify) {
    // Skip verification, use number of edges as edges traversed.
    for (int m = 0; m < NBFS; m++) {
      bfs_nedge[m] = nedge;
    }
    return;
  }

  tree_t* tree = xmalloc_large(nvtx_scale * sizeof(tree_t));
  int64_t* converted_tree = xmalloc_large(nvtx_scale * sizeof(int64_t));
  for (int m = 0; m < NBFS; ++m) {
    load_tree(bfs_root[m], tree);
    convert_tree(tree, converted_tree);
    if (VERBOSE) fprintf(stderr, "Verifying bfs %d...\n", m);
    bfs_nedge[m] = verify_bfs_tree(converted_tree, max_bfsvtx[m], bfs_root[m],
                                   IJ, nedge);
    if (VERBOSE) fprintf(stderr, "done\n");
    if (bfs_nedge[m] < 0) {
      fprintf(stderr,
              "bfs %d from %" PRId64 " failed verification (%" PRId64 ")\n",
               m, bfs_root[m], bfs_nedge[m]);
      abort();
    }
  }
  xfree_large(converted_tree);
  xfree_large(tree);
}

static void run_bfs(void) {
  for (int m = 0; m < NBFS; ++m) {
    tree_t* bfs_tree;

    /* Re-allocate. Some systems may randomize the addres... */
    bfs_tree = xmalloc_large(nvtx_scale * sizeof(*bfs_tree));
    // Force the allocation of the buffer by writing to all its pages (the
    // value being written can be anything).
    memset(bfs_tree, 1, nvtx_scale * sizeof(*bfs_tree));
    assert(bfs_root[m] < nvtx_scale);

    if (VERBOSE) fprintf(stderr, "Running bfs %d...", m);
    bfs_time[m] = make_bfs_tree(bfs_tree, &max_bfsvtx[m], bfs_root[m]);
    if (VERBOSE) fprintf(stderr, "done\n");

    // Check for NaN or negative time.
    if (bfs_time[m] != bfs_time[m] || bfs_time[m] < 0.0) {
      perror("make_bfs_tree failed");
      abort();
    }

    dump_tree(bfs_root[m], bfs_tree);
    xfree_large(bfs_tree);
  }
}

#define NSTAT 9
#define PRINT_STATS(lbl, israte)                                \
  do {                                                          \
    printf ("min_%s: %20.17e\n", lbl, stats[0]);                \
    printf ("firstquartile_%s: %20.17e\n", lbl, stats[1]);      \
    printf ("median_%s: %20.17e\n", lbl, stats[2]);             \
    printf ("thirdquartile_%s: %20.17e\n", lbl, stats[3]);      \
    printf ("max_%s: %20.17e\n", lbl, stats[4]);                \
    if (!israte) {                                              \
      printf ("mean_%s: %20.17e\n", lbl, stats[5]);             \
      printf ("stddev_%s: %20.17e\n", lbl, stats[6]);           \
    } else {                                                    \
      printf ("harmonic_mean_%s: %20.17e\n", lbl, stats[7]);    \
      printf ("harmonic_stddev_%s: %20.17e\n", lbl, stats[8]);  \
    }                                                           \
  } while (0)


static int dcmp(const void *a, const void *b) {
  const double da = *(const double*)a;
  const double db = *(const double*)b;
  if (da > db) return 1;
  if (db > da) return -1;
  if (da == db) return 0;
  fprintf(stderr, "No NaNs permitted in output.\n");
  abort();
  return 0;
}

void statistics(double *out, double *data, int64_t n) {
  long double s, mean;
  double t;
  int k;

  /* Quartiles */
  qsort(data, n, sizeof(*data), dcmp);
  out[0] = data[0];
  t = (n+1) / 4.0;
  k = (int) t;
  if (t == k)
    out[1] = data[k];
  else
    out[1] = 3*(data[k]/4.0) + data[k+1]/4.0;
  t = (n+1) / 2.0;
  k = (int) t;
  if (t == k)
    out[2] = data[k];
  else
    out[2] = data[k]/2.0 + data[k+1]/2.0;
  t = 3*((n+1) / 4.0);
  k = (int) t;
  if (t == k)
    out[3] = data[k];
  else
    out[3] = data[k]/4.0 + 3*(data[k+1]/4.0);
  out[4] = data[n-1];

  s = data[n-1];
  for (k = n-1; k > 0; --k)
    s += data[k-1];
  mean = s/n;
  out[5] = mean;
  s = data[n-1] - mean;
  s *= s;
  for (k = n-1; k > 0; --k) {
    long double tmp = data[k-1] - mean;
    s += tmp * tmp;
  }
  out[6] = sqrt(s/(n-1));

  s = (data[0]? 1.0L/data[0] : 0);
  for (k = 1; k < n; ++k)
    s += (data[k]? 1.0L/data[k] : 0);
  out[7] = n/s;
  mean = s/n;

  /*
    Nilan Norris, The Standard Errors of the Geometric and Harmonic
    Means and Their Application to Index Numbers, 1940.
    http://www.jstor.org/stable/2235723
  */
  s = (data[0]? 1.0L/data[0] : 0) - mean;
  s *= s;
  for (k = 1; k < n; ++k) {
    long double tmp = (data[k]? 1.0L/data[k] : 0) - mean;
    s += tmp * tmp;
  }
  s = (sqrt(s)/(n-1)) * out[7] * out[7];
  out[8] = s;
}

void output_results(const int64_t SCALE, int64_t nvtx_scale, int64_t edgefactor,
                    const double A, const double B, const double C,
                    const double D, const double generation_time,
                    const double construction_time,
                    const int NBFS, const double *bfs_time,
                    const int64_t *bfs_nedge) {
  int k;
  int64_t sz;
  double *tm;
  double *stats;

  tm = alloca(NBFS * sizeof(*tm));
  stats = alloca(NSTAT * sizeof(*stats));
  if (!tm || !stats) {
    perror ("Error allocating within final statistics calculation.");
    abort();
  }

  sz = (1L << SCALE) * edgefactor * 2 * sizeof(int64_t);
  printf("SCALE: %" PRId64 "\nnvtx: %" PRId64 "\nedgefactor: %" PRId64 "\n"
         "terasize: %20.17e\n", SCALE, nvtx_scale, edgefactor, sz/1.0e12);
  printf("A: %20.17e\nB: %20.17e\nC: %20.17e\nD: %20.17e\n", A, B, C, D);
  printf("generation_time: %20.17e\n", generation_time);
  printf("construction_time: %20.17e\n", construction_time);
  printf("nbfs: %d\n", NBFS);

  memcpy(tm, bfs_time, NBFS * sizeof(tm[0]));
  statistics(stats, tm, NBFS);
  PRINT_STATS("time", 0);

  for (k = 0; k < NBFS; ++k)
    tm[k] = bfs_nedge[k];
  statistics(stats, tm, NBFS);
  PRINT_STATS("nedge", 0);

  for (k = 0; k < NBFS; ++k)
    tm[k] = bfs_nedge[k] / bfs_time[k];
  statistics(stats, tm, NBFS);
  PRINT_STATS("TEPS", 1);
}
