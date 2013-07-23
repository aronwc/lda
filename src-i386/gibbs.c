#include <R.h>
#include <Rinternals.h>
#include <Rdefines.h>
#include <Rmath.h>

#define sLDA 1
#define corrLDA 2
#define prodLDA 3

double dv_update(SEXP annotations, int dd,
		 double beta_z, double var,
		 int nw, int method, int logistic) {
  if (method == sLDA) {
    return beta_z / nw;
  } else if (method == corrLDA) {
    if (logistic) {
      double x_d = LOGICAL(annotations)[dd] * 2 - 1;
      return 1.0 / (1.0 + exp(-x_d * beta_z)) / nw;
    } else {
      double x_d = REAL(annotations)[dd];
      return exp(-(x_d - beta_z) * (x_d - beta_z) / (2 * var)) / nw;
    }
  } else if (method == prodLDA) {
    error("Not implemented.");
  }
}

#define CHECK(VAL, TYPE) if (!is##TYPE(VAL)) { \
    error(#VAL " must be a(n) " #TYPE "."); \
  }

#define CHECKLEN(VAL, TYPE, LEN) if (!is##TYPE(VAL) || length(VAL) != LEN) { \
    error(#VAL " must be a length " #LEN " " #TYPE "."); \
  }

#define CHECKMATROW(VAL, TYPE, NROW) if (!isMatrix(VAL) || !is##TYPE(VAL) || NUMROWS(VAL) != NROW) { \
    error(#VAL " must be a matrix with " #NROW " rows of type " #TYPE "."); \
  }


#define NUMROWS(MAT) (INTEGER(GET_DIM(MAT))[0])
#define NUMCOLS(MAT) (INTEGER(GET_DIM(MAT))[1])

#define UPDATESUMS(weight) { \
  INTEGER(topics)[z + sumK * word] += weight * count; \
  INTEGER(topic_sums)[z] += weight * count; \
  int src = INTEGER(source)[c];	\
  int lt = INTEGER(local_topics)[src]; \
  INTEGER(VECTOR_ELT(document_sums, src))[z - partialK[lt]] += weight * count; \
  INTEGER(document_sources)[c] += weight * count; \
  document_lengths[src] += weight * count; \
  }

