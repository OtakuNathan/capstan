#pragma once
#include <capstan/protocol/types.hpp>
#include <map>
namespace capstan { namespace migration {
// Control-session-scoped, distinct from holder-transfer and directory tx IDs.
// Snapshot length/hash are deliberately absent: these are known only AFTER Drain.
struct ticket {
    id control=0,sequence=0,source_host=0,source_instance=0,target_host=0,target_instance=0,generation=0;
    grant source;
    bool operator==(const ticket& b) const noexcept {
        return control==b.control && sequence==b.sequence && source_host==b.source_host && source_instance==b.source_instance &&
            target_host==b.target_host && target_instance==b.target_instance && generation==b.generation && source==b.source;
    }
};
enum class authority_phase {draining,frozen,fenced,aborting,aborted,installed};
struct authority_record {
    ticket request;authority_phase state=authority_phase::draining;std::vector<grant> result_grants;
    bool operator==(const authority_record& b) const noexcept {return request==b.request && state==b.state && result_grants==b.result_grants;}
};
struct control_record {
    transition target;transfer_request source;
    bool operator==(const control_record& b) const noexcept {return target==b.target && source==b.source;}
};
struct authority_image {
    object_key key;
    id control=0,epoch=0,generation=0;
    std::map<std::pair<id,id>,bool> requests;
    std::map<authority_tx_key,control_record> transactions;
    std::map<id,authority_record> migrations;
    bool operator==(const authority_image& b) const {return key==b.key && control==b.control && epoch==b.epoch && generation==b.generation && requests==b.requests && transactions==b.transactions && migrations==b.migrations;}

};
}}
