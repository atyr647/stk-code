//
//  SuperTuxKart - a fun racing game with go-kart
//  Copyright (C) 2014-2015  SuperTuxKart Team
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 3
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

#ifndef HEADER_SCRIPT_ENGINE_HPP
#define HEADER_SCRIPT_ENGINE_HPP

#include "scriptengine/script_utils.hpp"
#include "utils/no_copy.hpp"
#include "utils/ptr_vector.hpp"
#include "utils/singleton.hpp"

#include <angelscript.h>
#include <functional>
#include <map>
#include <string>

class TrackObjectPresentation;

namespace Scripting
{
    /** Represents a scripting function to execute after a given time */
    struct PendingTimeout : NoCopy
    {
        double m_time;

        /** We have two callback types: a string containing the name of the function
          * to call (simple callback) or a "TimeoutBase" object (advanced callback)
          */
        std::string m_callback_name;
        asIScriptFunction* m_callback_delegate;

        PendingTimeout(double time, const std::string& callback_name)
        {
            m_callback_delegate = NULL;
            m_time = time;
            m_callback_name = callback_name;
        }

        PendingTimeout(double time, asIScriptFunction* callback_delegate);

        ~PendingTimeout();
    };

    class ScriptEngine : public AbstractSingleton<ScriptEngine>
    {
         ScriptEngine();
        ~ScriptEngine();

        // Give the singleton access to the constructor.
        friend class AbstractSingleton<ScriptEngine>;

    public:


        void runFunction(bool warn_if_not_found, std::string function_name);
        void runFunction(bool warn_if_not_found, std::string function_name,
            std::function<void(asIScriptContext*)> callback);
        void runFunction(bool warn_if_not_found, std::string function_name,
            std::function<void(asIScriptContext*)> callback,
            std::function<void(asIScriptContext*)> get_return_value);
        void runDelegate(asIScriptFunction* delegate_fn);
        void evalScript(std::string script_fragment);
        void cleanupCache();

        bool loadScript(std::string script_path, bool clear_previous);
        /** Builds the scripts added by loadScript. If bytecode_path names a
         *  precompiled module for exactly these script files (see
         *  STK_SCRIPT_EXPORT), it is loaded instead of compiling. */
        bool compileLoadedScripts(const std::string &bytecode_path = "");

        void addPendingTimeout(double time, const std::string& callback_name);
        void addPendingTimeout(double time, asIScriptFunction* delegate_fn);
        void update(float dt);

        asIScriptEngine* getEngine() { return m_engine; }

    private:
        asIScriptEngine *m_engine;
        std::map<std::string, asIScriptFunction*> m_functions_cache;
        /** Hash of the raw script files added since the last clear, which
         *  a precompiled module must have been built from. */
        uint64_t m_source_hash;
        unsigned m_source_count;
        PtrVector<PendingTimeout> m_pending_timeouts;

        void configureEngine(asIScriptEngine *engine);
        bool buildModule(const std::string &bytecode_path);
    };   // class ScriptEngine

}
#endif

