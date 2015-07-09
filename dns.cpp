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

#include <arpa/inet.h>


#include <dns.hpp>
#include <logger.hpp>


dns_cache inspect_dns_cache(200,true);
std::unordered_map<std::string,ptr_cache<std::string,DNS_Response>*> inspect_per_ip_dns_cache;

const char* dns_record_type_str(int a) {
    switch(a) {
        case A: return str_a;
        case AAAA: return str_aaaa;
        case CNAME: return str_cname;
        
        default: return _unknown;
    }
}

/*
 * returns 0 on OK, >0 if  there are still some bytes to read and -1 on error.
 */
int DNS_Packet::load(buffer* src) {

    if(src->size() > DNS_HEADER_SZ) {
        id_ = ntohs(src->get_at<unsigned short>(0));
        flags_ = ntohs(src->get_at<unsigned short>(2));
        questions_ = ntohs(src->get_at<unsigned short>(4));
        answers_ = ntohs(src->get_at<unsigned short>(6));
        authorities_ = ntohs(src->get_at<unsigned short>(8));
        additionals_ = ntohs(src->get_at<unsigned short>(10));
        //std::string    rq((const char*)&src->data()[DNS_HEADER_SZ]);
        
        uint16_t questions_togo = questions_;
        uint16_t answers_togo = answers_;
        uint16_t authorities_togo = authorities_;
        uint16_t additionals_togo = additionals_;
        
        DIA_("DNS_Packet::load: [0x%x] Q: %d, A: %d, AU: %d, AD: %d  (buffer length=%d)",id_, questions_,answers_,authorities_,additionals_,src->size());
        
        unsigned int mem_counter = DNS_HEADER_SZ;
            
        /* QUESTION */
        if(questions_togo > 0) {
            for(; mem_counter < src->size() && questions_togo > 0 && questions_togo > 0;) {
                DNS_Question question_temp;
                int s = 0;
                bool failure = false;
                
                for(unsigned int i = mem_counter; i < src->size() && questions_togo > 0;) {
                    s = src->get_at<uint8_t>(i);
                    if(s + i >= src->size()) break;
                    DEB_("DNS_Packet::load: question s=%d i=%d buffer_size=%d",s,i,src->size());
                    
                    // last part of the fqdn?
                    if(s == 0) {
                        
                        if(i+5 >= src->size()) {
                            DIA_("DNS_Packet::load: incomplete question data in the preamble, index+5 = %d is out of buffer bounds %d",i+5,src->size());
                            mem_counter = src->size();
                            failure = true;
                            break;
                        }
                        question_temp.rec_type = ntohs(src->get_at<unsigned short>(i+1));           DEB_("DNS_Packet::load: read 'type' at index %d", i+1);
                        question_temp.rec_class =  ntohs(src->get_at<unsigned short>(i+1+2));       DEB_("DNS_Packet::load: read 'class' at index %d", i+1+2);
                        DEB_("type=%d,class=%d",question_temp.rec_type,question_temp.rec_class);
                        mem_counter += 1+(2*2);
                        DEB_("DNS_Packet::load: s==0, mem counter changed to: %d (0x%x)",mem_counter,mem_counter);
                        
                        if(questions_togo > 0) {
                            questions_list.push_back(question_temp);
                            questions_togo--;
                        }
                        // we don't currently process authorities and additional records
                        break;
                    } else {
                        if(s > src->size()) {
                            DIA_("DNS_Packet::load: incomplete question data in the preamble, s %d is out of buffer bounds %d",s,src->size());
                            mem_counter = src->size();
                            failure = true;
                            break;
                        }
                        if(i+1 >= src->size()) {
                            DIA_("DNS_Packet::load: incomplete question data in the preamble, index+1 = %d is out of buffer bounds %d",i+1,src->size());
                            mem_counter = src->size();
                            failure = true;
                            break;
                            
                        }
                        
                        if(question_temp.rec_str.size() != 0) question_temp.rec_str += ".";
                        
                        std::string t((const char*)&src->data()[i+1],s);
                        
                        i += (s + 1);
                        mem_counter += (s+1);
                        question_temp.rec_str += t;
                    }
                }
                
                if(!failure) {
                    DIA_("DNS_Packet::load: question[%d]: name: %s, type: %s, class: %d",questions_togo, question_temp.rec_str.c_str(),
                                        dns_record_type_str(question_temp.rec_type),question_temp.rec_class);
                } else {
                    break;
                }
            }
        }
            
        /* ANSWER section */
        if(answers_togo > 0) {
            for(unsigned int i = mem_counter; i < src->size() && answers_togo > 0; ) {
                DNS_Answer answer_temp;
                answer_temp.name_ = ntohs(src->get_at<unsigned short>(i));
                answer_temp.type_ = ntohs(src->get_at<unsigned short>(i+2));
                answer_temp.class_ = ntohs(src->get_at<unsigned short>(i+4));
                answer_temp.ttl_ = ntohl(src->get_at<uint32_t>(i+6));
                answer_temp.datalen_ = ntohs(src->get_at<uint32_t>(i+10)); 
                if(answer_temp.datalen_ > 0)
                    answer_temp.data_.append(src->view(i+12,answer_temp.datalen_));
                int inc = 12 + answer_temp.datalen_;

                mem_counter += inc ;
                i += inc;
                
                DIA_("DNS_Packet::load: answer[%d]: name: %d, type: %d, class: %d, ttl: %d, len: %d, buflen: %d",answers_togo,
                                    answer_temp.name_,answer_temp.type_,answer_temp.class_,answer_temp.ttl_,answer_temp.datalen_,answer_temp.data_.size()  );
                answers_list.push_back(answer_temp);
                answers_togo--;
            }
        }
        
        /* AUTHORITIES sectin */
        if(authorities_togo > 0) {
            for(unsigned int i = mem_counter; i < src->size() && authorities_togo > 0; ) {
                DNS_Answer answer_temp;
                answer_temp.name_ = ntohs(src->get_at<unsigned short>(i));
                answer_temp.type_ = ntohs(src->get_at<unsigned short>(i+2));
                answer_temp.class_ = ntohs(src->get_at<unsigned short>(i+4));
                answer_temp.ttl_ = ntohl(src->get_at<uint32_t>(i+6));
                answer_temp.datalen_ = ntohs(src->get_at<uint32_t>(i+10)); 
                if(answer_temp.datalen_ > 0)
                    answer_temp.data_.append(src->view(i+12,answer_temp.datalen_));
                int inc = 12 + answer_temp.datalen_;

                mem_counter += inc ;
                i += inc;
                
                DIA_("DNS_Packet::load: authorities[%d]: name: %d, type: %d, class: %d, ttl: %d, len: %d, buflen: %d",authorities_togo,
                                    answer_temp.name_,answer_temp.type_,answer_temp.class_,answer_temp.ttl_,answer_temp.datalen_,answer_temp.data_.size()  );
                authorities_list.push_back(answer_temp);
                authorities_togo--;
            }
        }
        
        /* ADDITIONALS */
        if(additionals_togo > 0) {
            for(unsigned int i = mem_counter; i < src->size() && additionals_togo > 0; ) {
                DNS_AdditionalInfo answer_temp;
                answer_temp.name_ = src->get_at<uint8_t>(i);
                answer_temp.opt_ = ntohs(src->get_at<unsigned short>(i+1));
                answer_temp.udp_size_ = ntohs(src->get_at<unsigned short>(i+3));
                answer_temp.higher_bits_rcode_ = src->get_at<uint8_t>(i+5);
                answer_temp.edns0_version_ = src->get_at<uint8_t>(i+6);
                answer_temp.z_ = ntohs(src->get_at<uint16_t>(i+7)); 
                answer_temp.datalen_ = ntohs(src->get_at<uint16_t>(i+9)); 
                if(answer_temp.datalen_ > 0)
                    answer_temp.data_.append(src->view(i+11,answer_temp.datalen_));
                int inc = 11 + answer_temp.datalen_;

                mem_counter += inc ;
                i += inc;
                
                DIA_("DNS_Packet::load: additionals[%d]: name: %d, opt: %d, udp: %d, hb_rcode: %d, edns0: %d, z: %d, len %d, buflen: %d", additionals_togo,
                                    answer_temp.name_,answer_temp.opt_,answer_temp.udp_size_,answer_temp.higher_bits_rcode_,answer_temp.edns0_version_,answer_temp.z_,answer_temp.datalen_,answer_temp.data_.size()  );
                additionals_list.push_back(answer_temp);
                additionals_togo--;
            }
        }
        
        if(questions_togo == 0 && answers_togo == 0 && authorities_togo == 0 && additionals_togo == 0) {
            DIA_("DNS_Packet::load: finished mem_counter=%d buffer_size=%d",mem_counter,src->size());
            if(mem_counter == src->size()) {
                return 0;
            }
            return mem_counter;
        }
    }
    return -1;
};


std::string DNS_Packet::hr() const {
    std::string r = string_format("id: %d, type 0x%x [ ",id_,flags_);
    for(auto x = questions_list.begin(); x != questions_list.end(); ++x) {
        r += x->hr();
        if(x+1 != questions_list.end()) {
            r += ",";
        }
    }
    r+=" ]";
    
    if(answers_list.size() > 0) {
        r += " -> [";
        for(auto x = answers_list.begin(); x != answers_list.end(); ++x) {
            r += x->hr();
            if(x+1 != answers_list.end()) {
                r += " | ";
            }
        }
        r+=" ]";
    }
    
    return r;

}


std::string DNS_Packet::answer_str() const {
    std::string ret = "";
    
    for(auto x = answers_list.begin(); x != answers_list.end(); ++x) {
        if(x->type_ == A) {
            ret += " " + x->ip();
        }
    }
    
    return ret;
}
