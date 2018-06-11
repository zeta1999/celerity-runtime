#include "runtime.h"

#include <algorithm>
#include <limits>
#include <queue>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <allscale/utils/string_utils.h>
#include <mpi.h>
#include <spdlog/fmt/fmt.h>

#include "command.h"
#include "grid.h"
#include "logger.h"
#include "subrange.h"
#include "task.h"

namespace celerity {

std::unique_ptr<runtime> runtime::instance = nullptr;

void runtime::init(int* argc, char** argv[]) {
	instance = std::unique_ptr<runtime>(new runtime(argc, argv));
}

runtime& runtime::get_instance() {
	if(instance == nullptr) { throw std::runtime_error("Runtime has not been initialized"); }
	return *instance;
}

runtime::runtime(int* argc, char** argv[]) {
	// We specify MPI_THREAD_FUNNELED even though we currently don't use multiple threads,
	// as we link with various multi-threaded libraries. This will likely not make any difference,
	// but we do it anyway, just in case. See here for more information:
	// http://users.open-mpi.narkive.com/T04C74T4/ompi-users-mpi-thread-single-vs-mpi-thread-funneled
	int provided;
	MPI_Init_thread(argc, argv, MPI_THREAD_FUNNELED, &provided);
	assert(provided == MPI_THREAD_FUNNELED);

	int world_size;
	MPI_Comm_size(MPI_COMM_WORLD, &world_size);
	num_nodes = world_size;

	int world_rank;
	MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
	is_master = world_rank == 0;

	default_logger = logger("default").create_context({{"rank", std::to_string(world_rank)}});
	graph_logger = logger("graph").create_context({{"rank", std::to_string(world_rank)}});

	command_graph[boost::graph_bundle].name = "CommandGraph";

	btm = std::make_unique<buffer_transfer_manager>(default_logger);
}

runtime::~runtime() {
	// Allow BTM to clean up MPI data types before we finalize
	btm.reset();
	MPI_Finalize();
}

void send_command(node_id target, const command_pkg& pkg) {
	// Command packages are small enough to use a blocking send.
	// This way we don't have to ensure the data stays around long enough (until asynchronous send is complete).
	const auto result = MPI_Send(&pkg, sizeof(command_pkg), MPI_BYTE, target, CELERITY_MPI_TAG_CMD, MPI_COMM_WORLD);
	assert(result == MPI_SUCCESS);
}

void runtime::TEST_do_work() {
	assert(queue != nullptr);

	bool done = false;
	// Instead of sending commands to itself, master stores them here
	std::queue<command_pkg> master_commands;

	if(is_master) {
		queue->debug_print_task_graph(graph_logger);
		build_command_graph();
		graph_utils::print_graph(command_graph, graph_logger);

		// TODO: Is a BFS walk sufficient or do we need to properly check for fulfilled dependencies?
		// FIXME: This doesn't support disconnected tasks (e.g. two kernels with no dependencies whatsoever)
		graph_utils::search_vertex_bf(0, command_graph, [&master_commands, this](vertex v, const command_dag& cdag) {
			auto& v_data = cdag[v];
			if(v_data.cmd == command::NOP) { return false; }
			const command_pkg pkg{v_data.tid, v_data.cid, v_data.cmd, v_data.data};
			const node_id target = cdag[v].nid;
			if(target != 0) {
				send_command(target, pkg);
			} else {
				master_commands.push(pkg);
			}

			return false;
		});

		// Finally, send shutdown commands to all worker nodes
		for(auto n = 1; n < num_nodes; ++n) {
			command_pkg pkg{0, next_cmd_id++, command::SHUTDOWN, command_data{}};
			send_command(n, pkg);
		}

		// Master can just exit after handling all open jobs
		master_commands.push(command_pkg{0, next_cmd_id++, command::SHUTDOWN, command_data{}});
	}

	while(!done || !jobs.empty()) {
		btm->poll();

		for(auto it = jobs.begin(); it != jobs.end();) {
			auto job = *it;
			job->update();
			if(job->is_done()) {
				it = jobs.erase(it);
			} else {
				++it;
			}
		}

		bool has_pkg = false;
		command_pkg pkg;
		if(!is_master) {
			// Check for incoming commands
			// TODO: Move elswhere
			MPI_Status status;
			int flag;
			MPI_Iprobe(MPI_ANY_SOURCE, CELERITY_MPI_TAG_CMD, MPI_COMM_WORLD, &flag, &status);
			if(flag == 1) {
				// Commands should be small enough to block here
				MPI_Recv(&pkg, sizeof(command_pkg), MPI_BYTE, status.MPI_SOURCE, CELERITY_MPI_TAG_CMD, MPI_COMM_WORLD, &status);
				has_pkg = true;
			}
		} else {
			if(!master_commands.empty()) {
				pkg = master_commands.front();
				master_commands.pop();
				has_pkg = true;
			}
		}

		if(has_pkg) {
			if(pkg.cmd == command::SHUTDOWN) {
				done = true;
			} else {
				handle_command_pkg(pkg);
			}
		}
	}
}

void runtime::handle_command_pkg(const command_pkg& pkg) {
	switch(pkg.cmd) {
	case command::PUSH: create_job<push_job>(pkg, *btm); break;
	case command::AWAIT_PUSH: create_job<await_push_job>(pkg, *btm); break;
	case command::COMPUTE: create_job<compute_job>(pkg, *queue); break;
	case command::MASTER_ACCESS: create_job<master_access_job>(pkg); break;
	default: { assert(false && "Unexpected command"); }
	}
}

void runtime::register_queue(distr_queue* queue) {
	if(this->queue != nullptr) { throw std::runtime_error("Only one celerity::distr_queue can be created per process"); }
	this->queue = queue;
}

distr_queue& runtime::get_queue() {
	assert(queue != nullptr);
	return *queue;
}

// ---------------------------------------------------------------------------------------------------------------
// -----------------------------------------  COMMAND GRAPH GENERATION  ------------------------------------------
// ---------------------------------------------------------------------------------------------------------------

std::vector<subrange<1>> split_equal(const subrange<1>& sr, size_t num_chunks) {
	assert(num_chunks > 0);
	subrange<1> chunk;
	chunk.global_size = sr.global_size;
	chunk.start = cl::sycl::range<1>(0);
	chunk.range = cl::sycl::range<1>(sr.range.size() / num_chunks);

	std::vector<subrange<1>> result;
	for(auto i = 0u; i < num_chunks; ++i) {
		result.push_back(chunk);
		chunk.start = chunk.start + chunk.range;
		if(i == num_chunks - 1) { result[i].range += sr.range.size() % num_chunks; }
	}
	return result;
}

// We simply split by row for now
// TODO: There's other ways to split in 2D as well.
std::vector<subrange<2>> split_equal(const subrange<2>& sr, size_t num_chunks) {
	const auto rows =
	    split_equal(subrange<1>{cl::sycl::range<1>(sr.start[0]), cl::sycl::range<1>(sr.range[0]), cl::sycl::range<1>(sr.global_size[0])}, num_chunks);
	std::vector<subrange<2>> result;
	for(auto& row : rows) {
		result.push_back(subrange<2>{cl::sycl::range<2>(row.start[0], sr.start[1]), cl::sycl::range<2>(row.range[0], sr.range[1]), sr.global_size});
	}
	return result;
}

std::vector<subrange<3>> split_equal(const subrange<3>& sr, size_t num_chunks) {
	throw std::runtime_error("3D split_equal NYI");
}

/**
 * Assigns a number of chunks to a given set of free nodes.
 * Additionally computes the source nodes for the buffers required by the individual chunks.
 */
std::unordered_map<chunk_id, node_id> assign_chunks_to_nodes(size_t num_chunks, const chunk_buffer_requirements_map& chunk_reqs,
    const buffer_state_map& valid_buffer_regions, std::set<node_id> free_nodes, chunk_buffer_source_map& chunk_buffer_sources) {
	std::unordered_map<chunk_id, node_id> chunk_nodes;
	for(auto i = 0u; i < num_chunks; ++i) {
		bool node_assigned = false;
		node_id nid = std::numeric_limits<node_id>::max();

		for(auto& it : chunk_reqs.at(i)) {
			const buffer_id bid = it.first;

			// TODO: Are these always sorted (return value of buffer_state::get_source_nodes)? Probably not. (requried for set_intersection)
			std::unordered_set<node_id> source_nodes;

			if(it.second.count(cl::sycl::access::mode::read) > 0) {
				const auto& read_req = it.second.at(cl::sycl::access::mode::read);
				const auto& bs = valid_buffer_regions.at(bid);
				const auto sn = bs->get_source_nodes(read_req);
				assert(!sn.empty());
				chunk_buffer_sources[i][bid] = sn;
				if(sn.size() > 0) { source_nodes = sn[0].second; }
			}

			if(!node_assigned) {
				assert(free_nodes.size() > 0);

				// If the chunk doesn't have any read requirements (for this buffer!),
				// we also won't get any source nodes
				if(source_nodes.size() > 0) {
					// We simply pick the first node that contains the largest chunk of
					// the first requested buffer, given it is still available.
					// Otherwise we simply pick the first available node.
					// TODO: We should probably consider all buffers, not just the first
					std::vector<node_id> intersection;
					std::set_intersection(free_nodes.cbegin(), free_nodes.cend(), source_nodes.cbegin(), source_nodes.cend(), std::back_inserter(intersection));
					if(!intersection.empty()) {
						nid = intersection[0];
					} else {
						nid = *free_nodes.cbegin();
					}
				} else {
					nid = *free_nodes.cbegin();
				}

				assert(nid != std::numeric_limits<node_id>::max());
				node_assigned = true;
				free_nodes.erase(nid);
				chunk_nodes[i] = nid;
			}
		}
	}

	return chunk_nodes;
}

template <int Dims>
void process_compute_task(command_id& next_cmd_id, task_id tid, const compute_task* ctsk, size_t num_worker_nodes, bool master_only,
    const graph_utils::task_vertices& tv, const buffer_state_map& valid_buffer_regions, size_t& num_chunks, std::unordered_map<chunk_id, node_id>& chunk_nodes,
    chunk_buffer_requirements_map& chunk_requirements, chunk_buffer_source_map& chunk_buffer_sources, std::vector<vertex>& chunk_command_vertices,
    command_dag& command_graph) {
	assert(ctsk->get_dimensions() == Dims);

	// Split task into equal chunks for every worker node
	// TODO: In the future, we may want to adjust our split based on the range
	// mapper results and data location!
	num_chunks = num_worker_nodes;

	// The chunks have the same dimensionality as the task
	auto sr = subrange<Dims>();
	sr.global_size = boost::get<cl::sycl::range<Dims>>(ctsk->get_global_size());
	sr.range = sr.global_size;
	auto chunks = split_equal(sr, num_chunks);

	const auto& rms = ctsk->get_range_mappers();
	for(auto& it : rms) {
		const buffer_id bid = it.first;

		for(auto& rm : it.second) {
			auto mode = rm->get_access_mode();
			assert(mode == cl::sycl::access::mode::read || mode == cl::sycl::access::mode::write);
			assert(rm->get_kernel_dimensions() == Dims);

			for(auto i = 0u; i < chunks.size(); ++i) {
				subrange<3> req;
				// The chunk requirements have the dimensionality of the corresponding buffer
				switch(rm->get_buffer_dimensions()) {
				default:
				case 1: {
					req = subrange<3>((*rm).map_1(chunks[i]));
				} break;
				case 2: {
					req = subrange<3>((*rm).map_2(chunks[i]));
				} break;
				case 3: {
					req = subrange<3>((*rm).map_3(chunks[i]));
				} break;
				}
				const auto& reqs = chunk_requirements[i][bid][mode];
				chunk_requirements[i][bid][mode] = GridRegion<3>::merge(reqs, detail::subrange_to_grid_region(req));
			}
		}
	}

	std::set<node_id> free_nodes;
	for(auto i = 0u; i < num_worker_nodes + 1; ++i) {
		if(!master_only && i == 0) { continue; }
		free_nodes.insert(i);
	}

	chunk_nodes = assign_chunks_to_nodes(chunks.size(), chunk_requirements, valid_buffer_regions, free_nodes, chunk_buffer_sources);

	// Create a compute command for every chunk
	for(chunk_id i = 0u; i < chunks.size(); ++i) {
		const node_id nid = chunk_nodes[i];
		const auto cv = graph_utils::add_compute_cmd(next_cmd_id, nid, tv, subrange<3>(chunks[i]), command_graph);
		chunk_command_vertices.push_back(cv);
	}
}

void process_master_access_task(command_id& next_cmd_id, task_id tid, const master_access_task* matsk, const graph_utils::task_vertices& tv,
    const buffer_state_map& valid_buffer_regions, size_t& num_chunks, std::unordered_map<chunk_id, node_id>& chunk_nodes,
    chunk_buffer_requirements_map& chunk_requirements, chunk_buffer_source_map& chunk_buffer_sources, std::vector<vertex>& chunk_command_vertices,
    command_dag& command_graph) {
	const node_id master_node = 0;
	num_chunks = 1;
	chunk_nodes[master_node] = 0;

	const auto& buffer_accesses = matsk->get_accesses();
	for(auto& it : buffer_accesses) {
		const buffer_id bid = it.first;

		for(auto& bacc : it.second) {
			// Note that subrange_to_grid_region clamps to the global size, which is why we set this to size_t max
			// TODO: Subrange is not ideal here, we don't need the global size
			const auto req = subrange<3>{bacc.offset, bacc.range,
			    cl::sycl::range<3>(std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max())};
			const auto& reqs = chunk_requirements[master_node][bid][bacc.mode];
			chunk_requirements[master_node][bid][bacc.mode] = GridRegion<3>::merge(reqs, detail::subrange_to_grid_region(req));
		}
	}

	for(auto& it : chunk_requirements.at(master_node)) {
		const buffer_id bid = it.first;
		if(it.second.count(cl::sycl::access::mode::read) == 0) continue;

		const auto& read_req = it.second.at(cl::sycl::access::mode::read);
		const auto& bs = valid_buffer_regions.at(bid);
		const auto source_nodes = bs->get_source_nodes(read_req);
		assert(!source_nodes.empty());
		chunk_buffer_sources[master_node][bid] = source_nodes;
	}

	const auto cv = graph_utils::add_master_access_cmd(next_cmd_id, tv, command_graph);
	chunk_command_vertices.push_back(cv);
}


void update_buffer_state(const std::unordered_map<node_id, std::vector<GridRegion<3>>>& buffer_writers, detail::buffer_state& bs) {
	for(const auto& w : buffer_writers) {
		const node_id nid = w.first;
		GridRegion<3> region;
		for(const auto& r : w.second) {
			region = GridRegion<3>::merge(region, r);
		}
		bs.update_region(region, {nid});
	}
}

/**
 * Computes a command graph from the task graph, one task at a time.
 *
 * This currently (= likely to change in the future!) works as follows:
 *
 * 1) Obtain a satisfied task from the task graph.
 * 2) Split the task into equally sized chunks.
 * 3) Obtain all range mappers for that task and iterate over them,
 *    determining the read and write regions for every chunk. Note that a
 *    task may contain several read/write accessors for the same buffer,
 *    which is why we first have to compute their union regions.
 * 4) (In assign_chunks_to_nodes): Iterate over all per-chunk read regions
 *	  and try to find the most suitable node to execute that chunk on, i.e.
 *	  the node that requires the least amount of data-transfer in order to
 *	  execute that chunk. Note that currently only the first read buffer is
 *    considered, and nodes are assigned greedily.
 * 5) Insert execution (compute / master-access) commands for every node into
 *	  the command graph.
 * 6) Iterate over per-chunk reads & writes to (i) store per-buffer per-node
 *    written regions and (ii) create push / await-push commands for
 *    all nodes, inserting them as requirements for their respective
 *    execution commands.
 * 7) Finally, all per-buffer per-node written regions are used to update the
 *    data structure that keeps track of valid buffer regions.
 */
void runtime::build_command_graph() {
	// NOTE: We still need the ability to run the program on a single node (= master)
	// for easier debugging, so we create a single "split" instead of throwing
	// TODO: Remove this
	const size_t num_worker_nodes = std::max(num_nodes - 1, (size_t)1);
	const bool master_only = num_nodes == 1;

	const auto& task_graph = queue->get_task_graph();
	task_id tid;
	assert(graph_utils::get_satisfied_task(task_graph, tid));

	buffer_writers_map buffer_writers;

	auto tsk = queue->get_task(tid);
	graph_utils::task_vertices tv = graph_utils::add_task(tid, task_graph, command_graph);

	size_t num_chunks = 0;
	chunk_buffer_requirements_map chunk_reqs;
	chunk_buffer_source_map chunk_buffer_sources;
	std::unordered_map<chunk_id, node_id> chunk_nodes;
	// Vertices corresponding to the per-chunk compute / master access command
	// TODO: Either use map <chunk_id, node_id> for this, or vector for chunk_nodes - not both
	std::vector<vertex> chunk_command_vertices;

	if(tsk->get_type() == task_type::COMPUTE) {
		const auto ctsk = dynamic_cast<const compute_task*>(tsk.get());
		switch(ctsk->get_dimensions()) {
		default:
		case 1:
			process_compute_task<1>(next_cmd_id, tid, ctsk, num_worker_nodes, master_only, tv, valid_buffer_regions, num_chunks, chunk_nodes, chunk_reqs,
			    chunk_buffer_sources, chunk_command_vertices, command_graph);
			break;
		case 2:
			process_compute_task<2>(next_cmd_id, tid, ctsk, num_worker_nodes, master_only, tv, valid_buffer_regions, num_chunks, chunk_nodes, chunk_reqs,
			    chunk_buffer_sources, chunk_command_vertices, command_graph);
			break;
		case 3:
			process_compute_task<3>(next_cmd_id, tid, ctsk, num_worker_nodes, master_only, tv, valid_buffer_regions, num_chunks, chunk_nodes, chunk_reqs,
			    chunk_buffer_sources, chunk_command_vertices, command_graph);
			break;
		}
	} else if(tsk->get_type() == task_type::MASTER_ACCESS) {
		const auto matsk = dynamic_cast<const master_access_task*>(tsk.get());
		process_master_access_task(next_cmd_id, tid, matsk, tv, valid_buffer_regions, num_chunks, chunk_nodes, chunk_reqs, chunk_buffer_sources,
		    chunk_command_vertices, command_graph);
	}

	process_task_data_requirements(tid, chunk_nodes, chunk_reqs, chunk_buffer_sources, tv, chunk_command_vertices, buffer_writers);
	queue->mark_task_as_processed(tid);

	// Update buffer regions
	for(const auto& it : buffer_writers) {
		const buffer_id bid = it.first;
		update_buffer_state(it.second, *valid_buffer_regions[bid]);
	}

	// HACK: We recursively call this until all tasks have been processed
	// In the future, we may want to do this periodically in a worker thread
	if(graph_utils::get_satisfied_task(task_graph, tid)) { build_command_graph(); }
}

// Process writes and create push / await-push commands
void runtime::process_task_data_requirements(task_id tid, const std::unordered_map<chunk_id, node_id>& chunk_nodes,
    const chunk_buffer_requirements_map& chunk_requirements, const chunk_buffer_source_map& chunk_buffer_sources, const graph_utils::task_vertices& tv,
    const std::vector<vertex>& chunk_command_vertices, buffer_writers_map& buffer_writers) {
	for(auto i = 0u; i < chunk_nodes.size(); ++i) {
		const node_id nid = chunk_nodes.at(i);

		for(auto& it : chunk_requirements.at(i)) {
			const buffer_id bid = it.first;
			const vertex command_vertex = chunk_command_vertices.at(i);

			// ==== Writes ====
			if(it.second.count(cl::sycl::access::mode::write) > 0) {
				const auto& write_req = it.second.at(cl::sycl::access::mode::write);
				assert(write_req.area() > 0);
				buffer_writers[bid][nid].push_back(write_req);
				// Add to compute node label for debugging
				command_graph[command_vertex].label = fmt::format("{}\\nWrite {} {}", command_graph[command_vertex].label, bid, toString(write_req));
			}

			// ==== Reads ====
			// Add read to command node label for debugging
			if(it.second.count(cl::sycl::access::mode::read) > 0) {
				const auto& read_req = it.second.at(cl::sycl::access::mode::read);
				assert(read_req.area() > 0);
				command_graph[command_vertex].label = fmt::format("{}\\nRead {} {}", command_graph[command_vertex].label, bid, toString(read_req));
			} else {
				continue;
			}

			const std::vector<std::pair<GridBox<3>, std::unordered_set<node_id>>>& buffer_sources = chunk_buffer_sources.at(i).at(bid);

			for(auto& box_sources : buffer_sources) {
				const auto& box = box_sources.first;
				const auto& box_src_nodes = box_sources.second;

				if(box_src_nodes.count(nid) == 1) {
					// No need to push
					continue;
				}

				// We just pick the first source node for now
				const node_id source_nid = *box_src_nodes.cbegin();

				// TODO: Update buffer regions since we copied some stuff!!
				graph_utils::add_push_cmd(next_cmd_id, nid, source_nid, bid, tv, command_vertex, box, command_graph);
			}
		}
	}
}

void runtime::execute_master_access_task(task_id tid) const {
	const auto tsk = dynamic_cast<const master_access_task*>(queue->get_task(tid).get());
	master_access_livepass_handler handler;
	tsk->get_functor()(handler);
}

} // namespace celerity
