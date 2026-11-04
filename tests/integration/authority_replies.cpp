#include "src/apps/authority_replies.hpp"
#include "tests/check.hpp"
#include <memory>
#include <cstdlib>
#include <new>
#if CAPSTAN_HAS_EXCEPTIONS
namespace { int fail_after=-1; }
void* operator new(std::size_t size) {
    if(fail_after>=0 && fail_after--==0) {fail_after=-1;throw std::bad_alloc();}
    if(auto* p=std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {return ::operator new(size);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
#endif
using namespace capstan;
int main() {
    object_key key{1,2,3};
    host_authority host(key,4);
    app::authority_replies replies(host);
    grant g;g.key=key;g.epoch=1;g.grant_id=1;g.holder=1;
    g.mode=access_mode::exclusive;g.secret[0]=1;
    transition initial{key,4,1,0,1,false,{g}};
#if CAPSTAN_HAS_EXCEPTIONS
    // Fail every allocation in reply preparation and authority staging. Either
    // nothing commits and one Unknown is delivered, or the ACK is retained.
    bool reached_success=false;
    for(int allocation=0;allocation<1000 && !reached_success;++allocation) {
        host_authority candidate(key,4);app::authority_replies candidate_replies(candidate);
        unsigned completed=0;outcome observed=outcome::not_submitted;
        io_completion done=[&](io_result r){++completed;observed=r.state;};
        fail_after=allocation;
        auto code=candidate_replies.set(initial,std::move(done));
        fail_after=-1;
        if(code==status::ok) {
            CHECK(completed==0 && candidate_replies.size()==1 && candidate.inspect().epoch==1);
            candidate_replies.flush();CHECK(completed==1 && observed==outcome::completed);
            reached_success=true;
        } else {
            CHECK(completed==1 && observed==outcome::unknown);
            CHECK(candidate_replies.size()==0 && candidate.inspect().epoch==0);
        }
    }
    CHECK(reached_success);
#endif
    unsigned callbacks=0;
    auto check=[&](transition target) {
        return [&,target](io_result result) {
            ++callbacks;
            CHECK(result.state==outcome::completed);
            auto response=wire::decode_reply(result.payload);
            CHECK(response.code==status::ok && response.values.size()==1);
            CHECK(wire::decode_transition(wire::binary(wire::string(response.values[0])))==target);
            CHECK(host.inspect().active==0);
        };
    };
    CHECK(replies.set(initial,check(initial))==status::ok);
    replies.flush();CHECK(callbacks==1);
    auto first=host.admit({g,1,method::add_range},1);
    auto second=host.admit({g,2,method::add_range},1);
    CHECK(first.code==status::ok && second.code==status::ok);
    host_authority::operation operation;
    CHECK(host.start_next(operation));
    transition next{key,4,2,1,2,false,{}};
    CHECK(replies.set(next,check(next))==status::pending);
    CHECK(replies.set(next,check(next))==status::pending);
    auto conflict=next;conflict.destroy=true;
    CHECK(replies.set(conflict,[](io_result r){CHECK(wire::decode_reply(r.payload).code==status::conflict);})==status::conflict);
    CHECK(host.admit({g,3,method::version},1).code==status::closed);
    replies.flush();CHECK(callbacks==1);
    CHECK(host.complete(first.pin)==status::ok);
    replies.flush();CHECK(callbacks==1 && host.inspect().epoch==1);
    CHECK(host.start_next(operation));
    CHECK(host.complete(second.pin)==status::ok);
    replies.flush();CHECK(callbacks==3 && host.inspect().epoch==2);
    replies.flush();CHECK(callbacks==3);
    // Lost ACK / weak session expiration has no effect on installed history.
    auto session=std::make_shared<int>(0);std::weak_ptr<int> weak=session;
    transition third{key,4,3,2,3,false,{}};
    CHECK(replies.set(third,[weak](io_result){CHECK(weak.expired());})==status::ok);
    session.reset();replies.flush();
    acknowledgement ack;CHECK(host.ack(3,ack)==status::ok && ack.installed==third);
    CHECK(replies.set(third,check(third))==status::ok);replies.flush();
    CHECK(callbacks==4 && host.inspect().epoch==3);
    CHECK(replies.set(initial,check(initial))==status::ok);replies.flush();
    CHECK(callbacks==5 && host.inspect().epoch==3);
    // Callback reentrancy sees detached storage, and throwing completion is
    // invoked once without starving other replies in the same batch.
    transition fourth{key,4,4,3,4,false,{}};
    CHECK(replies.set(fourth,[&](io_result){
        CHECK(replies.size()==0);
        CHECK(replies.set(fourth,check(fourth))==status::ok);
        replies.flush();
    })==status::ok);
#if CAPSTAN_HAS_EXCEPTIONS
    CHECK(replies.set(fourth,[](io_result){throw 7;})==status::ok);
#endif
    CHECK(replies.set(fourth,check(fourth))==status::ok);
    replies.flush();CHECK(callbacks==7 && replies.size()==0);
    // Bound duplicate waiters, then rollback a never-dispatched queued pin:
    // only the final rollback allows tombstone ACKs.
    g.epoch=5;transition fifth{key,4,5,4,5,false,{g}};
    CHECK(replies.set(fifth,check(fifth))==status::ok);replies.flush();
    auto queued=host.admit({g,4,method::version},1);CHECK(queued.code==status::ok);
    transition destroy{key,4,6,5,6,true,{}};
    for(unsigned i=0;i<64;++i) CHECK(replies.set(destroy,check(destroy))==status::pending);
    CHECK(replies.set(destroy,[](io_result r){CHECK(wire::decode_reply(r.payload).code==status::busy);})==status::busy);
    CHECK(callbacks==8 && host.inspect().epoch==5);
    replies.flush();CHECK(callbacks==8);
    CHECK(host.dispatch_failed(queued.pin)==status::ok);
    replies.flush();CHECK(callbacks==72 && host.inspect().gate==host_authority::phase::dead);
    CHECK(host.ack(6,ack)==status::ok && ack.installed==destroy);
}
