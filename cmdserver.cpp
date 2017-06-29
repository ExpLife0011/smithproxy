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

#include <string>
#include <thread>
#include <set>

#include <cstring>
#include <cstdlib>
#include <ctime>
#include <csignal>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <logger.hpp>
#include <cmdserver.hpp>
#include <cfgapi.hpp>
#include <timeops.hpp>

#include <socle.hpp>
#include <sslcom.hpp>
#include <sslcertstore.hpp>

#include <smithproxy.hpp>
#include <mitmproxy.hpp>
#include <sobject.hpp>
#include <dns.hpp>
#include <inspectors.hpp>

int cli_port = 50000;
std::string cli_enable_password = "";


static const char* debug_levels="\n\t0\tNONE\n\t1\tFATAL\n\t2\tCRITICAL\n\t3\tERROR\n\t4\tWARNING\n\t5\tNOTIFY\n\t6\tINFORMATIONAL\n\t7\tDIAGNOSE\t(may impact performance)\n\t8\tDEBUG\t(impacts performance)\n\t9\tEXTREME\t(severe performance drop)\n\t10\tDUMPALL\t(performance killer)\n\treset\treset back to level configured in config file";

int orig_ssl_loglevel = 0;
int orig_sslmitm_loglevel = 0;
int orig_sslca_loglevel = 0;

int orig_dns_insp_loglevel = 0;
int orig_dns_packet_loglevel = 0;

int orig_baseproxy_loglevel = 0;
int orig_epoll_loglevel = 0;
int orig_mitmproxy_loglevel = 0;
int orig_mitmmasterproxy_loglevel = 0;

void load_defaults() {
    orig_ssl_loglevel = SSLCom::log_level_ref();
    orig_sslmitm_loglevel = SSLMitmCom::log_level_ref();
    orig_sslca_loglevel= SSLCertStore::log_level_ref();
    
    orig_dns_insp_loglevel = DNS_Inspector::log_level_ref();
    orig_dns_packet_loglevel = DNS_Packet::log_level_ref();
    
    orig_baseproxy_loglevel = baseProxy::log_level_ref();
    orig_epoll_loglevel = epoll::log_level;
    orig_mitmproxy_loglevel = MitmProxy::log_level_ref();
    orig_mitmmasterproxy_loglevel = MitmMasterProxy::log_level_ref();
}

void cmd_show_status(struct cli_def* cli) {
    
    //cli_print(cli,":connected using socket %d",cli->client->_fileno);
  
    cli_print(cli,"Version: %s%s",SMITH_VERSION,SMITH_DEVEL ? " (dev)" : "");
    cli_print(cli,"Socle: %s%s",SOCLE_VERSION,SOCLE_DEVEL ? " (dev)" : "");
    cli_print(cli," ");
    time_t uptime = time(nullptr) - system_started;
    cli_print(cli,"Uptime: %s",uptime_string(uptime).c_str());
 
}

int cli_show_status(struct cli_def *cli, const char *command, char *argv[], int argc)
{
    //cli_print(cli, "called %s with %s, argc %d\r\n", __FUNCTION__, command, argc);

    
    cmd_show_status(cli);
    return CLI_OK;
}

int cli_diag_ssl_cache_stats(struct cli_def *cli, const char *command, char *argv[], int argc) {
    
    SSLCertStore* store = SSLCom::certstore();

    store->lock();
    int n_cache = store->cache().size();
    int n_fqdn_cache = store->fqdn_cache().size();
    store->unlock();

    cli_print(cli,"certificate store stats: ");
    cli_print(cli,"    CN cert cache size: %d ",n_cache);
    cli_print(cli,"    FQDN to CN cache size: %d ",n_fqdn_cache);
    
    return CLI_OK;
}


