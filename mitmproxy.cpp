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

#include <mitmproxy.hpp>
#include <mitmhost.hpp>
#include <logger.hpp>
#include <cfgapi.hpp>
#include <sockshostcx.hpp>


MitmProxy::MitmProxy(baseCom* c): baseProxy(c) {

    std::string data_dir = "mitm";
    std::string file_pref = "";
    std::string file_suff = "smcap";
    
    cfgapi.getRoot()["settings"].lookupValue("write_payload_dir",data_dir);
    cfgapi.getRoot()["settings"].lookupValue("write_payload_file_prefix",file_pref);
    cfgapi.getRoot()["settings"].lookupValue("write_payload_file_suffix",file_suff);
    
    tlog_ = new trafLog(this,data_dir.c_str(),file_pref.c_str(),file_suff.c_str());
}


MitmProxy::~MitmProxy() {
    
    if(write_payload()) {
        DEBS_("MitmProxy::destructor: syncing writer");

        for(typename std::vector<baseHostCX*>::iterator j = this->left_sockets.begin(); j != this->left_sockets.end(); ++j) {
            auto cx = (*j);
            if(cx->log().size()) {
                tlog()->write('L', cx->log());
                cx->log() = "";
            }
        }               
        
        for(typename std::vector<baseHostCX*>::iterator j = this->right_sockets.begin(); j != this->right_sockets.end(); ++j) {
            auto cx = (*j);
            if(cx->log().size()) {
                tlog()->write('R', cx->log());
                cx->log() = "";
            }
        }         
        
        tlog()->left_write("Connection stop\n");
    }
    
    delete tlog_;
}

void MitmProxy::on_left_bytes(baseHostCX* cx) {
        
    if(write_payload()) {
        if(cx->log().size()) {
            tlog()->write('L', cx->log());
            cx->log() = "";
        }
        
        tlog()->left_write(cx->to_read());
    }
    
    // because we have left bytes, let's copy them into all right side sockets!
    for(typename std::vector<baseHostCX*>::iterator j = this->right_sockets.begin(); j != this->right_sockets.end(); j++) {

        // to_read: returns readbuf's buffer "view" of previously processed bytes 
        // to_write: this is appending to caller's write buffer

        // next line therefore calls processing context to return new processed bytes (to_read is like to offer: I have new processed data, read it if you want)
        // those processed data will be wiped by next read() call, so let's now write them all to right socket!
        (*j)->to_write(cx->to_read());
    }    
}

void MitmProxy::on_right_bytes(baseHostCX* cx) {
        
    if(write_payload()) {
        if(cx->log().size()) {
            tlog()->write('R',cx->log());
            cx->log() = "";
        }
        
        tlog()->right_write(cx->to_read());
    }
    
    for(typename std::vector<baseHostCX*>::iterator j = this->left_sockets.begin(); j != this->left_sockets.end(); j++) {
        (*j)->to_write(cx->to_read());
    }
}

void MitmProxy::on_left_error(baseHostCX* cx) {
    
    DEB_("on_left_error[%s]: proxy marked dead",(this->error_on_read ? "read" : "write"));
    DUMS_(this->hr());
    
    if(write_payload()) {
        tlog()->left_write("Client side connection closed: " + cx->name() + "\n");
    }
    

    INF_("Connection from %s closed, sent=%d/%dB received=%d/%dB, flags=%c",
                        cx->full_name('L').c_str(),
                                        cx->meter_read_count,cx->meter_read_bytes,
                                                            cx->meter_write_count, cx->meter_write_bytes,
                                                                        'L');
    this->dead(true); 
}

void MitmProxy::on_right_error(baseHostCX* cx)
{
    DEB_("on_right_error[%s]: proxy marked dead",(this->error_on_read ? "read" : "write"));
    
    if(write_payload()) {
        tlog()->right_write("Server side connection closed: " + cx->name() + "\n");
    }
    
//         INF_("Created new proxy 0x%08x from %s:%s to %s:%d",new_proxy,f,f_p, t,t_p );
    
    INF_("Connection from %s closed, sent=%d/%dB received=%d/%dB, flags=%c",
                            cx->full_name('R').c_str(), 
                                            cx->meter_write_count, cx->meter_write_bytes,
                                                            cx->meter_read_count,cx->meter_read_bytes,
                                                                    'R');
    this->dead(true); 
}



