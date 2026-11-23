// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#include "bridge_worker.h"

#include "pyr_rt.h"

#include <cstring>
#include <limits>

namespace priml {
namespace {

// Largest payload accepted from a worker: a malformed header must not make the
// host allocate gigabytes. Four times a 4096-px RGB16 prepare image is ample.
inline constexpr uint32_t kBridgeMaxPayloadBytes = 1u << 30;


bool worker_fail ( std::string *error, std::string message )
{
	if (error)
		*error = std::move( message );
	return false;
}


std::string path_utf8 ( const std::filesystem::path &path )
{
	const auto text = path.u8string();
	return std::string( text.begin(), text.end() );
}

} // namespace


bool format_bridge_fields ( const BridgeFields &fields, std::string *text, std::string *error )
{
	text->clear();
	for (const auto &[key, value] : fields) {
		if (key.empty() || key.find_first_of( "=\r\n" ) != std::string::npos ||
			value.find_first_of( "\r\n" ) != std::string::npos)
			return worker_fail( error, "parameter '" + key + "' contains '=', a line break, or is empty" );
		*text += key + "=" + value + "\n";
	}
	return true;
}


BridgeFields parse_bridge_fields ( std::string_view text )
{
	auto fields = BridgeFields{};
	while (!text.empty()) {
		const auto end = text.find( '\n' );
		const auto line = text.substr( 0, end );
		text = end == std::string_view::npos ? std::string_view{} : text.substr( end + 1 );
		const auto eq = line.find( '=' );
		if (eq != std::string_view::npos && eq > 0)
			fields.emplace_back( std::string( line.substr( 0, eq ) ), std::string( line.substr( eq + 1 ) ) );
	}
	return fields;
}


bool BridgeWorker::start ( const BridgeWorkerSpec &spec, std::string *error )
{
	index_ = spec.index;
	auto process = SubprocessSpec{};
	process.executable = spec.python;
//
// -u: unbuffered streams, so a traceback reaches stderr before a crash.
	process.arguments = { "-u", path_utf8( spec.worker_script ), spec.mode, path_utf8( spec.script ) };
	process.environment = { { "PYTHONIOENCODING", "utf-8" } };
	if (spec.single_threaded)
		for (const auto *name : { "OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS",
			"NUMEXPR_NUM_THREADS", "VECLIB_MAXIMUM_THREADS" })
			process.environment.emplace_back( name, "1" );
	process.stderr_line = [index = spec.index, mode = spec.mode] ( const std::string &line ) {
		pyr::plog( "py %s[%d]: %s\n", mode.c_str(), index, line.c_str() );
	};
	if (!process_.start( process, error ))
		return false;
	started_ = true;
	auto hello = BridgeMessage{};
	if (!receive( &hello, error ))
		return false;
	if ((BridgeKind)hello.header.kind != BridgeKind::Hello)
		return worker_fail( error, "Python worker did not start with a Hello message" );
	hello_ = parse_bridge_fields( hello.text );
	return true;
}


bool BridgeWorker::send ( BridgeKind kind, BridgeHeader header, std::string_view text,
	const std::vector<std::span<const uint8_t>> &payload, std::string *error )
{
	std::memcpy( header.magic, kBridgeMagic, sizeof( header.magic ) );
	header.version = kBridgeProtocolVersion;
	header.kind = (uint16_t)kind;
	header.text_bytes = (uint32_t)text.size();
	auto bytes = uint64_t{};
	for (const auto &part : payload)
		bytes += part.size();
	if (bytes > std::numeric_limits<uint32_t>::max())
		return worker_fail( error, "bridge message payload exceeds 4 GB" );
	header.payload_bytes = (uint32_t)bytes;
	if (!process_.write_all( &header, sizeof( header ), error ) ||
		!process_.write_all( text.data(), text.size(), error ))
		return false;
	for (const auto &part : payload)
		if (!process_.write_all( part.data(), part.size(), error ))
			return false;
	return true;
}


bool BridgeWorker::receive ( BridgeMessage *message, std::string *error )
{
	if (!process_.read_exact( &message->header, sizeof( message->header ), error ))
		return false;
	const auto &header = message->header;
	if (std::memcmp( header.magic, kBridgeMagic, sizeof( header.magic ) ) != 0 ||
		header.version != kBridgeProtocolVersion)
		return worker_fail( error, "Python worker sent a malformed message (protocol mismatch?)" );
	if (header.text_bytes > kBridgeMaxPayloadBytes || header.payload_bytes > kBridgeMaxPayloadBytes)
		return worker_fail( error, "Python worker announced an oversized message" );
	message->text.resize( header.text_bytes );
	message->payload.resize( header.payload_bytes );
	if (!process_.read_exact( message->text.data(), message->text.size(), error ) ||
		!process_.read_exact( message->payload.data(), message->payload.size(), error ))
		return false;
	if ((BridgeKind)header.kind == BridgeKind::Error)
		return worker_fail( error, "Python worker " + std::to_string( index_ ) + " failed:\n" + message->text );
	return true;
}


void BridgeWorker::stop ()
{
	if (!started_)
		return;
	auto ignored = std::string{};
	if (process_.running())
		send( BridgeKind::Shutdown, {}, {}, {}, &ignored );
	process_.close_input();
	process_.wait();
	started_ = false;
}


void BridgeWorker::kill ()
{
	process_.kill();
}


std::string BridgeWorker::hello ( std::string_view key ) const
{
	for (const auto &[name, value] : hello_)
		if (name == key)
			return value;
	return {};
}

} // namespace priml
