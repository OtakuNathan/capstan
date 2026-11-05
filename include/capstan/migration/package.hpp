#pragma once
#include <capstan/migration/authority_wire.hpp>
#include <capstan/migration/transfer.hpp>
namespace capstan { namespace migration {
// Views are used only while the caller retains the immutable package buffer.
// Decode directly into owning native words/history; no extra Dataset byte copy.
inline status decode_package(const buffer& bytes,dataset_snapshot& data,authority_image& history) {
    reader in(bytes);std::uint64_t magic,schema,length;
    if(!in.get(4,magic) || magic!=0x474d5043u || !in.get(4,schema) || schema!=1 ||
       !in.get(8,length) || length>in.remaining()) return status::invalid;
    dataset_snapshot staged_data;authority_image staged_history;
    auto code=decode(in.current(),static_cast<std::size_t>(length),staged_data);
    if(code!=status::ok) return code;
    if(!in.skip(static_cast<std::size_t>(length)) || !in.get(8,length) || length!=in.remaining()) return status::invalid;
    code=decode_authority_image(in.current(),static_cast<std::size_t>(length),staged_history);
    if(code!=status::ok) return code;
    data=std::move(staged_data);history=std::move(staged_history);return status::ok;
}
inline bool matches(const manifest& m,const ticket& t) noexcept {
    return m.transaction==transaction_id{t.control,t.sequence} && m.object==t.source.key && m.source_host==t.source_host &&
        m.source_instance==t.source_instance && m.target_host==t.target_host && m.target_instance==t.target_instance && m.generation==t.generation;
}
}}
