#include "external_upscaler_host.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <sstream>
#include <vector>

#include "log.hpp"

static LogScope ext_log( "external_upscaler" );

namespace gamescope::ExternalUpscaler
{
	static void *s_pDl = nullptr;
	static const gs_upscaler_api_t *s_pApi = nullptr;
	static gs_upscaler_instance *s_pInstance = nullptr;
	static gs_upscaler_negotiate_result_t s_lastResult = {};

	static void UnloadLibrary()
	{
		if ( s_pInstance && s_pApi && s_pApi->destroy )
			s_pApi->destroy( s_pInstance );
		s_pInstance = nullptr;
		s_pApi = nullptr;
		if ( s_pDl )
			dlclose( s_pDl );
		s_pDl = nullptr;
	}

	bool Loaded()
	{
		return s_pInstance != nullptr;
	}

	void Shutdown()
	{
		UnloadLibrary();
		s_lastResult = {};
	}

	// Top-level string and integer members of a flat JSON object; nested values are skipped.
	// Enough for the manifest, which the host reads only a handful of keys from.
	static bool ParseManifestJson( const std::string &doc, std::vector<std::pair<std::string, std::string>> &members )
	{
		size_t i = 0;
		int depth = 0;
		std::string key;
		bool expectValue = false;
		auto readString = [&]( std::string &out ) -> bool
		{
			out.clear();
			for ( ++i; i < doc.size(); ++i )
			{
				char c = doc[i];
				if ( c == '"' )
					return true;
				if ( c == '\\' && i + 1 < doc.size() )
				{
					char e = doc[++i];
					out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
					continue;
				}
				out += c;
			}
			return false;
		};
		for ( ; i < doc.size(); ++i )
		{
			char c = doc[i];
			if ( c == '{' || c == '[' )
			{
				depth++;
				expectValue = false;
			}
			else if ( c == '}' || c == ']' )
				depth--;
			else if ( c == ':' && depth == 1 )
				expectValue = true;
			else if ( c == ',' )
				expectValue = false;
			else if ( c == '"' )
			{
				std::string str;
				if ( !readString( str ) )
					return false;
				if ( depth != 1 )
					continue;
				if ( expectValue )
				{
					members.emplace_back( key, str );
					expectValue = false;
				}
				else
					key = str;
			}
			else if ( depth == 1 && expectValue && ( isdigit( (unsigned char)c ) || c == '-' ) )
			{
				size_t j = i;
				while ( j < doc.size() && ( isdigit( (unsigned char)doc[j] ) || doc[j] == '-' ) )
					j++;
				members.emplace_back( key, doc.substr( i, j - i ) );
				i = j - 1;
				expectValue = false;
			}
		}
		return depth == 0;
	}

	static std::vector<std::string> SearchDirs()
	{
		std::vector<std::string> dirs;
		auto addList = [&]( const char *pszList, const char *pszSuffix )
		{
			if ( !pszList )
				return;
			std::stringstream ss( pszList );
			std::string dir;
			while ( std::getline( ss, dir, ':' ) )
				if ( !dir.empty() )
					dirs.push_back( dir + pszSuffix );
		};
		addList( getenv( "GAMESCOPE_UPSCALER_PATH" ), "" );
		if ( const char *pszDataHome = getenv( "XDG_DATA_HOME" ); pszDataHome && *pszDataHome )
			dirs.push_back( std::string( pszDataHome ) + "/gamescope-upscalers" );
		else if ( const char *pszHome = getenv( "HOME" ) )
			dirs.push_back( std::string( pszHome ) + "/.local/share/gamescope-upscalers" );
		dirs.push_back( GAMESCOPE_UPSCALER_DATADIR );
		const char *pszDataDirs = getenv( "XDG_DATA_DIRS" );
		addList( pszDataDirs && *pszDataDirs ? pszDataDirs : "/usr/local/share:/usr/share", "/gamescope-upscalers" );
		return dirs;
	}

