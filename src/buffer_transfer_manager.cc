#include "buffer_transfer_manager.h"

#include <cassert>

#include "runtime.h"

namespace celerity {

void buffer_transfer_manager::poll() {
	poll_transfers();
	update_incoming_transfers();
	update_outgoing_transfers();
}

void buffer_transfer_manager::poll_transfers() {
	MPI_Status status;
	int flag;
	MPI_Message msg;
	MPI_Improbe(MPI_ANY_SOURCE, CELERITY_MPI_TAG_DATA_TRANSFER, MPI_COMM_WORLD, &flag, &msg, &status);
	if(flag == 0) {
		// No incoming transfers at the moment
		return;
	}

	int count;
	MPI_Get_count(&status, MPI_CHAR, &count);
	const int data_size = count - sizeof(data_header);

	auto transfer = std::make_unique<transfer_in>();
	transfer->data.resize(data_size);
	transfer->data_type = mpi_support::build_single_use_composite_type({{sizeof(data_header), &transfer->header}, {data_size, &transfer->data[0]}});

	// Start receiving data
	MPI_Imrecv(MPI_BOTTOM, 1, *transfer->data_type, &msg, &transfer->request);
	incoming_transfers.push_back(std::move(transfer));

	transfer_logger->info("Receiving incoming data of size {} from {}", data_size, status.MPI_SOURCE);
}

// TODO: Copy buffer subrange in case we want to overwrite it (handle here or on job-level?)
std::shared_ptr<const buffer_transfer_manager::transfer_handle> buffer_transfer_manager::push(const command_pkg& pkg) {
	assert(pkg.cmd == command::PUSH);
	const push_data& data = pkg.data.push;

	auto t_handle = std::make_shared<transfer_handle>();
	auto data_handle =
	    runtime::get_instance().get_buffer_data(data.bid, cl::sycl::range<3>(data.subrange.offset[0], data.subrange.offset[1], data.subrange.offset[2]),
	        cl::sycl::range<3>(data.subrange.range[0], data.subrange.range[1], data.subrange.range[2]));

	const auto data_size = data_handle->linearized_data_size;
	auto transfer = std::make_unique<transfer_out>(std::move(data_handle));
	transfer->handle = t_handle;
	transfer->header.subrange = data.subrange;
	transfer->header.bid = data.bid;
	transfer->header.push_cid = pkg.cid;
	transfer->data_type = mpi_support::build_single_use_composite_type({{sizeof(data_header), &transfer->header}, {data_size, transfer->get_raw_ptr()}});

	// Start transmitting data
	MPI_Isend(MPI_BOTTOM, 1, *transfer->data_type, static_cast<int>(data.target), CELERITY_MPI_TAG_DATA_TRANSFER, MPI_COMM_WORLD, &transfer->request);
	outgoing_transfers.push_back(std::move(transfer));

	return t_handle;
}

std::shared_ptr<const buffer_transfer_manager::transfer_handle> buffer_transfer_manager::await_push(const command_pkg& pkg) {
	assert(pkg.cmd == command::AWAIT_PUSH);
	const await_push_data& data = pkg.data.await_push;

	std::shared_ptr<incoming_transfer_handle> t_handle;
	// Check to see if we have (fully) received the push already
	if(push_blackboard.count(data.source_cid) != 0) {
		t_handle = push_blackboard[data.source_cid];
		push_blackboard.erase(data.source_cid);
		assert(t_handle->transfer != nullptr);
		assert(t_handle->transfer->header.bid == data.bid);
		assert(t_handle->transfer->header.subrange == data.subrange);
		write_data_to_buffer(std::move(t_handle->transfer));
	} else {
		t_handle = std::make_shared<incoming_transfer_handle>();
		// Store new handle so we can mark it as complete when the push is received
		push_blackboard[data.source_cid] = t_handle;
	}

	return t_handle;
}

void buffer_transfer_manager::update_incoming_transfers() {
	for(auto it = incoming_transfers.begin(); it != incoming_transfers.end();) {
		auto& transfer = *it;
		int flag;
		MPI_Test(&transfer->request, &flag, MPI_STATUS_IGNORE);
		if(flag == 0) {
			++it;
			continue;
		}

		// Check whether we already have an await push request
		std::shared_ptr<incoming_transfer_handle> t_handle = nullptr;
		if(push_blackboard.count(transfer->header.push_cid) != 0) {
			t_handle = push_blackboard[transfer->header.push_cid];
			push_blackboard.erase(transfer->header.push_cid);
			write_data_to_buffer(std::move(*it));
		} else {
			t_handle = std::make_shared<incoming_transfer_handle>();
			push_blackboard[transfer->header.push_cid] = t_handle;
			t_handle->transfer = std::move(*it);
		}

		t_handle->complete = true;
		it = incoming_transfers.erase(it);
	}
}

void buffer_transfer_manager::update_outgoing_transfers() {
	for(auto it = outgoing_transfers.begin(); it != outgoing_transfers.end();) {
		auto& t = *it;
		int flag;
		MPI_Test(&t->request, &flag, MPI_STATUS_IGNORE);
		if(flag == 0) {
			++it;
			continue;
		}
		t->handle->complete = true;

		// Transfer cannot be accessed after this!
		it = outgoing_transfers.erase(it);
	}
}

void buffer_transfer_manager::write_data_to_buffer(std::unique_ptr<transfer_in>&& transfer) {
	const auto& header = transfer->header;
	const detail::raw_data_handle dh{&transfer->data[0], cl::sycl::range<3>(header.subrange.range[0], header.subrange.range[1], header.subrange.range[2]),
	    cl::sycl::id<3>(header.subrange.offset[0], header.subrange.offset[1], header.subrange.offset[2])};
	runtime::get_instance().set_buffer_data(header.bid, dh);
}

} // namespace celerity
