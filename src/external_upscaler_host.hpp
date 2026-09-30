// Host-side loader for the external_upscaler.h plugin ABI. Owns exactly one plugin instance at a
// time (gamescope runs one upscaler); negotiation and the generic (non-zero-copy) submit path
// live here so a plugin only has to implement the ABI, never gamescope's readback shaders.
#pragma once

#include <cstdint>
#include <string>

#include "external_upscaler.h"

namespace gamescope::ExternalUpscaler
{
	struct PluginManifest
	{
		std::string name;
		std::string library;       // absolute path, resolved against the manifest's directory
		std::string defaultConfig; // used when -F external:<name> carries no config
		std::string description;
		uint32_t abiVersion = 0;
	};

	// A spec containing '/' is a library path. Anything else is a plugin name, looked up as
	// <dir>/gamescope-upscalers/<name>/manifest.json over $GAMESCOPE_UPSCALER_PATH (the directories
	// that hold the per-plugin folders), $XDG_DATA_HOME, this build's own datadir and $XDG_DATA_DIRS.
	// The first manifest found wins; one whose abi_version differs from the host's is an error, not
	// a reason to keep searching.
	bool Resolve( const std::string &spec, PluginManifest &out );

	// Resolves spec, loads it, calls create(); returns false (and logs why) on any failure: no
	// manifest, missing file or symbol, ABI mismatch, create() returning null. Safe to call again
	// after Shutdown().
	bool Load( const std::string &spec, const std::string &config );
	void Shutdown();
	bool Loaded();

	// Re-negotiates for a new size/colorspace; safe to call repeatedly (layer resize). Returns
	// false when the plugin declines.
	bool Negotiate( const gs_upscaler_negotiate_desc_t &desc, gs_upscaler_negotiate_result_t &out );

	const gs_upscaler_negotiate_result_t &LastNegotiation();
	gs_upscaler_device_info_t DeviceInfo();

	// Async submit; returns the out-fence fd (caller closes it) or -1 (out already ready) or -2
	// (hard failure).
	int Submit( const gs_upscaler_submit_t &submit );
}
