// Online Weighted Majority

#ifndef WM_H
#define WM_H

#include "parallel.h"
#include "sparsela.h"
#include <boost/thread/thread.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

void WmUpdate(size_t tid) {
  SVEC *temp_v = NULL;
  if (global.comm_method == 1) {
    if (l1.opt_method == "dwm_i") {
      temp_v = CreateConstDvector(l1.num_experts, 1.0);
      for (size_t h=0; h<global.num_threads; h++) {
	SparseMultiplyOverwrite(temp_v, l1.w_vec_pool[h]);
      }
      SparsePowerOverwrite(temp_v, 1.0/global.num_threads);
    }
    else { // l1.opt_method == "dwm_a"
      temp_v = CreateConstDvector(l1.num_experts, 0.0);
      for (size_t h=0; h<global.num_threads; h++) {
	SparseAddOverwrite(temp_v, l1.w_vec_pool[h]);
      }
      SparseScaleOverwrite(temp_v, 1.0/global.num_threads);
    }
  }
  else if (global.comm_method == 0) {
    if (l1.opt_method == "dwm_i") {
      temp_v = CreateConstDvector(l1.num_experts, 1.0);
      SparseMultiplyOverwrite(temp_v, l1.w_vec_pool[tid]);
    }
    else { // l1.opt_method == "dwm_a"
      temp_v = CreateConstDvector(l1.num_experts, 0.0);
      SparseAddOverwrite(temp_v, l1.w_vec_pool[tid]);
    }
  }

  CopyFromSvec(l1.w_vec_pool[tid], temp_v);

  DestroySvec(temp_v);
  // dummy updating time
  //boost::this_thread::sleep(boost::posix_time::microseconds(1));
}

void *WmThread(void *in_par) {
  thread_param* par = (thread_param*) in_par;
  size_t tid = par->thread_id;
  EXAMPLE **exs;
  int b;
  vector<T_LBL> exp_pred (l1.num_experts, 0.0);
  T_LBL pred_lbl;
  double sum_weight_pos, sum_weight_neg;

  exs = (EXAMPLE **)my_malloc( sizeof(EXAMPLE *) * global.mb_size );

  while (true) {
    switch (par->thread_state) {
    case 0: // waiting to read data
      for (b = 0; b<global.mb_size; b++) {
	if ( GetImmedExample(exs+b, tid, l1) != 0 ) { // new example read
	  //print_ex(exs[b]);
	}
	else { // all epoches finished
	  return NULL;
	}
      }
      par->thread_state = 1;
      break;
    case 1:
      for (b = 0; b<global.mb_size; b++) {
	sum_weight_pos = 0.0; sum_weight_neg = 0.0;
	for (size_t p=0; p<l1.num_experts; p++) {
	  exp_pred[p] = l1.weak_learners[p]->WeakPredictLabel(exs[b]);
	  if (exp_pred[p] != exs[b]->label) {
	    l1.expert_misp[p][tid] = l1.expert_misp[p][tid] + 1;
	  }
	  if (exp_pred[p] ==  1) {
	    sum_weight_pos = sum_weight_pos + l1.w_vec_pool[tid]->feats[p].wval;
	  }
	  else {
	    sum_weight_neg = sum_weight_neg + l1.w_vec_pool[tid]->feats[p].wval;
	  }
	}
	if (sum_weight_pos > sum_weight_neg) {
	  pred_lbl = (T_LBL)1;
	}
	else {
	  pred_lbl = (T_LBL)-1;
	}

	l1.t_ct[tid]  = l1.t_ct[tid] + 1;
	// ------------for log-------------------
	if (global.calc_loss) {
	  // Calculate number of misclassifications
	  if (l1.type == "classification") {
	    //cout << "ex_label: "<< exs[b]->label << ", pred_label: " << pred_lbl << endl;
	    if (pred_lbl != exs[b]->label) {
	      l1.total_misp_pool[tid] = l1.total_misp_pool[tid] + 1;
	    }
	    if (l1.num_log > 0) {
	      if (l1.t_ct[tid] == l1.t_int && l1.lp_ct[tid] < l1.num_log) {
		l1.log_err[tid][l1.lp_ct[tid]] = l1.total_misp_pool[tid];
		for (size_t p=0; p<l1.num_experts; p++) {
		  if (exp_pred[p] != exs[b]->label) {
		    l1.log_err_expert[p][l1.lp_ct[tid]] = l1.log_err_expert[p][l1.lp_ct[tid]] + 1;
		  }
		}
		l1.t_ct[tid] = 0;
		l1.lp_ct[tid] = l1.lp_ct[tid] + 1;
	      }
	    }
	  }
	}
	//----------- log end-------------

	// local update
	for (size_t p=0; p<l1.num_experts; p++) {
	  if (exp_pred[p] != exs[b]->label) {
	    l1.w_vec_pool[tid]->feats[p].wval = l1.w_vec_pool[tid]->feats[p].wval * l1.alpha;
	  }
	}
	
	//if (l1.t_ct[tid] % 2000 == 0)
	//  SparseScaleOverwrite(l1.w_vec_pool[tid], pow(10,20));
	
      }

      // dummy gradient calc time
      //boost::this_thread::sleep(boost::posix_time::microseconds(1));

      // wait till all threads send their messages
      pthread_barrier_wait(&barrier_msg_all_sent);
      
      par->thread_state = 2;
      break;
    case 2:
      // update using messages
      WmUpdate(tid);
      // wait till all threads used messages they received
      pthread_barrier_wait(&barrier_msg_all_used);
      // communication done
      par->thread_state = 0;
      break;
    default:
      cout << "ERROR! Unknown thread state number !" << endl;
      return NULL;
    }
  }

}


