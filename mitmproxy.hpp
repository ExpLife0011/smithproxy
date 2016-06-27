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


#ifndef MITMPROXY_HPP
 #define MITMPROXY_HPP

#include <basecom.hpp>
#include <hostcx.hpp>
#include <sockshostcx.hpp>
#include <baseproxy.hpp>
#include <threadedacceptor.hpp>
#include <threadedreceiver.hpp>
#include <traflog.hpp>
#include <policy.hpp>
#include <cfgapi_auth.hpp>

class leafProxy : public baseProxy, public socle::sobject {
public:
    
    explicit leafProxy(baseProxy* parent): baseProxy(parent->com()) {}
    virtual ~leafProxy() {};
    
    virtual std::string to_string(int verbosity=INF) { return std::string("leafProxy"); };
        
    DECLARE_C_NAME("leafProxy");
    DECLARE_LOGGING(to_string);
};

class MitmProxy : public baseProxy, public socle::sobject {
    
protected:
    trafLog *tlog_;
    
    bool write_payload_ = false;
    
    bool identity_resolved_ = false;    // meant if attempt has been done, regardless of it's result.
    bool identity_resolved_time = 0;
    shm_logon_info identity_;
    
    std::vector<baseHostCX*> backends_;
    std::vector<ProfileContentRule>* content_rule_ = nullptr; //save some space and store it as a pointer. Init it only when needed and delete in dtor.
    
    int matched_policy_ = -1;
public: 
    bool opt_auth_authenticate = false;
    bool opt_auth_resolve = false;
    bool auth_block_identity = false;
    
    bool opt_av_check = true;
    #define AV_STAT_FAILED -1
    #define AV_STAT_NONE 0
    #define AV_STAT_OK 1
    #define AV_RESULT_OK 2
    #define AV_RESULT_INFECTED 64
    #define AV_FINISHED 192
    int av_backend_status = AV_STAT_NONE;
    int av_backend_init();
    baseHostCX* av_proxy = nullptr;
    
    int matched_policy() { return matched_policy_; }
    void matched_policy(int p) { matched_policy_ = p; }    
    
    inline bool identity_resolved();
    inline void identity_resolved(bool b);
    shm_logon_info& identity() { return identity_; }
    void identity(shm_logon_info& i) { identity_ = i; }
    bool resolve_identity(baseHostCX*,bool);
    bool update_identity(baseHostCX*);
    bool apply_id_policies(baseHostCX* cx);
    
    std::vector<baseHostCX*>& backends() { return backends_; };
    
    bool write_payload(void) { return write_payload_; } 
    void write_payload(bool b) { write_payload_ = b; }
    
    bool detect_dns(MitmHostCX* mh, unsigned char side);
    
    trafLog* tlog() { return tlog_; }
    
    explicit MitmProxy(baseCom* c);
    virtual ~MitmProxy();
    
    // this virtual method is called whenever there are new bytes in any LEFT host context!
    virtual void on_left_bytes(baseHostCX* cx);    
    virtual void on_right_bytes(baseHostCX* cx);
    
    // ... and also when there is error on L/R side, claim the proxy DEAD. When marked dead, it will be safely 
    // closed by it's master proxy next cycle.
    
    virtual void on_left_error(baseHostCX* cx);
    virtual void on_right_error(baseHostCX* cx);
    
    virtual void handle_replacement(MitmHostCX* cx);
    virtual void handle_internal_data(baseHostCX* cx);
    
    virtual bool ask_destroy() { dead(true); return true; };
    virtual std::string to_string(int verbosity=INF);
    
    virtual int handle_sockets_once(baseCom*);
    
    bool is_backend_cx(baseHostCX*);
    void on_backend_error(baseHostCX*);
    
    void init_content_replace();
    std::vector<ProfileContentRule>* content_rule() { return content_rule_; }    
    void content_replace(std::vector<ProfileContentRule>& x) { 
	for(auto i: x) {
	    content_rule_->push_back(i);
	}
    }
    
    buffer content_replace_apply(buffer);
    
public:

    static unsigned long meter_left_bytes_second;
    static unsigned long meter_right_bytes_second;
    static time_t cnt_left_bytes_second;
    static time_t cnt_right_bytes_second;

    
    DECLARE_C_NAME("MitmProxy");
    DECLARE_LOGGING(to_string);
};




class MitmMasterProxy : public ThreadedAcceptorProxy<MitmProxy> {
public:
    
    MitmMasterProxy(baseCom* c, int worker_id) : ThreadedAcceptorProxy< MitmProxy >(c,worker_id) {};
    
    virtual baseHostCX* new_cx(int s);
    virtual void on_left_new(baseHostCX* just_accepted_cx);
    virtual int handle_sockets_once(baseCom* c);
    
    static bool ssl_autodetect;
    static bool ssl_autodetect_harder;
    bool detect_ssl_on_plain_socket(int s);
    
    time_t auth_table_refreshed = 0;
};


class MitmUdpProxy : public ThreadedReceiverProxy<MitmProxy> {
public:
    MitmUdpProxy(baseCom* c, int worker_id) : ThreadedReceiverProxy< MitmProxy >(c,worker_id) {};
    virtual void on_left_new(baseHostCX* just_accepted_cx);
    baseHostCX* new_cx(int s);
};

#endif //MITMPROXY_HPP