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


DEFINE_LOGGING(DNS_Packet);

const char* _unknown = "unknown";
const char* str_a = "A";
const char* str_aaaa = "AAAA";
const char* str_cname = "CNAME";
const char* str_txt= "TXT";
const char* str_opt = "OPT";

dns_cache inspect_dns_cache("DNS cache - global",2000,true);
std::unordered_map<std::string,ptr_cache<std::string,DNS_Response>*> inspect_per_ip_dns_cache;

domain_cache_t domain_cache("DNS 3l domain cache",2000,true);

const char* dns_record_type_str(int a) {
    switch(a) {
        case A: return str_a;
        case AAAA: return str_aaaa;
        case CNAME: return str_cname;
        case TXT: return str_txt;
        case OPT: return str_opt;
        
        default: return _unknown;
    }
}


int load_qname(unsigned char* ptr, unsigned int maxlen, std::string* str_storage=nullptr) {
    int xi = 0;
    int name_i = 0;
    
    DEB_("load_qname:\n%s",hex_dump(ptr,maxlen).c_str());
    
    
    if(ptr[xi] == 0) {
        xi++;
        DEBS_("zero label");
    } 
    else if(ptr[xi] < 0xC0) {
        std::string lab;
        for(; xi < maxlen; ++xi) {
            uint8_t c = ptr[xi];
            if (c == 0) break;
            lab += c;
        }
        ++xi;
        
        if(str_storage) str_storage->assign(lab);
        
        DEB_("plain label: %s",ESC(lab.c_str())); 
    } else {
        uint8_t label = ptr[++xi];
        DEB_("ref label: %d",label);
        ++xi;
    }
    
    return xi;
}


/*
 * returns 0 on OK, >0 if  there are still some bytes to read and -1 on error.
 */