int cli_diag_ssl_cache_list(struct cli_def *cli, const char *command, char *argv[], int argc) {
    
    SSLCertStore* store = SSLCom::certstore();
    bool print_refs = false;
    
    if(argc > 0) {
        std::string a1 = argv[0];
        if(a1 == "7") print_refs = true;
    }
    
    
    store->lock();
    
    cli_print(cli,"certificate store entries: ");
    
    for (auto x = store->cache().begin(); x != store->cache().end(); ++x ) {
        std::string fqdn = x->first;
        X509_PAIR* ptr = x->second;
        
        cli_print(cli,"    %s",fqdn.c_str());
        if(print_refs)
            cli_print(cli,"            refcounts: key=%d cert=%d",ptr->first->references, ptr->second->references);

    }
        
    cli_print(cli,"\ncertificate fqdn cache: ");
    for (auto x = store->fqdn_cache().begin(); x != store->fqdn_cache().end(); ++x ) {
        std::string fqdn = x->first;
        std::string cn = x->second;
        cli_print(cli,"    %s -> %s",fqdn.c_str(), cn.c_str());
    }

    store->unlock();
    
    return CLI_OK;
}

int cli_diag_ssl_cache_clear(struct cli_def *cli, const char *command, char *argv[], int argc) {
    
    SSLCertStore* store = SSLCom::certstore();
    store->lock();
    
    for (auto x = store->cache().begin(); x != store->cache().end(); ++x ) {
        std::string fqdn = x->first;
        cli_print(cli,"removing    %s",fqdn.c_str());
        X509_PAIR* ptr = x->second;
        
        if(argc > 0) {
            std::string a1 = argv[0];
            if(a1 == "7") cli_print(cli,"            refcounts: key=%d cert=%d",ptr->first->references, ptr->second->references);
        }
        
        EVP_PKEY_free(ptr->first);
        X509_free(ptr->second);
    }
    store->cache().clear();
        
    for (auto x = store->fqdn_cache().begin(); x != store->fqdn_cache().end(); ++x ) {
        std::string fqdn = x->first;
        std::string cn = x->second;
    }
    store->fqdn_cache().clear();

    store->unlock();
    
    return CLI_OK;
}

int cli_diag_ssl_wl_list(struct cli_def *cli, const char *command, char *argv[], int argc) {
    
    cli_print(cli,"\nSSL whitelist:");
    std::string out;
    
    MitmProxy::whitelist_verify.lock();
    for(auto we: MitmProxy::whitelist_verify.cache()) {
        out += "\n\t" + we.first;
    }
    MitmProxy::whitelist_verify.unlock();
    
    cli_print(cli,"%s",out.c_str());
    return CLI_OK;
}

int cli_diag_ssl_wl_clear(struct cli_def *cli, const char *command, char *argv[], int argc) {
    
    
    MitmProxy::whitelist_verify.lock();
    MitmProxy::whitelist_verify.clear();
    MitmProxy::whitelist_verify.unlock();
    return CLI_OK;
}

int cli_diag_ssl_crl_list(struct cli_def *cli, const char *command, char *argv[], int argc) {
    
    SSLCertStore* store = SSLCom::certstore();
    std::string out;
    
    out += "Downloaded CRLs:\n\n";
    
    store->crl_cache.lock();
    for (auto x: store->crl_cache.cache()) {
       std::string uri = x.first;
       
       out += "    " + uri + "\n";
    }
    store->crl_cache.unlock();
    
    cli_print(cli,"\n%s",out.c_str());
    
    return CLI_OK;
}


int cli_diag_dns_cache_list(struct cli_def *cli, const char *command, char *argv[], int argc) {
    inspect_dns_cache.lock();
    
    cli_print(cli,"\nDNS cache populated from traffic: ");
    for(auto it = inspect_dns_cache.cache().begin(); it != inspect_dns_cache.cache().end() ; ++it ) {
        std::string s = it->first;
        DNS_Response* r = it->second;
        
        cli_print(cli,"    %s  ->%s",s.c_str(),r->answer_str().c_str());
    }
    
    inspect_dns_cache.unlock();
    
    return CLI_OK;
}

int cli_diag_dns_cache_stats(struct cli_def *cli, const char *command, char *argv[], int argc) {

    cli_print(cli,"\nDNS cache statistics: ");
    inspect_dns_cache.lock();
    int cache_size = inspect_dns_cache.cache().size();
    int max_size = inspect_dns_cache.max_size();
    bool del = inspect_dns_cache.auto_delete();
    inspect_dns_cache.unlock();

    cli_print(cli,"  Current size: %5d",cache_size);
    cli_print(cli,"  Maximum size: %5d",max_size);
    cli_print(cli,"\n    Autodelete: %5d",del);

    return CLI_OK;
}

