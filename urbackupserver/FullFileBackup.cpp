/*************************************************************************
*    UrBackup - Client/Server backup system
*    Copyright (C) 2011-2015 Martin Raiber
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU Affero General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
**************************************************************************/

#include "FullFileBackup.h"
#include "database.h"
#include <vector>
#include "../Interface/Server.h"
#include "dao/ServerBackupDao.h"
#include "ClientMain.h"
#include "server_log.h"
#include "../urbackupcommon/fileclient/FileClient.h"
#include "../urbackupcommon/filelist_utils.h"
#include "server_running.h"
#include "ServerDownloadThread.h"
#include "../urbackupcommon/file_metadata.h"
#include <stack>

extern std::string server_identity;
extern std::string server_token;


FullFileBackup::FullFileBackup( ClientMain* client_main, int clientid, std::wstring clientname, LogAction log_action, int group, bool use_tmpfiles, std::wstring tmpfile_path, bool use_reflink, bool use_snapshots )
	: FileBackup(client_main, clientid, clientname, log_action, false, group, use_tmpfiles, tmpfile_path, use_reflink, use_snapshots)
{

}


SBackup FullFileBackup::getLastFullDurations( void )
{
	std::vector<ServerBackupDao::SDuration> durations = 
		backup_dao->getLastFullDurations(clientid);

	ServerBackupDao::SDuration duration = interpolateDurations(durations);

	SBackup b;

	b.indexing_time_ms = duration.indexing_time_ms;
	b.backup_time_ms = duration.duration*1000;

	return b;
}

