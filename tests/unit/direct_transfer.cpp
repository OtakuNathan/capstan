#include <capstan/authority/host.hpp>
#include <capstan/authority/directory.hpp>
#include <capstan/protocol/wire.hpp>
#include "tests/check.hpp"
#include <limits>
#include <thread>
#include <atomic>
using namespace capstan;
static const object_key key{11,22,33};
static grant authority(id epoch,id holder,access_mode mode=access_mode::exclusive) {
    grant g;g.key=key;g.epoch=epoch;g.holder=holder;g.mode=mode;g.grant_id=100+epoch*10+holder;g.secret[0]=static_cast<unsigned char>(g.grant_id);return g;
}
static transition configure(id tx,id previous,std::vector<grant> grants,bool destroy=false) {
    return {key,99,tx,previous,previous+1,destroy,std::move(grants)};
}
static transfer_request move_request(const grant& g,id recipient,id sequence=1) {
    return {g,{authority_origin::holder_transfer,g.holder,sequence},recipient};
}
static bool mint(grant& g) {g.grant_id=100+g.epoch*10+g.holder;g.secret[0]=static_cast<unsigned char>(g.grant_id);return true;}
static void drain(host_authority& host) {
    host_authority::operation op;while(host.start_next(op)) CHECK(host.complete(op.pin)==status::ok);
}
int main() {
    auto a=authority(1,1);auto request=move_request(a,2);transfer_receipt receipt;grant claimed;
    host_authority host(key,99);
    auto boot=configure(1,0,{a});CHECK(host.set_authority(boot)==status::ok);
    // Same numeric transaction sequence, different authenticated namespaces.
    CHECK(host.transfer_exclusive(request,1,mint,receipt)==status::ok);
    CHECK(receipt.installed_epoch==2 && receipt.usability==transfer_usability::current);
    CHECK(host.inspect().epoch==2 && host.inspect().grants.size()==1);
    CHECK(host.admit({a,1,method::version},1).code==status::stale);
    CHECK(host.claim_transfer(request.reference(),1,claimed)==status::forbidden);
    CHECK(host.claim_transfer(request.reference(),3,claimed)==status::forbidden);
    CHECK(host.claim_transfer(request.reference(),2,claimed)==status::ok);
    CHECK(claimed==authority(2,2));
    CHECK(host.admit({claimed,1,method::add_range},2).code==status::ok);drain(host);
    auto never_mint=[](grant&)->bool {std::abort();};
    CHECK(host.transfer_exclusive(request,1,never_mint,receipt)==status::ok);
    CHECK(host.inspect().epoch==2);
    auto conflict=request;conflict.recipient=1;
    CHECK(host.transfer_exclusive(conflict,1,never_mint,receipt)==status::conflict);
    conflict=request;conflict.source.secret[0]^=1;
    CHECK(host.transfer_exclusive(conflict,1,never_mint,receipt)==status::conflict);
    CHECK(host.query_transfer(request.reference(),1).observation==transfer_observation::installed);
    CHECK(host.query_transfer(request.reference(),7).code==status::forbidden);
    CHECK(host.query_transfer(request.reference(),7).observation==transfer_observation::rejected);
    acknowledgement ack;CHECK(host.ack(1,ack)==status::ok && ack.installed==boot);
    CHECK(host.set_authority(configure(2,1,{authority(2,1)}))==status::stale);
    // A later control decision supersedes a historical transfer without erasing it.
    CHECK(host.set_authority(configure(2,2,{authority(3,1)}))==status::ok);
    CHECK(host.query_transfer(request.reference(),1).usability==transfer_usability::superseded);
    CHECK(host.claim_transfer(request.reference(),2,claimed)==status::superseded);
    CHECK(host.transfer_exclusive(request,1,never_mint,receipt)==status::ok && host.inspect().epoch==3);
    CHECK(receipt.usability==transfer_usability::superseded);

    // Queued and executing work are both pins. Transfer has no business pin.
    host_authority active(key,99);CHECK(active.set_authority(boot)==status::ok);
    CHECK(active.admit({a,1,method::summary},1).code==status::ok);
    CHECK(active.admit({a,2,method::version},1).code==status::ok);
    host_authority::operation op;CHECK(active.start_next(op));
    CHECK(active.transfer_exclusive(request,1,mint,receipt)==status::pending);
    CHECK(active.inspect().active==2 && active.inspect().epoch==1);
    CHECK(active.admit({a,3,method::version},1).code==status::closed);
    CHECK(active.claim_transfer(request.reference(),2,claimed)==status::pending);
    CHECK(active.transfer_exclusive(request,1,never_mint,receipt)==status::pending);
    CHECK(active.set_authority(configure(2,1,{authority(2,1)}))==status::busy);
    CHECK(active.complete(op.pin)==status::ok && active.inspect().epoch==1);
    drain(active);CHECK(active.inspect().epoch==2 && active.inspect().active==0);
    CHECK(active.claim_transfer(request.reference(),2,claimed)==status::ok);
    CHECK(active.admit({claimed,1,method::summary},2).code==status::ok);
    CHECK(active.set_authority(configure(2,2,{},true))==status::pending);
    CHECK(active.query_transfer(request.reference(),1).usability==transfer_usability::closed);
    CHECK(active.claim_transfer(request.reference(),2,claimed)==status::closed);
    drain(active);CHECK(active.claim_transfer(request.reference(),2,claimed)==status::dead);

    host_authority rejected(key,99);CHECK(rejected.set_authority(boot)==status::ok);
    CHECK(rejected.query_transfer(request.reference(),1).code==status::not_seen);
    auto bad=request;bad.source.secret[0]^=1;
    CHECK(rejected.transfer_exclusive(bad,1,mint,receipt)==status::bad_grant);
    CHECK(rejected.transfer_exclusive(request,2,mint,receipt)==status::forbidden);
    for(id recipient:{id{0},id{1},id{3},id{77}}) {
        bad=request;bad.recipient=recipient;CHECK(rejected.transfer_exclusive(bad,1,mint,receipt)==status::invalid);
    }
    bad=request;bad.transaction.origin=authority_origin::directory;
    CHECK(rejected.transfer_exclusive(bad,1,mint,receipt)==status::invalid);
    bad=request;bad.transaction.sequence=0;CHECK(rejected.transfer_exclusive(bad,1,mint,receipt)==status::invalid);
    bad=request;bad.transaction.sequence=std::numeric_limits<id>::max();
    CHECK(rejected.transfer_exclusive(bad,1,mint,receipt)==status::invalid);
    bad=request;++bad.source.key.incarnation;CHECK(rejected.transfer_exclusive(bad,1,mint,receipt)==status::wrong_object);
    CHECK(rejected.transfer_exclusive(request,1,[](grant&){return false;},receipt)==status::exhausted);
    CHECK(rejected.inspect().epoch==1 && rejected.inspect().gate==host_authority::phase::open);
    CHECK(rejected.transfer_exclusive(request,1,mint,receipt)==status::ok); // NotSeen was never rollback.
    host_authority shared(key,99);auto read=authority(1,1,access_mode::shared);
    CHECK(shared.set_authority(configure(1,0,{read}))==status::ok);
    CHECK(shared.transfer_exclusive(move_request(read,2),1,mint,receipt)==status::forbidden);
    host_authority full(key,99,64,1);CHECK(full.set_authority(boot)==status::ok);
    CHECK(full.transfer_exclusive(request,1,mint,receipt)==status::exhausted && full.inspect().gate==host_authority::phase::open);

    // Directory observations are monotonic; explicit rejection clears a command,
    // uncertainty does not. A delayed historical ACK cannot republish old grants.
    directory_entry directory(key,99);transition old;
    CHECK(directory.begin_at(0,{a},false,old)==status::ok);
    CHECK(directory.observe(2)==status::ok);
    CHECK(directory.receive({old})==status::stale && directory.epoch()==2);
    std::vector<grant> result;CHECK(directory.take_grants(result)==status::pending);
    transition next;CHECK(directory.begin_at(1,{a},false,next)==status::stale);
    CHECK(directory.begin_at(2,{authority(3,1)},false,next)==status::ok);
    CHECK(directory.reject(next,status::pending)==status::invalid);
    transition retry;CHECK(directory.retry(retry)==status::ok && retry==next);
    CHECK(directory.reject(next,status::stale)==status::stale);
    CHECK(directory.begin_at(3,{authority(4,2)},false,next)==status::ok);
    CHECK(directory.observe(2)==status::stale && directory.epoch()==3);

    // Checked schemas reject wrong versions/truncation without partial output.
    auto encoded=wire::encode_transfer_request(request);transfer_request decoded;
    CHECK(wire::try_decode_transfer_request(encoded,decoded) && decoded==request);
    for(std::size_t n=0;n<encoded.size();++n) CHECK(!wire::try_decode_transfer_request(encoded.substr(0,n),decoded));
    auto historical=host.query_transfer(request.reference(),1);transfer_receipt decoded_receipt;
    CHECK(wire::try_decode_transfer_receipt(wire::encode_transfer_receipt(historical),decoded_receipt));
    CHECK(decoded_receipt.observation==transfer_observation::installed && decoded_receipt.usability==transfer_usability::superseded);
    auto transition_wire=wire::encode_transition(boot);auto legacy=wire::decode(transition_wire);
    CHECK(!wire::try_decode_transition(wire::command(801,legacy.args),next));
    auto snapshot=host.inspect();authority_view view{key,snapshot.epoch,static_cast<unsigned>(snapshot.gate),snapshot.grants},decoded_view;
    CHECK(wire::try_decode_authority_view(wire::encode_authority_view(view),decoded_view) && decoded_view.epoch==3);
    std::cout<<"direct transfer: shared decision domain, drain, history, claim, fencing, bounds and schemas passed\n";
    // Two origins race through the same mutex/slot while a real pin keeps Close
    // visible. Exactly one decision wins; the loser cannot silently rebase.
    for(unsigned iteration=0;iteration<50;++iteration) {
        host_authority racing(key,99);CHECK(racing.set_authority(boot)==status::ok);
        auto pin=racing.admit({a,1,method::summary},1);CHECK(pin.code==status::ok);
        std::atomic<unsigned> ready{0};std::atomic<bool> go{false};
        status direct_status=status::invalid,admin_status=status::invalid;transfer_receipt observed;
        auto start=[&]{++ready;while(!go.load()) std::this_thread::yield();};
        std::thread direct([&]{start();direct_status=racing.transfer_exclusive(request,1,mint,observed);});
        std::thread admin([&]{start();admin_status=racing.set_authority(configure(2,1,{authority(2,1)}));});
        while(ready.load()!=2) std::this_thread::yield();
        go.store(true);direct.join();admin.join();
        CHECK((direct_status==status::pending && admin_status==status::busy) || (direct_status==status::busy && admin_status==status::pending));
        CHECK(racing.inspect().active==1 && racing.inspect().epoch==1);
        CHECK(racing.dispatch_failed(pin.pin)==status::ok);
        CHECK(racing.inspect().epoch==2 && racing.inspect().grants.front().holder==(direct_status==status::pending ? 2 : 1));
    }

}