int cli_diag_dns_cache_clear(struct cli_def *cli, const char *command, char *argv[], int argc) {
    inspect_dns_cache.lock();
    
    inspect_dns_cache.clear();
    
    cli_print(cli,"\nDNS cache cleared.");
    inspect_dns_cache.unlock();
    
    return CLI_OK;
}

int cli_diag_dns_domain_cache_list(struct cli_def *cli, const char *command, char *argv[], int argc) {
    cli_print(cli, "\n Domain cache list:");
    std::string out;
    domain_cache.lock();
    
    for(auto sub_domain_cache: domain_cache.cache()) {
        
        std::string domain = sub_domain_cache.first;
        std::string str;
        
        for(auto sub_e: sub_domain_cache.second->cache()) {
           str += " " + sub_e.first;
        }
        out += string_format("\n\t%s: \t%s",domain.c_str(),str.c_str());
        
    }
    
    domain_cache.unlock();
    cli_print(cli,"%s",out.c_str());
    
    return CLI_OK;
}

int cli_diag_dns_domain_cache_clear(struct cli_def *cli, const char *command, char *argv[], int argc) {
    cli_print(cli, "\n Clearing domain cache:");
    std::string out;
    domain_cache.lock();
    
    domain_cache.clear();
    
    domain_cache.unlock();
    cli_print(cli," done.");
    
    return CLI_OK;
}



int cli_diag_identity_ip_list(struct cli_def *cli, const char *command, char *argv[], int argc) {
    
    cli_print(cli, "\nIPv4 identities:");
    std::string out;
    
    cfgapi_identity_ip_lock.lock();
    for (auto ip: auth_ip_map) {
        std::string s;
        IdentityInfo& id = ip.second;
        
        s +=   "    ipv4: " + ip.first + ", user: " + id.username + ", groups: " + id.groups + ", rx/tx: " + number_suffixed(id.tx_bytes) + "/" + number_suffixed(id.rx_bytes);
        s += "\n          uptime: " + std::to_string(id.uptime()) + ", idle: " + std::to_string(id.i_time());
        s += "\n          status: " + std::to_string(!id.i_timeout()) + ", last policy: " + std::to_string(id.last_seen_policy);
        out += s;
    }
    cfgapi_identity_ip_lock.unlock();
    cli_print(cli, "%s", out.c_str());
    

    out.clear();
    
    cli_print(cli, "\nIPv6 identities:");
    

    cfgapi_identity_ip6_lock.lock();
    for (auto ip: auth_ip6_map) {
        std::string s;
        IdentityInfo6& id = ip.second;
        
        s +=   "    ipv6: " + ip.first + ", user: " + id.username + ", groups: " + id.groups + ", rx/tx: " + number_suffixed(id.tx_bytes) + "/" + number_suffixed(id.rx_bytes);
        s += "\n          uptime: " + std::to_string(id.uptime()) + ", idle: " + std::to_string(id.i_time());        
        s += "\n          status: " + std::to_string(!id.i_timeout()) + ", last policy: " + std::to_string(id.last_seen_policy);
        out += s;
    }
    cfgapi_identity_ip6_lock.unlock();
    cli_print(cli, "%s", out.c_str());    
    
    return CLI_OK;
}


void cli_print_log_levels(struct cli_def *cli) {
    logger_profile* lp = get_logger()->target_profiles()[(uint64_t)cli->client->_fileno];
    
    cli_print(cli,"This cli debug level is set to: %d",lp->level_);
    cli_print(cli,"General logging level set to: %d",get_logger()->level());
    for(auto i = get_logger()->remote_targets().begin(); i != get_logger()->remote_targets().end(); ++i) {
        cli_print(cli, "Logging level for: %s: %d",get_logger()->target_name((uint64_t)(*i)),get_logger()->target_profiles()[(uint64_t)(*i)]->level_);
    }
    for(auto i = get_logger()->targets().begin(); i != get_logger()->targets().end(); ++i) {
        cli_print(cli, "Logging level for: %s: %d",get_logger()->target_name((uint64_t)(*i)),get_logger()->target_profiles()[(uint64_t)(*i)]->level_);
    }         
}