int DNS_Packet::load(buffer* src) {

    loaded_at = ::time(nullptr);
    
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
        
        DIA___("DNS_Packet::load: processing [0x%x] Q: %d, A: %d, AU: %d, AD: %d  (buffer length=%d)",id_, questions_,answers_,authorities_,additionals_,src->size());
        
        unsigned int mem_counter = DNS_HEADER_SZ;
            
        /* QUESTION */
        if(questions_togo > 0) {
            for(; mem_counter < src->size() && questions_togo > 0 && questions_togo > 0;) {
                DEB___("DNS_Packet::load: question loop start: current memory pos: %d",mem_counter);
                DNS_Question question_temp;
                unsigned int field_len = 0;
                bool failure = false;
                
                for(unsigned int cur_mem = mem_counter; cur_mem < src->size() && questions_togo > 0;) {
                    
                    // load next field length
                    field_len = src->get_at<uint8_t>(cur_mem);
                    
                    // 
                    if(cur_mem + field_len >= src->size()) 
                        break;
                    
                    DEB___("DNS_Packet::load: question field_len=%d i=%d buffer_size=%d",field_len,cur_mem,src->size());
                    
                    // last part of the fqdn?
                    if(field_len == 0) {
                        
                        if(cur_mem+5 > src->size()) {
                            DIA___("DNS_Packet::load: incomplete question data in the preamble, index+5 = %d is out of buffer bounds %d",cur_mem+5,src->size());
                            mem_counter = src->size();
                            failure = true;
                            break;
                        }
                        question_temp.rec_type = ntohs(src->get_at<unsigned short>(cur_mem+1));           DEB___("DNS_Packet::load: read 'type' at index %d", cur_mem+1);
                        question_temp.rec_class =  ntohs(src->get_at<unsigned short>(cur_mem+1+2));       DEB___("DNS_Packet::load: read 'class' at index %d", cur_mem+1+2);
                        DEB___("type=%d,class=%d",question_temp.rec_type,question_temp.rec_class);
                        mem_counter += 1+ (2*2);
                        DEB___("DNS_Packet::load: s==0, mem counter changed to: %d (0x%x)",mem_counter,mem_counter);
                        
                        if(questions_togo > 0) {
                            questions_list.push_back(question_temp);
                            questions_togo--;
                        }
                        // we don't currently process authorities and additional records
                        break;
                    } else {
                        if(field_len > src->size()) {
                            DIA___("DNS_Packet::load: incomplete question data in the preamble, field_len %d is out of buffer bounds %d",field_len,src->size());
                            mem_counter = src->size();
                            failure = true;
                            break;
                        }
                        if(cur_mem+1 >= src->size()) {
                            DIA___("DNS_Packet::load: incomplete question data in the preamble, cur_mem+1 = %d is out of buffer bounds %d",cur_mem+1,src->size());
                            mem_counter = src->size();
                            failure = true;
                            break;
                            
                        }
                        
                        if(question_temp.rec_str.size() != 0) question_temp.rec_str += ".";
                        
                        std::string t((const char*)&src->data()[cur_mem+1],field_len);
                        
                        cur_mem += (field_len + 1);
                        mem_counter += (field_len+1);
                        question_temp.rec_str += t;
                    }
                }
                
                if(!failure) {
                    DIA___("DNS_Packet::load: OK question[%d]: name: %s, type: %s, class: %d",questions_togo, question_temp.rec_str.c_str(),
                                        dns_record_type_str(question_temp.rec_type),question_temp.rec_class);
                } else {
                    DIA___("DNS_Packet::load: FAILED question[%d]",questions_togo);
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
                answer_ttl_idx.push_back(i+6);
                answer_temp.ttl_ = ntohl(src->get_at<uint32_t>(i+6));
                answer_temp.datalen_ = ntohs(src->get_at<uint32_t>(i+10)); 
                if(answer_temp.datalen_ > 0)
                    answer_temp.data_.append(src->view(i+12,answer_temp.datalen_));
                int inc = 12 + answer_temp.datalen_;

                mem_counter += inc ;
                i += inc;
                
                DIA___("DNS_Packet::load: answer[%d]: name: %d, type: %d, class: %d, ttl: %d, len: %d, buflen: %d",answers_togo,
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
                
                DIA___("DNS_Packet::load: authorities[%d]: name: %d, type: %d, class: %d, ttl: %d, len: %d, buflen: %d",authorities_togo,
                                    answer_temp.name_,answer_temp.type_,answer_temp.class_,answer_temp.ttl_,answer_temp.datalen_,answer_temp.data_.size()  );
                authorities_list.push_back(answer_temp);
                authorities_togo--;
            }
        }
        
        /* ADDITIONALS */
        if(additionals_togo > 0) {
            for(unsigned int i = mem_counter; i < src->size() && additionals_togo > 0; ) {
  
                
                int xi = load_qname(src->data()+i,src->size()-i);
                
                //unsigned short pre_type = ntohs(src->get_at<unsigned short>(i+1));
                unsigned short pre_type = ntohs(src->get_at<unsigned short>(i+xi));                
                i += (xi + 2);
                
               
                DEB_("DNS inspect: packet pre_type = %s(%d)",dns_record_type_str(pre_type), pre_type);
                
                if(pre_type == OPT) {
                    //THIS IS DNSSEC ADDITIONALS - we need to handle it better, now remove                
                
                    DNS_DnssecAdditionalInfo answer_temp;
                    //answer_temp.name_ = src->get_at<uint8_t>(i);
                    answer_temp.opt_ = pre_type;
                    answer_temp.udp_size_ = ntohs(src->get_at<unsigned short>(i)); i+=2;
                    answer_temp.higher_bits_rcode_ = src->get_at<uint8_t>(i);      i+=1;
                    answer_temp.edns0_version_ = src->get_at<uint8_t>(i);          i+=1; 
                    answer_temp.z_ = ntohs(src->get_at<uint16_t>(i));              i+=2;
                    answer_temp.datalen_ = ntohs(src->get_at<uint16_t>(i));        i+=2;
                    
                    
                    if(i + answer_temp.datalen_ <= src->size()) {
                        
                        if(answer_temp.datalen_ > 0) {
                            answer_temp.data_.append(src->view(i,answer_temp.datalen_));
                            i += answer_temp.datalen_;
                        }

                        DIA___("DNS_Packet::load: additional DNSSEC info[%d]: name: %d, opt: %d, udp: %d, hb_rcode: %d, edns0: %d, z: %d, len %d, buflen: %d", additionals_togo,
                                            answer_temp.name_,answer_temp.opt_,answer_temp.udp_size_,answer_temp.higher_bits_rcode_,answer_temp.edns0_version_,answer_temp.z_,answer_temp.datalen_,answer_temp.data_.size()  );
                        
                        mem_counter = i;
                        
                        
                        additionals_togo--;
                    } else {
                        mem_counter = src->size();
                        i = src->size();
                    }
                }
                else if (pre_type == A || pre_type == AAAA || pre_type == TXT) {
                    DNS_Answer answer_temp;
                    //answer_temp.name_ = ntohs(src->get_at<unsigned short>(i));
                    
                    
                    
                    answer_temp.type_ = pre_type;
                    answer_temp.class_ = ntohs(src->get_at<unsigned short>(i));    i+=2;
                    answer_ttl_idx.push_back(i);
                    answer_temp.ttl_ = ntohl(src->get_at<uint32_t>(i));            i+=4;
                    answer_temp.datalen_ = ntohs(src->get_at<uint16_t>(i));        i+=2;

                    if(answer_temp.datalen_ > 0) {
                        answer_temp.data_.append(src->view(i,answer_temp.datalen_));
                        i += answer_temp.datalen_;
                    }

                    mem_counter = i;
                    
                    DEB___("mem_counter: %d, size %d",i, src->size());
                    
                    DIA___("DNS_Packet::load: additional answer[%d]: name: %d, type: %d, class: %d, ttl: %d, len: %d, buflen: %d",additionals_togo,
                                        answer_temp.name_,answer_temp.type_,answer_temp.class_,answer_temp.ttl_,answer_temp.datalen_,answer_temp.data_.size()  );
                    additionals_list.push_back(answer_temp);
                    additionals_togo--;
                }
                else {
                    
                    WARS___("unsupported additional message, skipping the rest of message.");
                    
                    mem_counter = src->size();
                    i = mem_counter;
                    break;
                }
            }
            
            //fix additionals number, for case we omitted some
            additionals_ = additionals_list.size();
        }
        
        if(questions_togo == 0 && answers_togo == 0 && authorities_togo == 0 /*&& additionals_togo == 0*/) {
            DIA___("DNS_Packet::load: finished mem_counter=%d buffer_size=%d",mem_counter,src->size());
            if(mem_counter == src->size()) {
                return 0;
            }
            return mem_counter;
        }
    }
    return -1;
};


std::string DNS_Packet::to_string(int verbosity) {
    std::string r = string_format("%s: id: %d, type 0x%x [ ",c_name(),id_,flags_);
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
        if(x->type_ == A || x->type_ == AAAA) {
            ret += " " + x->ip();
        }
    }
    
    return ret;
}

std::vector< CidrAddress*> DNS_Packet::get_a_anwsers() {
    std::vector<CidrAddress*> ret;
    
    for(auto x = answers_list.begin(); x != answers_list.end(); ++x) {
        if(x->type_ == A || x->type_ == AAAA) {
            ret.push_back(new CidrAddress(x->cidr()));
        }
    }
    
    return ret;
}

