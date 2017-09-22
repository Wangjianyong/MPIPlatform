/*
* Copyright 2017 [Zhouyuan Huo]
*
*    Licensed under the Apache License, Version 2.0 (the "License");
*    you may not use this file except in compliance with the License.
*    You may obtain a copy of the License at
*
*        http://www.apache.org/licenses/LICENSE-2.0
*
*    Unless required by applicable law or agreed to in writing, software
*    distributed under the License is distributed on an "AS IS" BASIS,
*    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*    See the License for the specific language governing permissions and
*    limitations under the License.
*/
#ifndef _SERVER_TRAINER_
#define _SERVER_TRAINER_ 

#include "../Datapoint/ARMADatapoint.h"
#include "../Gradient/Gradient.h"
#include "../Tools/Tools.h"


class ServerTrainer : public Trainer {
 public:

  ServerTrainer(Model *model, Datapoint *datapoints) : Trainer(model, datapoints) {}
  ~ServerTrainer() {}

  TrainStatistics Train(Model *model, Datapoint *datapoints, Updater *updater) override {
 
    // Keep track of statistics of training.
    TrainStatistics stats;

	Datapoint *sub_datapoints = new ARMADatapoint();

	MPI_Status status;

	// Train.
	Timer gradient_timer;
	printf("Epoch: 	Time(s): Loss:   Evaluation(AUC or Accuracy): \n");
	for (int epoch = 0; epoch < FLAGS_n_epochs; epoch++) {
	  srand(epoch);

      // compute loss and print working time.
	  this->EpochBegin(epoch, gradient_timer, model, datapoints, &stats);

	  gradient_timer.Tick();
	  updater->EpochBegin();
      double learning_rate = FLAGS_learning_rate / std::pow(1+epoch, FLAGS_learning_rate_dec);
      std::vector<int> delay_counter(FLAGS_num_workers, 1);
	  std::vector<double> worker_gradient(model->NumParameters(), 0);
	  std::vector<double> message;

	  for(int iter_counter = 0; iter_counter < FLAGS_in_iters; iter_counter++) {
		int cur_worker_size = 0;
		std::vector<int> cur_revceived_workers(FLAGS_num_workers, 0);

		// receive information and update.
		bool flag_receive = true;
		while(flag_receive) {
		  MPI_Probe(MPI_ANY_SOURCE, 101, MPI_COMM_WORLD, &status);	
		  int taskid = status.MPI_SOURCE;
		  MPI_Recv(&worker_gradient[0], worker_gradient.size(), MPI_DOUBLE, taskid, 101, MPI_COMM_WORLD, &status);

		  gradient->coeffs = worker_gradient;
		  updater->ApplyGradient(gradient, learning_rate);

		  cur_worker_size += 1;
		  delay_counter[taskid - 1] = 1;
		  cur_revceived_workers[taskid - 1] = 1;

		  flag_receive = false;
		  if ( ((cur_worker_size < FLAGS_group_size) || (max_element(delay_counter) > FLAGS_max_delay)) && (iter_counter < FLAGS_in_iters - 1))
		    flag_receive = true;
		  if ( (cur_worker_size < FLAGS_num_workers) && (iter_counter == FLAGS_in_iters - 1)) 
			flag_receive = true;
		}

		// proximal operator occurs in the server if not decoupled.
		if(FLAGS_l1_lambda)
		  updater->ApplyProximalOperator(learning_rate * FLAGS_l1_lambda);
		else if(FLAGS_trace_lambda)
		  updater->ApplyProximalOperator(learning_rate * FLAGS_trace_lambda);
		
		// build message.
		std::vector<double> & master_model = model->ModelData();
		message = master_model;
		if (iter_counter < FLAGS_in_iters - 1) {
		  message.push_back(0);
		  message.push_back(0);
		}
		else if(epoch < FLAGS_n_epochs - 1) {
		  message.push_back(1);
		  message.push_back(0);
		}
		else {
		  message.push_back(1);
		  message.push_back(1);
		}

		// send messages to workers.
		for(int i = 0; i < FLAGS_num_workers; i++) {
		  if(cur_revceived_workers[i] == 0)
			delay_counter[i] += 1;
		  else {
			MPI_Send(&message[0], message.size(), MPI_DOUBLE, i+1, 102, MPI_COMM_WORLD);
		  }
		}
	  }
		
	  updater->EpochFinish();
	  gradient_timer.Tock();
	}
	model->StoreModel();
	return stats;
  }

  virtual void EpochBegin (int epoch, Timer &gradient_timer, Model *model, Datapoint *datapoints, TrainStatistics *stats) override {
	double cur_time;
    if(stats->times.size()==0) 
	  cur_time = 0;
	else
	  cur_time = gradient_timer.elapsed + stats->times[stats->times.size()-1];

	double worker_eval = 0, master_eval = 0; 
	double worker_loss = 0, master_loss = 0;
	int worker_num = 0, master_num = 0;
	MPI_Reduce(&worker_num, &master_num, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
	MPI_Reduce(&worker_eval, &master_eval, 1,  MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
	MPI_Reduce(&worker_loss, &master_loss, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
	master_loss /= master_num; 
	master_eval /= master_num; 

	Trainer::TrackTimeLoss(cur_time, master_loss, stats);
	if (FLAGS_print_loss_per_epoch && epoch % FLAGS_interval_print == 0) {
	  Trainer::PrintTimeLoss(cur_time, master_loss, epoch, master_eval);
	}
  }
};

#endif