int cli_debug_terminal(struct cli_def *cli, const char *command, char *argv[], int argc) {
    
    logger_profile* lp = get_logger()->target_profiles()[(uint64_t)cli->client->_fileno];
    if(argc > 0) {
        
        std::string a1 = argv[0];

        if(a1 == "?") {
            cli_print(cli,"valid parameters: %s",debug_levels);
        } 
        else if(a1 == "reset") {
            lp->level_ = NON;
            //get_logger()->level(cfgapi_table.logging.level);
        }
        else {
            //cli_print(cli, "called %s with %s, argc %d\r\n", __FUNCTION__, command, argc);
            lp->level_ = std::atoi(argv[0]);
            //get_logger()->level(lp->level_);
        }
    } else {
        
        cli_print_log_levels(cli);
    }
    
    return CLI_OK;
}


int cli_debug_logfile(struct cli_def *cli, const char *command, char *argv[], int argc) {
    
    if(argc > 0) {
        
        std::string a1 = argv[0];

        if(a1 == "?") {
            cli_print(cli,"valid parameters: %s",debug_levels);
        } 
        else if(a1 == "reset") {
            get_logger()->level(cfgapi_table.logging.level);
        }
        else {
            get_logger()->level(std::atoi(argv[0]));
        }
    } else {
        cli_print_log_levels(cli);
    }
    
    return CLI_OK;
}

int cli_debug_ssl(struct cli_def *cli, const char *command, char *argv[], int argc) {
    if(argc > 0) {
        std::string a1 = argv[0];
        if(a1 == "?") {
            cli_print(cli,"valid parameters: %s",debug_levels);
        } 
        else if(a1 == "reset") {
            SSLCom::log_level_ref() = orig_ssl_loglevel;
            SSLMitmCom::log_level_ref() = orig_sslmitm_loglevel;
            SSLCertStore::log_level_ref() = orig_sslca_loglevel;
        }
        else {
            int lev = std::atoi(argv[0]);
            SSLCom::log_level_ref() = lev;
            SSLMitmCom::log_level_ref() = lev;
            SSLCertStore::log_level_ref() = lev;
            
        }
    } else {
        int l = SSLCom::log_level_ref();
        cli_print(cli,"SSL debug level: %d",l);
        l = SSLMitmCom::log_level_ref();
        cli_print(cli,"SSL MitM debug level: %d",l);
        l = SSLCertStore::log_level_ref();
        cli_print(cli,"SSL CA debug level: %d",l);
        cli_print(cli,"\n");
        cli_print(cli,"valid parameters: %s",debug_levels);
    }
    
    return CLI_OK;
}


int cli_debug_dns(struct cli_def *cli, const char *command, char *argv[], int argc) {
    if(argc > 0) {
        std::string a1 = argv[0];
        if(a1 == "?") {
            cli_print(cli,"valid parameters: %s",debug_levels);
        } 
        else if(a1 == "reset") {
            DNS_Inspector::log_level_ref() = orig_dns_insp_loglevel;
            DNS_Packet::log_level_ref() = orig_dns_packet_loglevel;
        }
        else {
            int lev = std::atoi(argv[0]);
            DNS_Inspector::log_level_ref() = lev;
            DNS_Packet::log_level_ref() = lev;
            
        }
    } else {
        int l = DNS_Inspector::log_level_ref();
        cli_print(cli,"DNS Inspector debug level: %d",l);
        l = DNS_Packet::log_level_ref();
        cli_print(cli,"DNS Packet debug level: %d",l);
        cli_print(cli,"\n");
        cli_print(cli,"valid parameters: %s",debug_levels);
    }
    
    return CLI_OK;
}


