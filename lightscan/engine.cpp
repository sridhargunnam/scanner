/* Copyright 2016 Carnegie Mellon University, NVIDIA Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "lightscan/engine.h"

#include "lightscan/util/common.h"
#include "lightscan/util/util.h"

///////////////////////////////////////////////////////////////////////////////
/// Worker thread arguments
struct LoadThreadArgs {
  // Uniform arguments
  std::string dataset_name;
  const std::vector<std::string>& video_paths;
  const std::vector<DatasetItemMetadata>& metadata;
  const std::vector<VideoWorkItem>& work_items;

  // Per worker arguments
  StorageConfig* storage_config;
  Profiler& profiler;

  // Queues for communicating work
  Queue<LoadWorkEntry>& load_work;
  Queue<DecodeWorkEntry>& decode_work;
};

struct DecodeThreadArgs {
  // Uniform arguments
  const std::vector<DatasetItemMetadata>& metadata;
  const std::vector<std::vector<char>>& metadata_packets;
  const std::vector<VideoWorkItem>& work_items;

  // Per worker arguments
  int gpu_device_id;
  CUcontext cuda_context; // context to use to decode frames
  Profiler& profiler;

  // Queues for communicating work
  Queue<DecodeWorkEntry>& decode_work;
  Queue<DecodeBufferEntry>& empty_decode_buffers;
  Queue<EvalWorkEntry>& eval_work;
};

struct EvaluateThreadArgs {
  // Uniform arguments
  const std::vector<DatasetItemMetadata>& metadata;
  const std::vector<VideoWorkItem>& work_items;
  const NetDescriptor& net_descriptor;

  // Per worker arguments
  int gpu_device_id; // for hardware decode, need to know gpu
  Profiler& profiler;

  // Queues for communicating work
  Queue<EvalWorkEntry>& eval_work;
  Queue<DecodeBufferEntry>& empty_decode_buffers;
  Queue<SaveWorkEntry>& save_work;
};

struct SaveThreadArgs {
  // Uniform arguments
  std::string job_name;
  const std::vector<std::string>& video_paths;
  const std::vector<DatasetItemMetadata>& metadata;
  const std::vector<VideoWorkItem>& work_items;

  // Per worker arguments
  StorageConfig* storage_config;
  Profiler& profiler;

  // Queues for communicating work
  Queue<SaveWorkEntry>& save_work;
};

///////////////////////////////////////////////////////////////////////////////
/// Thread to asynchronously load video
void* load_video_thread(void* arg) {
  LoadThreadArgs& args = *reinterpret_cast<LoadThreadArgs*>(arg);

  auto setup_start = now();

  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  // Setup a distinct storage backend for each IO thread
  StorageBackend* storage =
    StorageBackend::make_from_config(args.storage_config);

  std::string last_video_path;
  RandomReadFile* video_file = nullptr;
  uint64_t file_size;

  args.profiler.add_interval("setup", setup_start, now());

  while (true) {
    auto idle_start = now();

    LoadWorkEntry load_work_entry;
    args.load_work.pop(load_work_entry);

    if (load_work_entry.work_item_index == -1) {
      break;
    }

    args.profiler.add_interval("idle", idle_start, now());

    auto work_start = now();

    const VideoWorkItem& work_item =
      args.work_items[load_work_entry.work_item_index];

    const std::string& video_path = args.video_paths[work_item.video_index];
    const DatasetItemMetadata& metadata = args.metadata[work_item.video_index];

    if (video_path != last_video_path) {
      if (video_file != nullptr) {
        delete video_file;
        video_file = nullptr;
      }

      // Open the video file for reading
      storage->make_random_read_file(
        dataset_item_data_path(args.dataset_name, video_path),
        video_file);

      video_file->get_size(file_size);
    }
    last_video_path = video_path;

    // Place end of file and num frame at end of iframe to handle edge case
    std::vector<int64_t> keyframe_positions = metadata.keyframe_positions;
    std::vector<int64_t> keyframe_byte_offsets = metadata.keyframe_byte_offsets;
    keyframe_positions.push_back(metadata.frames);
    keyframe_byte_offsets.push_back(file_size);

    // Read the bytes from the file that correspond to the sequences
    // of frames we are interested in decoding. This sequence will contain
    // the bytes starting at the iframe at or preceding the first frame we are
    // interested and will continue up to the bytes before the iframe at or
    // after the last frame we are interested in.

    size_t start_keyframe_index = std::numeric_limits<size_t>::max();
    for (size_t i = 1; i < keyframe_positions.size(); ++i) {
      if (keyframe_positions[i] > work_item.start_frame) {
        start_keyframe_index = i - 1;
        break;
      }
    }
    assert(start_keyframe_index != std::numeric_limits<size_t>::max());
    uint64_t start_keyframe_byte_offset =
      static_cast<uint64_t>(keyframe_byte_offsets[start_keyframe_index]);

    size_t end_keyframe_index = 0;
    for (size_t i = start_keyframe_index; i < keyframe_positions.size(); ++i) {
      if (keyframe_positions[i] >= work_item.end_frame) {
        end_keyframe_index = i;
        break;
      }
    }
    assert(end_keyframe_index != 0);
    uint64_t end_keyframe_byte_offset =
      static_cast<uint64_t>(keyframe_byte_offsets[end_keyframe_index]);

    size_t data_size = end_keyframe_byte_offset - start_keyframe_byte_offset;

    char* buffer = new char[data_size];

    auto io_start = now();

    size_t size_read;
    StoreResult result;
    EXP_BACKOFF(
      video_file->read(
        start_keyframe_byte_offset, data_size, buffer, size_read),
      result);
    assert(size_read == data_size);
    assert(result == StoreResult::Success ||
           result == StoreResult::EndOfFile);

    args.profiler.add_interval("io", io_start, now());

    args.profiler.add_interval("task", work_start, now());

    DecodeWorkEntry decode_work_entry;
    decode_work_entry.work_item_index = load_work_entry.work_item_index;
    decode_work_entry.start_keyframe = keyframe_positions[start_keyframe_index];
    decode_work_entry.end_keyframe = keyframe_positions[end_keyframe_index];
    decode_work_entry.encoded_data_size = data_size;
    decode_work_entry.buffer = buffer;
    args.decode_work.push(decode_work_entry);
  }

  printf("(N: %d) Load thread finished.\n",
         rank);

  // Cleanup
  if (video_file != nullptr) {
    delete video_file;
  }
  delete storage;

  THREAD_RETURN_SUCCESS();
}

///////////////////////////////////////////////////////////////////////////////
/// Thread to decode video
void* decode_thread(void* arg) {
  DecodeThreadArgs& args = *reinterpret_cast<DecodeThreadArgs*>(arg);

  auto setup_start = now();

  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  // HACK(apoms): For the metadata that the VideoDecoder cares about (chroma and
  //              codec type) all videos should be the same for now so just use
  //              the first.
  CU_CHECK(cudaSetDevice(args.gpu_device_id));

  VideoDecoder decoder{
    args.cuda_context,
    args.metadata[0],
    args.metadata_packets[0]};
  decoder.set_profiler(&args.profiler);

  args.profiler.add_interval("setup", setup_start, now());

  while (true) {
    auto idle_start = now();

    DecodeWorkEntry decode_work_entry;
    args.decode_work.pop(decode_work_entry);

    if (decode_work_entry.work_item_index == -1) {
      break;
    }

    DecodeBufferEntry decode_buffer_entry;
    args.empty_decode_buffers.pop(decode_buffer_entry);

    args.profiler.add_interval("idle", idle_start, now());

    auto work_start = now();

    const VideoWorkItem& work_item =
      args.work_items[decode_work_entry.work_item_index];
    const DatasetItemMetadata& metadata = args.metadata[work_item.video_index];

    size_t encoded_buffer_size = decode_work_entry.encoded_data_size;
    char* encoded_buffer = decode_work_entry.buffer;

    size_t decoded_buffer_size = decode_buffer_entry.buffer_size;
    char* decoded_buffer = decode_buffer_entry.buffer;

    size_t frame_size =
      av_image_get_buffer_size(AV_PIX_FMT_NV12,
                               metadata.width,
                               metadata.height,
                               1);

    size_t encoded_buffer_offset = 0;

    bool discontinuity = true;
    int current_frame = decode_work_entry.start_keyframe;
    while (current_frame < work_item.end_frame) {
      auto video_start = now();

      int encoded_packet_size = 0;
      char* encoded_packet = nullptr;
      if (encoded_buffer_offset < encoded_buffer_size) {
        encoded_packet_size =
          *reinterpret_cast<int*>(encoded_buffer + encoded_buffer_offset);
        encoded_buffer_offset += sizeof(int);
        encoded_packet = encoded_buffer + encoded_buffer_offset;
        encoded_buffer_offset += encoded_packet_size;
      }

      if (decoder.feed(encoded_packet, encoded_packet_size, discontinuity)) {
        // New frames
        bool more_frames = true;
        while (more_frames && current_frame < work_item.end_frame) {
          if (current_frame >= work_item.start_frame) {
            size_t frames_buffer_offset =
              frame_size * (current_frame - work_item.start_frame);
            assert(frames_buffer_offset < decoded_buffer_size);
            char* current_frame_buffer_pos =
              decoded_buffer + frames_buffer_offset;

            more_frames =
              decoder.get_frame(current_frame_buffer_pos, frame_size);
          } else {
            more_frames = decoder.discard_frame();
          }
          current_frame++;
        }
      }
      discontinuity = false;
    }
    // Wait on all memcpys from frames to be done
    decoder.wait_until_frames_copied();

    if (decoder.decoded_frames_buffered() > 0) {
      while (decoder.discard_frame()) {};
    }

    // Must clean up buffer allocated by load thread
    delete[] encoded_buffer;

    //decode_times.push_back(decoder.time_spent_on_decode());
    //memcpy_times.push_back(memcpy_time);

    args.profiler.add_interval("task", work_start, now());

    EvalWorkEntry eval_work_entry;
    eval_work_entry.work_item_index = decode_work_entry.work_item_index;
    eval_work_entry.decoded_frames_size = decoded_buffer_size;
    eval_work_entry.buffer = decoded_buffer;
    args.eval_work.push(eval_work_entry);
  }

  printf("(N/GPU: %d/%d) Decode thread finished.\n",
         rank, args.gpu_device_id);

  THREAD_RETURN_SUCCESS();
}

///////////////////////////////////////////////////////////////////////////////
/// Thread to run net evaluation
void* evaluate_thread(void* arg) {
  EvaluateThreadArgs& args = *reinterpret_cast<EvaluateThreadArgs*>(arg);

  auto setup_start = now();

  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  CU_CHECK(cudaSetDevice(args.gpu_device_id));

  // Setup caffe net
  NetBundle net_bundle{args.net_descriptor, args.gpu_device_id};

  caffe::Net<float>& net = net_bundle.get_net();

  const boost::shared_ptr<caffe::Blob<float>> input_blob{
    net.blob_by_name(args.net_descriptor.input_layer_name)};

  const boost::shared_ptr<caffe::Blob<float>> output_blob{
    net.blob_by_name(args.net_descriptor.output_layer_name)};

  int dim = input_blob->shape(2);

  cv::cuda::setDevice(args.gpu_device_id);

  // Resize into
  std::vector<float> mean_image = args.net_descriptor.mean_image;
  cv::Mat cpu_mean_mat(
    args.net_descriptor.mean_height * 3,
    args.net_descriptor.mean_width,
    CV_32FC1,
    mean_image.data());
  cv::cuda::GpuMat unsized_mean_mat(cpu_mean_mat);
  cv::cuda::GpuMat mean_mat;
  // HACK(apoms): Resizing the mean like this is not likely to produce a correct
  //              result.
  cv::cuda::resize(unsized_mean_mat, mean_mat, cv::Size(dim, dim * 3));

  // OpenCV matrices
  std::vector<cv::cuda::Stream> cv_streams(NUM_CUDA_STREAMS);

  std::vector<cv::cuda::GpuMat> input_mats;
  for (size_t i = 0; i < NUM_CUDA_STREAMS; ++i) {
    input_mats.push_back(
      cv::cuda::GpuMat(args.metadata[0].height + args.metadata[0].height / 2,
                       args.metadata[0].width,
                       CV_8UC1));
  }

  std::vector<cv::cuda::GpuMat> rgba_mat;
  for (size_t i = 0; i < NUM_CUDA_STREAMS; ++i) {
    rgba_mat.push_back(
      cv::cuda::GpuMat(args.metadata[0].height,
                       args.metadata[0].width,
                       CV_8UC4));
  }

  std::vector<cv::cuda::GpuMat> rgb_mat;
  for (size_t i = 0; i < NUM_CUDA_STREAMS; ++i) {
    rgb_mat.push_back(
      cv::cuda::GpuMat(args.metadata[0].height,
                       args.metadata[0].width,
                       CV_8UC3));
  }

  std::vector<cv::cuda::GpuMat> conv_input;
  for (size_t i = 0; i < NUM_CUDA_STREAMS; ++i) {
    conv_input.push_back(
      cv::cuda::GpuMat(dim, dim, CV_8UC3));
  }

  std::vector<cv::cuda::GpuMat> conv_planar_input;
  for (size_t i = 0; i < NUM_CUDA_STREAMS; ++i) {
    conv_planar_input.push_back(
      cv::cuda::GpuMat(dim * 3, dim, CV_8UC1));
  }

  std::vector<cv::cuda::GpuMat> float_conv_input;
  for (size_t i = 0; i < NUM_CUDA_STREAMS; ++i) {
    float_conv_input.push_back(
      cv::cuda::GpuMat(dim * 3, dim, CV_32FC1));
  }

  std::vector<cv::cuda::GpuMat> normed_input;
  for (size_t i = 0; i < NUM_CUDA_STREAMS; ++i) {
    normed_input.push_back(
      cv::cuda::GpuMat(dim * 3, dim, CV_32FC1));
  }

  args.profiler.add_interval("setup", setup_start, now());

  while (true) {
    auto idle_start = now();
    // Wait for buffer to process
    EvalWorkEntry work_entry;
    args.eval_work.pop(work_entry);

    if (work_entry.work_item_index == -1) {
      break;
    }

    args.profiler.add_interval("idle", idle_start, now());

    auto work_start = now();

    char* frame_buffer = work_entry.buffer;

    const VideoWorkItem& work_item =
      args.work_items[work_entry.work_item_index];
    const DatasetItemMetadata& metadata = args.metadata[work_item.video_index];

    size_t frame_size =
      av_image_get_buffer_size(AV_PIX_FMT_NV12,
                               metadata.width,
                               metadata.height,
                               1);

    // Create size of output buffer equal to number of frames multiplied by
    // the size of the output vector for each image of a batch
    size_t output_size_per_frame = output_blob->count(1) * sizeof(float);
    size_t output_buffer_size =
      (work_item.end_frame - work_item.start_frame) * output_size_per_frame;
    char* output_buffer = new char[output_buffer_size];

    int current_frame = work_item.start_frame;
    while (current_frame < work_item.end_frame) {
      int frame_offset = current_frame - work_item.start_frame;
      int batch_size =
        std::min(GLOBAL_BATCH_SIZE, work_item.end_frame - current_frame);

      if (input_blob->shape(0) != batch_size) {
        input_blob->Reshape({batch_size, 3, dim, dim});
      }

      float* net_input_buffer = input_blob->mutable_gpu_data();

      // Process batch of frames
      auto cv_start = now();
      for (int i = 0; i < batch_size; ++i) {
        int sid = i % NUM_CUDA_STREAMS;
        cv::cuda::Stream& cv_stream = cv_streams[sid];

        char* buffer = frame_buffer + frame_size * (i + frame_offset);

        input_mats[sid] =
          cv::cuda::GpuMat(
            metadata.height + metadata.height / 2,
            metadata.width,
            CV_8UC1,
            buffer);

        convertNV12toRGBA(input_mats[sid], rgba_mat[sid],
                          metadata.width, metadata.height,
                          cv_stream);
        cv::cuda::cvtColor(rgba_mat[sid], rgb_mat[sid], CV_BGRA2BGR, 0,
                           cv_stream);
        cv::cuda::resize(rgb_mat[sid], conv_input[sid], cv::Size(dim, dim),
                         0, 0, cv::INTER_LINEAR, cv_stream);
        // Changed from interleaved BGR to planar BGR
        convertRGBInterleavedToPlanar(conv_input[sid], conv_planar_input[sid],
                                      dim, dim,
                                      cv_stream);
        conv_planar_input[sid].convertTo(
          float_conv_input[sid], CV_32FC1, cv_stream);
        cv::cuda::subtract(float_conv_input[sid], mean_mat, normed_input[sid],
                           cv::noArray(), -1, cv_stream);
        cudaStream_t s = cv::cuda::StreamAccessor::getStream(cv_stream);
        CU_CHECK(cudaMemcpy2DAsync(
                   net_input_buffer + i * (dim * dim * 3),
                   dim * sizeof(float),
                   normed_input[sid].data,
                   normed_input[sid].step,
                   dim * sizeof(float),
                   dim * 3,
                   cudaMemcpyDeviceToDevice,
                   s));

        // For checking for proper encoding
        if (false && ((current_frame + i) % 512) == 0) {
          CU_CHECK(cudaDeviceSynchronize());
          size_t image_size = metadata.width * metadata.height * 3;
          uint8_t* image_buff = new uint8_t[image_size];

          for (int i = 0; i < rgb_mat[sid].rows; ++i) {
            CU_CHECK(cudaMemcpy(image_buff + metadata.width * 3 * i,
                                rgb_mat[sid].ptr<uint8_t>(i),
                                metadata.width * 3,
                                cudaMemcpyDeviceToHost));
          }
          JPEGWriter writer;
          writer.header(metadata.width, metadata.height, 3, JPEG::COLOR_RGB);
          std::vector<uint8_t*> rows(metadata.height);
          for (int i = 0; i < metadata.height; ++i) {
            rows[i] = image_buff + metadata.width * 3 * i;
          }
          std::string image_path =
            "frame" + std::to_string(current_frame + i) + ".jpg";
          writer.write(image_path, rows.begin());
          delete[] image_buff;
        }
      }
      CU_CHECK(cudaDeviceSynchronize());
      args.profiler.add_interval("cv", cv_start, now());

      // Compute features
      auto net_start = now();
      net.Forward();
      args.profiler.add_interval("net", net_start, now());

      // Save batch of frames
      CU_CHECK(cudaMemcpy(
                 output_buffer + frame_offset * output_size_per_frame,
                 output_blob->gpu_data(),
                 batch_size * output_size_per_frame,
                 cudaMemcpyDeviceToHost));

      current_frame += batch_size;
    }
    args.profiler.add_interval("task", work_start, now());

    DecodeBufferEntry empty_buffer_entry;
    empty_buffer_entry.buffer_size = work_entry.decoded_frames_size;
    empty_buffer_entry.buffer = frame_buffer;
    args.empty_decode_buffers.push(empty_buffer_entry);

    SaveWorkEntry save_work_entry;
    save_work_entry.work_item_index = work_entry.work_item_index;
    save_work_entry.output_buffer_size = output_buffer_size;
    save_work_entry.buffer = output_buffer;
    args.save_work.push(save_work_entry);
  }

  printf("(N/GPU: %d/%d) Evaluate thread finished.\n",
         rank, args.gpu_device_id);

  THREAD_RETURN_SUCCESS();
}

///////////////////////////////////////////////////////////////////////////////
/// Thread to asynchronously save result buffers
void* save_thread(void* arg) {
  SaveThreadArgs& args = *reinterpret_cast<SaveThreadArgs*>(arg);

  auto setup_start = now();

  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  // Setup a distinct storage backend for each IO thread
  StorageBackend* storage =
    StorageBackend::make_from_config(args.storage_config);

  args.profiler.add_interval("setup", setup_start, now());

  while (true) {
    auto idle_start = now();

    SaveWorkEntry save_work_entry;
    args.save_work.pop(save_work_entry);

    if (save_work_entry.work_item_index == -1) {
      break;
    }

    args.profiler.add_interval("idle", idle_start, now());

    auto work_start = now();

    const VideoWorkItem& work_item =
      args.work_items[save_work_entry.work_item_index];

    const std::string& video_path = args.video_paths[work_item.video_index];
    const DatasetItemMetadata& metadata = args.metadata[work_item.video_index];

    const std::string output_path =
      job_item_output_path(args.job_name,
                           video_path,
                           work_item.start_frame,
                           work_item.end_frame);

    // Open the video file for reading
    WriteFile* output_file = nullptr;
    storage->make_write_file(output_path, output_file);

    auto io_start = now();

    StoreResult result;
    EXP_BACKOFF(
      output_file->append(
        save_work_entry.output_buffer_size,
        save_work_entry.buffer),
      result);
    assert(result == StoreResult::Success ||
           result == StoreResult::EndOfFile);

    output_file->save();

    delete output_file;

    delete[] save_work_entry.buffer;

    args.profiler.add_interval("io", io_start, now());

    args.profiler.add_interval("task", work_start, now());
  }

  printf("(N: %d) Save thread finished.\n", rank);

  // Cleanup
  delete storage;

  THREAD_RETURN_SUCCESS();
}

void run_job(
  StorageConfig* config,
  const std::string& job_name,
  const std::string& dataset_name,
  const std::string& net_descriptor_file)
{
  StorageBackend* storage = StorageBackend::make_from_config(config);

  // Load the dataset descriptor to find all data files
  DatasetDescriptor descriptor;
  {
    std::unique_ptr<RandomReadFile> file;
    exit_on_error(
      make_unique_random_read_file(storage,
                                   dataset_descriptor_path(dataset_name),
                                   file));
    int64_t pos = 0;
    descriptor = deserialize_dataset_descriptor(file, pos);
  }

  // Load net descriptor for specifying target network
  NetDescriptor net_descriptor;
  {
    std::ifstream s{net_descriptor_file};
    net_descriptor = descriptor_from_net_file(s);
  }

  // Establish base time to use for profilers
  timepoint_t base_time = now();

  // Get video metadata for all videos for distributing with work items
  std::vector<std::string>& video_paths = descriptor.item_names;

  std::vector<DatasetItemMetadata> video_metadata;
  for (size_t i = 0; i < video_paths.size(); ++i) {
    const std::string& path = video_paths[i];
    std::unique_ptr<RandomReadFile> metadata_file;
    exit_on_error(
      make_unique_random_read_file(
        storage,
        dataset_item_metadata_path(dataset_name, path),
        metadata_file));
    int64_t pos = 0;
    video_metadata.push_back(
      deserialize_dataset_item_metadata(metadata_file, pos));
  }

  // Break up videos and their frames into equal sized work items
  const int WORK_ITEM_SIZE = frames_per_work_item();
  std::vector<VideoWorkItem> work_items;
  // Track how work was broken up for each video so we can know how the
  // output will be chunked up when saved out
  JobDescriptor job_descriptor;
  job_descriptor.dataset_name = dataset_name;
  uint32_t total_frames = 0;
  for (size_t i = 0; i < video_paths.size(); ++i) {
    const DatasetItemMetadata& meta = video_metadata[i];

    std::vector<std::tuple<int, int>>& work_intervals =
      job_descriptor.intervals[video_paths[i]];
    int32_t allocated_frames = 0;
    while (allocated_frames < meta.frames) {
      int32_t frames_to_allocate =
        std::min(WORK_ITEM_SIZE, meta.frames - allocated_frames);

      VideoWorkItem item;
      item.video_index = i;
      item.start_frame = allocated_frames;
      item.end_frame = allocated_frames + frames_to_allocate;
      work_items.push_back(item);
      work_intervals.emplace_back(item.start_frame, item.end_frame);

      allocated_frames += frames_to_allocate;
    }

    total_frames += meta.frames;
  }
  if (is_master(rank)) {
    printf("Total work items: %lu, Total frames: %u\n",
           work_items.size(),
           total_frames);
  }

  // Setup shared resources for distributing work to processing threads
  Queue<LoadWorkEntry> load_work;
  Queue<DecodeWorkEntry> decode_work;
  std::vector<Queue<DecodeBufferEntry>> empty_decode_buffers(GPUS_PER_NODE);
  std::vector<Queue<EvalWorkEntry>> eval_work(GPUS_PER_NODE);
  Queue<SaveWorkEntry> save_work;

  // Allocate several buffers to hold the intermediate of an entire work item
  // to allow pipelining of load/eval
  // HACK(apoms): we are assuming that all videos have the same frame size
  // We should allocate the buffer in the load thread if we need to support
  // multiple sizes or analyze all the videos an allocate buffers for the
  // largest possible size
  size_t frame_size =
    av_image_get_buffer_size(AV_PIX_FMT_NV12,
                             video_metadata[0].width,
                             video_metadata[0].height,
                             1);
  size_t frame_buffer_size = frame_size * frames_per_work_item();
  const int LOAD_BUFFERS = TASKS_IN_QUEUE_PER_GPU;
  char*** gpu_frame_buffers = new char**[GPUS_PER_NODE];
  for (int gpu = 0; gpu < GPUS_PER_NODE; ++gpu) {
    CU_CHECK(cudaSetDevice(gpu));
    gpu_frame_buffers[gpu] = new char*[LOAD_BUFFERS];
    char** frame_buffers = gpu_frame_buffers[gpu];
    for (int i = 0; i < LOAD_BUFFERS; ++i) {
      CU_CHECK(cudaMalloc(&frame_buffers[i], frame_buffer_size));
      // Add the buffer index into the empty buffer queue so workers can
      // fill it to pass to the eval worker
      empty_decode_buffers[gpu].emplace(
        DecodeBufferEntry{frame_buffer_size, frame_buffers[i]});
    }
  }

  // Setup load workers
  std::vector<Profiler> load_thread_profilers(
    LOAD_WORKERS_PER_NODE,
    Profiler(base_time));
  std::vector<LoadThreadArgs> load_thread_args;
  for (int i = 0; i < LOAD_WORKERS_PER_NODE; ++i) {
    // Create IO thread for reading and decoding data
    load_thread_args.emplace_back(LoadThreadArgs{
        // Uniform arguments
          dataset_name,
          video_paths,
          video_metadata,
          work_items,

          // Per worker arguments
          config,
          load_thread_profilers[i],

          // Queues
          load_work,
          decode_work,
          });
  }
  std::vector<pthread_t> load_threads(LOAD_WORKERS_PER_NODE);
  for (int i = 0; i < LOAD_WORKERS_PER_NODE; ++i) {
    pthread_create(&load_threads[i], NULL, load_video_thread,
                   &load_thread_args[i]);
  }

  // Setup decode workers
  std::vector<Profiler> decode_thread_profilers(
    GPUS_PER_NODE,
    Profiler(base_time));
  std::vector<DecodeThreadArgs> decode_thread_args;
  for (int i = 0; i < GPUS_PER_NODE; ++i) {
    // Retain primary context to use for decoder
    CUcontext cuda_context;
    CUD_CHECK(cuDevicePrimaryCtxRetain(&cuda_context, i));
    // Create IO thread for reading and decoding data
    decode_thread_args.emplace_back(DecodeThreadArgs{
        // Uniform arguments
        video_metadata,
          metadata_packets,
          work_items,

          // Per worker arguments
          i % GPUS_PER_NODE,
          cuda_context,
          decode_thread_profilers[i],

          // Queues
          decode_work,
          empty_decode_buffers[i],
          eval_work[i],
          });
  }
  std::vector<pthread_t> decode_threads(GPUS_PER_NODE);
  for (int i = 0; i < GPUS_PER_NODE; ++i) {
    pthread_create(&decode_threads[i], NULL, decode_thread,
                   &decode_thread_args[i]);
  }

  // Setup evaluate workers
  std::vector<Profiler> eval_thread_profilers(
    GPUS_PER_NODE,
    Profiler(base_time));
  std::vector<EvaluateThreadArgs> eval_thread_args;
  for (int i = 0; i < GPUS_PER_NODE; ++i) {
    int gpu_device_id = i;

    // Create eval thread for passing data through neural net
    eval_thread_args.emplace_back(EvaluateThreadArgs{
        // Uniform arguments
        video_metadata,
          work_items,
          net_descriptor,

          // Per worker arguments
          gpu_device_id,
          eval_thread_profilers[i],

          // Queues
          eval_work[i],
          empty_decode_buffers[i],
          save_work
          });
  }
  std::vector<pthread_t> eval_threads(GPUS_PER_NODE);
  for (int i = 0; i < GPUS_PER_NODE; ++i) {
    pthread_create(&eval_threads[i], NULL, evaluate_thread,
                   &eval_thread_args[i]);
  }

  // Setup save workers
  std::vector<Profiler> save_thread_profilers(
    SAVE_WORKERS_PER_NODE,
    Profiler(base_time));
  std::vector<SaveThreadArgs> save_thread_args;
  for (int i = 0; i < SAVE_WORKERS_PER_NODE; ++i) {
    // Create IO thread for reading and decoding data
    save_thread_args.emplace_back(SaveThreadArgs{
        // Uniform arguments
          job_name,
          video_paths,
          video_metadata,
          work_items,

          // Per worker arguments
          config,
          save_thread_profilers[i],

          // Queues
          save_work,
          });
  }
  std::vector<pthread_t> save_threads(SAVE_WORKERS_PER_NODE);
  for (int i = 0; i < SAVE_WORKERS_PER_NODE; ++i) {
    pthread_create(&save_threads[i], NULL, save_thread,
                   &save_thread_args[i]);
  }

  // Push work into load queues
  if (is_master(rank)) {
    // Begin distributing work on master node
    int next_work_item_to_allocate = 0;
    // Wait for clients to ask for work
    while (next_work_item_to_allocate < static_cast<int>(work_items.size())) {
      // Check if we need to allocate work to our own processing thread
      int local_work = load_work.size() + decode_work.size();
      for (size_t i = 0; i < eval_work.size(); ++i) {
        local_work += eval_work[i].size();
      }
      if (local_work < GPUS_PER_NODE * TASKS_IN_QUEUE_PER_GPU) {
        LoadWorkEntry entry;
        entry.work_item_index = next_work_item_to_allocate++;
        load_work.push(entry);

        if ((static_cast<int>(work_items.size()) - next_work_item_to_allocate)
            % 10 == 0)
        {
          printf("Work items left: %d\n",
                 static_cast<int>(work_items.size()) -
                 next_work_item_to_allocate);
          fflush(stdout);
        }
        continue;
      }

      if (num_nodes > 1) {
        int more_work;
        MPI_Status status;
        MPI_Recv(&more_work, 1, MPI_INT,
                 MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        int next_item = next_work_item_to_allocate++;
        MPI_Send(&next_item, 1, MPI_INT,
                 status.MPI_SOURCE, 0, MPI_COMM_WORLD);

        if (next_work_item_to_allocate % 10 == 0) {
          printf("Work items left: %d\n",
                 static_cast<int>(work_items.size()) -
                 next_work_item_to_allocate);
        }
      }
      std::this_thread::yield();
    }
    int workers_done = 1;
    while (workers_done < num_nodes) {
      int more_work;
      MPI_Status status;
      MPI_Recv(&more_work, 1, MPI_INT,
               MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
      int next_item = -1;
      MPI_Send(&next_item, 1, MPI_INT,
               status.MPI_SOURCE, 0, MPI_COMM_WORLD);
      workers_done += 1;
      std::this_thread::yield();
    }
  } else {
    // Monitor amount of work left and request more when running low
    while (true) {
      int local_work = load_work.size() + decode_work.size();
      for (size_t i = 0; i < eval_work.size(); ++i) {
        local_work += eval_work[i].size();
      }
      if (local_work < GPUS_PER_NODE * TASKS_IN_QUEUE_PER_GPU) {
        // Request work when there is only a few unprocessed items left
        int more_work = true;
        MPI_Send(&more_work, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);
        int next_item;
        MPI_Recv(&next_item, 1, MPI_INT, 0, MPI_ANY_TAG, MPI_COMM_WORLD,
                 MPI_STATUS_IGNORE);
        if (next_item == -1) {
          // No more work left
          break;
        } else {
          LoadWorkEntry entry;
          entry.work_item_index = next_item;
          load_work.push(entry);
        }
      }
      std::this_thread::yield();
    }
  }

  // Push sentinel work entries into queue to terminate load threads
  for (int i = 0; i < LOAD_WORKERS_PER_NODE; ++i) {
    LoadWorkEntry entry;
    entry.work_item_index = -1;
    load_work.push(entry);
  }

  for (int i = 0; i < LOAD_WORKERS_PER_NODE; ++i) {
    // Wait until load has finished
    void* result;
    int err = pthread_join(load_threads[i], &result);
    if (err != 0) {
      fprintf(stderr, "error in pthread_join of load thread\n");
      exit(EXIT_FAILURE);
    }
    free(result);

  }

  // Push sentinel work entries into queue to terminate decode threads
  for (int i = 0; i < GPUS_PER_NODE; ++i) {
    DecodeWorkEntry entry;
    entry.work_item_index = -1;
    decode_work.push(entry);
  }

  for (int i = 0; i < GPUS_PER_NODE; ++i) {
    // Wait until eval has finished
    void* result;
    int err = pthread_join(decode_threads[i], &result);
    if (err != 0) {
      fprintf(stderr, "error in pthread_join of decode thread\n");
      exit(EXIT_FAILURE);
    }
    free(result);
  }

  // Cleanup
  for (int gpu = 0; gpu < GPUS_PER_NODE; ++gpu) {
    CUD_CHECK(cuDevicePrimaryCtxRelease(gpu));
  }

  // Push sentinel work entries into queue to terminate eval threads
  for (int i = 0; i < GPUS_PER_NODE; ++i) {
    EvalWorkEntry entry;
    entry.work_item_index = -1;
    eval_work[i].push(entry);
  }

  for (int i = 0; i < GPUS_PER_NODE; ++i) {
    // Wait until eval has finished
    void* result;
    int err = pthread_join(eval_threads[i], &result);
    if (err != 0) {
      fprintf(stderr, "error in pthread_join of eval thread\n");
      exit(EXIT_FAILURE);
    }
    free(result);
  }

  // Push sentinel work entries into queue to terminate save threads
  for (int i = 0; i < SAVE_WORKERS_PER_NODE; ++i) {
    SaveWorkEntry entry;
    entry.work_item_index = -1;
    save_work.push(entry);
  }

  for (int i = 0; i < SAVE_WORKERS_PER_NODE; ++i) {
    // Wait until eval has finished
    void* result;
    int err = pthread_join(save_threads[i], &result);
    if (err != 0) {
      fprintf(stderr, "error in pthread_join of save thread\n");
      exit(EXIT_FAILURE);
    }
    free(result);
  }

  // Write out metadata to describe where the output results are for each
  // video
  {
    const std::string job_file_path = job_descriptor_path(job_name);
    std::unique_ptr<WriteFile> output_file;
    make_unique_write_file(storage, job_file_path, output_file);

    serialize_job_descriptor(output_file.get(), job_descriptor);

    output_file->save();
  }

  // Execution done, write out profiler intervals for each worker
  std::string profiler_file_name = job_profiler_path(job_name, rank);
  std::ofstream profiler_output(profiler_file_name, std::fstream::binary);

  // Write out total time interval
  timepoint_t end_time = now();

  int64_t start_time_ns =
    std::chrono::time_point_cast<std::chrono::nanoseconds>(base_time)
    .time_since_epoch()
    .count();
  int64_t end_time_ns =
    std::chrono::time_point_cast<std::chrono::nanoseconds>(end_time)
    .time_since_epoch()
    .count();
  profiler_output.write((char*)&start_time_ns, sizeof(start_time_ns));
  profiler_output.write((char*)&end_time_ns, sizeof(end_time_ns));

  int64_t out_rank = rank;
  // Load worker profilers
  uint8_t load_worker_count = LOAD_WORKERS_PER_NODE;
  profiler_output.write((char*)&load_worker_count, sizeof(load_worker_count));
  for (int i = 0; i < LOAD_WORKERS_PER_NODE; ++i) {
    write_profiler_to_file(
      profiler_output, out_rank, "load", i, load_thread_profilers[i]);
  }

  // Decode worker profilers
  uint8_t decode_worker_count = GPUS_PER_NODE;
  profiler_output.write((char*)&decode_worker_count,
                        sizeof(decode_worker_count));
  for (int i = 0; i < GPUS_PER_NODE; ++i) {
    write_profiler_to_file(
      profiler_output, out_rank, "decode", i, decode_thread_profilers[i]);
  }

  // Evaluate worker profilers
  uint8_t eval_worker_count = GPUS_PER_NODE;
  profiler_output.write((char*)&eval_worker_count,
                        sizeof(eval_worker_count));
  for (int i = 0; i < GPUS_PER_NODE; ++i) {
    write_profiler_to_file(
      profiler_output, out_rank, "eval", i, eval_thread_profilers[i]);
  }

  // Save worker profilers
  uint8_t save_worker_count = SAVE_WORKERS_PER_NODE;
  profiler_output.write((char*)&save_worker_count, sizeof(save_worker_count));
  for (int i = 0; i < SAVE_WORKERS_PER_NODE; ++i) {
    write_profiler_to_file(
      profiler_output, out_rank, "save", i, save_thread_profilers[i]);
  }

  profiler_output.close();

  // Cleanup
  for (int gpu = 0; gpu < GPUS_PER_NODE; ++gpu) {
    char** frame_buffers = gpu_frame_buffers[gpu];
    for (int i = 0; i < LOAD_BUFFERS; ++i) {
      CU_CHECK(cudaSetDevice(gpu));
      CU_CHECK(cudaFree(frame_buffers[i]));
    }
    delete[] frame_buffers;
  }
  delete[] gpu_frame_buffers;

  delete storage;
}

}