baseHostCX* MitmMasterProxy::new_cx(int s) {
    auto r = new MitmHostCX(com()->replicate(),s);
    DEB_("Pausing new connection %s",r->c_name());
    r->paused(true);
    return r; 
}
void MitmMasterProxy::on_left_new(baseHostCX* just_accepted_cx) {
    // ok, we just accepted socket, created context for it (using new_cx) and we probably need ... 
    // to create child proxy and attach this cx to it.
    
    // NEW: whole method is reorganized 

    if(! just_accepted_cx->com()->nonlocal_dst_resolved()) {
        ERRS_("Was not possible to resolve original destination!");
        just_accepted_cx->close();
        delete just_accepted_cx;
    } 
    else {
        MitmProxy* new_proxy = new MitmProxy(com()->replicate());
        
        // let's add this just_accepted_cx into new_proxy
        if(just_accepted_cx->paused_read()) {
            DEBS_("MitmMasterProxy::on_left_new: ldaadd the new paused cx");
            new_proxy->ldaadd(just_accepted_cx);
        } else{
            DEBS_("MitmMasterProxy::on_left_new: ladd the new cx (unpaused)");
            new_proxy->ladd(just_accepted_cx);
        }
        MitmHostCX *target_cx = new MitmHostCX(com()->replicate(), just_accepted_cx->com()->nonlocal_dst_host().c_str(), 
                                            string_format("%d",just_accepted_cx->com()->nonlocal_dst_port()).c_str()
                                            );
        // connect it! - btw ... we don't want to block of course...
        
        std::string h;
        std::string p;
        just_accepted_cx->name();
        just_accepted_cx->com()->resolve_socket_src(just_accepted_cx->socket(),&h,&p);
        
        
        just_accepted_cx->peer(target_cx);
        target_cx->peer(just_accepted_cx);

        target_cx->com()->nonlocal_src(true); //FIXME
        target_cx->com()->nonlocal_src_host() = h;
        target_cx->com()->nonlocal_src_port() = std::stoi(p);
        
        target_cx->connect(false);        
        //NEW: end of new
        
        // almost done, just add this target_cx to right side of new proxy
        new_proxy->radd(target_cx);
        
        // apply policy and get result
        baseProxy* verdict = cfgapi_obj_policy_apply(just_accepted_cx,new_proxy);
        if(verdict != nullptr) {
            this->proxies().push_back(new_proxy);
        }
    }
    
    DEBS_("MitmMasterProxy::on_left_new: finished");
}

int MitmMasterProxy::handle_sockets_once(baseCom* c) {
    //T_DIAS_("slist",5,this->hr()+"\n===============\n");
    return ThreadedAcceptorProxy<MitmProxy>::handle_sockets_once(c);
}


void MitmUdpProxy::on_left_new(baseHostCX* just_accepted_cx)
{
    MitmProxy* new_proxy = new MitmProxy(com()->replicate());
    // let's add this just_accepted_cx into new_proxy
    if(just_accepted_cx->paused_read()) {
        DEBS_("MitmMasterProxy::on_left_new: ldaadd the new paused cx");
        new_proxy->ldaadd(just_accepted_cx);
    } else{
        DEBS_("MitmMasterProxy::on_left_new: ladd the new cx (unpaused)");
        new_proxy->ladd(just_accepted_cx);
    }
    
    MitmHostCX *target_cx = new MitmHostCX(com()->replicate(), just_accepted_cx->com()->nonlocal_dst_host().c_str(), 
                                    string_format("%d",just_accepted_cx->com()->nonlocal_dst_port()).c_str()
                                    );
    

    std::string h;
    std::string p;
    just_accepted_cx->name();
    just_accepted_cx->com()->resolve_socket_src(just_accepted_cx->socket(),&h,&p);
    
    just_accepted_cx->peer(target_cx);
    target_cx->peer(just_accepted_cx);

    target_cx->com()->nonlocal_src(true); //FIXME
    target_cx->com()->nonlocal_src_host() = h;
    target_cx->com()->nonlocal_src_port() = std::stoi(p);    
    
    target_cx->connect(false);    
    
    ((AppHostCX*)just_accepted_cx)->mode(AppHostCX::MODE_NONE);
    target_cx->mode(AppHostCX::MODE_NONE);
    
    new_proxy->radd(target_cx);

    // apply policy and get result
    baseProxy* verdict = cfgapi_obj_policy_apply(just_accepted_cx,new_proxy);
    if(verdict != nullptr) {
        this->proxies().push_back(new_proxy);
    }
    
    
    // FINAL point: adding new child proxy to the list
//     this->proxies().push_back(new_proxy);
//     
//     INF_("Connection from %s established", just_accepted_cx->full_name('L').c_str());        
// 
//     if(new_proxy->write_payload()) {
//         new_proxy->tlog().left_write("Connection start\n");
//     }    
}