SEXP nubbi(SEXP documents,
	   SEXP sources,
	   SEXP local_topics,
	   SEXP K,
	   SEXP V_,
	   SEXP N_,
	   SEXP alpha_,
	   SEXP eta_,
	   SEXP xi_) {
  GetRNGstate();
  int ii;
  int dd;
  int ww;
  int kk;
  int ss;

  // Check the inputs.
  CHECK(documents, NewList);
  int D = length(documents);
  CHECKLEN(sources, NewList, D);
  CHECKLEN(local_topics, Integer, D);
  CHECK(K, Integer);

  int* partialK = (int*) R_alloc(length(K), sizeof(int));
  int sumK = 0;
  for (ii = 0; ii < length(K); ++ii) {
    partialK[ii] = sumK;
    sumK += INTEGER(K)[ii];
  }
  CHECKLEN(V_, Integer, 1);
  int V = INTEGER(V_)[0];
  CHECKLEN(N_, Integer, 1);
  int N = INTEGER(N_)[0];

  // Check the hyperparameters.
  CHECKLEN(alpha_, Real, 1);
  double alpha = REAL(alpha_)[0];
  CHECKLEN(eta_, Real, 1);
  double eta = REAL(eta_)[0];
  CHECK(xi_, Real);

  // Allocate the outputs.
  SEXP topic_assignments;
  SEXP source_assignments;
  SEXP topics;
  SEXP topic_sums;
  SEXP document_sums;
  SEXP document_source_sums;

  SEXP retval;
  PROTECT(retval = allocVector(VECSXP, 6));

  SET_VECTOR_ELT(retval, 0, topic_assignments = allocVector(VECSXP, D));
  SET_VECTOR_ELT(retval, 1, source_assignments = allocVector(VECSXP, D));
  SET_VECTOR_ELT(retval, 2, topics = allocMatrix(INTSXP, sumK, V));
  SET_VECTOR_ELT(retval, 3, topic_sums = allocVector(INTSXP, sumK));
  SET_VECTOR_ELT(retval, 4, document_sums = allocVector(VECSXP, D));
  SET_VECTOR_ELT(retval, 5, document_source_sums = allocVector(VECSXP, D));

  // Allocate some working memory.
  int* document_lengths = (int*) R_alloc(D, sizeof(int));

  // Make sure all counts are zero.
  for (kk = 0; kk < sumK; ++kk) {
    for (ww = 0; ww < V; ++ww) {
      INTEGER(topics)[kk + sumK * ww] = 0;
    }
    INTEGER(topic_sums)[kk] = 0;
  }

  int max_choices = 0;
  for (dd = 0; dd < D; ++dd) {
    SEXP document = VECTOR_ELT(documents, dd);
    SEXP source = VECTOR_ELT(sources, dd);

    CHECK(source, Integer);
    CHECKMATROW(document, Integer, 2);

    int W = NUMCOLS(document);

    // These are not sums; they will be initialized below.
    SET_VECTOR_ELT(topic_assignments, dd, allocVector(INTSXP, W));
    SET_VECTOR_ELT(source_assignments, dd, allocVector(INTSXP, W));

    // These need to be set to zero.
    document_lengths[dd] = 0;
    int num_local_topics = INTEGER(K)[INTEGER(local_topics)[dd]];
    SET_VECTOR_ELT(document_sums, dd,
		   allocVector(INTSXP, num_local_topics));
    for (kk = 0; kk < num_local_topics; ++kk) {
      INTEGER(VECTOR_ELT(document_sums, dd))[kk] = 0;
    }
    SET_VECTOR_ELT(document_source_sums, dd,
		   allocVector(INTSXP, length(source)));

    int num_choices = 0;
    for (ss = 0; ss < length(source); ++ss) {
      INTEGER(VECTOR_ELT(document_source_sums, dd))[ss] = 0;
      int src_d = INTEGER(source)[ss];
      if (src_d < 0 || src_d >= D) {
	error("src_d must be >= 0 and < D.");
      }
      int local_topic = INTEGER(local_topics)[src_d];
      if (local_topic < 0 || local_topic >= length(K)) {
	error("local_topic must be >= 0 and < length(K).");
      }
      num_choices += INTEGER(K)[local_topic];
    }
    if (num_choices > max_choices) {
      max_choices = num_choices;
    }
  }

  double* probs = (double*) R_alloc(max_choices, sizeof(double));

   for (ii = 0; ii < N; ++ii) {
     Rprintf("Iteration %d\n", ii);
     for (dd = 0; dd < D; ++dd) {
       R_CheckUserInterrupt();

       SEXP document = VECTOR_ELT(documents, dd);
       SEXP source_assignment = VECTOR_ELT(source_assignments, dd);
       SEXP topic_assignment = VECTOR_ELT(topic_assignments, dd);
       SEXP source = VECTOR_ELT(sources, dd);
       SEXP document_sources = VECTOR_ELT(document_source_sums, dd);

       int W = NUMCOLS(document);
       for (ww = 0; ww < W; ++ww) {
	 int z = INTEGER(topic_assignment)[ww];
	 int c = INTEGER(source_assignment)[ww];

	 int word = INTEGER(document)[2 * ww + 0];
	 int count = INTEGER(document)[2 * ww + 1];

	 if (word < 0 || word >= V) {
	   error("word out of range.");
	 }

	 if (ii > 0) {
	   UPDATESUMS(-1);
	 }
	 /* Compute transition probabilities. */
	 int num_choices = 0;
	 double psum = 0;
	 for (ss = 0; ss < length(source); ++ss) {
	   int src_d = INTEGER(source)[ss];
	   int local_topic = INTEGER(local_topics)[src_d];

	   for (kk = 0;
		kk < INTEGER(K)[local_topic];
		++kk) {
	     if (ii > 0) {
	       probs[num_choices] =
		 (INTEGER(VECTOR_ELT(document_sums, src_d))[kk] + alpha) *
		 (INTEGER(topics)[kk + partialK[local_topic] + word * sumK] + eta) /
		 (INTEGER(topic_sums)[kk + partialK[local_topic]] + eta * V) /
		 (document_lengths[src_d] + INTEGER(K)[local_topic] * alpha) *
		 (INTEGER(document_sources)[ss] + REAL(xi_)[ss % length(xi_)]);
	     } else {
	       probs[num_choices] = 1.0;
	     }
	     psum += probs[num_choices];
	     ++num_choices;
	   }
	 }

	 /* Select a new one. */
	 double r = unif_rand();
	 num_choices = 0;
	 z = -1;
	 c = -1;

	 for (ss = 0; ss < length(source); ++ss) {
	   int src_d = INTEGER(source)[ss];
	   int local_topic = INTEGER(local_topics)[src_d];
	   for (kk = 0;
		kk < INTEGER(K)[local_topic];
		++kk) {
	     if (r < probs[num_choices] / psum) {
	       z = kk + partialK[local_topic];
	       c = ss;
	       ss = length(source);
	       break;
	     }
	     r -= probs[num_choices] / psum;
	     ++num_choices;
	   }
	 }
	 if (z == -1 || c == -1) {
	   error("Internal error.");
	 }

	 /* Do the assignment. */
	 UPDATESUMS(1);
	 INTEGER(topic_assignment)[ww] = z;
	 INTEGER(source_assignment)[ww] = c;
      }
    }
  }

  PutRNGstate();
  UNPROTECT(1);
  return retval;
}