void Wm(learner &l) {
  size_t n_threads = l.num_threads;
  size_t t, k;

  l.t_ct = (size_t*)malloc(n_threads * sizeof(size_t));
  for (t=0; t<n_threads; t++)
    l.t_ct[t] = 0;
  //// for log
  if (l.num_log > 0) {
    l.t_int = (size_t)floor( (global.num_epoches*num_train_exps+global.num_iter_res)/(n_threads * l.num_log) );
    l.lp_ct = (size_t*)malloc(n_threads * sizeof(size_t));
    l.log_err = (size_t**)malloc(n_threads * sizeof(size_t*));
    l.log_err_expert = (size_t**)malloc(l.num_experts * sizeof(size_t*));
    for (t=0; t<n_threads; t++) {
      l.log_err[t] = (size_t*)malloc(l.num_log * sizeof(size_t));
      l.lp_ct[t] = 0;
    }
    for (t=0; t<n_threads; t++) {
      for (k=0; k<l.num_log; k++) {
	l.log_err[t][k] = 0;
      }
    }
    for (t=0; t<l.num_experts; t++) {
      l.log_err_expert[t] = (size_t*)malloc(l.num_log * sizeof(size_t));
    }
    for (t=0; t<l.num_experts; t++) {
      for (k=0; k<l.num_log; k++) {
	l.log_err_expert[t][k] = 0;
      }
    }
  }
  l.expert_misp = (size_t**)malloc(l.num_experts * sizeof(size_t*));
  for (k=0; k< l.num_experts; k++) {
    l.expert_misp[k] = (size_t*)malloc(n_threads * sizeof(size_t));
  }
  for (k=0; k<l.num_experts; k++)
    for (t=0; t<n_threads; t++)
      l.expert_misp[k][t] = 0;
  //// log end
  
  if (l.num_experts <= 0) {
    cout << "Number of experts not specified for WM! Bailing!" << endl;
    exit(1);
  }
  //// For weak learners
  if (l.wl_name == "stump") {
    // choose splitting dimensions
    if (l.num_experts > global.max_feature_idx) {
      cout << "Number of experts: " <<l.num_experts << " larger than number of feature dimension: "<< global.max_feature_idx <<" ! Bailing!" << endl;
      exit(1);
    }
    vector<size_t> dims;
    // feature index starts from 1
    for (size_t d=1; d<=global.max_feature_idx; d++)
      dims.push_back(d);
    random_shuffle ( dims.begin(), dims.end() );
    // Train experts
    size_t n_it;
    if (num_train_exps > 10000)
      n_it = min(200, (int)ceil(num_train_exps/50));
    else
      n_it = max(200, (int)ceil(num_train_exps/50));
    cout << "Training experts...Number of iterations for each expert is "<<n_it<<endl;
    for (k=0; k< l.num_experts; k++) {
      l.weak_learners[k] = GetWeakLearner( l.wl_name, dims[k], n_it);
      l.weak_learners[k]->WeakTrain(train_exps, num_train_exps, NULL);
      cout << k << ".";
    }
    cout << " Done!" << endl;
  }
  else {
    for (k=0; k< l.num_experts; k++) {
      l.weak_learners[k] = GetWeakLearner(l.wl_name, 0, 0);
    }
  }

  threads = (pthread_t*)calloc(n_threads, sizeof(pthread_t));
  t_par = (thread_param**)calloc(n_threads, sizeof(thread_param*));

  pthread_barrier_init(&barrier_msg_all_sent, NULL, n_threads);
  pthread_barrier_init(&barrier_msg_all_used, NULL, n_threads);

  // initial learning rate  
  for (t = 0; t < n_threads; t++) {
    // init thread parameters
    t_par[t] = (thread_param*)calloc(1, sizeof(thread_param));
    t_par[t]->thread_id = t;
    t_par[t]->l = &l;
    t_par[t]->thread_state = 0;
    // init thread weights
    //l.w_vec_pool[t] = CreateConstDvector(l.num_experts, pow(num_train_exps*global.num_epoches + global.num_iter_res, 10));
    l.w_vec_pool[t] = CreateConstDvector(l.num_experts, num_train_exps*global.num_epoches + global.num_iter_res);
    l.msg_pool[t] = CreateEmptySvector();
    l.num_used_exp[t] = 0;

    l.total_misp_pool[t] = 0;
    // begin learning iterations
    pthread_create(&threads[t], NULL, WmThread, (void*)t_par[t]);
  }

  FinishThreads(n_threads);

  //// save log
  if (l.num_log > 0) {
    FILE *fp;
    string log_fn (global.train_data_fn);
    log_fn += ".";
    log_fn += l.opt_method;
    log_fn += ".log";
    if ((fp = fopen (log_fn.c_str(), "w")) == NULL) {
      cerr << "Cannot save log file!"<< endl;
      exit (1);
    }
    fprintf(fp, "Log intervals: %ld. Number of logs: %ld\n\n", l.t_int, l.num_log);
    fprintf(fp, "Errors cumulated:\n");
    for (t=0; t<n_threads; t++) {
      for (k=0; k<l.num_log; k++) {
	fprintf(fp, "%ld", l.log_err[t][k]);
	fprintf(fp, " ");
      }
      fprintf(fp, ";\n");
    }
    // accumulate log_err_expert
    for (t=0; t<l.num_experts; t++) {
      for (k=1; k<l.num_log; k++) {
	l.log_err_expert[t][k] = l.log_err_expert[t][k-1] + l.log_err_expert[t][k];
      }
    }
    fprintf(fp, "Expert Errors:\n");
    for (t=0; t<l.num_experts; t++) {
      for (k=0; k<l.num_log; k++) {
	fprintf(fp, "%ld", l.log_err_expert[t][k]);
	fprintf(fp, " ");
      }
      fprintf(fp, ";\n");
    }
    fclose(fp);
  }

  // prediction accuracy for classifications
  if (l.type == "classification") {
    size_t t_m = 0, t_s = 0;
    for (t = 0; t < n_threads; t++) {
      t_m += l.total_misp_pool[t];
      t_s += l.num_used_exp[t];
      cout << "t"<< t << ": " << l.num_used_exp[t] << " samples processed. Misprediction: " << l.total_misp_pool[t]<< ", accuracy: "<< 1.0-(double)l.total_misp_pool[t]/(double)l.num_used_exp[t] << endl;
      if (!global.quiet) {
	for (k=0; k<l.num_experts; k++) {
	  cout << "Expert " << k << " made " << l1.expert_misp[k][t] << " mispredictions over agent " << t << ". Weight: " << l1.w_vec_pool[t]->feats[k].wval << "." << endl;
	}
      }
    }
    cout << "--------------------- Total mispredictions: " << t_m << ", accuracy: " << 1.0-(double)t_m/(double)t_s<< endl;
    /*
    for (k=0; k<l.num_experts; k++) {
      cout <<"Expert " << k << " made " << l.expert_misp[k] << " mispredictions over all " << n_threads << " agents." << endl;
    }
    */
  }

  FinishLearner(l, n_threads);
  FinishData();
}


#endif
