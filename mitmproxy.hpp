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


/*
 now let's override baseProxy, and use on_left/right_bytes method!
 this proxy is working with *already accepted* sockets

 basically Proxy class recognizes LEFT and RIGHT side. You can organize those contexts on both sides.
 it's up to you what will do with them, it doesn't have any particular technical meaning; it just 
 follows the principle that you are usually proxying 2 sides (most commonly clients with servers, but 
 left-right is more generic and follows common sense.
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
#include <cfgapi_auth.hpp>

class MitmProxy : public baseProxy {
    
protected:
    trafLog *tlog_;
    
    bool write_payload_ = false;
    
    bool       identity_resolved_ = false;    // meant if attempt has been done, regardless of it's result.
    logon_info identity_;
    
public: 
    bool opt_auth_authenticate = false;
    bool opt_auth_resolve = false;
    
    
    inline bool identity_resolved() { return identity_resolved_; };
    inline void identity_resolved(bool b) { identity_resolved_ = b; };
    logon_info& identity() { return identity_; }
    void identity(logon_info& i) { identity_ = i; }
    bool resolve_identity(baseHostCX*,bool);
    bool update_identity(baseHostCX*);
    
    bool write_payload(void) { return write_payload_; } 
    void write_payload(bool b) { write_payload_ = b; }
    
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
};




class MitmMasterProxy : public ThreadedAcceptorProxy<MitmProxy> {
public:
    
    MitmMasterProxy(baseCom* c, int worker_id) : ThreadedAcceptorProxy< MitmProxy >(c,worker_id) {};
    
    virtual baseHostCX* new_cx(int s);
    virtual void on_left_new(baseHostCX* just_accepted_cx);
    virtual int handle_sockets_once(baseCom* c);
};


class MitmUdpProxy : public ThreadedReceiverProxy<MitmProxy> {
public:
    MitmUdpProxy(baseCom* c, int worker_id) : ThreadedReceiverProxy< MitmProxy >(c,worker_id) {};
    virtual void on_left_new(baseHostCX* just_accepted_cx);
};

#endif //MITMPROXY_HPP