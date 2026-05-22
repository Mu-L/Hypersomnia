#include <filesystem>

#include "augs/log.h"
#include "augs/string/typesafe_sprintf.h"
#include "augs/filesystem/file.h"
#include "augs/readwrite/byte_file.h"

#include "all_paths.h"
#include "application/setups/server/crash_recovery/server_recovery_worker.h"

augs::path_type get_server_recovery_file_path(const port_type port) {
	return USER_DIR / typesafe_sprintf("server_recovery.%x.dat", port);
}

void write_bytes_to_recovery_file(const std::vector<std::byte>& bytes, const augs::path_type& target_path) {
	/*
		Write to a temp file then atomically rename over the target.
		On Linux rename(2) is atomic, so a crash mid-write leaves the previous
		good recovery file intact instead of a truncated one.
	*/

	auto tmp_path = target_path;
	tmp_path += ".tmp";

	try {
		augs::bytes_to_file(bytes, tmp_path);
	}
	catch (...) {
		LOG("Failed to write the recovery temp file %x.", tmp_path.string());
		return;
	}

	std::error_code ec;
	std::filesystem::rename(tmp_path, target_path, ec);

	if (ec) {
		LOG("Failed to rename recovery file %x -> %x: %x", tmp_path.string(), target_path.string(), ec.message());
	}
}

std::optional<std::vector<std::byte>> read_recovery_file(const augs::path_type& source_path) {
	if (!augs::exists(source_path)) {
		return std::nullopt;
	}

	try {
		return augs::file_to_bytes(source_path);
	}
	catch (...) {
		LOG("Exception while reading recovery file %x. Ignoring.", source_path.string());
		return std::nullopt;
	}
}

void server_recovery_worker::push(
	std::vector<std::byte>&& bytes,
	augs::path_type target_path
) {
	{
		std::unique_lock<std::mutex> guard(lk);

		if (!started) {
			started = true;
			worker = std::thread([this]() { worker_loop(); });
		}

		queue.push_back(task { std::move(bytes), std::move(target_path) });
	}

	cv.notify_one();
}

void server_recovery_worker::worker_loop() {
	while (true) {
		task current;

		{
			std::unique_lock<std::mutex> guard(lk);
			cv.wait(guard, [this]() { return stop || !queue.empty(); });

			if (queue.empty()) {
				/* Only woken to stop. */
				break;
			}

			current = std::move(queue.front());
			queue.pop_front();
		}

		::write_bytes_to_recovery_file(current.bytes, current.target_path);
	}
}

server_recovery_worker::~server_recovery_worker() {
	{
		std::unique_lock<std::mutex> guard(lk);
		stop = true;
	}

	cv.notify_all();

	if (worker.joinable()) {
		worker.join();
	}
}