int cli_debug_proxy(struct cli_def *cli, const char *command, char *argv[], int argc) {
    if(argc > 0) {
        std::string a1 = argv[0];
        if(a1 == "?") {
            cli_print(cli,"valid parameters: %s",debug_levels);
        } 
        else if(a1 == "reset") {
            baseProxy::log_level_ref() = orig_baseproxy_loglevel;
            epoll::log_level = orig_epoll_loglevel;
            MitmMasterProxy::log_level_ref() = orig_mitmproxy_loglevel;
            MitmProxy::log_level_ref() = orig_mitmproxy_loglevel;
        }
        else {
            int lev = std::atoi(argv[0]);
            baseProxy::log_level_ref() = lev;
            epoll::log_level = lev;
            MitmMasterProxy::log_level_ref() = lev;
            MitmProxy::log_level_ref() = lev;
            
            
        }
    } else {
        int l = baseProxy::log_level_ref();
        cli_print(cli,"baseProxy debug level: %d",l);

        l = epoll::log_level;
        cli_print(cli,"epoll debug level: %d",l);

        l = MitmMasterProxy::log_level_ref();
        cli_print(cli,"MitmMasterProxy debug level: %d",l);

        l = MitmProxy::log_level_ref();
        cli_print(cli,"MitmProxy debug level: %d",l);


        cli_print(cli,"\n");
        cli_print(cli,"valid parameters: %s",debug_levels);
    }
    
    return CLI_OK;
}



int cli_diag_mem_buffers_stats(struct cli_def *cli, const char *command, char *argv[], int argc) {
    cli_print(cli,"Memory buffers stats: ");
    cli_print(cli,"memory alloc   bytes: %lld",buffer::alloc_bytes);
    cli_print(cli,"memory free    bytes: %lld",buffer::free_bytes);
    cli_print(cli,"memory current bytes: %lld",buffer::alloc_bytes-buffer::free_bytes);
    cli_print(cli,"\nmemory alloc   counter: %lld",buffer::alloc_count);
    cli_print(cli,"memory free    counter: %lld",buffer::free_count);
    cli_print(cli,"memory current counter: %lld",buffer::alloc_count-buffer::free_count);
#ifdef SOCLE_MEM_PROFILE
    if(argc > 0) {
        
        std::string arg1(argv[0]);
        if(arg1 == "?") {
            cli_print(cli,"buffers        print all still allocated buffers' traces");
            cli_print(cli,"buffers_all    print all buffers' traces, including properly freed");
            cli_print(cli,"clear          remove all buffer tracking entries");
            return CLI_OK;
        }
        
        bool b = false;
        bool ba = false;
        bool clr = false;
        
        if(arg1 == "buffers") { b = true; }
        if(arg1 == "buffers_all") { b = true; ba = true; }
        if(arg1 == "clear") { clr = true; }
        
        if(b) {
            cli_print(cli,"\nExtra memory traces: ");
            buffer::alloc_map_lock();
            for( auto it = buffer::alloc_map.begin(); it != buffer::alloc_map.end(); ++it) {
                std::string bt = it->first;
                int& counter = it->second;
                
                if(counter > 0 || ba) {
                    cli_print(cli,"\nActive trace: %d references %s",counter,bt.c_str());
                }
            }
            buffer::alloc_map_unlock();
        }
        else if (clr) {
            buffer::alloc_bytes = 0;
            buffer::free_bytes = 0;
            buffer::alloc_count = 0;
            buffer::free_count = 0;
            cli_print(cli,"buffer usage counters reset.");

            int n = buffer::alloc_map.size();
            buffer::counter_clear_bt();
            cli_print(cli,"%d entries from buffer tracker database deleted.",n);
        }
    }

    buffer::alloc_map_unlock();
#endif
    return CLI_OK;
}

int cli_diag_mem_objects_stats(struct cli_def *cli, const char *command, char *argv[], int argc) {
    
    cli_print(cli,"Statistics:\n");
    cli_print(cli,"%s",socle::sobject_db_stats_string(nullptr).c_str());
    return CLI_OK;

}


