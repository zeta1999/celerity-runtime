#pragma once

#include <deque>
#include <memory>
#include <unordered_map>

#include <mpi.h>

#include "buffer_transfer_manager.h"
#include "distr_queue.h"
#include "logger.h"
#include "mpi_support.h"
#include "types.h"

namespace celerity {

namespace detail {
	class buffer_storage_base;
	class graph_generator;
	class scheduler;
	class executor;
	class task_manager;
} // namespace detail

class runtime {
  public:
	static void init(int* argc, char** argv[]);
	static runtime& get_instance();

	~runtime();

	void startup(distr_queue* queue);
	void shutdown();

	detail::task_manager& get_task_manager();

	buffer_id register_buffer(cl::sycl::range<3> range, std::shared_ptr<detail::buffer_storage_base> buf_storage, bool host_initialized);

	/**
	 * Currently this is being called by the distr_queue on shutdown (dtor).
	 * We have to make sure all SYCl objects are free'd before the queue is destroyed.
	 * TODO: Once we get rid of TEST_do_work we'll need an alternative solution that blocks the distr_queue dtor until we're done.
	 */
	void free_buffers();

	/**
	 * This is currently a no-op. We don't know whether it is safe to free a buffer.
	 * TODO: We could mark when a buffer is no longer needed in the task graph, and free the memory accordingly.
	 */
	void unregister_buffer(buffer_id bid) {}

	std::shared_ptr<detail::raw_data_read_handle> get_buffer_data(buffer_id bid, const cl::sycl::id<3>& offset, const cl::sycl::range<3>& range) {
		assert(buffer_ptrs.at(bid) != nullptr);
		return buffer_ptrs[bid]->get_data(offset, range);
	}

	void set_buffer_data(buffer_id bid, const detail::raw_data_handle& dh) {
		assert(buffer_ptrs.at(bid) != nullptr);
		buffer_ptrs[bid]->set_data(dh);
	}

	std::shared_ptr<logger> get_logger() const { return default_logger; }

  private:
	static std::unique_ptr<runtime> instance;
	std::shared_ptr<logger> default_logger;
	std::shared_ptr<logger> graph_logger;

	distr_queue* queue = nullptr;
	size_t num_nodes;
	bool is_master;

	size_t buffer_count = 0;
	std::unordered_map<buffer_id, std::shared_ptr<detail::buffer_storage_base>> buffer_ptrs;

	// The graph generator and scheduler are only constructed on the master node.
	std::shared_ptr<detail::graph_generator> ggen;
	std::unique_ptr<detail::scheduler> scheduler;

	std::shared_ptr<detail::task_manager> task_mngr;
	std::unique_ptr<detail::executor> executor;
	std::unique_ptr<buffer_transfer_manager> btm;

	struct flush_handle {
		command_pkg pkg;
		std::vector<command_id> dependencies;
		MPI_Request req;
		mpi_support::single_use_data_type data_type;
	};
	std::deque<flush_handle> active_flushes;

	runtime(int* argc, char** argv[]);
	runtime(const runtime&) = delete;
	runtime(runtime&&) = delete;

	void flush_command(node_id target, const command_pkg& pkg, const std::vector<command_id>& dependencies);

#ifdef CELERITY_TEST
	// ------------------------------------------ TESTING UTILS ------------------------------------------
	// We have to jump through some hoops to be able to re-initialize the runtime for unit testing.
	// MPI does not like being initialized more than once per process, so we have to skip that part for
	// re-initialization.
	// ---------------------------------------------------------------------------------------------------

  public:
	/**
	 * Initializes the runtime singleton without running the MPI lifecycle more than once per process.
	 */
	static void init_for_testing() {
		if(instance == nullptr) {
			init(nullptr, nullptr);
			return;
		}
		test_skip_mpi_lifecycle = true;
		instance.reset();
		init(nullptr, nullptr);
		test_skip_mpi_lifecycle = false;
	}
#endif
  private:
	static bool test_skip_mpi_lifecycle;
};

} // namespace celerity
