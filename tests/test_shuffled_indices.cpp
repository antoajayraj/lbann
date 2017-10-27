////////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2014-2016, Lawrence Livermore National Security, LLC.
// Produced at the Lawrence Livermore National Laboratory.
// Written by the LBANN Research Team (B. Van Essen, et al.) listed in
// the CONTRIBUTORS file. <lbann-dev@llnl.gov>
//
// LLNL-CODE-697807.
// All rights reserved.
//
// This file is part of LBANN: Livermore Big Artificial Neural Network
// Toolkit. For details, see http://software.llnl.gov/LBANN or
// https://github.com/LLNL/LBANN.
//
// Licensed under the Apache License, Version 2.0 (the "Licensee"); you
// may not use this file except in compliance with the License.  You may
// obtain a copy of the License at:
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the license.
//
// lbann_proto.cpp - prototext application
////////////////////////////////////////////////////////////////////////////////

#include "lbann/lbann.hpp"
#include "lbann/proto/proto_common.hpp"

using namespace lbann;

const int lbann_default_random_seed = 42;

int mini_batch_size = 5;

void test_is_shuffled(generic_data_reader *reader, bool is_shuffled, const char *msg = 0);

int main(int argc, char *argv[]) {
  int random_seed = lbann_default_random_seed;
  lbann_comm *comm = initialize(argc, argv, random_seed);
  bool master = comm->am_world_master();

  try {
    // Initialize options db (this parses the command line)
    options *opts = options::get();
    opts->init(argc, argv);
    if (opts->has_string("h") or opts->has_string("help") or argc == 1) {
      print_help(comm);
      finalize(comm);
      return 0;
    }

    //read data_reader prototext file 
    if (not opts->has_string("fn")) {
      std::stringstream err;
      err << __FILE__ << " " << __LINE__ << " :: "
          << "you must run with: --fn=<string> where <string> is\n"
          << "a data_reader prototext filePathName\n";
      throw lbann_exception(err.str());
    }
    lbann_data::LbannPB pb;
    string reader_fn(opts->get_string("fn").c_str());
    read_prototext_file(reader_fn.c_str(), pb, master);
    const lbann_data::DataReader & d_reader = pb.data_reader();

    int size = d_reader.reader_size();
    for (int j=0; j<size; j++) {
      const lbann_data::Reader& readme = d_reader.reader(j);
      if (readme.role() == "train") {
        generic_data_reader *reader;

        bool shuffle = true;
        reader = new mnist_reader(mini_batch_size, shuffle);
        if (readme.data_filename() != "") { reader->set_data_filename( readme.data_filename() ); }
        if (readme.label_filename() != "") { reader->set_label_filename( readme.label_filename() ); }
        if (readme.data_filedir() != "") { reader->set_file_dir( readme.data_filedir() ); }
        reader->load();
        test_is_shuffled(reader, true, "TEST #1");
        delete reader;

        //test: indices should not be shuffled; same as previous, except we call
        //      set_firstN(true);
        bool firstN = true;
        reader = new mnist_reader(mini_batch_size, shuffle);
        if (readme.data_filename() != "") { reader->set_data_filename( readme.data_filename() ); }
        if (readme.label_filename() != "") { reader->set_label_filename( readme.label_filename() ); }
        if (readme.data_filedir() != "") { reader->set_file_dir( readme.data_filedir() ); }
        reader->set_firstN(firstN);
        reader->load();
        test_is_shuffled(reader, false, "TEST #2");
        delete reader;

        //test: indices should not be shuffled, due to ctor argument
        shuffle = false;
        reader = new mnist_reader(mini_batch_size, shuffle);
        if (readme.data_filename() != "") { reader->set_data_filename( readme.data_filename() ); }
        if (readme.label_filename() != "") { reader->set_label_filename( readme.label_filename() ); }
        if (readme.data_filedir() != "") { reader->set_file_dir( readme.data_filedir() ); }
        reader->load();
        test_is_shuffled(reader, false, "TEST #3");
        delete reader;

        //test: set_shuffled_indices; indices should not be shuffled
        shuffle = true;
        reader = new mnist_reader(mini_batch_size, shuffle);
        if (readme.data_filename() != "") { reader->set_data_filename( readme.data_filename() ); }
        if (readme.label_filename() != "") { reader->set_label_filename( readme.label_filename() ); }
        if (readme.data_filedir() != "") { reader->set_file_dir( readme.data_filedir() ); }
        reader->load();
        //at this point the indices should be shuffled (same as first test)
        test_is_shuffled(reader, true, "TEST #4");
        std::vector<int> indices(mini_batch_size);
        std::iota(indices.begin(), indices.end(), 0);
        reader->set_shuffled_indices(indices);
        test_is_shuffled(reader, false, "TEST #5");
        delete reader;

        break;
      }
    }

  } catch (lbann_exception& e) {
    lbann_report_exception(e, comm);
  }

  finalize(comm);
  return 0;
}

void test_is_shuffled(generic_data_reader *reader, bool is_shuffled, const char *msg) {
  const std::vector<int> &indices = reader->get_shuffled_indices();
  std::cerr << "\nstarting test_is_suffled; mini_batch_size: " << mini_batch_size 
            << " indices.size(): " << indices.size();
  if (msg) {
    std::cout << " :: " << msg;
  }
  std::cout << std::endl;

  bool yes = false;  //if true true: indices are actaully shuffled
  for (int h=0; h<mini_batch_size; h++) {
    if (indices[h] != h) {
      yes = true;
    }
  }
  std::cout << "testing for is_shuffled = " << is_shuffled << " test shows the shuffled is actually "
            << yes << " :: ";
  if (yes == is_shuffled) {
    std::cout << "PASSED!\n";
  } else {
    std::cout << "FAILED!\n";
  }
}