int cli_diag_mem_objects_list(struct cli_def *cli, const char *command, char *argv[], int argc) {
    
    std::string object_filter;
    int verbosity = INF;
    
    if(argc > 0) {
        std::string a1 = argv[0];
        if(a1 == "?") {
            cli_print(cli,"valid parameters:");
            cli_print(cli,"         <empty>");
            cli_print(cli,"         MitmProxy");
            cli_print(cli,"         DNS_Inspector");
            cli_print(cli,"         DNS_Response");
            cli_print(cli,"         DNS_Request");
            cli_print(cli,"         DNS_Packet");
            
            return CLI_OK;
        } else {
            // a1 is param for the lookup
            if("*" == a1 || "ALL" == a1) {
                object_filter = "";
            } else {
                object_filter = a1.c_str();
            }
        }
        
        if(argc > 1) {
            std::string a2 = argv[1];
            verbosity = atoi(a2.c_str());
        }
    }
    
    
    std::string r = socle::sobject_db_to_string((object_filter.size() == 0) ? nullptr : object_filter.c_str(),nullptr,verbosity);
                r += "\n" + socle::sobject_db_stats_string((object_filter.size() == 0) ? nullptr : object_filter.c_str());

    
    cli_print(cli,"Smithproxy objects (filter: %s):\n%s\nFinished.",(object_filter.size() == 0) ? "ALL" : object_filter.c_str() ,r.c_str());
    return CLI_OK;
}

int cli_diag_mem_objects_clear(struct cli_def *cli, const char *command, char *argv[], int argc) {
    std::string address;
    
    if(argc > 0) {
        std::string a1 = argv[0];
        if(a1 == "?") {
            cli_print(cli,"valid parameters:");
            cli_print(cli,"         <object id>");
            
            return CLI_OK;
        } else {
            // a1 is param for the lookup
            address = a1.c_str();
            
            uint64_t key = strtol(address.c_str(),nullptr,16);
            cli_print(cli,"Trying to clear 0x%lx",key);
            
            socle::sobject_db.lock();
            int ret = socle::sobject_db_ask_destroy((void*)key);
            socle::sobject_db.unlock();
            
            switch(ret) {
                case 1:
                    cli_print(cli,"object agrees to terminate.");
                    break;
                case 0:
                    cli_print(cli,"object doesn't agree to terminate, or doesn't support it.");
                    break;
                case -1:
                    cli_print(cli, "object not found.");
                    break;
                default:
                    cli_print(cli, "unknown result.");
                    break;
            }
        }
    }

    return CLI_OK;
}

int cli_diag_proxy_session_list(struct cli_def *cli, const char *command, char *argv[], int argc) {
    char *a[2];
    a[0] = "MitmProxy";
    a[1] = nullptr;
    
    if(argc > 0) {
        a[1] = argv[0];
    }
    
    int ret = cli_diag_mem_objects_list(cli,command,a,argc > 0 ? 2 : 1);

    
    unsigned long l = socle::time_get_counter_sec(&MitmProxy::cnt_left_bytes_second,&MitmProxy::meter_left_bytes_second,1);
    unsigned long r = socle::time_get_counter_sec(&MitmProxy::cnt_right_bytes_second,&MitmProxy::meter_right_bytes_second,1);
    cli_print(cli,"\nProxy performance: upload %sbps, download %sbps in last second",number_suffixed(l*8).c_str(),number_suffixed(r*8).c_str());
    
    return ret;

}

int cli_diag_proxy_session_clear(struct cli_def *cli, const char *command, char *argv[], int argc) {
    return cli_diag_mem_objects_clear(cli,command,argv,argc);
}

struct cli_ext : public cli_def {
    int socket;
};