	bool Resolve( const std::string &spec, PluginManifest &out )
	{
		out = {};
		if ( spec.find( '/' ) != std::string::npos )
		{
			out.name = spec;
			out.library = spec;
			out.abiVersion = GAMESCOPE_EXTERNAL_UPSCALER_ABI_VERSION;
			return true;
		}

		std::string searched;
		for ( const std::string &dir : SearchDirs() )
		{
			std::string manifestPath = dir + "/" + spec + "/manifest.json";
			searched += "\n  " + manifestPath;
			std::ifstream file( manifestPath );
			if ( !file )
				continue;
			std::stringstream buf;
			buf << file.rdbuf();

			std::vector<std::pair<std::string, std::string>> members;
			if ( !ParseManifestJson( buf.str(), members ) )
			{
				ext_log.errorf( "%s: malformed JSON", manifestPath.c_str() );
				return false;
			}
			for ( auto &[key, value] : members )
			{
				if ( key == "name" ) out.name = value;
				else if ( key == "library" ) out.library = value;
				else if ( key == "default_config" ) out.defaultConfig = value;
				else if ( key == "description" ) out.description = value;
				else if ( key == "abi_version" ) out.abiVersion = (uint32_t)strtoul( value.c_str(), nullptr, 10 );
			}
			if ( out.library.empty() )
			{
				ext_log.errorf( "%s: no \"library\"", manifestPath.c_str() );
				return false;
			}
			if ( out.abiVersion != GAMESCOPE_EXTERNAL_UPSCALER_ABI_VERSION )
			{
				ext_log.errorf( "%s: plugin ABI %u, this host speaks %u; reinstall the plugin against this host's header",
					manifestPath.c_str(), out.abiVersion, GAMESCOPE_EXTERNAL_UPSCALER_ABI_VERSION );
				return false;
			}
			if ( out.library[0] != '/' )
				out.library = dir + "/" + spec + "/" + out.library;
			return true;
		}
		ext_log.errorf( "no upscaler plugin named \"%s\"; looked for:%s", spec.c_str(), searched.c_str() );
		return false;
	}

	bool Load( const std::string &spec, const std::string &userConfig )
	{
		Shutdown();

		PluginManifest manifest;
		if ( !Resolve( spec, manifest ) )
			return false;
		const std::string &path = manifest.library;
		const std::string &config = userConfig.empty() ? manifest.defaultConfig : userConfig;

		s_pDl = dlopen( path.c_str(), RTLD_NOW | RTLD_LOCAL );
		if ( !s_pDl )
		{
			ext_log.errorf( "dlopen(%s) failed: %s", path.c_str(), dlerror() );
			return false;
		}

		auto get_api = ( gs_upscaler_get_api_fn )dlsym( s_pDl, "gamescope_external_upscaler_get_api" );
		if ( !get_api )
		{
			ext_log.errorf( "%s has no gamescope_external_upscaler_get_api symbol", path.c_str() );
			UnloadLibrary();
			return false;
		}

		s_pApi = get_api( GAMESCOPE_EXTERNAL_UPSCALER_ABI_VERSION );
		if ( !s_pApi )
		{
			ext_log.errorf( "%s declined host ABI version %u", path.c_str(), GAMESCOPE_EXTERNAL_UPSCALER_ABI_VERSION );
			UnloadLibrary();
			return false;
		}
		if ( !s_pApi->create || !s_pApi->destroy || !s_pApi->get_device_info || !s_pApi->negotiate || !s_pApi->submit )
		{
			ext_log.errorf( "%s returned an api_t with a null required function pointer", path.c_str() );
			s_pApi = nullptr;
			UnloadLibrary();
			return false;
		}

		gs_upscaler_create_desc_t desc = {};
		desc.size = sizeof( desc );
		desc.config = config.empty() ? nullptr : config.c_str();
		s_pInstance = s_pApi->create( &desc );
		if ( !s_pInstance )
		{
			ext_log.errorf( "%s create() failed (config \"%s\")", path.c_str(), config.c_str() );
			UnloadLibrary();
			return false;
		}

		gs_upscaler_device_info_t info = {};
		info.size = sizeof( info );
		s_pApi->get_device_info( s_pInstance, &info );
		ext_log.infof( "loaded %s: device \"%s\" kind %d", path.c_str(), info.name, (int)info.kind );
		return true;
	}

	bool Negotiate( const gs_upscaler_negotiate_desc_t &desc, gs_upscaler_negotiate_result_t &out )
	{
		out = {};
		out.size = sizeof( out );
		if ( !Loaded() )
			return false;
		s_pApi->negotiate( s_pInstance, &desc, &out );
		s_lastResult = out;
		if ( !out.accepted )
			ext_log.infof( "plugin declined %ux%u -> %ux%u, using GPU FSR", desc.in_w, desc.in_h, desc.out_w, desc.out_h );
		return out.accepted;
	}

	const gs_upscaler_negotiate_result_t &LastNegotiation()
	{
		return s_lastResult;
	}

	gs_upscaler_device_info_t DeviceInfo()
	{
		gs_upscaler_device_info_t info = {};
		info.size = sizeof( info );
		if ( Loaded() )
			s_pApi->get_device_info( s_pInstance, &info );
		return info;
	}

	int Submit( const gs_upscaler_submit_t &submit )
	{
		if ( !Loaded() )
			return -2;
		return s_pApi->submit( s_pInstance, &submit );
	}
}
