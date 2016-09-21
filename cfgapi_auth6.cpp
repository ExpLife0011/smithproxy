/*
    Smithproxy- transparent proxy with SSL inspection capabilities.
    Copyright (c) 2014, Ales Stibal <astib@mag0.net>, All rights reserved.

    Smithproxy is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Smithproxy is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Smithproxy.  If not, see <http://www.gnu.org/licenses/>.
    
*/

#include <cfgapi.hpp>
#include <cfgapi_auth.hpp>
#include <logger.hpp>

template <class ShmLogonType>
unsigned int IdentityInfoType<ShmLogonType>::global_idle_timeout = 600;

int cfgapi_auth_shm_ip6_table_refresh()  {
    std::lock_guard<std::recursive_mutex> l(cfgapi_write_lock);
    
    auth_shm_ip6_map.attach(string_format(AUTH_IP6_MEM_NAME,cfgapi_tenant_name.c_str()).c_str(),AUTH_IP6_MEM_SIZE,string_format(AUTH_IP6_SEM_NAME,cfgapi_tenant_name.c_str()).c_str());
    
    DEBS_("cfgapi_auth_shm_ip6_table_refresh: acquring semaphore");
    int rc = auth_shm_ip6_map.acquire();
    DIAS_("cfgapi_auth_shm_ip6_table_refresh: acquring semaphore: done");
    if(rc) {
        WARS_("cfgapi_auth_shm_ip6_table_refresh: cannot acquire semaphore for token table");
        return -1;
    }
    
    DEBS_("cfgapi_auth_shm_ip6_table_refresh: loading table");
    int l_ip = auth_shm_ip6_map.load();
    DIAS_("cfgapi_auth_shm_ip6_table_refresh: loading table: done, releasing semaphore");
    auth_shm_ip6_map.release();
    DEBS_("cfgapi_auth_shm_ip6_table_refresh: semaphore released");
    
    if(l_ip >= 0) {
        // new data!
        cfgapi_identity_ip_lock.lock();
        
        if(l_ip == 0 && auth_ip6_map.size() > 0) {
            DIAS_("cfgapi_auth_shm_ip6_table_refresh: zero sized table received, flusing ip map");
            auth_ip6_map.clear();
        }
        
        DIA_("cfgapi_auth_shm_ip6_table_refresh: new data: version %d, entries %d",auth_shm_ip6_map.header_version(),auth_shm_ip6_map.header_entries());
        for(typename std::vector<shm_logon_info6>::iterator i = auth_shm_ip6_map.entries().begin(); i != auth_shm_ip6_map.entries().end() ; ++i) {
            shm_logon_info6& rt = (*i);
            
            char b[64]; memset(b,0,64);
            inet_ntop(AF_INET6,&rt.ip,b,64);
            
            std::string ip = std::string(b);
            
            std::unordered_map <std::string, IdentityInfo6 >::iterator found = auth_ip6_map.find(ip);
            if(found != auth_ip6_map.end()) {
                DIA_("Updating identity in database: %s",ip.c_str());
                IdentityInfo6& id = (*found).second;
                id.ip_address = ip;
                id.last_logon_info = rt;
                id.username = rt.username;
                id.update_groups_vec();
            } else {
                IdentityInfo6 i;
                i.ip_address = ip;
                i.last_logon_info = rt;
                i.username = rt.username;
                i.update_groups_vec();
                auth_ip6_map[ip] = i;
                INF_("New identity in database: ip: %s, username: %s, groups: %s ",ip.c_str(),i.username.c_str(),i.groups.c_str());
            }
            DIA_("cfgapi_auth_shm_ip6_table_refresh: loaded: %d,%s,%s",ip.c_str(),rt.username,rt.groups);
        }
        cfgapi_identity_ip_lock.unlock();
        
        return l_ip;
    }
    return 0;
}

IdentityInfo6* cfgapi_ip6_auth_get(std::string& host) {
    IdentityInfo6* ret = nullptr;

    cfgapi_identity_ip_lock.lock();    
    auto ip = auth_ip6_map.find(host);

    if (ip != auth_ip6_map.end()) {
        IdentityInfo6& id = (*ip).second;
        ret  = &id;
    }
    
    cfgapi_identity_ip_lock.unlock();
    return ret;
   
}

// remove IP from AUTH IP MAP and synchronize with SHM AUTH IP TABLE (table which is used to communicate with bend daemon)
void cfgapi_ip6_auth_remove(std::string& host) {

    cfgapi_identity_ip_lock.lock();    
    auto ip = auth_ip6_map.find(host);

    if (ip != auth_ip6_map.end()) {
        // erase internal ip map entry

        DIA_("cfgapi_ip_map_remove: auth ip map - removing: %s",host.c_str());
        auth_ip6_map.erase(ip);

        // for debug only: print all shm table entries
        if(LEV_(DEB)) {
            DEBS_(":: - SHM AUTH IP map - before removal::");
            for(auto& x_it: auth_shm_ip6_map.map_entries()) {
                DEB_("::  %s::",x_it.first.c_str());
            }
        }
        
        auth_shm_ip6_map.acquire();

        // erase shared ip map entry
        auto sh_it = auth_shm_ip6_map.map_entries().find(host);

        if(sh_it != auth_shm_ip6_map.map_entries().end()) {
            DIA_("cfgapi_ip_map_remove: shm auth ip table  - removing: %s",host.c_str());
            auth_shm_ip6_map.map_entries().erase(sh_it);
            
            if(LEV_(DEB)) {
                DEBS_(":: - SHM AUTH IP map - after removal::");
                for(auto& x_it: auth_shm_ip6_map.map_entries()) {
                        DEB_("::   %s::",x_it.first.c_str());
                }                
                
            }
            auth_shm_ip6_map.save(true);
        }
        auth_shm_ip6_map.release();
    }
    cfgapi_identity_ip_lock.unlock();
}


void cfgapi_ip6_auth_timeout_check(void) {
    DIAS_("cfgapi_ip_auth_timeout_check: started");
    cfgapi_identity_ip_lock.lock();
    
    std::set<std::string> to_remove;
    
    for(auto& e: auth_ip6_map) {
        const std::string&  ip = e.first;
        IdentityInfo6& id = e.second;
        
        DIA_("cfgapi_ip_auth_timeout_check: %s", ip.c_str());
        if(id.i_timeout()) {
            DIA_("cfgapi_ip_auth_timeout_check: idle timeout, adding to list %s", ip.c_str());
            to_remove.insert(ip);
        }
    }
    
    for(auto tr: to_remove) {
        cfgapi_ip_auth_remove(tr);
        INF_("IP address %s identity timed out.", tr.c_str());
    }
    
    cfgapi_identity_ip_lock.unlock();
}
