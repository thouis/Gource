/*
    Copyright (C) 2010 Andrew Caudwell (acaudwell@gmail.com)

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version
    3 of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "svn.h"

Regex svn_xml_tag("^<\\??xml");
Regex svn_logentry_start("^<logentry");
Regex svn_logentry_end("^</logentry>");
Regex svn_logentry_timestamp("(\\d{4})-(\\d{2})-(\\d{2})T(\\d{2}):(\\d{2}):(\\d{2})");

std::string gGourceSVNLogCommand = "svn log -r 1:HEAD --xml --verbose --quiet";

SVNCommitLog::SVNCommitLog(const std::string& logfile) : RCommitLog(logfile, '<') {

    log_command = gGourceSVNLogCommand;

    //can generate log from directory
    if(!logf && is_dir) {
        logf = generateLog(logfile);

        if(logf) {
            success  = true;
            seekable = true;
        }
    }

    logentry.reserve(1024);
}


BaseLog* SVNCommitLog::generateLog(const std::string& dir) {
    //get working directory
    char cwd_buff[1024];

    if(getcwd(cwd_buff, 1024) != cwd_buff) {
        return 0;
    }

    //does directory have a .svn ?
    std::string gitdir = dir + std::string("/.svn");
    struct stat dirinfo;
    int stat_rc = stat(gitdir.c_str(), &dirinfo);
    if(stat_rc!=0 || !(dirinfo.st_mode & S_IFDIR)) {
        return 0;
    }

    std::string command = getLogCommand();

    //create temp file
    createTempLog();

    if(temp_file.size()==0) return 0;

    if(chdir(dir.c_str()) != 0) {
        return 0;
    }

    char cmd_buff[2048];
    sprintf(cmd_buff, "%s > %s", command.c_str(), temp_file.c_str());

    int command_rc = system(cmd_buff);

    chdir(cwd_buff);

    if(command_rc != 0) {
        return 0;
    }

    BaseLog* seeklog = new SeekLog(temp_file);

    return seeklog;
}

bool SVNCommitLog::parseCommit(RCommit& commit) {

    //fprintf(stderr,"parsing svn log\n");

    std::string line;

    if(!getNextLine(line)) return false;

    //start of log entry
    if(!svn_logentry_start.match(line)) {

        //is this the start of the document
        if(!svn_xml_tag.match(line)) return false;

        //fprintf(stderr,"found xml tag\n");

        //if so find the first logentry tag

        bool found_logentry = false;
        
        while(getNextLine(line)) {
            if(svn_logentry_start.match(line)) {
                found_logentry = true;
                break;
            }
        }

        if(!found_logentry) return false;   
    }

    //fprintf(stderr,"found logentry\n");

    logentry.clear();

    logentry.append(line);
    logentry.append("\n");

    //fprintf(stderr,"found opening tag\n");

    bool endfound = false;
    
    while(getNextLine(line)) {
        logentry.append(line);
        logentry.append("\n");
        if(svn_logentry_end.match(line)) {
            //fprintf(stderr,"found closing tag\n");
            endfound=true;
            break;
        }
    }

    //incomplete commit
    if(!endfound) return false;

    //fprintf(stderr,"read logentry\n");

    TiXmlDocument doc;

    if(!doc.Parse(logentry.c_str())) return false;

    //fprintf(stderr,"try to parse logentry: %s\n", logentry.c_str());
    
    TiXmlElement* leE = doc.FirstChildElement( "logentry" );
    
    std::vector<std::string> entries;
    
    if(!leE) return false;

    //parse date
    TiXmlElement* dateE = leE->FirstChildElement( "date" );

    if(!dateE) return false;

    std::string timestamp_str(dateE->GetText());

    if(!svn_logentry_timestamp.match(timestamp_str, &entries))
        return false;
                    
    struct tm time_str;

    time_str.tm_year  = atoi(entries[0].c_str()) - 1900;
    time_str.tm_mon   = atoi(entries[1].c_str()) - 1;
    time_str.tm_mday  = atoi(entries[2].c_str());
    time_str.tm_hour  = atoi(entries[3].c_str());
    time_str.tm_min   = atoi(entries[4].c_str());
    time_str.tm_sec   = atoi(entries[5].c_str());
    time_str.tm_isdst = -1;

    commit.timestamp = timegm(&time_str);            
   
    //parse author
    TiXmlElement* authorE = leE->FirstChildElement("author");
    
    if(authorE != 0) {
    
        std::string author(authorE->GetText());

        if(author.empty()) author = "Unknown";
    
        commit.username = author;
    }

    TiXmlElement* pathsE = leE->FirstChildElement( "paths" );

    //log entries sometimes dont have any paths
    if(!pathsE) return true;

    //parse changes
    
    for(TiXmlElement* pathE = pathsE->FirstChildElement("path"); pathE != 0; pathE = pathE->NextSiblingElement()) {
        //parse path
        
        const char* kind   = pathE->Attribute("kind");
        const char* action = pathE->Attribute("action");

        //check for action
        if(action == 0) continue;

        bool is_dir = false;

        //if has the 'kind' attribute (old versions of svn dont have this), check if it is a dir
        if(kind != 0 && strcmp(kind,"dir") == 0) {

            //accept only deletes for directories
            if(strcmp(action, "D") != 0) continue;

            is_dir = true;
        }

        std::string file(pathE->GetText());
        std::string status(action);       
        
        if(file.empty()) continue;
        if(status.empty()) continue;

        //append trailing slash if is directory
        if(is_dir && file[file.size()-1] != '/') {
            file = file + std::string("/");
        }

        commit.addFile(file, status);
    }
    
    //fprintf(stderr,"parsed logentry\n");

    //read files
    
    return true;
}
