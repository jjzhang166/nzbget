/*
 *  This file is part of nzbget
 *
 *  Copyright (C) 2013-2015 Andrey Prygunkov <hugbug@users.sourceforge.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * $Revision$
 * $Date$
 *
 */


#include "nzbget.h"
#include "Util.h"
#include "FileSystem.h"
#include "Options.h"
#include "Log.h"
#include "ScriptConfig.h"

static const char* BEGIN_SCRIPT_SIGNATURE = "### NZBGET ";
static const char* POST_SCRIPT_SIGNATURE = "POST-PROCESSING";
static const char* SCAN_SCRIPT_SIGNATURE = "SCAN";
static const char* QUEUE_SCRIPT_SIGNATURE = "QUEUE";
static const char* SCHEDULER_SCRIPT_SIGNATURE = "SCHEDULER";
static const char* FEED_SCRIPT_SIGNATURE = "FEED";
static const char* END_SCRIPT_SIGNATURE = " SCRIPT";
static const char* QUEUE_EVENTS_SIGNATURE = "### QUEUE EVENTS:";

ScriptConfig* g_ScriptConfig = nullptr;


ScriptConfig::Script::Script(const char* name, const char* location)
{
	m_name = name;
	m_location = location;
	m_displayName = name;
	m_postScript = false;
	m_scanScript = false;
	m_queueScript = false;
	m_schedulerScript = false;
	m_feedScript = false;
}


ScriptConfig::ScriptConfig()
{
	InitScripts();
	InitConfigTemplates();
}

bool ScriptConfig::LoadConfig(Options::OptEntries* optEntries)
{
	// read config file
	DiskFile infile;

	if (!infile.Open(g_Options->GetConfigFilename(), DiskFile::omRead))
	{
		return false;
	}

	int fileLen = (int)FileSystem::FileSize(g_Options->GetConfigFilename());
	CString buf;
	buf.Reserve(fileLen);

	while (infile.ReadLine(buf, fileLen + 1))
	{
		// remove trailing '\n' and '\r' and spaces
		Util::TrimRight(buf);

		// skip comments and empty lines
		if (buf[0] == 0 || buf[0] == '#' || strspn(buf, " ") == strlen(buf))
		{
			continue;
		}

		CString optname;
		CString optvalue;
		if (g_Options->SplitOptionString(buf, optname, optvalue))
		{
			optEntries->emplace_back(optname, optvalue);
		}
	}

	infile.Close();

	return true;
}

bool ScriptConfig::SaveConfig(Options::OptEntries* optEntries)
{
	// save to config file
	DiskFile infile;

	if (!infile.Open(g_Options->GetConfigFilename(), DiskFile::omReadWrite))
	{
		return false;
	}

	std::vector<CString> config;
	std::set<Options::OptEntry*> writtenOptions;

	// read config file into memory array
	int fileLen = (int)FileSystem::FileSize(g_Options->GetConfigFilename()) + 1;
	CString content;
	content.Reserve(fileLen);
	while (infile.ReadLine(content, fileLen + 1))
	{
		config.push_back(*content);
	}
	content.Clear();

	// write config file back to disk, replace old values of existing options with new values
	infile.Seek(0);
	for (CString& buf : config)
	{
		const char* eq = strchr(buf, '=');
		if (eq && buf[0] != '#')
		{
			// remove trailing '\n' and '\r' and spaces
			buf.TrimRight();

			CString optname;
			CString optvalue;
			if (g_Options->SplitOptionString(buf, optname, optvalue))
			{
				Options::OptEntry* optEntry = optEntries->FindOption(optname);
				if (optEntry)
				{
					infile.Print("%s=%s\n", optEntry->GetName(), optEntry->GetValue());
					writtenOptions.insert(optEntry);
				}
			}
		}
		else
		{
			infile.Print("%s", *buf);
		}
	}

	// write new options
	for (Options::OptEntry& optEntry : *optEntries)
	{
		std::set<Options::OptEntry*>::iterator fit = writtenOptions.find(&optEntry);
		if (fit == writtenOptions.end())
		{
			infile.Print("%s=%s\n", optEntry.GetName(), optEntry.GetValue());
		}
	}

	// close and truncate the file
	int pos = (int)infile.Position();
	infile.Close();

	FileSystem::TruncateFile(g_Options->GetConfigFilename(), pos);

	return true;
}

