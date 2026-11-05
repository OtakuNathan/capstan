-------------------------- MODULE LeasePermission --------------------------
EXTENDS Naturals

(* Semantic interface of Lease's ParticipantCountRefined model, kept local so
   Capstan's formal CI does not require a sibling checkout. These are permission
   guards, not std::shared_ptr ownership and not a distributed reference count.
   Count includes the live root plus non-root read participants. The rw holder
   is thread-confined; acquiring a reader during a write is not a legal call. *)
Modes == {"rw", "ro", "none"}
BoolToNat(b) == IF b THEN 1 ELSE 0
Participants(alive, readers) == BoolToNat(alive) + readers
MayRead(alive, mode, writing) == alive /\ mode \in {"rw", "ro"} /\ ~writing
MayWrite(alive, mode, readers, writing) ==
    alive /\ mode="rw" /\ readers=0 /\ ~writing
CountMatches(alive, readers, count) == count=Participants(alive, readers)
=============================================================================
