#include <capstan/access/transfer.hpp>
#include <capstan/migration/wire.hpp>
#include "tests/check.hpp"
#include <atomic>
#include <cstdlib>
#include <new>
// Sweep allocations rather than depending on removed pointer-holder/result objects.
static std::atomic<unsigned> fail_after{0};
static bool injected=false;
void* operator new(std::size_t size) {
    auto remaining=fail_after.load();
    if(remaining && fail_after.fetch_sub(1)==1){
        injected=true;
#if CAPSTAN_HAS_EXCEPTIONS
        throw std::bad_alloc();
#else
        std::abort(); // Standard allocator OOM is process-fatal without exceptions.
#endif
    }
    if(void* p=std::malloc(size?size:1))return p;
#if CAPSTAN_HAS_EXCEPTIONS
    throw std::bad_alloc();
#else
    std::abort();
#endif
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
struct never_submit:capstan::io_endpoint {
    int submitted=0;
    bool submit(capstan::bytes,capstan::io_completion) noexcept override {++submitted;return false;}
};
struct pending_endpoint:capstan::io_endpoint {
    capstan::io_completion pending;
    bool submit(capstan::bytes,capstan::io_completion cb) noexcept override {
        pending=std::move(cb);return true;
    }
};
int main(){
    // The borrowed chunk decoder must work with allocation disabled, including
    // malformed input. Its view is consumed while the owning frame is alive.
    auto chunk_frame=capstan::wire::encode_reply(capstan::outcome::completed,capstan::status::ok,
        {capstan::wire::u(0),capstan::wire::value::string(std::string(48*1024,'x'))});
    capstan::migration::chunk_reply_view chunk;
    injected=false;fail_after=1;
    const bool decoded_chunk=capstan::migration::try_decode_chunk_reply(chunk_frame,chunk);
    fail_after=0;CHECK(decoded_chunk && !injected && chunk.size==48*1024 && chunk.data[0]=='x');
    chunk_frame.pop_back();
    fail_after=1;
    const bool malformed_chunk=capstan::migration::try_decode_chunk_reply(chunk_frame,chunk);
    fail_after=0;CHECK(!malformed_chunk && !injected);
    never_submit endpoint;
#if CAPSTAN_HAS_EXCEPTIONS
    unsigned failures=0;
    for(unsigned n=1;n<32;++n){
        capstan::bytes request{1,2,3};int callbacks=0;
        auto authority=std::make_shared<int>(1);std::weak_ptr<int> weak=authority;
        capstan::io_completion done=[&](capstan::io_result r){CHECK(r.state==capstan::outcome::not_submitted);CHECK(weak.expired());++callbacks;};
        const int submitted=endpoint.submitted;
        injected=false;fail_after=n;
        capstan::start_io(endpoint,std::move(request),std::move(done),std::move(authority));
        fail_after=0;
        CHECK(callbacks==1);
        if(!injected)break;
        ++failures;CHECK(endpoint.submitted==submitted);
    }
    CHECK(failures>=2);
    capstan::grant g{{4,5,6},1,2,1,capstan::access_mode::exclusive,{}};g.secret[0]=4;
    capstan::status code;auto source=capstan::exclusive_capability::try_create(g,code);CHECK(source);
    auto typed_grant=g;typed_grant.grant_id+=1;
    auto typed_owner=std::make_shared<capstan::exclusive_capability>(typed_grant);
    capstan::async_exclusive_capability typed(typed_owner,endpoint);
    unsigned typed_failures=0;
    for(unsigned n=1;n<32;++n){
        unsigned callbacks=0;injected=false;
        {
            auto call=typed.add_range(0,1,1);
            fail_after=n;
            try {std::move(call).start([&](dynabridge::async_result<capstan::id> result) noexcept {
                fail_after=0;
                CHECK(!result.has_value());
                CHECK(bool(typed_owner->try_add_range(0,1,1)));
                ++callbacks;
            });} catch(const std::bad_alloc&) {CHECK(callbacks==0);}
        }
        fail_after=0;
        CHECK(callbacks<=1 && typed_owner->try_add_range(0,1,1));
        if(!injected){CHECK(callbacks==1);break;}
        ++typed_failures;
    }
    CHECK(typed_failures>=2);
    pending_endpoint responding;
    capstan::async_exclusive_capability responding_view(typed_owner,responding);
    unsigned decode_callbacks=0;
    std::move(responding_view.add_range(0,1,1)).start([&](dynabridge::async_result<capstan::id> result) noexcept {
        fail_after=0;capstan::outcome state;capstan::status status;
        CHECK(!result.has_value() && capstan::decode_rpc_error(result.error,state,status));
        CHECK(state==capstan::outcome::unknown);
        CHECK(bool(typed_owner->try_add_range(0,1,1)));++decode_callbacks;
    });
    auto response=capstan::wire::encode_reply(capstan::outcome::completed,capstan::status::ok,{capstan::wire::u(1)});
    auto respond=std::exchange(responding.pending,nullptr);
    injected=false;fail_after=1;
    respond({capstan::outcome::completed,std::move(response)});
    fail_after=0;CHECK(injected && decode_callbacks==1);
    auto transfer=capstan::transfer_plan::prepare(*source,2,1,code);CHECK(transfer);
    for(unsigned n=1;n<16;++n){
        injected=false;fail_after=n;
        try {transfer->start(endpoint,[](capstan::io_result r){CHECK(r.state==capstan::outcome::not_submitted);});}
        catch(const std::bad_alloc&) {} // Preparation can fail before start_io owns completion.
        fail_after=0;
        if(!injected){CHECK(transfer->sealed());break;}
        CHECK(!transfer->sealed()&&source->try_version());
    }
#else
    CHECK(rejects<std::bad_alloc>([&]{
        capstan::bytes request{1};capstan::io_completion done=[](capstan::io_result){};
        fail_after=1;capstan::start_io(endpoint,std::move(request),std::move(done));
    }));
    CHECK(endpoint.submitted==0);
#endif
    std::cout<<"allocation: pre-submission failure preserves authority; no-exception allocator OOM is fatal\n";
}