bool ScriptConfig::LoadConfigTemplates(ConfigTemplates* configTemplates)
{
	char* buffer;
	int length;
	if (!FileSystem::LoadFileIntoBuffer(g_Options->GetConfigTemplate(), &buffer, &length))
	{
		return false;
	}
	configTemplates->emplace_back(Script("", ""), buffer);
	free(buffer);

	if (!g_Options->GetScriptDir())
	{
		return true;
	}

	Scripts scriptList;
	LoadScripts(&scriptList);

	const int beginSignatureLen = strlen(BEGIN_SCRIPT_SIGNATURE);
	const int queueEventsSignatureLen = strlen(QUEUE_EVENTS_SIGNATURE);

	for (Script& script : scriptList)
	{
		DiskFile infile;
		if (!infile.Open(script.GetLocation(), DiskFile::omRead))
		{
			configTemplates->emplace_back(std::move(script), "");
			continue;
		}

		StringBuilder templ;
		char buf[1024];
		bool inConfig = false;

		while (infile.ReadLine(buf, sizeof(buf) - 1))
		{
			if (!strncmp(buf, BEGIN_SCRIPT_SIGNATURE, beginSignatureLen) &&
				strstr(buf, END_SCRIPT_SIGNATURE) &&
				(strstr(buf, POST_SCRIPT_SIGNATURE) ||
				 strstr(buf, SCAN_SCRIPT_SIGNATURE) ||
				 strstr(buf, QUEUE_SCRIPT_SIGNATURE) ||
				 strstr(buf, SCHEDULER_SCRIPT_SIGNATURE) ||
				 strstr(buf, FEED_SCRIPT_SIGNATURE)))
			{
				if (inConfig)
				{
					break;
				}
				inConfig = true;
				continue;
			}

			bool skip = !strncmp(buf, QUEUE_EVENTS_SIGNATURE, queueEventsSignatureLen);

			if (inConfig && !skip)
			{
				templ.Append(buf);
			}
		}

		infile.Close();

		configTemplates->emplace_back(std::move(script), templ);
	}

	return true;
}

void ScriptConfig::InitConfigTemplates()
{
	if (!LoadConfigTemplates(&m_configTemplates))
	{
		error("Could not read configuration templates");
	}
}

void ScriptConfig::InitScripts()
{
	LoadScripts(&m_scripts);
}

void ScriptConfig::LoadScripts(Scripts* scripts)
{
	if (strlen(g_Options->GetScriptDir()) == 0)
	{
		return;
	}

	Scripts tmpScripts;
	LoadScriptDir(&tmpScripts, g_Options->GetScriptDir(), false);

	tmpScripts.sort(
		[](Script& script1, Script& script2)
		{
			return strcmp(script1.GetName(), script2.GetName()) < 0;
		});

	// first add all scripts from ScriptOrder
	Tokenizer tok(g_Options->GetScriptOrder(), ",;");
	while (const char* scriptName = tok.Next())
	{
		Scripts::iterator pos = std::find_if(tmpScripts.begin(), tmpScripts.end(),
			[scriptName](Script& script)
			{
				return !strcmp(script.GetName(), scriptName);
			});

		if (pos != tmpScripts.end())
		{
			scripts->splice(scripts->end(), tmpScripts, pos);
		}
	}

	// then add all other scripts from scripts directory
	scripts->splice(scripts->end(), std::move(tmpScripts));

	BuildScriptDisplayNames(scripts);
}