#include <sslcom.hpp>
#include <tcpcom.hpp>


SocksProxy::SocksProxy(baseCom* c): MitmProxy(c) {}
SocksProxy::~SocksProxy() {}

void SocksProxy::on_left_message(baseHostCX* basecx) {

    socksServerCX* cx = static_cast<socksServerCX*>(basecx);
    if(cx != nullptr) {
        switch(cx->state_) {
            case WAIT_POLICY:
                DIAS_("SocksProxy::on_left_message: policy check: accepted");
                cx->verdict(ACCEPT);
                break;
            
            case HANDOFF:
                DIAS_("SocksProxy::on_left_message: socksHostCX handoff msg received");
                cx->state(ZOMBIE);
                
                socks5_handoff(cx);
                break;
            default:
                WARS_("SocksProxy::on_left_message: unknown message");
                break;
        }
    }
}

void SocksProxy::socks5_handoff(socksServerCX* cx) {

    DEBS_("SocksProxy::socks5_handoff: start");
    
    int s = ::dup(cx->socket());
    bool ssl = false;
    
    baseCom* new_com = nullptr;
    switch(cx->com()->nonlocal_dst_port()) {
        case 443:
        case 465:
        case 636:
        case 993:
        case 995:
            new_com = new socksSSLMitmCom();
            ssl = true;
            break;
        default:
            new_com = new socksTCPCom();
    }
    
    MitmHostCX* n_cx = new MitmHostCX(new_com, s);
    n_cx->paused(true);
    n_cx->com()->name();
    n_cx->name();
    n_cx->com()->nonlocal_dst(true);
    n_cx->com()->nonlocal_dst_host() = cx->com()->nonlocal_dst_host();
    n_cx->com()->nonlocal_dst_port() = cx->com()->nonlocal_dst_port();
    n_cx->com()->nonlocal_dst_resolved(true);
//     n_cx->writebuf()->append(cx->writebuf()->data(),cx->writebuf()->size());
    
    // get rid of it
    cx->socket(0);
    delete cx;
    
    left_sockets.clear();
    ldaadd(n_cx);
    n_cx->on_delay_socket(s);
    
    MitmHostCX *target_cx = new MitmHostCX(n_cx->com()->replicate(), n_cx->com()->nonlocal_dst_host().c_str(), 
                                        string_format("%d",n_cx->com()->nonlocal_dst_port()).c_str()
                                        );
    std::string h;
    std::string p;
    n_cx->name();
    n_cx->com()->resolve_socket_src(n_cx->socket(),&h,&p);
    
    
    n_cx->peer(target_cx);
    target_cx->peer(n_cx);

    target_cx->com()->nonlocal_src(true); //FIXME
    target_cx->com()->nonlocal_src_host() = h;
    target_cx->com()->nonlocal_src_port() = std::stoi(p);
    
    target_cx->connect(false);       
    
    if(ssl) {
        ((SSLCom*)n_cx->com())->upgrade_server_socket(n_cx->socket());
        DEBS_("SocksProxy::socks5_handoff: mark1");        
        
        ((SSLCom*)target_cx->com())->upgrade_client_socket(target_cx->socket());
    }
    
    radd(target_cx);
        
    DIAS_("SocksProxy::socks5_handoff: finished");
}







baseHostCX* MitmSocksProxy::new_cx(int s) {
    auto r = new socksServerCX(com()->replicate(),s);
    return r; 
}

void MitmSocksProxy::on_left_new(baseHostCX* just_accepted_cx) {

    SocksProxy* new_proxy = new SocksProxy(com()->replicate());
    
    // let's add this just_accepted_cx into new_proxy
    std::string h;
    std::string p;
    just_accepted_cx->name();
    just_accepted_cx->com()->resolve_socket_src(just_accepted_cx->socket(),&h,&p);
    
    new_proxy->ladd(just_accepted_cx);    
    
    this->proxies().push_back(new_proxy);
    
    DEBS_("MitmMasterProxy::on_left_new: finished");
}