bool FullFileBackup::doFileBackup()
{
	ServerLogger::Log(logid, "Starting full file backup...", LL_INFO);

	SBackup last_backup_info = getLastFullDurations();

	int64 eta_set_time = Server->getTimeMS();
	ServerStatus::setProcessEta(clientname, status_id, last_backup_info.backup_time_ms + last_backup_info.indexing_time_ms, eta_set_time);

	int64 indexing_start_time = Server->getTimeMS();

	bool no_backup_dirs=false;
	bool connect_fail=false;
	bool b=request_filelist_construct(true, false, group, true, no_backup_dirs, connect_fail);
	if(!b)
	{
		has_early_error=true;

		if(no_backup_dirs || connect_fail)
		{
			log_backup=false;
		}
		else
		{
			log_backup=true;
		}

		return false;
	}

	bool hashed_transfer=true;
	bool save_incomplete_files=false;

	if(client_main->isOnInternetConnection())
	{
		if(server_settings->getSettings()->internet_full_file_transfer_mode=="raw")
			hashed_transfer=false;
		if(server_settings->getSettings()->internet_incr_file_transfer_mode=="blockhash")
			save_incomplete_files=true;
	}
	else
	{
		if(server_settings->getSettings()->local_full_file_transfer_mode=="raw")
			hashed_transfer=false;
		if(server_settings->getSettings()->local_incr_file_transfer_mode=="blockhash")
			save_incomplete_files=true;
	}

	if(hashed_transfer)
	{
		ServerLogger::Log(logid, clientname+L": Doing backup with hashed transfer...", LL_DEBUG);
	}
	else
	{
		ServerLogger::Log(logid, clientname+L": Doing backup without hashed transfer...", LL_DEBUG);
	}
	std::string identity = client_main->getSessionIdentity().empty()?server_identity:client_main->getSessionIdentity();
	FileClient fc(false, identity, client_main->getProtocolVersions().filesrv_protocol_version,
		client_main->isOnInternetConnection(), client_main, use_tmpfiles?NULL:client_main);
	_u32 rc=client_main->getClientFilesrvConnection(&fc, server_settings.get(), 10000);
	if(rc!=ERR_CONNECTED)
	{
		ServerLogger::Log(logid, L"Full Backup of "+clientname+L" failed - CONNECT error", LL_ERROR);
		has_early_error=true;
		log_backup=false;
		return false;
	}

	IFile *tmp=ClientMain::getTemporaryFileRetry(use_tmpfiles, tmpfile_path, logid);
	if(tmp==NULL) 
	{
		ServerLogger::Log(logid, L"Error creating temporary file in ::doFullBackup", LL_ERROR);
		return false;
	}

	ServerLogger::Log(logid, clientname+L": Loading file list...", LL_INFO);

	int64 full_backup_starttime=Server->getTimeMS();

	rc=fc.GetFile(group>0?("urbackup/filelist_"+nconvert(group)+".ub"):"urbackup/filelist.ub", tmp, hashed_transfer, false);
	if(rc!=ERR_SUCCESS)
	{
		ServerLogger::Log(logid, L"Error getting filelist of "+clientname+L". Errorcode: "+widen(fc.getErrorString(rc))+L" ("+convert(rc)+L")", LL_ERROR);
		has_early_error=true;
		return false;
	}

	getTokenFile(fc, hashed_transfer);

	backup_dao->newFileBackup(0, clientid, backuppath_single, 0, Server->getTimeMS()-indexing_start_time, group);
	backupid = static_cast<int>(db->getLastInsertID());

	tmp->Seek(0);

	FileListParser list_parser;

	IFile *clientlist=Server->openFile(clientlistName(group, true), MODE_WRITE);

	if(clientlist==NULL )
	{
		ServerLogger::Log(logid, L"Error creating clientlist for client "+clientname, LL_ERROR);
		has_early_error=true;
		return false;
	}

	if(ServerStatus::getProcess(clientname, status_id).stop)
	{
		ServerLogger::Log(logid, L"Server admin stopped backup. -1", LL_ERROR);
		has_early_error=true;
		return false;
	}

	if(!startFileMetadataDownloadThread())
	{
		ServerLogger::Log(logid, "Error starting file metadata download thread", LL_ERROR);
		has_early_error=true;
		return false;
	}

	_i64 filelist_size=tmp->Size();

	char buffer[4096];
	_u32 read;
	std::wstring curr_path;
	std::wstring curr_os_path;
	std::string curr_orig_path;
	std::string orig_sep;
	SFile cf;
	int depth=0;
	bool r_done=false;
	int64 laststatsupdate=0;
	int64 last_eta_update=0;
	int64 last_eta_received_bytes=0;
	double eta_estimated_speed=0;
	ServerRunningUpdater *running_updater=new ServerRunningUpdater(backupid, false);
	Server->getThreadPool()->execute(running_updater);

	ServerLogger::Log(logid, clientname+L": Started loading files...", LL_INFO);

	std::wstring last_backuppath;
	std::wstring last_backuppath_complete;
	std::auto_ptr<ServerDownloadThread> server_download(new ServerDownloadThread(fc, NULL, backuppath,
		backuppath_hashes, last_backuppath, last_backuppath_complete,
		hashed_transfer, save_incomplete_files, clientid, clientname,
		use_tmpfiles, tmpfile_path, server_token, use_reflink,
		backupid, false, hashpipe_prepare, client_main, client_main->getProtocolVersions().filesrv_protocol_version, 0, logid));

	bool queue_downloads = client_main->getProtocolVersions().filesrv_protocol_version>2;

	THREADPOOL_TICKET server_download_ticket = 
		Server->getThreadPool()->execute(server_download.get());

	std::vector<size_t> diffs;
	_i64 files_size=getIncrementalSize(tmp, diffs, true);
	fc.resetReceivedDataBytes();
	tmp->Seek(0);

	size_t line = 0;
	int64 linked_bytes = 0;

	size_t max_ok_id=0;

	bool c_has_error=false;
	bool is_offline=false;
	bool script_dir=false;

	while( (read=tmp->Read(buffer, 4096))>0 && r_done==false && c_has_error==false)
	{
		for(size_t i=0;i<read;++i)
		{
			std::map<std::wstring, std::wstring> extra_params;
			bool b=list_parser.nextEntry(buffer[i], cf, &extra_params);
			if(b)
			{
				FileMetadata metadata;
				metadata.read(extra_params);

				bool has_orig_path = metadata.has_orig_path;
				if(has_orig_path)
				{
					curr_orig_path = metadata.orig_path;
					orig_sep = Server->ConvertToUTF8(extra_params[L"orig_sep"]);
					if(orig_sep.empty()) orig_sep="\\";
				}


				int64 ctime=Server->getTimeMS();
				if(ctime-laststatsupdate>status_update_intervall)
				{
					if(ServerStatus::getProcess(clientname, status_id).stop)
					{
						r_done=true;
						ServerLogger::Log(logid, L"Server admin stopped backup.", LL_ERROR);
						server_download->queueSkip();
						break;
					}

					laststatsupdate=ctime;
					if(files_size==0)
					{
						ServerStatus::setProcessPcDone(clientname, status_id, 100);
					}
					else
					{
						ServerStatus::setProcessPcDone(clientname, status_id,
							(std::min)(100,(int)(((float)fc.getReceivedDataBytes() + linked_bytes)/((float)files_size/100.f)+0.5f)));
					}

					ServerStatus::setProcessQueuesize(clientname, status_id,
						(_u32)hashpipe->getNumElements(), (_u32)hashpipe_prepare->getNumElements());
				}

				if(ctime-last_eta_update>eta_update_intervall)
				{
					calculateEtaFileBackup(last_eta_update, eta_set_time, ctime, fc, NULL, linked_bytes, last_eta_received_bytes, eta_estimated_speed, files_size);
				}

				if(server_download->isOffline())
				{
					ServerLogger::Log(logid, L"Client "+clientname+L" went offline.", LL_ERROR);
					is_offline = true;
					r_done=true;
					break;
				}

				std::wstring osspecific_name=fixFilenameForOS(cf.name);
				if(cf.isdir)
				{
					if(cf.name!=L"..")
					{
						std::wstring orig_curr_path = curr_path;
						std::wstring orig_curr_os_path = curr_os_path;
						curr_path+=L"/"+cf.name;
						curr_os_path+=L"/"+osspecific_name;
						std::wstring local_curr_os_path=convertToOSPathFromFileClient(curr_os_path);

						if(!has_orig_path)
						{
							curr_orig_path += orig_sep + Server->ConvertToUTF8(cf.name);
							metadata.orig_path = curr_orig_path;
                            metadata.exist=true;
							metadata.has_orig_path=true;
						}

						str_map::iterator sym_target = extra_params.find(L"sym_target");
						if(sym_target!=extra_params.end())
						{
							if(!createSymlink(backuppath+local_curr_os_path, depth, sym_target->second, Server->ConvertToUnicode(orig_sep), true))
							{
								ServerLogger::Log(logid, L"Creating symlink at \""+backuppath+local_curr_os_path+L"\" to \""+sym_target->second+L" failed. " + widen(systemErrorInfo()), LL_ERROR);
								c_has_error=true;
								break;
							}
						}
						else if(!os_create_dir(os_file_prefix(backuppath+local_curr_os_path)))
						{
							ServerLogger::Log(logid, L"Creating directory  \""+backuppath+local_curr_os_path+L"\" failed. " + widen(systemErrorInfo()), LL_ERROR);
							c_has_error=true;
							break;
						}
						
						if(!os_create_dir(os_file_prefix(backuppath_hashes+local_curr_os_path)))
						{
							ServerLogger::Log(logid, L"Creating directory  \""+backuppath_hashes+local_curr_os_path+L"\" failed. " + widen(systemErrorInfo()), LL_ERROR);
							c_has_error=true;
							break;
						}
						else if(metadata.exist && !write_file_metadata(backuppath_hashes+local_curr_os_path+os_file_sep()+metadata_dir_fn, client_main, metadata, false))
						{
							ServerLogger::Log(logid, L"Writing directory metadata to \""+backuppath_hashes+local_curr_os_path+os_file_sep()+metadata_dir_fn+L"\" failed.", LL_ERROR);
							c_has_error=true;
							break;
						}

						++depth;
						if(depth==1)
						{
							std::wstring t=curr_path;
							t.erase(0,1);
							if(t==L"urbackup_backup_scripts")
							{
								script_dir=true;
							}
							else
							{
								ServerLogger::Log(logid, L"Starting shadowcopy \""+t+L"\".", LL_DEBUG);
								server_download->addToQueueStartShadowcopy(t);

								continuous_sequences[cf.name]=SContinuousSequence(
									watoi64(extra_params[L"sequence_id"]), watoi64(extra_params[L"sequence_next"]));
							}							
						}
					}
					else
					{
                        if(client_main->getProtocolVersions().file_meta>0 && !script_dir)
						{
							server_download->addToQueueFull(line, ExtractFileName(curr_path, L"/"),
								ExtractFileName(curr_os_path, L"/"), ExtractFilePath(curr_path, L"/"), ExtractFilePath(curr_os_path, L"/"), queue_downloads?0:-1,
								metadata, false, true);
						}

						--depth;
						if(depth==0)
						{
							std::wstring t=curr_path;
							t.erase(0,1);
							if(t==L"urbackup_backup_scripts")
							{
								script_dir=false;
							}
							else
							{
								ServerLogger::Log(logid, L"Stoping shadowcopy \""+t+L"\".", LL_DEBUG);
								server_download->addToQueueStopShadowcopy(t);
							}							
						}

						curr_path=ExtractFilePath(curr_path, L"/");
						curr_os_path=ExtractFilePath(curr_os_path, L"/");

						if(!has_orig_path)
						{
							curr_orig_path = ExtractFilePath(curr_orig_path, orig_sep);
						}
					}
				}
				else
				{
					if(!has_orig_path)
					{
						metadata.orig_path = curr_orig_path + orig_sep + Server->ConvertToUTF8(cf.name);
					}

					bool file_ok=false;

                    str_map::iterator sym_target = extra_params.find(L"sym_target");
                    if(sym_target!=extra_params.end())
                    {
                        std::wstring symlink_path = backuppath + convertToOSPathFromFileClient(curr_os_path)+os_file_sep()+osspecific_name;
                        if(!createSymlink(symlink_path, depth, sym_target->second, Server->ConvertToUnicode(orig_sep), true))
                        {
                            ServerLogger::Log(logid, L"Creating symlink at \""+symlink_path+L"\" to \""+sym_target->second+L" failed. " + widen(systemErrorInfo()), LL_ERROR);
                            c_has_error=true;
                            break;
                        }
                        else
                        {
                            file_ok=true;
                        }
                    }

					std::map<std::wstring, std::wstring>::iterator hash_it=( (local_hash.get()==NULL)?extra_params.end():extra_params.find(L"sha512") );
					if( hash_it!=extra_params.end())
					{
						if(link_file(cf.name, osspecific_name, curr_path, curr_os_path, base64_decode_dash(wnarrow(hash_it->second)), cf.size,
							true, metadata))
						{
							file_ok=true;
							linked_bytes+=cf.size;
							if(line>max_ok_id)
							{
								max_ok_id=line;
							}
						}
					}

                    if(file_ok)
                    {
                        server_download->addToQueueFull(line, cf.name, osspecific_name, curr_path, curr_os_path, queue_downloads?0:-1,
                            metadata, script_dir, true);
                    }
                    else
					{
						server_download->addToQueueFull(line, cf.name, osspecific_name, curr_path, curr_os_path, queue_downloads?cf.size:-1,
							metadata, script_dir, false);
					}
				}

				++line;
			}
		}

		if(read<4096)
			break;
	}

	server_download->queueStop(false);

	ServerLogger::Log(logid, L"Waiting for file transfers...", LL_INFO);

	while(!Server->getThreadPool()->waitFor(server_download_ticket, 1000))
	{
		if(files_size==0)
		{
			ServerStatus::setProcessPcDone(clientname, status_id, 100);
		}
		else
		{
			ServerStatus::setProcessPcDone(clientname, status_id,
				(std::min)(100,(int)(((float)fc.getReceivedDataBytes())/((float)files_size/100.f)+0.5f)));
		}

		ServerStatus::setProcessQueuesize(clientname, status_id,
			(_u32)hashpipe->getNumElements(), (_u32)hashpipe_prepare->getNumElements());

		int64 ctime = Server->getTimeMS();
		if(ctime-last_eta_update>eta_update_intervall)
		{
			calculateEtaFileBackup(last_eta_update, eta_set_time, ctime, fc, NULL, linked_bytes, last_eta_received_bytes, eta_estimated_speed, files_size);
		}
	}

	if(server_download->isOffline() && !is_offline)
	{
		ServerLogger::Log(logid, L"Client "+clientname+L" went offline.", LL_ERROR);
		r_done=true;
	}

	size_t max_line = line;

	if(!r_done && !c_has_error)
	{
		sendBackupOkay(true);
	}
	else
	{
		sendBackupOkay(false);
	}

	running_updater->stop();
	backup_dao->updateFileBackupRunning(backupid);

	ServerLogger::Log(logid, L"Writing new file list...", LL_INFO);

	tmp->Seek(0);
	line = 0;
	list_parser.reset();
	while( (read=tmp->Read(buffer, 4096))>0 )
	{
		for(size_t i=0;i<read;++i)
		{
			bool b=list_parser.nextEntry(buffer[i], cf, NULL);
			if(b)
			{
				if(cf.isdir && line<max_line)
				{
					writeFileItem(clientlist, cf);
				}
				else if(!cf.isdir && 
					line <= (std::max)(server_download->getMaxOkId(), max_ok_id) &&
					server_download->isDownloadOk(line) )
				{
					if(server_download->isDownloadPartial(line))
					{
						cf.last_modified *= Server->getRandomNumber();
					}
					writeFileItem(clientlist, cf);
				}				
				++line;
			}
		}
	}

	Server->destroy(clientlist);

	ServerLogger::Log(logid, L"Waiting for file hashing and copying threads...", LL_INFO);

	waitForFileThreads();

	bool verification_ok = true;
	if(!r_done && !c_has_error
	        && (server_settings->getSettings()->end_to_end_file_backup_verification
		|| (client_main->isOnInternetConnection()
		&& server_settings->getSettings()->verify_using_client_hashes 
		&& server_settings->getSettings()->internet_calculate_filehashes_on_client) ) )
	{
		if(!verify_file_backup(tmp))
		{
			ServerLogger::Log(logid, "Backup verification failed", LL_ERROR);
			c_has_error=true;
			verification_ok = false;
		}
		else
		{
			ServerLogger::Log(logid, "Backup verification ok", LL_INFO);
		}
	}



	if( bsh->hasError() || bsh_prepare->hasError() )
	{
		disk_error=true;
	}
	else if(verification_ok)
	{
		db->BeginTransaction();
		if(!os_rename_file(widen(clientlistName(group, true)), widen(clientlistName(group, false))) )
		{
			ServerLogger::Log(logid, "Renaming new client file list to destination failed", LL_ERROR);
		}
		backup_dao->setFileBackupDone(backupid);
		db->EndTransaction();
	}

	if( !r_done && !c_has_error && !disk_error
		&& (group==c_group_default || group==c_group_continuous)) 
	{
		std::wstring backupfolder=server_settings->getSettings()->backupfolder;

		std::wstring name=L"current";
		if(group==c_group_continuous)
		{
			name=L"continuous";
		}

		std::wstring currdir=backupfolder+os_file_sep()+clientname+os_file_sep()+name;
		os_remove_symlink_dir(os_file_prefix(currdir));
		os_link_symbolic(os_file_prefix(backuppath), os_file_prefix(currdir));

		if(group==c_group_default)
		{
			currdir=backupfolder+os_file_sep()+L"clients";
			if(!os_create_dir(os_file_prefix(currdir)) && !os_directory_exists(os_file_prefix(currdir)))
			{
				Server->Log("Error creating \"clients\" dir for symbolic links", LL_ERROR);
			}
			currdir+=os_file_sep()+clientname;
			os_remove_symlink_dir(os_file_prefix(currdir));
			os_link_symbolic(os_file_prefix(backuppath), os_file_prefix(currdir));

			if(server_settings->getSettings()->create_linked_user_views)
			{
				ServerLogger::Log(logid, "Creating user views...", LL_INFO);

				createUserViews(tmp);
			}

			saveUsersOnClient();
		}
	}

	{
		std::wstring tmp_fn=tmp->getFilenameW();
		Server->destroy(tmp);
		Server->deleteFile(os_file_prefix(tmp_fn));
	}

	_i64 transferred_bytes=fc.getTransferredBytes();
	_i64 transferred_compressed=fc.getRealTransferredBytes();
	int64 passed_time=Server->getTimeMS()-full_backup_starttime;
	if(passed_time==0) passed_time=1;

	ServerLogger::Log(logid, "Transferred "+PrettyPrintBytes(transferred_bytes)+" - Average speed: "+PrettyPrintSpeed((size_t)((transferred_bytes*1000)/(passed_time)) ), LL_INFO );
	if(transferred_compressed>0)
	{
		ServerLogger::Log(logid, "(Before compression: "+PrettyPrintBytes(transferred_compressed)+" ratio: "+nconvert((float)transferred_compressed/transferred_bytes)+")");
	}

	stopFileMetadataDownloadThread();

	ClientMain::run_script(L"urbackup" + os_file_sep() + L"post_full_filebackup", L"\""+ backuppath + L"\"", logid);

	if(c_has_error)
		return false;

	return !r_done;
}

