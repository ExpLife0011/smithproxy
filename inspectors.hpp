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

#ifndef INSPECTORS_HPP_
#define INSPECTORS_HPP_

#include <basecom.hpp>
#include <tcpcom.hpp>
#include <dns.hpp>
#include <signature.hpp>
#include <apphostcx.hpp>
#include <regex>

#include <sobject.hpp>

class Inspector : public socle::sobject {
public:
    virtual ~Inspector() {}
    virtual void update(AppHostCX* cx) = 0;
    virtual bool l4_prefilter(AppHostCX* cx) = 0;
    virtual bool interested(AppHostCX*) = 0;
    
    inline bool completed() const   { return completed_; }
    inline bool in_progress() const { return in_progress_; }
    inline bool result() const { return result_; }

protected:
    bool completed_ = false;
    void completed(bool b) { completed_ = b; }
    bool in_progress_ = false;
    void in_progress(bool b) { in_progress_ = b; }
    bool result_ = false;
    void result(bool b) { result_ = b; }
    
    int stage = 0;
    
    
    virtual bool ask_destroy() { return false; };
    virtual std::string to_string(int verbosity=INF) { return string_format("%s: in-progress: %d stage: %d completed: %d result: %d",
                                                c_name(),in_progress(), stage, completed(),result()); };
    
                                                
    static std::string remove_redundant_dots(std::string);
    static std::vector<std::string> split(std::string, unsigned char delimiter);
                                                
    DECLARE_C_NAME("Inspector");
};


class DNS_Inspector : public Inspector {
public:
    virtual ~DNS_Inspector() {
        // clear local request cache
        for(auto x: requests_) { if(x.second) {delete x.second; } };
    };  
    virtual void update(AppHostCX* cx);

    virtual bool l4_prefilter(AppHostCX* cx) { return interested(cx); };
    virtual bool interested(AppHostCX*cx);
    
    bool opt_match_id = false;
    bool opt_randomize_id = false;
    
    DNS_Request* find_request(unsigned int r) { auto it = requests_.find(r); if(it == requests_.end()) { return nullptr; } else { return it->second; }  }
    bool validate_response(DNS_Response* ptr);
    bool store(DNS_Response* ptr);
    
    virtual std::string to_string(int verbosity=INF);

    static std::regex wildcard;
private:
    bool is_tcp = false;


    std::unordered_map<unsigned int,DNS_Request*>  requests_;
    int responses_ = 0;
    bool stored_ = false;

    DECLARE_C_NAME("DNS_Inspector");
    DECLARE_LOGGING(name);
};


#endif //INSPECTORS_HPP_