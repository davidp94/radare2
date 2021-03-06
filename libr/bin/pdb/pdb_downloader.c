/* radare - LGPL - Copyright 2014-2017 - inisider */

#include <string.h>
#include <r_util.h>
#include <r_core.h>
#include "pdb_downloader.h"

static bool checkExtract() {
#if __WINDOWS__ && !__CYGWIN__
	if (r_sys_cmd ("expand -? >nul") != 0) {
		return false;
	}
#else
	if (r_sys_cmd ("cabextract -v > /dev/null") != 0) {
		return false;
	}
#endif
	return true;
}

static bool checkCurl() {
#if __WINDOWS__ && !__CYGWIN__
	const char nul[] = "nul";
#else
	const char nul[] = "/dev/null";
#endif
	if (r_sys_cmdf ("curl --version > %s", nul) != 0) {
		return false;
	}
	return true;
}

static int download(struct SPDBDownloader *pd) {
	SPDBDownloaderOpt *opt = pd->opt;
	char *curl_cmd = NULL;
	char *extractor_cmd = NULL;
	char *abspath_to_archive = NULL;
	char *archive_name = NULL;
	size_t archive_name_len = 0;
	const char *basepath = ".";
	int res = 0;
	int cmd_ret;
	if (!opt->dbg_file || !*opt->dbg_file) {
		// no pdb debug file
		return 0;
	}
	if (!checkCurl ()) {
		return 0;
	}
	// dbg_file len is > 0
	archive_name_len = strlen (opt->dbg_file);
	archive_name = malloc (archive_name_len + 1);
	if (!archive_name) {
		return 0;
	}
	memcpy (archive_name, opt->dbg_file, archive_name_len + 1);
	archive_name[archive_name_len - 1] = '_';
	if (opt->path && *opt->path) {
		basepath = opt->path;
	}
	if (checkExtract () || opt->extract == 0) {
		res = 1;
		abspath_to_archive = r_str_newf ("%s%s%s", basepath,
		                                 R_SYS_DIR, archive_name);
		curl_cmd = r_str_newf ("curl -sfA \"%s\" \"%s/%s/%s/%s\" -o \"%s\"",
		                       opt->user_agent,
		                       opt->symbol_server,
		                       opt->dbg_file,
		                       opt->guid,
		                       archive_name,
		                       abspath_to_archive);
#if __WINDOWS__ && !__CYGWIN__
		const char *cabextractor = "expand";
		const char *format = "%s %s %s";
		char *abspath_to_file = strdup (abspath_to_archive);
		if (abspath_to_file) {
			int abspath_to_archive_len = archive_name_len + strlen (basepath) + 2;
			abspath_to_file[abspath_to_archive_len - 2] = 'b';
			// extractor_cmd -> %1 %2 %3
			// %1 - 'expand'
			// %2 - absolute path to archive
			// %3 - absolute path to file that will be dearchive
			extractor_cmd = r_str_newf (format, cabextractor,
				abspath_to_archive, abspath_to_file);
		}
#else
		const char *cabextractor = "cabextract";
		const char *format = "%s -d \"%s\" \"%s\"";
		// cabextract -d %1 %2
		// %1 - path to directory where to extract all files from cab archive
		// %2 - absolute path to cab archive
		extractor_cmd = r_str_newf (format,
		                            cabextractor, basepath, abspath_to_archive);
#endif
		if ((cmd_ret = r_sys_cmd (curl_cmd) != 0)) {
			eprintf("curl exited with error %d\n", cmd_ret);
			res = 0;
		}
		if (opt->extract > 0) {
			if (res && ((cmd_ret = r_sys_cmd (extractor_cmd)) != 0)) {
				eprintf ("cab extractor exited with error %d\n", cmd_ret);
				res = 0;
			}
			r_file_rm (abspath_to_archive);
		}
		R_FREE (curl_cmd);
		R_FREE (abspath_to_archive);
	}
	if (res == 0) {
		eprintf ("Falling back to uncompressed pdb\n");
		res = 1;
		archive_name[archive_name_len - 1] = 'b';
		abspath_to_archive = r_str_newf ("%s%s%s", basepath,
		                                 R_SYS_DIR, archive_name);
		curl_cmd = r_str_newf ("curl -sfA \"%s\" \"%s/%s/%s/%s\" -o \"%s\"",
		                       opt->user_agent,
		                       opt->symbol_server,
		                       opt->dbg_file,
		                       opt->guid,
		                       archive_name,
		                       abspath_to_archive);
		if ((cmd_ret = r_sys_cmd (curl_cmd) != 0)) {
			eprintf("curl exited with error %d\n", cmd_ret);
			res = 0;
		}
		R_FREE (curl_cmd);
		R_FREE (abspath_to_archive);
	}
	R_FREE (archive_name);
	R_FREE (extractor_cmd);
	return res;
}

void init_pdb_downloader(SPDBDownloaderOpt *opt, SPDBDownloader *pd) {
	pd->opt = R_NEW0 (SPDBDownloaderOpt);
	if (!pd->opt) {
		return;
	}
	pd->opt->dbg_file = strdup (opt->dbg_file);
	pd->opt->guid = strdup (opt->guid);
	pd->opt->symbol_server = strdup (opt->symbol_server);
	pd->opt->user_agent = strdup (opt->user_agent);
	pd->opt->path = strdup (opt->path);
	pd->opt->extract = opt->extract;
	pd->download = download;
}

void deinit_pdb_downloader(SPDBDownloader *pd) {
	R_FREE (pd->opt->dbg_file);
	R_FREE (pd->opt->guid);
	R_FREE (pd->opt->symbol_server);
	R_FREE (pd->opt->user_agent);
	R_FREE (pd->opt->path);
	R_FREE (pd->opt);
	pd->download = 0;
}

int r_bin_pdb_download(RCore *core, int isradjson, int *actions_done, SPDBOptions *options) {
	int ret;
	char *path;
	SPDBDownloaderOpt opt;
	SPDBDownloader pdb_downloader;
	RBinInfo *info = r_bin_get_info (core->bin);

	if (!info || !info->debug_file_name) {
		eprintf ("Can't find debug filename\n");
		return 1;
	}

	if (!options || !options->symbol_server || !options->user_agent) {
		eprintf ("Can't retrieve pdb configurations\n");
		return 1;
	}

	path = info->file ? r_file_dirname (info->file) : strdup (".");

	opt.dbg_file = info->debug_file_name;
	opt.guid = info->guid;
	opt.symbol_server = options->symbol_server;
	opt.user_agent = options->user_agent;
	opt.path = path;
	opt.extract = options->extract;

	init_pdb_downloader (&opt, &pdb_downloader);
	ret = pdb_downloader.download (&pdb_downloader);
	if (isradjson && actions_done) {
		printf ("%s\"pdb\":{\"file\":\"%s\",\"download\":%s}",
		        *actions_done ? "," : "", opt.dbg_file, ret ? "true" : "false");
	} else {
		printf ("PDB \"%s\" download %s\n",
		        opt.dbg_file, ret ? "success" : "failed");
	}
	if (actions_done) {
		(*actions_done)++;
	}
	deinit_pdb_downloader (&pdb_downloader);

	free (path);
	return 0;
}