#define UPDATERTMSUMS(weight) { \
  if (dd < test_start) { \
    INTEGER(topics)[z + K * word] += weight * count; \
    INTEGER(topic_sums)[z] += weight * count; \
  } \
  INTEGER(document_sums)[z + K * dd] += weight * count; \
  }

SEXP rtm(SEXP documents,
	 SEXP links,
	 SEXP K_,
	 SEXP V_,
	 SEXP N_,
	 SEXP alpha_,
	 SEXP eta_,
	 SEXP beta_,
	 SEXP trace_,
	 SEXP test_start_) {
  GetRNGstate();
  int ii;
  int dd;
  int ww;
  int kk;
  int ll;

  // Check the inputs.
  CHECK(documents, NewList);
  int D = length(documents);
  CHECKLEN(links, NewList, D);

  CHECKLEN(K_, Integer, 1);
  int K = INTEGER(K_)[0];
  CHECKLEN(V_, Integer, 1);
  int V = INTEGER(V_)[0];
  CHECKLEN(N_, Integer, 1);
  int N = INTEGER(N_)[0];

  CHECKLEN(trace_, Integer, 1);
  int trace = INTEGER(trace_)[0];

  CHECKLEN(test_start_, Integer, 1);
  int test_start = INTEGER(test_start_)[0];

  // Check the hyperparameters.
  CHECKLEN(alpha_, Real, 1);
  double alpha = REAL(alpha_)[0];
  CHECKLEN(eta_, Real, 1);
  double eta = REAL(eta_)[0];
  CHECKLEN(beta_, Real, K);

  // Allocate the outputs.
  SEXP topic_assignments;
  SEXP topics;
  SEXP topic_sums;
  SEXP document_sums;
  SEXP likelihoods;

  SEXP retval;
  PROTECT(retval = allocVector(VECSXP, 5));

  SET_VECTOR_ELT(retval, 0, topic_assignments = allocVector(VECSXP, D));
  SET_VECTOR_ELT(retval, 1, topics = allocMatrix(INTSXP, K, V));
  SET_VECTOR_ELT(retval, 2, topic_sums = allocVector(INTSXP, K));
  SET_VECTOR_ELT(retval, 3, document_sums = allocMatrix(INTSXP, K, D));
  SET_VECTOR_ELT(retval, 4, likelihoods = allocMatrix(REALSXP, D, N));

  // Allocate some working memory.
  int* document_lengths = (int*) R_alloc(D, sizeof(int));

  // Make sure all counts are zero.
  for (kk = 0; kk < K; ++kk) {
    for (ww = 0; ww < V; ++ww) {
      INTEGER(topics)[kk + K * ww] = 0;
    }
    INTEGER(topic_sums)[kk] = 0;
  }

  for (dd = 0; dd < D; ++dd) {
    SEXP document = VECTOR_ELT(documents, dd);
    SEXP link = VECTOR_ELT(links, dd);

    CHECK(link, Integer);
    CHECKMATROW(document, Integer, 2);

    int W = NUMCOLS(document);

    // These are not sums; they will be initialized below.
    SET_VECTOR_ELT(topic_assignments, dd, allocVector(INTSXP, W));

    document_lengths[dd] = 0;
    for (ww = 0; ww < W; ++ww) {
      int count = INTEGER(document)[2 * ww + 1];
      document_lengths[dd] += count;
    }

    // These need to be set to zero.
    for (kk = 0; kk < K; ++kk) {
      INTEGER(document_sums)[kk + K * dd] = 0;
    }
  }

  double* probs = (double*) R_alloc(K, sizeof(double));
  double* link_probs = (double*) R_alloc(K, sizeof(double));

  for (ii = 0; ii < N; ++ii) {
    if (trace > 0) {
      Rprintf("Iteration %d\n", ii);
    }
    for (dd = 0; dd < D; ++dd) {
      R_CheckUserInterrupt();

      SEXP document = VECTOR_ELT(documents, dd);
      SEXP link = VECTOR_ELT(links, dd);
      SEXP topic_assignment = VECTOR_ELT(topic_assignments, dd);

      // First calculate the per-document link probabilities.
      if (ii > 0) {
	for (kk = 0; kk < K; ++kk) {
	  link_probs[kk] = 1.0;
	}
	for (ll = 0; ll < length(link); ++ll) {
	  int dd2 = INTEGER(link)[ll];
	  if (dd2 < 0 || dd2 >= D) {
	    error("Link out of range.");
	  }
	  for (kk = 0; kk < K; ++kk) {
	    link_probs[kk] *=
	      exp(REAL(beta_)[kk] *
		  INTEGER(document_sums)[kk + K * dd2] /
		  document_lengths[dd] /
		  document_lengths[dd2]);
	  }
	}
      }

      int W = NUMCOLS(document);
      double doc_ll = 0.0;
      for (ww = 0; ww < W; ++ww) {
	int z = INTEGER(topic_assignment)[ww];

	int word = INTEGER(document)[2 * ww + 0];
	int count = INTEGER(document)[2 * ww + 1];

	if (word < 0 || word >= V) {
	  error("word out of range.");
	}

	if (ii > 0) {
	  UPDATERTMSUMS(-1);
	}
	/* Compute transition probabilities. */
	double psum = 0;
	int n_d = 0;
	for (kk = 0; kk < K; ++kk) {
	  n_d += INTEGER(document_sums)[kk + K * dd];
	  if (ii > 0) {
	    probs[kk] = link_probs[kk] *
	      (INTEGER(document_sums)[kk + K * dd] + alpha) *
	      (INTEGER(topics)[kk + word * K] + eta) /
	      (INTEGER(topic_sums)[kk] + eta * V);
	  } else {
	    probs[kk] = 1.0;
	  }
	  psum += probs[kk];
	}

	/* Select a new one. */
	double r = unif_rand();
	z = -1;

	for (kk = 0; kk < K; ++kk) {
	  if (r < probs[kk] / psum) {
	    z = kk;
	    break;
	  }
	  r -= probs[kk] / psum;
	}

	if (z == -1) {
	  error("Internal error.");
	}

	/* Do the assignment. */
	UPDATERTMSUMS(1);
	INTEGER(topic_assignment)[ww] = z;

	double like = 0.0;
	for (kk = 0; kk < K; ++kk) {
	  like += (INTEGER(document_sums)[kk + K * dd] + alpha) /
	    (n_d + K * alpha) *
	    (INTEGER(topics)[kk + word * K] + eta) /
	    (INTEGER(topic_sums)[kk] + eta * V);
	}
	doc_ll += log(like) * count;
      }
      /*
      double doc_ll = 0;
      double sum = alpha * K;
      for (kk = 0; kk < K; ++kk) {
	doc_ll += lgammafn(INTEGER(document_sums)[K * dd + kk] + alpha);
	sum += INTEGER(document_sums)[K * dd + kk];
      }
      doc_ll -= lgammafn(sum);
      doc_ll -= K * lgammafn(alpha) - lgammafn(alpha * K);
      */
      REAL(likelihoods)[dd + ii * D] = doc_ll;
    }
    // const_ll = (V * lgammafn(eta) - lgammafn(eta * V)) * K;
  }

   PutRNGstate();
   UNPROTECT(1);
   return retval;
}