void ScriptConfig::LoadScriptDir(Scripts* scripts, const char* directory, bool isSubDir)
{
	int bufSize = 1024*10;
	char* buffer = (char*)malloc(bufSize+1);

	const int beginSignatureLen = strlen(BEGIN_SCRIPT_SIGNATURE);
	const int queueEventsSignatureLen = strlen(QUEUE_EVENTS_SIGNATURE);

	DirBrowser dir(directory);
	while (const char* filename = dir.Next())
	{
		if (filename[0] != '.' && filename[0] != '_')
		{
			BString<1024> fullFilename("%s%s", directory, filename);

			if (!FileSystem::DirectoryExists(fullFilename))
			{
				// check if the file contains pp-script-signature
				DiskFile infile;
				if (infile.Open(fullFilename, DiskFile::omRead))
				{
					// read first 10KB of the file and look for signature
					int readBytes = (int)infile.Read(buffer, bufSize);
					infile.Close();
					buffer[readBytes] = '\0';

					// split buffer into lines
					Tokenizer tok(buffer, "\n\r", true);
					while (char* line = tok.Next())
					{
						if (!strncmp(line, BEGIN_SCRIPT_SIGNATURE, beginSignatureLen) &&
							strstr(line, END_SCRIPT_SIGNATURE))
						{
							bool postScript = strstr(line, POST_SCRIPT_SIGNATURE);
							bool scanScript = strstr(line, SCAN_SCRIPT_SIGNATURE);
							bool queueScript = strstr(line, QUEUE_SCRIPT_SIGNATURE);
							bool schedulerScript = strstr(line, SCHEDULER_SCRIPT_SIGNATURE);
							bool feedScript = strstr(line, FEED_SCRIPT_SIGNATURE);
							if (postScript || scanScript || queueScript || schedulerScript || feedScript)
							{
								BString<1024> scriptName;
								if (isSubDir)
								{
									BString<1024> directory2 = directory;
									int len = strlen(directory2);
									if (directory2[len-1] == PATH_SEPARATOR || directory2[len-1] == ALT_PATH_SEPARATOR)
									{
										// trim last path-separator
										directory2[len-1] = '\0';
									}

									scriptName.Format("%s%c%s", FileSystem::BaseFileName(directory2), PATH_SEPARATOR, filename);
								}
								else
								{
									scriptName = filename;
								}

								char* queueEvents = nullptr;
								if (queueScript)
								{
									while (char* line = tok.Next())
									{
										if (!strncmp(line, QUEUE_EVENTS_SIGNATURE, queueEventsSignatureLen))
										{
											queueEvents = line + queueEventsSignatureLen;
											break;
										}
									}
								}

								scripts->emplace_back(scriptName, fullFilename);
								Script& script = scripts->back();
								script.SetPostScript(postScript);
								script.SetScanScript(scanScript);
								script.SetQueueScript(queueScript);
								script.SetSchedulerScript(schedulerScript);
								script.SetFeedScript(feedScript);
								script.SetQueueEvents(queueEvents);
								break;
							}
						}
					}
				}
			}
			else if (!isSubDir)
			{
				fullFilename.Format("%s%s%c", directory, filename, PATH_SEPARATOR);
				LoadScriptDir(scripts, fullFilename, true);
			}
		}
	}

	free(buffer);
}

void ScriptConfig::BuildScriptDisplayNames(Scripts* scripts)
{
	// trying to use short name without path and extension.
	// if there are other scripts with the same short name - using a longer name instead (with ot without extension)

	for (Script& script : scripts)
	{
		BString<1024> shortName = script.GetName();
		if (char* ext = strrchr(shortName, '.')) *ext = '\0'; // strip file extension

		const char* displayName = FileSystem::BaseFileName(shortName);

		for (Script& script2 : scripts)
		{
			BString<1024> shortName2 = script2.GetName();
			if (char* ext = strrchr(shortName2, '.')) *ext = '\0'; // strip file extension

			const char* displayName2 = FileSystem::BaseFileName(shortName2);

			if (!strcmp(displayName, displayName2) && script.GetName() != script2.GetName())
			{
				if (!strcmp(shortName, shortName2))
				{
					displayName = script.GetName();
				}
				else
				{
					displayName = shortName;
				}
				break;
			}
		}

		script.SetDisplayName(displayName);
	}
}