void client_thread(int client_socket) {
        struct cli_command *show;
        struct cli_command *debuk;
        struct cli_command *diag;
            struct cli_command *diag_ssl;
                struct cli_command *diag_ssl_cache;
                struct cli_command *diag_ssl_wl;
                struct cli_command *diag_ssl_crl;
            struct cli_command *diag_mem;
                struct cli_command *diag_mem_buffers;
                struct cli_command *diag_mem_objects;
            struct cli_command *diag_dns;
                struct cli_command *diag_dns_cache;
                struct cli_command *diag_dns_domains;
            struct cli_command *diag_proxy;
                struct cli_command *diag_proxy_session;
            struct cli_command *diag_identity;
                struct cli_command *diag_identity_user;
        
        struct cli_def *cli;
        
        char hostname[64]; memset(hostname,0,64);
        gethostname(hostname,63);
        

        // Must be called first to setup data structures
        cli = cli_init();

        // Set the hostname (shown in the the prompt)
        cli_set_hostname(cli, string_format("smithproxy(%s) ",hostname).c_str());

        // Set the greeting
        cli_set_banner(cli, "--==[ Smithproxy command line utility ]==--");

        cli_allow_enable(cli, cli_enable_password.c_str());

        // Set up 2 commands "show counters" and "show junk"
        show  = cli_register_command(cli, NULL, "show", NULL, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "show basic information");
                cli_register_command(cli, show, "status", cli_show_status, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "show smithproxy status");
        diag  = cli_register_command(cli, NULL, "diag", NULL, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "diagnose commands helping to troubleshoot");
            diag_ssl = cli_register_command(cli, diag, "ssl", NULL, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "ssl related troubleshooting commands");
                diag_ssl_cache = cli_register_command(cli, diag_ssl, "cache", NULL, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "diagnose ssl certificate cache");
                        cli_register_command(cli, diag_ssl_cache, "stats", cli_diag_ssl_cache_stats, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "display ssl cert cache statistics");
                        cli_register_command(cli, diag_ssl_cache, "list", cli_diag_ssl_cache_list, PRIVILEGE_PRIVILEGED, MODE_EXEC, "list all ssl cert cache entries");
                        cli_register_command(cli, diag_ssl_cache, "clear", cli_diag_ssl_cache_clear, PRIVILEGE_PRIVILEGED, MODE_EXEC, "remove all ssl cert cache entries");
                diag_ssl_wl = cli_register_command(cli, diag_ssl, "whitelist", NULL, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "diagnose ssl temporary verification whitelist");                        
                        cli_register_command(cli, diag_ssl_wl, "list", cli_diag_ssl_wl_list, PRIVILEGE_PRIVILEGED, MODE_EXEC, "list all verification whitelist entries");
                        cli_register_command(cli, diag_ssl_wl, "clear", cli_diag_ssl_wl_clear, PRIVILEGE_PRIVILEGED, MODE_EXEC, "clear all verification whitelist entries");
                diag_ssl_crl = cli_register_command(cli, diag_ssl, "crl", NULL, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "diagnose dynamically downloaded CRLs");                           
                        cli_register_command(cli, diag_ssl_crl, "list", cli_diag_ssl_crl_list, PRIVILEGE_PRIVILEGED, MODE_EXEC, "list all CRLs");
            diag_mem = cli_register_command(cli, diag, "mem", NULL, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "memory related troubleshooting commands");
                diag_mem_buffers = cli_register_command(cli, diag_mem, "buffers", NULL, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "memory buffers troubleshooting commands");
                        cli_register_command(cli, diag_mem_buffers, "stats", cli_diag_mem_buffers_stats, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "memory buffers statistics");
                diag_mem_objects = cli_register_command(cli, diag_mem, "objects", NULL, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "memory object troubleshooting commands");                        
                        cli_register_command(cli, diag_mem_objects, "stats", cli_diag_mem_objects_stats, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "memory objects statistics");
                        cli_register_command(cli, diag_mem_objects, "list", cli_diag_mem_objects_list, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "memory objects list");
                        cli_register_command(cli, diag_mem_objects, "clear", cli_diag_mem_objects_clear, PRIVILEGE_PRIVILEGED, MODE_EXEC, "clears memory object");
            diag_dns = cli_register_command(cli, diag, "dns", NULL, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "DNS traffic related troubleshooting commands");
                diag_dns_cache = cli_register_command(cli, diag_dns, "cache", NULL, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "DNS traffic cache troubleshooting commands");
                        cli_register_command(cli, diag_dns_cache, "list", cli_diag_dns_cache_list, PRIVILEGE_PRIVILEGED, MODE_EXEC, "list all DNS traffic cache entries");
                        cli_register_command(cli, diag_dns_cache, "stats", cli_diag_dns_cache_stats, PRIVILEGE_PRIVILEGED, MODE_EXEC, "DNS traffic cache statistics");
                        cli_register_command(cli, diag_dns_cache, "clear", cli_diag_dns_cache_clear, PRIVILEGE_PRIVILEGED, MODE_EXEC, "clear DNS traffic cache");
                diag_dns_domains = cli_register_command(cli, diag_dns, "domain", NULL, PRIVILEGE_UNPRIVILEGED, MODE_EXEC, "DNS domain cache troubleshooting commands");
                        cli_register_command(cli, diag_dns_domains, "list", cli_diag_dns_domain_cache_list, PRIVILEGE_PRIVILEGED, MODE_EXEC, "DNS sub-domain list");
                        cli_register_command(cli, diag_dns_domains, "clear", cli_diag_dns_domain_cache_clear, PRIVILEGE_PRIVILEGED, MODE_EXEC, "clear DNS sub-domain cache");
            diag_proxy = cli_register_command(cli, diag, "proxy",NULL, PRIVILEGE_PRIVILEGED, MODE_EXEC, "proxy related troubleshooting commands");
                diag_proxy_session = cli_register_command(cli,diag_proxy,"session",NULL,PRIVILEGE_PRIVILEGED, MODE_EXEC,"proxy session commands");
                        cli_register_command(cli, diag_proxy_session,"list",cli_diag_proxy_session_list, PRIVILEGE_PRIVILEGED, MODE_EXEC,"proxy session list");
                        cli_register_command(cli, diag_proxy_session,"clear",cli_diag_proxy_session_clear, PRIVILEGE_PRIVILEGED, MODE_EXEC,"proxy session clear");
            diag_identity = cli_register_command(cli,diag,"identity",NULL,PRIVILEGE_PRIVILEGED, MODE_EXEC,"identity related commands");
                diag_identity_user = cli_register_command(cli, diag_identity,"user",NULL, PRIVILEGE_PRIVILEGED, MODE_EXEC,"identity commands related to users");
                        cli_register_command(cli, diag_identity_user,"list",cli_diag_identity_ip_list, PRIVILEGE_PRIVILEGED, MODE_EXEC,"list all known users");
                        
                        
        debuk = cli_register_command(cli, NULL, "debug", NULL, PRIVILEGE_PRIVILEGED, MODE_EXEC, "diagnostic commands");
            cli_register_command(cli, debuk, "term", cli_debug_terminal, PRIVILEGE_PRIVILEGED, MODE_EXEC, "set level of logging to this terminal");
            cli_register_command(cli, debuk, "file", cli_debug_logfile, PRIVILEGE_PRIVILEGED, MODE_EXEC, "set level of logging of standard log file");
            cli_register_command(cli, debuk, "ssl", cli_debug_ssl, PRIVILEGE_PRIVILEGED, MODE_EXEC, "set ssl file logging level");
            cli_register_command(cli, debuk, "dns", cli_debug_dns, PRIVILEGE_PRIVILEGED, MODE_EXEC, "set dns file logging level");
            cli_register_command(cli, debuk, "proxy", cli_debug_proxy, PRIVILEGE_PRIVILEGED, MODE_EXEC, "set proxy file logging level");
        
        // Pass the connection off to libcli
        get_logger()->remote_targets(string_format("cli-%d",client_socket),client_socket);

        logger_profile lp;
        lp.level_ = cfgapi_table.logging.cli_init_level;
        get_logger()->target_profiles()[(uint64_t)client_socket] = &lp;
        
        
        load_defaults();
        cli_loop(cli, client_socket);
        
        get_logger()->remote_targets().remove(client_socket);
        get_logger()->target_profiles().erase(client_socket);
        close(client_socket);
        
        // Free data structures
        cli_done(cli);    
}

void cli_loop(short unsigned int port) {
    struct sockaddr_in servaddr;
    int on = 1;

    // Create a socket
    int s = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    servaddr.sin_port = htons(port);
    bind(s, (struct sockaddr *)&servaddr, sizeof(servaddr));

    // Wait for a connection
    listen(s, 50);

    int client_socket = 0;
    while ((client_socket = accept(s, NULL, 0)))
    {
        std::thread* n = new std::thread(client_thread,client_socket);
    }
}