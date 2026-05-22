#include "augs/log.h"
#include "augs/string/typesafe_sprintf.h"
#include "augs/templates/remove_cref.h"
#include "augs/filesystem/file.h"

#include "hypersomnia_version.h"
#include "game/cosmos/cosmos.h"
#include "game/cosmos/change_solvable_significant.h"
#include "game/modes/arena_mode.h"

#include "application/setups/server/crash_recovery/server_crash_recovery.h"
#include "application/setups/server/crash_recovery/server_recovery_serialize.h"

augs::path_type server_crash_recovery::get_file_path() const {
	return ::get_server_recovery_file_path(port);
}

void server_crash_recovery::delete_file_now() {
	const auto path = get_file_path();

	if (augs::exists(path)) {
		augs::remove_file(path);
	}
}

void server_crash_recovery::discard() {
	pending_bytes.reset();
	delete_file_now();
}

std::optional<server_recovery_target> server_crash_recovery::load(
	const port_type in_port,
	const bool enabled
) {
	port = in_port;

	if (!enabled) {
		/*
			Don't leave a stale file from a previous run lying around. If the operator
			flips recovery back on later, an old dump on this port would resurrect a
			match from the previous configuration.
		*/
		delete_file_now();
		return std::nullopt;
	}

	pending_bytes = ::read_recovery_file(get_file_path());

	if (!pending_bytes.has_value()) {
		return std::nullopt;
	}

	auto metadata = server_recovery_metadata();
	std::size_t body_offset = 0;

	const bool usable =
		::parse_server_recovery_header(*pending_bytes, metadata, body_offset)
		&& metadata.version == hypersomnia_version().get_version_string()
	;

	if (!usable) {
		/*
			Don't leave a corrupt or version-mismatched file lying around -
			it would just fail to parse again on every restart.
		*/
		pending_bytes.reset();
		delete_file_now();
		return std::nullopt;
	}

	/*
		File on disk represents some past LIVE tick. Pretend we were LIVE last call so
		that the very first update_live_state() will detect the falling edge if we end
		up sitting in warmup (e.g. consume failed), and clean the orphan up.
	*/
	was_ranked_live = true;

	return server_recovery_target { metadata.arena_name, metadata.game_mode };
}

void server_crash_recovery::update_live_state(const bool ranked_live) {
	/*
		Falling edge: the match just left the LIVE state (round summary, warmup,
		map change, admin /restart, ...). The on-disk snapshot was written during
		LIVE and is now stale - drop it once, here. Every other warmup tick is
		just two bool reads.
	*/
	if (was_ranked_live && !ranked_live) {
		delete_file_now();
	}

	was_ranked_live = ranked_live;
}

void server_crash_recovery::push(std::vector<std::byte>&& bytes) {
	worker.push(std::move(bytes), get_file_path());
}

void server_crash_recovery::dump_now(
	const online_arena_handle<true>& arena,
	const bool enabled,
	const bool ranked_live,
	const std::string& arena_name,
	const augs::secure_hash_type& arena_hash
) {
	if (!enabled) {
		return;
	}

	/* Only dump a live ranked match, never warmup or non-ranked. */
	if (!ranked_live) {
		return;
	}

	auto metadata = server_recovery_metadata();

	metadata.version = hypersomnia_version().get_version_string();
	metadata.arena_name = arena_name;
	metadata.game_mode = std::string(arena.get_current_game_mode_name());
	metadata.arena_hash = arena_hash;

	/*
		Serialize on the main thread, reusing the same snapshot serialization that
		sends full state to joining clients (the only serialization instantiated for
		the complete solvable significant). Only the blocking disk write is deferred
		to the shared worker.
	*/

	const auto& signi = arena.advanced_cosm.get_solvable().significant;
	const auto& flavours = arena.advanced_cosm.get_common_significant().flavours;

	auto file_bytes = ::serialize_server_recovery_state(
		metadata,
		flavours,
		arena.clean_round_state,
		signi,
		arena.current_mode_state
	);

	push(std::move(file_bytes));
}

bool server_crash_recovery::consume(
	const online_arena_handle<false>& arena,
	const augs::secure_hash_type& arena_hash
) {
	const auto pending = std::move(pending_bytes);
	pending_bytes.reset();

	if (!pending.has_value()) {
		return false;
	}

	const auto& all = *pending;

	auto discard_with_reason = [](const std::string& why) {
		LOG("Discarding the recovery file: %x", why);
	};

	auto metadata = server_recovery_metadata();
	auto significant = cosmos_solvable_significant();
	auto mode = all_modes_variant();

	if (!::parse_server_recovery_state(all, arena.clean_round_state, metadata, significant, mode)) {
		discard_with_reason("unreadable or corrupt file");
		return false;
	}

	/*
		The version was already validated by load(); the arena was loaded from this very
		file (it is authoritative), so the only thing left to verify is that its content
		hasn't changed since the crash - otherwise the saved solvable would reference stale
		flavour ids.
	*/
	if (metadata.arena_hash != arena_hash) {
		discard_with_reason(typesafe_sprintf("arena content changed since the crash (arena: %x)", metadata.arena_name));
		return false;
	}

	/*
		Apply the solvable significant the same way a client applies a full snapshot.
		REFRESH reinfers the cosmos right here, so the solver is correct next tick.
	*/
	cosmic::change_solvable_significant(
		arena.advanced_cosm,
		[&](cosmos_solvable_significant& into) {
			into = std::move(significant);
			return changer_callback_result::REFRESH;
		}
	);

	arena.current_mode_state = std::move(mode);

	arena.on_mode(
		[&](auto& typed_mode) {
			typed_mode.recover_from_crash();
		}
	);

	LOG(
		"Successfully recovered the ranked match state for arena %x (%x).",
		metadata.arena_name,
		metadata.game_mode
	);

	return true;
}
