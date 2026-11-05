#include <capstan/authority/host.hpp>
#include <capstan/migration/authority_wire.hpp>
#include "tests/check.hpp"
using namespace capstan;
namespace m=capstan::migration;
grant make_grant(object_key key,id epoch,id nonce) {
    grant g;g.key=key;g.epoch=epoch;g.holder=1;g.grant_id=nonce;g.mode=access_mode::exclusive;g.secret[0]=static_cast<unsigned char>(nonce);return g;
}
m::ticket ticket(grant source,id sequence,id generation=1,id from=1,id to=2) {
    m::ticket t;t.control=7;t.sequence=sequence;t.source_host=from;t.target_host=to;t.source_instance=from+10;t.target_instance=to+10;t.generation=generation;t.source=source;return t;
}
int main() {
    object_key key{1,2,3};auto initial=make_grant(key,1,1);
    host_authority a(key,7),b(key,7);
    CHECK(a.set_authority({key,7,1,0,1,false,{initial}})==status::ok);
    auto first=a.admit({initial,41,method::add_range},1),second=a.admit({initial,42,method::summary},1);
    CHECK(first.code==status::ok && second.code==status::ok);
    host_authority::operation op;CHECK(a.start_next(op) && op.pin==first.pin);
    auto t=ticket(initial,1);
    CHECK(a.freeze_for_migration(t)==status::pending);
    CHECK(a.admit({initial,43,method::version},1).code==status::closed);
    CHECK(a.set_authority({key,7,2,1,2,false,{make_grant(key,2,2)}})==status::busy);
    m::authority_image image;CHECK(a.export_authority(t,image)==status::pending);
    CHECK(a.complete(first.pin)==status::ok);CHECK(a.export_authority(t,image)==status::pending);
    CHECK(a.start_next(op) && op.pin==second.pin);CHECK(a.complete(second.pin)==status::ok);
    CHECK(a.freeze_for_migration(t)==status::ok);CHECK(a.export_authority(t,image)==status::ok);
    m::buffer serialized;CHECK(m::encode_authority_image(image,serialized)==status::ok);
    m::authority_image restored;CHECK(m::decode_authority_image(serialized,restored)==status::ok && restored==image);
    for(std::size_t i=0;i<serialized.size();++i) {
        m::buffer truncated(serialized.begin(),serialized.begin()+i);
        CHECK(m::decode_authority_image(truncated,restored)==status::invalid && restored==image);
    }
    CHECK(m::decode_authority_image(serialized,restored,0)==status::invalid);
    CHECK(image.requests.size()==2 && image.transactions.size()==1);
    CHECK(a.inspect().gate==host_authority::phase::closing);
    auto next=make_grant(key,2,2);
    CHECK(b.reserve_import(t)==status::ok && b.reserve_import(t)==status::ok);
    CHECK(b.activate_import(t)==status::pending);
    CHECK(b.set_authority({key,7,9,0,1,false,{initial}})==status::busy);
    CHECK(b.stage_import(t,image,next)==status::ok);
    auto changed=image;changed.requests.clear();CHECK(b.stage_import(t,changed,next)==status::conflict);
    CHECK(b.admit({next,99,method::version},1).code==status::closed);
    CHECK(a.fence_migration(t)==status::ok);CHECK(a.abort_migration(t,next)==status::conflict);
    CHECK(b.activate_import(t)==status::ok);CHECK(b.activate_import(t)==status::ok);
    auto conflicting_successor=next;++conflicting_successor.grant_id;
    CHECK(b.stage_import(t,image,conflicting_successor)==status::conflict);
    CHECK(b.placement_generation()==2 && b.inspect().epoch==2);
    CHECK(a.admit({initial,99,method::version},1).code==status::closed);
    CHECK(b.admit({initial,99,method::version},1).code==status::stale);
    CHECK(b.admit({next,41,method::add_range},1).code==status::duplicate);
    CHECK(b.admit({next,42,method::summary},1).code==status::duplicate);
    // Return to the original Host, retaining all old transaction/request evidence.
    auto back=ticket(next,2,2,2,1);CHECK(b.freeze_for_migration(back)==status::ok);
    m::authority_image returning;CHECK(b.export_authority(back,returning)==status::ok);
    auto third=make_grant(key,3,3);CHECK(a.stage_import(back,returning,third)==status::ok);
    CHECK(b.fence_migration(back)==status::ok);CHECK(a.activate_import(back)==status::ok);
    CHECK(a.placement_generation()==3 && a.admit({third,41,method::add_range},1).code==status::duplicate);
    CHECK(a.fence_migration(t)==status::conflict); // Imported history cannot fence current placement.
    CHECK(a.set_authority({key,7,1,0,1,false,{initial}})==status::ok); // Historical ACK only.
    CHECK(a.inspect().epoch==3);
    // Abort with queued work restores only after Drain and with fresh credentials.
    auto active=a.admit({third,55,method::version},1);CHECK(active.code==status::ok);
    auto aborting=ticket(third,3,3);CHECK(a.freeze_for_migration(aborting)==status::pending);
    auto fourth=make_grant(key,4,4);CHECK(a.abort_migration(aborting,fourth)==status::pending);
    CHECK(a.inspect().gate==host_authority::phase::closing);CHECK(a.start_next(op));CHECK(a.complete(op.pin)==status::ok);
    CHECK(a.inspect().epoch==4 && a.inspect().gate==host_authority::phase::open);
    CHECK(a.freeze_for_migration(aborting)==status::closed);
    CHECK(a.abort_migration(aborting,fourth)==status::ok);
    auto changed_abort=fourth;++changed_abort.grant_id;
    CHECK(a.abort_migration(aborting,changed_abort)==status::conflict);
    CHECK(a.admit({third,60,method::version},1).code==status::stale);
    // Abort-before-Begin tombstones prevent delayed preparation.
    host_authority empty(key,7);CHECK(empty.abort_import(t)==status::ok);
    CHECK(empty.stage_import(t,image,next)==status::conflict);
    // Destination abort restores its prior fenced placement, never Open authority.
    auto retry=ticket(fourth,4,3);CHECK(a.freeze_for_migration(retry)==status::ok);
    m::authority_image newer;CHECK(a.export_authority(retry,newer)==status::ok);
    auto fifth=make_grant(key,5,5);CHECK(b.stage_import(retry,newer,fifth)==status::ok);
    CHECK(b.abort_import(retry)==status::ok && b.inspect().gate==host_authority::phase::closing);
    CHECK(b.activate_import(retry)==status::conflict);
    CHECK(a.abort_migration(retry,fifth)==status::ok && a.inspect().epoch==5);
    // A frozen object's snapshot reader is still an actual backend resource
    // user. Abort must not admit a writer until that capture pin completes.
    auto capturing=ticket(fifth,5,3);CHECK(a.freeze_for_migration(capturing)==status::ok);
    auto capture=a.begin_migration_capture(capturing);CHECK(capture.code==status::ok);
    auto sixth=make_grant(key,6,6);CHECK(a.abort_migration(capturing,sixth)==status::pending);
    CHECK(a.admit({sixth,80,method::add_range},1).code==status::closed);
    CHECK(a.fence_migration(capturing)==status::conflict);
    CHECK(a.complete(capture.pin)==status::ok);
    CHECK(a.inspect().epoch==6 && a.inspect().gate==host_authority::phase::open);
    // Holder-transfer history remains queryable after physical relocation;
    // replay cannot mint a second successor or publish a superseded grant.
    host_authority origin(key,7),destination(key,7);
    CHECK(origin.set_authority({key,7,1,0,1,false,{initial}})==status::ok);
    auto mint=[](grant& g) {g.grant_id=g.epoch;g.secret[0]=static_cast<unsigned char>(g.epoch);return true;};
    transfer_request handoff{initial,{authority_origin::holder_transfer,1,1},2};transfer_receipt receipt;
    CHECK(origin.transfer_exclusive(handoff,1,mint,receipt)==status::ok);
    grant holder2;CHECK(origin.claim_transfer(handoff.reference(),2,holder2)==status::ok);
    transfer_request handback{holder2,{authority_origin::holder_transfer,2,1},1};
    CHECK(origin.transfer_exclusive(handback,2,mint,receipt)==status::ok);
    grant owner;CHECK(origin.claim_transfer(handback.reference(),1,owner)==status::ok);
    auto relocation=ticket(owner,1);CHECK(origin.freeze_for_migration(relocation)==status::ok);
    m::authority_image evidence;CHECK(origin.export_authority(relocation,evidence)==status::ok);
    CHECK(m::encode_authority_image(evidence,serialized)==status::ok);
    CHECK(m::decode_authority_image(serialized,restored)==status::ok && restored==evidence);
    auto relocated=make_grant(key,4,4);CHECK(destination.stage_import(relocation,restored,relocated)==status::ok);
    CHECK(origin.fence_migration(relocation)==status::ok && destination.activate_import(relocation)==status::ok);
    bool minted_again=false;
    CHECK(destination.transfer_exclusive(handoff,1,[&](grant&) {minted_again=true;return false;},receipt)==status::ok);
    CHECK(!minted_again && receipt.usability==transfer_usability::superseded);
    CHECK(destination.claim_transfer(handoff.reference(),2,holder2)==status::superseded);
    std::cout<<"Real Host Drain, migration fencing, returned placement, dedup and abort checks passed\n";
}
