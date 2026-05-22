#pragma once
#if CRASH_RECOVERY
#include <cstddef>
#include <string>
#include <vector>
#include <optional>

#include "augs/misc/secure_hash.h"
#include "augs/network/port_type.h"
#include "augs/network/network_types.h"
#include "application/network/network_common.h"
#include "application/setups/server/crash_recovery/server_recovery_worker.h"

/* The arena+mode a recovery file says it belongs to. The file is authoritative: the server loads this. */
struct server_recovery_target {
	arena_identifier arena;
	game_mode_name_type game_mode;
};

/*
	Per-server-instance crash-recovery bookkeeping. server_setup feeds it only the few
	values it needs; the game-state (de)serialization is done here, against the passed-in
	arena handle.

	File lifetime:
	  - load() reads it once at boot. If recovery is disabled, load() deletes any
	    file found so an old dump from a previous run can never be resurrected
	    after the operator flips recovery off and back on.
	  - dump_now() rewrites it on every ROUND_START mode notification while the
	    match is effectively LIVE (mode.ranked_state == LIVE). "Effectively LIVE"
	    means the mode itself, not the server config; a config flip alone (admin
	    sets ranked.autostart_when=NEVER mid-match through rcon) does NOT stop
	    the dumps as long as the current match is still actually running.
	  - update_live_state() runs every tick and deletes the file on the exact tick
	    the match stops being effectively LIVE, by spotting the ranked_live: true
	    -> false transition. This covers every reason the game might transition
	    out of a live ranked state:
	        * natural match end (final round, summary)
	        * admin /restart_match (arena_mode::restart_match() forces ranked_state=NONE)
	        * admin reconfigures the server (ranked -> casual through rcon)
	          AND then restarts the match - the config flip alone keeps the file
	          alive, but the restart that follows tears the LIVE state down and
	          the falling edge fires
	        * abandon / disqualification path (sets ranked_state=NONE)
	        * /map cycle or any other rechoose that takes the mode out of LIVE
	        * any future code that, for whatever reason, drops ranked_state away
	          from LIVE - this hook does not care WHY, only that it happened
	    A file written during LIVE becomes meaningless the moment the match is
	    no longer effectively LIVE; we do not want a crash during the following
	    warmup to load the previous match's last LIVE state.
	  - Graceful shutdown is the one intentional exception: if the process exits
	    cleanly while still LIVE, the file is left in place so a relaunch
	    resumes the interrupted match (admin may have hit /quit by mistake).
*/

struct server_crash_recovery {
	server_recovery_worker& worker;
	std::optional<std::vector<std::byte>> pending_bytes;
	port_type port = 0;

	/*
		Previous tick's ranked_live, so we can spot the live -> non-live falling
		edge (= match just ended) and drop the now-stale snapshot exactly once.

		Initialized true by load() when a usable file was read at boot: any file on
		disk was written during LIVE, so if we are no longer LIVE on the first tick
		(e.g. consume failed and we are sitting in fresh warmup), the falling edge
		fires and the orphan file gets cleaned up. Defaults to false otherwise.
	*/
	bool was_ranked_live = false;

	explicit server_crash_recovery(server_recovery_worker& worker) : worker(worker) {}

	/*
		Reads this port's recovery file (only if enabled) and, if a usable snapshot is
		present (matching version), returns the arena+mode to resume - the rest is kept in
		memory to be applied by consume() once that arena is loaded. Returns nullopt if
		there is nothing to recover.

		Unreadable / corrupt / wrong-version files are deleted in-place so we don't keep
		trying to parse them on every restart. When recovery is disabled, any file found
		is also deleted - leaving it would let an old dump resurrect itself the next time
		the operator re-enables recovery on this port.
	*/
	std::optional<server_recovery_target> load(port_type port, bool enabled);

	/*
		Throws away any in-memory snapshot AND the on-disk file. Use when the caller
		decided not to consume after load() returned a target (e.g. the recovery
		arena no longer exists on disk).
	*/
	void discard();

	/*
		Falling-edge match-end cleanup. Call every tick. Deletes the file the first
		tick after ranked_live drops from true to false. No serialization, no IO
		beyond that single delete - cheap to call unconditionally.
	*/
	void update_live_state(bool ranked_live);

	/*
		Serializes the current arena state and hands the bytes to the shared worker
		to write. Triggered by the ROUND_START mode notification. No-op when recovery
		is disabled, the match is not effectively LIVE, or the previous write is
		still in flight (one queued snapshot per instance at a time).
	*/
	void dump_now(
		const online_arena_handle<true>& arena,
		bool enabled,
		bool ranked_live,
		const std::string& arena_name,
		const augs::secure_hash_type& arena_hash
	);

	/*
		Applies the loaded snapshot onto the (already loaded, file-chosen) arena. Still
		verifies the arena's content hash to catch the map changing between crash and
		restart; on any mismatch the snapshot is discarded. Returns whether it recovered.
	*/
	bool consume(
		const online_arena_handle<false>& arena,
		const augs::secure_hash_type& arena_hash
	);

private:
	augs::path_type get_file_path() const;
	void delete_file_now();
	void push(std::vector<std::byte>&& bytes);
};
#endif