// Things may not work if annotations and netannotations are both non-null.
SEXP collapsedGibbsSampler(SEXP documents,
			   SEXP K_,
			   SEXP V_,
			   SEXP N_,
			   SEXP alpha_,
			   SEXP eta_,
			   SEXP annotations,
			   SEXP beta,
			   SEXP var_,
			   SEXP method_,
			   SEXP lambda_,
			   SEXP nbeta,
			   SEXP net_annotations,
			   SEXP initial_,
			   SEXP burnin_,
			   SEXP compute_log_likelihood_,
			   SEXP trace_,
			   SEXP freeze_topics_) {
  GetRNGstate();
  int dd;
  int ii;
  int kk;
  double var = 0;
  int logistic = 0;
  double lambda = 0;
  int burnin = -1;

  CHECKLEN(alpha_, Real, 1);
  double alpha = REAL(alpha_)[0];

  CHECKLEN(eta_, Real, 1);
  double eta = REAL(eta_)[0];

  CHECKLEN(K_, Integer, 1);
  int K = INTEGER(K_)[0];

  CHECKLEN(trace_, Integer, 1);
  int trace = INTEGER(trace_)[0];

  CHECK(V_, Integer);
  int V = 0;
  for (ii = 0; ii < length(V_); ++ii) {
    V += INTEGER(V_)[ii];
  }

  CHECKLEN(N_, Integer, 1);
  int N = INTEGER(N_)[0];

  int method = -1;

  CHECK(documents, NewList);
  int nd = length(documents);

  double* dv = NULL;
  double* wx2 = NULL;
  double* wx = NULL;

  if (!isNull(annotations)) {
    if (length(annotations) != nd) {
      error("annotations must have same length as documents.");
    }
    if (isReal(annotations)) {
      logistic = 0;
    } else if (isLogical(annotations)) {
      logistic = 1;
    } else {
      error("annotations must be real or logical.");
    }

    CHECKLEN(var_, Real, 1);
    var = REAL(var_)[0];

    CHECKLEN(beta, Real, K);

    CHECKLEN(method_, Integer, 1);
    method = INTEGER(method_)[0];
    if (method < 1 || method > 3) {
      error("method must be between 1 and 3.");
    }

    dv = (double *)R_alloc(nd, sizeof(double));
    if (method == prodLDA) {
      wx = (double *)R_alloc(K, sizeof(double));
      wx2 = (double *)R_alloc(K, sizeof(double));
      CHECKLEN(lambda_, Real, 1);
      lambda = REAL(lambda_)[0];
    }
  } else {
    if (!isNull(beta)) {
      error("beta must be null when annotations are empty.");
    }
    if (!isNull(var_)) {
      error("var must be null when annotations are empty.");
    }
  }

  SEXP retval;
  PROTECT(retval = allocVector(VECSXP, 10));

  SEXP nbeta_one, nbeta_zero;
  SEXP nassignments_left, nassignments_right;

  if (!isNull(net_annotations)) {
    CHECKLEN(net_annotations, Logical, nd * nd);
    CHECKLEN(nbeta, NewList, 2);
    CHECKLEN(VECTOR_ELT(nbeta, 0), Real, K*K);
    CHECKLEN(VECTOR_ELT(nbeta, 1), Real, K*K);

    SET_VECTOR_ELT(retval, 5, nassignments_left = allocMatrix(INTSXP, nd, nd));
    SET_VECTOR_ELT(retval, 6, nassignments_right = allocMatrix(INTSXP, nd, nd));
    SET_VECTOR_ELT(retval, 7, nbeta_zero = allocMatrix(INTSXP, K, K));
    SET_VECTOR_ELT(retval, 8, nbeta_one = allocMatrix(INTSXP, K, K));

    for (ii = 0; ii < K * K; ++ii) {
      INTEGER(nbeta_one)[ii] = 0;
      INTEGER(nbeta_zero)[ii] = 0;
    }
    for (ii = 0; ii < nd * nd; ++ii) {
      INTEGER(nassignments_left)[ii] = -1;
      INTEGER(nassignments_right)[ii] = -1;
    }
  }

  SEXP assignments;
  SEXP topics = NULL;
  SEXP topic_sums = NULL;
  SEXP document_expects = NULL;
  SEXP document_sums;
  SEXP initial = NULL;
  SEXP initial_topic_sums = NULL;
  SEXP initial_topics = NULL;
  SEXP log_likelihood = NULL;

  SET_VECTOR_ELT(retval, 0, assignments = allocVector(VECSXP, nd));
  SET_VECTOR_ELT(retval, 1, topics = allocMatrix(INTSXP, K, V));
  SET_VECTOR_ELT(retval, 2, topic_sums = allocMatrix(INTSXP, K, length(V_)));
  SET_VECTOR_ELT(retval, 3, document_sums = allocMatrix(INTSXP, K, nd));

  CHECKLEN(compute_log_likelihood_, Logical, 1);
  int compute_log_likelihood = LOGICAL(compute_log_likelihood_)[0];
  CHECKLEN(freeze_topics_, Logical, 1);
  int freeze_topics = LOGICAL(freeze_topics_)[0];
  if (compute_log_likelihood) {
    SET_VECTOR_ELT(retval, 9, log_likelihood = allocMatrix(REALSXP, 2, N));
  }
  if (length(burnin_) > 0) {
    CHECKLEN(burnin_, Integer, 1);
    burnin = INTEGER(burnin_)[0];
    if (burnin < 0) {
      error("burnin must be positive.");
    }

    SET_VECTOR_ELT(retval, 4, document_expects = allocMatrix(INTSXP, K, nd));
    for (ii = 0; ii < K * nd; ++ii) {
      INTEGER(document_expects)[ii] = 0;
    }
  }

  if (!isNull(initial_)) {
    CHECK(initial_, NewList);
    SEXP names = getAttrib(initial_, R_NamesSymbol);

    for (ii = 0; ii < length(initial_); ++ii) {
      if (!strcmp(CHAR(STRING_ELT(names, ii)), "assignments")) {
	initial = VECTOR_ELT(initial_, ii);
	CHECKLEN(initial, NewList, nd);
      } else if (!strcmp(CHAR(STRING_ELT(names, ii)), "topic_sums")) {
	initial_topic_sums = VECTOR_ELT(initial_, ii);
	if (!isInteger(initial_topic_sums) ||
	    INTEGER(GET_DIM(initial_topic_sums))[0] != K ||
	    INTEGER(GET_DIM(initial_topic_sums))[1] != length(V_)) {
	  error("Initial topic sums must be a K x length(V) integer matrix.");
	}
      } else if (!strcmp(CHAR(STRING_ELT(names, ii)), "topics")) {
	initial_topics = VECTOR_ELT(initial_, ii);
	if (!isInteger(initial_topics) ||
	    INTEGER(GET_DIM(initial_topics))[0] != K ||
	    INTEGER(GET_DIM(initial_topics))[1] != V) {
	  error("Initial topics (%d x %d) must be a %d x %d integer matrix.",
		INTEGER(GET_DIM(initial_topics))[0],
		INTEGER(GET_DIM(initial_topics))[1],
		K,
		V);
	}
      } else {
	error("Unrecognized initialization field: '%s'",
	      CHAR(STRING_ELT(names, ii)));
      }
    }
  }

  if ((initial_topic_sums == NULL) ^ (initial_topics == NULL)) {
    error("initial topic sums and topics must both be specified.");
  }


  if (initial_topics == NULL) {
    for (ii = 0; ii < K * V; ++ii) {
      INTEGER(topics)[ii] = 0;
    }
  } else {
    for (ii = 0; ii < K * V; ++ii) {
      INTEGER(topics)[ii] = INTEGER(initial_topics)[ii];
    }
  }
  if (initial_topic_sums == NULL) {
    for (ii = 0; ii < K * length(V_); ++ii) {
      INTEGER(topic_sums)[ii] = 0;
    }
  } else {
    for (ii = 0; ii < K * length(V_); ++ii) {
      INTEGER(topic_sums)[ii] = INTEGER(initial_topic_sums)[ii];
    }
  }

  for (ii = 0; ii < K * nd; ++ii) {
    INTEGER(document_sums)[ii] = 0;
  }


  for (dd = 0; dd < nd; ++dd) {
    int ww;
    SEXP document = VECTOR_ELT(documents, dd);

    CHECKMATROW(document, Integer, 2);

    if (!isNull(annotations)) {
      if (method == corrLDA || logistic) {
	dv[dd] = 0.0;
      } else {
	dv[dd] = REAL(annotations)[dd];
      }
    }

    int nw = INTEGER(GET_DIM(document))[1];
    SET_VECTOR_ELT(assignments, dd, allocVector(INTSXP, nw));
    SEXP zs = VECTOR_ELT(assignments, dd);

    for (ww = 0; ww < nw; ++ww) {
      int word = INTEGER(document)[ww * 2];
      int count = INTEGER(document)[ww * 2 + 1];
      if (count < 0) {
	error("Count must be positive.");
      }
      if (word >= V || word < 0) {
	error("Word (%d) must be positive and less than or "
	      "equal to the number of words (%d).", word, V);
      }
      INTEGER(zs)[ww] = -1;
    }
  }

  if (method == prodLDA) {
    for (kk = 0; kk < K; ++kk) {
      wx[kk] = 0.0;
      wx2[kk] = 0.0;
    }
  }

  double* p = (double *)R_alloc(K, sizeof(double));
  double* p_pair = NULL;
  if (!isNull(net_annotations)) {
    p_pair = (double *)R_alloc(K * K, sizeof(double));
  }


  double const_prior = 0;
  double const_ll = 0;

  if (compute_log_likelihood) {
    //                log B(\alpha)
    const_prior = (K * lgammafn(alpha) - lgammafn(alpha * K)) * nd;
    //                log B(\eta)
    const_ll = (V * lgammafn(eta) - lgammafn(eta * V)) * K;
  }

  int iteration;
  for (iteration = 0; iteration < N; ++iteration) {
    if (trace >= 1) {
      Rprintf("Iteration %d\n", iteration);
    }
    for (dd = 0; dd < nd; ++dd) {
      R_CheckUserInterrupt();
      SEXP zs = VECTOR_ELT(assignments, dd);
      SEXP document = VECTOR_ELT(documents, dd);
      int ww;
      int nw = INTEGER(GET_DIM(document))[1];
      SEXP initial_d = NULL;

      if (initial != NULL) {
	initial_d = VECTOR_ELT(initial, dd);
	CHECKLEN(initial_d, Integer, nw);
      }
      if (!isNull(net_annotations)) {
	for (ww = 0; ww < nd; ++ww) {
	  if (ww == dd) {
	    continue;
	  }

	  int* z = &INTEGER(nassignments_left)[nd * dd + ww];
	  int* z2 = &INTEGER(nassignments_right)[nd * ww + dd];
	  int y = LOGICAL(net_annotations)[nd * dd + ww];

	  if (*z != -1) {
	    if (*z2 == -1) {
	      error("Internal error (1).");
	    }
	    INTEGER(document_sums)[K * dd + *z]--;
	    INTEGER(document_sums)[K * ww + *z2]--;
	    if (y == 1) {
	    INTEGER(nbeta_one)[K * (*z) + (*z2)]--;
	    } else {
	      INTEGER(nbeta_zero)[K * (*z) + (*z2)]--;
	    }
	  } else if (*z2 != -1) {
	    error("Internal error (2).");
	  }

	  double p_sum = 0.0;
	  int jj;
	  for (ii = 0; ii < K; ++ii) {
	    for (jj = 0; jj < K; ++jj) {
	      if (*z == -1) {
		p_pair[ii * K + jj] = 1.0;
	      } else {
		p_pair[ii * K + jj] = (INTEGER(document_sums)[K * dd + ii] + alpha)*
		  (INTEGER(document_sums)[K * ww + jj] + alpha);
		if (y == 1) {
		  p_pair[ii * K + jj] *= INTEGER(nbeta_one)[ii * K + jj] +
		    REAL(VECTOR_ELT(nbeta, 1))[ii * K + jj];
		} else if (y == 0) {
		  p_pair[ii * K + jj] *= INTEGER(nbeta_zero)[ii * K + jj] +
		    REAL(VECTOR_ELT(nbeta, 0))[ii * K + jj];
		} else {
		  error("What the hell happened?");
	      }
		p_pair[ii * K + jj] /= INTEGER(nbeta_one)[ii * K + jj] +
		  INTEGER(nbeta_zero)[ii * K + jj] +
		  REAL(VECTOR_ELT(nbeta, 0))[ii * K + jj] +
		  REAL(VECTOR_ELT(nbeta, 1))[ii * K + jj];
	      }
	      if (p_pair[ii * K + jj] < 0) {
		error("What the WHAT?! (%d, %d)",
		      INTEGER(nbeta_one)[ii * K + jj],
		      INTEGER(nbeta_zero)[ii * K + jj]);
	      }
	      p_sum += p_pair[ii * K + jj];
	    }
	  }

	  *z = -1;
	  *z2 = -1;
	  double r = unif_rand();
	  double r_orig = r;
	  for (ii = 0; ii < K; ++ii) {
	    for (jj = 0; jj < K; ++jj) {
	      if (r < p_pair[ii * K + jj] / p_sum) {
		*z = ii;
		*z2 = jj;
		ii = jj = K;
		break;
	      }
	      r -= p_pair[ii * K + jj] / p_sum;
	    }
	  }
	  if (*z == -1 || *z2 == -1) {
	    error("The laws of science be a harsh mistress "
		  "(%d, %d, %g, %g, %g).",
		  *z, *z2, r_orig, r, p_sum);
	  }

	  INTEGER(document_sums)[K * dd + *z]++;
	  INTEGER(document_sums)[K * ww + *z2]++;

	  if (burnin > -1 && iteration >= burnin) {
	    INTEGER(document_expects)[K * dd + *z]++;
	    INTEGER(document_expects)[K * ww + *z2]++;
	  }

	  if (y == 1) {
	    INTEGER(nbeta_one)[K * (*z) + (*z2)]++;
	  } else {
	    INTEGER(nbeta_zero)[K * (*z) + (*z2)]++;
	  }
	}
      }

      for (ww = 0; ww < nw; ++ww) {
	int* z = &INTEGER(zs)[ww];
	int word = -1;
	int count = 1;
	int* topic_wk;
	int* topic_k;
	int* document_k;

	word = INTEGER(document)[ww * 2];
	int partialsum = 0;
	int topic_index = -1;
	for (ii = 0; ii < length(V_); ++ii) {
	  partialsum += INTEGER(V_)[ii];
	  if (word < partialsum) {
	    topic_index = ii;
	  }
	}
	if (topic_index == -1) {
	  error("Oops I did it again");
	}
	count = INTEGER(document)[ww * 2 + 1];

	if (*z != -1) {
	  topic_wk = &INTEGER(topics)[(*z) + K * word];
	  topic_k = &INTEGER(topic_sums)[*z + K * topic_index];
	  if(!freeze_topics)
	  {
	    *topic_wk -= count;
	    *topic_k -= count;
	  }
	  document_k = &INTEGER(document_sums)[K * dd + *z];
	  *document_k -= count;

	  if (!isNull(annotations)) {
	    if (method == prodLDA) {
	      wx2[*z] -= count * REAL(annotations)[dd] * REAL(annotations)[dd];
	      wx[*z] -= count * REAL(annotations)[dd];
	    } else {
	      dv[dd] += count * dv_update(annotations, dd, REAL(beta)[*z],
					  var, nw, method, logistic);
	    }
	  }

	  if (*topic_wk < 0 || *topic_k < 0 || *document_k < 0) {
	    error("Counts became negative for word (%d): (%d, %d, %d)",
		  word, *topic_wk, *topic_k, *document_k);
	  }
	}

	double r = unif_rand();
	double p_sum = 0.0;
	for (kk = 0; kk < K; ++kk) {
	  if (*z == -1) {
	    if (initial != NULL) {
	      if (INTEGER(initial_d)[ww] == kk) {
		p[kk] = 1;
	      } else {
		p[kk] = 0;
	      }
	    } else {
	      p[kk] = 1;
	    }
	  } else {
	    p[kk] = (INTEGER(document_sums)[K * dd + kk] + alpha);
	    p[kk] *= (INTEGER(topics)[kk + K * word] + eta);
	    p[kk] /= (INTEGER(topic_sums)[kk + K * topic_index] + V * eta);

	    if (!isNull(annotations)) {
	      if (method == corrLDA) {
		p[kk] *= dv_update(annotations, dd, REAL(beta)[kk],
				   var, nw, method, logistic) - dv[dd];
	      } else if (method == sLDA) {
		if (REAL(beta)[kk] != REAL(beta)[kk]) {
			Rprintf("NaN! beta[0]=%g beta[1]=%g\n", kk, REAL(beta)[0], REAL(beta)[1]);
			REAL(beta)[kk] = 0.00001;
			Rprintf("\t now beta[%d]=%g nw=%d\n", kk, REAL(beta)[kk], nw);
		}
		double change = REAL(beta)[kk] / nw;
		if (logistic) {
		  double yv = 2 * LOGICAL(annotations)[dd] - 1.0;
		  p[kk] /= 1.0 + exp(yv * (dv[dd] - change));
		} else {
      // How does this work?
      // dv[dd] = y - sum_{i != n} beta_{z_i} / N
      // change = beta_{z_n} / N
      // What we want to compute i:
      // exp(2 * change * (dv[dd]) - change^2)
		  p[kk] *= exp(change * (dv[dd] - change / 2) / var);
		  if (p[kk] != p[kk]) {
			Rprintf("encountered NaN with change=%g dv[dd]=%g var=%g beta[kk]=%g nw=%d\n", change, dv[dd], var, REAL(beta)[kk], nw);
		  }
		}
	      } else if (method == prodLDA) {
		double x_d = REAL(annotations)[dd];
		int n_k = INTEGER(topic_sums)[kk + K * topic_index] + 1 + lambda;
		p[kk] *= sqrt(n_k) *
		  exp(-(wx2[kk] - wx2[0] - (wx[kk] + x_d)*(wx[kk] + x_d)/n_k + (wx[0] + x_d) * (wx[0] + x_d) / n_k) / (2 * var));
	      } else {
		error("Not implemented.");
	      }
	    }
	  }
          if (p[kk] != p[kk]) // is NaN 
	  {
		Rprintf("pr of topic %d is NaN. Assigning random probability.\n", kk);
		p[kk] = unif_rand();
	  }
	  p_sum += p[kk];
	}

	if (p_sum <= 0.0) {
	  kk = K - 1;
	  error("Numerical problems (%g, %g).", dv[dd],
		dv_update(annotations, dd, REAL(beta)[kk],
			  var, nw, method, logistic));
	}

	*z = -1;
	for (kk = 0; kk < K; ++kk) {
	  if (r < p[kk] / p_sum) {
	    *z = kk;
	    break;
	  }
	  r -= p[kk] / p_sum;
	}

	if (*z == -1) {
	  for (kk = 0; kk < K; ++kk) {
	    Rprintf("%g\n", p[kk]);
	  }
	  Rprintf("no topic selected. picking at random:", r);
	  *z = 0;
	  //error("This should not have happened (%g).", r);
	}

	if(!freeze_topics)
	{
	  INTEGER(topics)[*z + K * word] += count;
	  INTEGER(topic_sums)[*z + K * topic_index] += count;
	}
	INTEGER(document_sums)[K * dd + *z] += count;
	if (burnin > -1 && iteration >= burnin) {
	  INTEGER(document_expects)[K * dd + *z] += count;
	}

	if (!isNull(annotations)) {
	  if (method == prodLDA) {
	    wx2[*z] += count * REAL(annotations)[dd] * REAL(annotations)[dd];
	    wx[*z] += count * REAL(annotations)[dd];
	  } else {
	    dv[dd] -= count * dv_update(annotations, dd, REAL(beta)[*z],
					var, nw, method, logistic);
	  }
	}
      }
    }


    /*Compute the likelihoods:*/
    if (compute_log_likelihood) {
      double doc_ll = 0;
      for (dd = 0; dd < nd; ++dd) {
	double sum = alpha * K;
	for (kk = 0; kk < K; ++kk) {
	  doc_ll += lgammafn(INTEGER(document_sums)[K * dd + kk] + alpha);
	  sum += INTEGER(document_sums)[K * dd + kk];
	}
	doc_ll -= lgammafn(sum);
      }
      double topic_ll = 0;
      for (kk = 0; kk < K; ++kk) {
	double sum = eta * V;
	for (ii = 0; ii < V; ++ii) {
	  topic_ll += lgammafn(INTEGER(topics)[kk + K * ii] + eta);
	  sum += INTEGER(topics)[kk + K * ii];
	}
	topic_ll -= lgammafn(sum);
      }
      if (trace >= 2) {
	Rprintf("ll: %g + %g - %g - %g = %g\n", doc_ll, topic_ll, const_ll, const_prior,
	       doc_ll + topic_ll - const_ll - const_prior);
      }
      REAL(log_likelihood)[2 * iteration] = doc_ll - const_prior + topic_ll - const_ll;
      REAL(log_likelihood)[2 * iteration + 1] = topic_ll - const_ll;
    }
  }


  PutRNGstate();
  UNPROTECT(1);
  return retval;
}

