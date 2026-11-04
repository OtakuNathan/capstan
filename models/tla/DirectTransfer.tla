-------------------------- MODULE DirectTransfer --------------------------
EXTENDS Naturals, FiniteSets, TLC
CONSTANTS MaxEpoch, RequireSource, RequireDrain, RequireInstalled, SeparateKeys
Holders == {1,2}
NoTx == [origin |-> 0, issuer |-> 0, seq |-> 0, source |-> 0, from |-> 0, to |-> 0]
Key(t) == IF SeparateKeys THEN <<t.origin,t.issuer,t.seq>> ELSE <<t.seq>>
VARIABLES epoch, holder, gate, pins, held, pending, accepted, installed,
          receipt, observed, published, badSource, badClaim, collision
vars == <<epoch,holder,gate,pins,held,pending,accepted,installed,
          receipt,observed,published,badSource,badClaim,collision>>
Init == /\ epoch=0 /\ holder=1 /\ gate="Open" /\ pins={}
        /\ held={[who |-> 1, at |-> 0]} /\ pending=NoTx
        /\ accepted={} /\ installed={} /\ receipt=NoTx
        /\ observed=0 /\ published={} /\ badSource=FALSE /\ badClaim=FALSE /\ collision=FALSE
Admit == /\ gate="Open" /\ pins={} /\ epoch<MaxEpoch
         /\ pins'={epoch} /\ UNCHANGED <<epoch,holder,gate,held,pending,accepted,installed,receipt,observed,published,badSource,badClaim,collision>>
Complete == /\ pins#{} /\ pins'={}
            /\ UNCHANGED <<epoch,holder,gate,held,pending,accepted,installed,receipt,observed,published,badSource,badClaim,collision>>
Submit(t) ==
    LET prior == {r \in accepted: Key(r)=Key(t)} IN
    \/ /\ prior#{}
       /\ collision'=(collision \/ (\E r \in prior: <<r.origin,r.issuer,r.seq>>#<<t.origin,t.issuer,t.seq>>))
       /\ receipt'=IF t \in installed THEN t ELSE receipt
       /\ UNCHANGED <<epoch,holder,gate,pins,held,pending,accepted,installed,observed,published,badSource,badClaim>>
    \/ /\ prior={} /\ gate="Open" /\ epoch<MaxEpoch
       /\ (t.origin=0 => t.source=epoch)
       /\ (t.origin=1 /\ RequireSource => t.source=epoch /\ t.from=holder)
       /\ badSource'=(badSource \/ (t.origin=1 /\ (t.source#epoch \/ t.from#holder)))
       /\ gate'="Closing" /\ pending'=t /\ accepted'=accepted \cup {t}
       /\ UNCHANGED <<epoch,holder,pins,held,installed,receipt,observed,published,badClaim,collision>>
Transfer == \E h \in held, seq \in 1..2:
    Submit([origin |-> 1, issuer |-> h.who, seq |-> seq, source |-> h.at, from |-> h.who, to |-> 3-h.who])
Admin == \E target \in Holders, seq \in 1..2:
    Submit([origin |-> 0, issuer |-> 3, seq |-> seq, source |-> observed, from |-> 0, to |-> target])
Install == /\ gate="Closing" /\ (~RequireDrain \/ pins={})
           /\ epoch'=epoch+1 /\ holder'=pending.to /\ gate'="Open"
           /\ installed'=installed \cup {pending} /\ pending'=NoTx
           /\ UNCHANGED <<pins,held,accepted,receipt,observed,published,badSource,badClaim,collision>>
Query == \E t \in installed:
    /\ receipt'=t
    /\ UNCHANGED <<epoch,holder,gate,pins,held,pending,accepted,installed,observed,published,badSource,badClaim,collision>>
Claim == \E t \in accepted:
    /\ (~RequireInstalled \/ (t \in installed /\ t.source+1=epoch /\ t.to=holder /\ gate="Open"))
    /\ badClaim'=(badClaim \/ ~(t \in installed /\ t.source+1=epoch /\ t.to=holder /\ gate="Open"))
    /\ held'=held \cup {[who |-> t.to, at |-> t.source+1]}
    /\ UNCHANGED <<epoch,holder,gate,pins,pending,accepted,installed,receipt,observed,published,badSource,collision>>
Observe == /\ observed'=epoch
           /\ published'=IF gate="Open" THEN {[who |-> holder, at |-> epoch]} ELSE {}
           /\ UNCHANGED <<epoch,holder,gate,pins,held,pending,accepted,installed,receipt,badSource,badClaim,collision>>
(* A historical ACK can advance observation, but cannot republish its grants. *)
LateAck == \E t \in installed:
    /\ t.origin=0 /\ observed'=IF observed>t.source+1 THEN observed ELSE t.source+1
    /\ UNCHANGED <<epoch,holder,gate,pins,held,pending,accepted,installed,receipt,published,badSource,badClaim,collision>>
Next == Admit \/ Complete \/ Transfer \/ Admin \/ Install \/ Query \/ Claim \/ Observe \/ LateAck
TypeOK == /\ epoch \in 0..MaxEpoch /\ holder \in Holders /\ gate \in {"Open","Closing"}
          /\ observed<=epoch /\ pins \subseteq 0..MaxEpoch
ActiveEpoch == \A e \in pins:e=epoch
SourceCurrent == ~badSource
ClaimCurrent == ~badClaim
NamespaceIsolation == ~collision
ReceiptInstalled == receipt=NoTx \/ receipt \in installed
ObservationMonotone == observed<=epoch
Spec == Init /\ [][Next]_vars
=============================================================================
