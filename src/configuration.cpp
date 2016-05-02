/*************************************************************************
 * (C) Copyright 2016 Barcelona Supercomputing Center                    * 
 *                    Centro Nacional de Supercomputacion                *
 *                                                                       *
 * This file is part of the Echo Filesystem NG.                          *
 *                                                                       *
 * See AUTHORS file in the top level directory for information           *
 * regarding developers and contributors.                                *
 *                                                                       *
 * This library is free software; you can redistribute it and/or         *
 * modify it under the terms of the GNU Lesser General Public            *
 * License as published by the Free Software Foundation; either          *
 * version 3 of the License, or (at your option) any later version.      *
 *                                                                       *
 * Mercurium C/C++ source-to-source compiler is distributed in the hope  *
 * that it will be useful, but WITHOUT ANY WARRANTY; without even the    *
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR       *
 * PURPOSE.  See the GNU Lesser General Public License for more          *
 * details.                                                              *
 *                                                                       *
 * You should have received a copy of the GNU Lesser General Public      *
 * License along with Mercurium C/C++ source-to-source compiler; if      *
 * not, write to the Free Software Foundation, Inc., 675 Mass Ave,       *
 * Cambridge, MA 02139, USA.                                             *
 *************************************************************************/

#include <fstream>
#include <string>
#include <boost/filesystem/fstream.hpp>
#include <boost/log/trivial.hpp>

#include <libconfig.h++>

#include "command-line.h"
#include "configuration.h"


namespace bfs = boost::filesystem;

namespace efsng{

bool Configuration::load(const bfs::path& config_file, const std::shared_ptr<Arguments>& out){

    Arguments out_backup(*out);

    libconfig::Config cfg;

    try{
        cfg.readFile(config_file.c_str());
    }
    catch(const libconfig::FileIOException& fioex){
        BOOST_LOG_TRIVIAL(warning) << "An error happened while reading the configuration file.";
        return false;
    }
    catch(const libconfig::ParseException& pex){
        BOOST_LOG_TRIVIAL(error) << "Parse error at " << pex.getFile() << ":" << pex.getLine() << " - " 
                                 << pex.getError();
        BOOST_LOG_TRIVIAL(error) << "The configuration file will be ignored";
        return false;
    }

    const libconfig::Setting& root = cfg.getRoot();

    /* parse 'root-dir' */
    try{
        const libconfig::Setting& cfg_root_dir = root["efs-ng"]["root-dir"];
        std::string optval = cfg_root_dir;

        /* command-line arguments override the options passed in the configuration file. 
         * Thus, if 'root_dir' already has a value different from the default one, ignore the passed cfg_value */
        if(out->root_dir == "none"){
            out->root_dir = std::string(optval);

            BOOST_LOG_TRIVIAL(debug) << " * root-dir = " << out->root_dir;
        }
    }
    catch(const libconfig::SettingNotFoundException& nfex){
        /* ignore */
    }

    /* parse 'mount-point' */
    try{
        const libconfig::Setting& cfg_mount_point = root["efs-ng"]["mount-point"];
        std::string optval = cfg_mount_point;

        /* command-line arguments override the options passed in the configuration file. 
         * Thus, if 'root_dir' already has a value different from the default one, ignore the passed cfg_value */
        if(out->mount_point == "none"){
            out->mount_point = std::string(optval);

            BOOST_LOG_TRIVIAL(debug) << " * mount-point = " << optval;
        }
    }
    catch(const libconfig::SettingNotFoundException& nfex){
        /* ignore */
    }

    /* parse 'preload-files' */
    try{
        const libconfig::Setting& files_to_preload = root["efs-ng"]["preload-files"];
        int count = files_to_preload.getLength();

        BOOST_LOG_TRIVIAL(debug) << "files_to_preload.getLength() = " << count;

        for(int i=0; i<count; ++i){

            const std::string filename = files_to_preload[i];

            out->files_to_preload.insert(filename);

            BOOST_LOG_TRIVIAL(debug) << "  \"" << filename << "\"";
        }
    }
    catch(const libconfig::SettingNotFoundException& nfex){
        // ignore
        BOOST_LOG_TRIVIAL(warning) << "'preload-files' setting not found.";
        return false;
    }

    return true;
}

} // namespace efsng
