-------------------------- MODULE PlacementReplay --------------------------
EXTENDS Naturals, TLC
CONSTANTS CheckRetireGeneration, CheckActivationPlacement
VARIABLES owner, generation, native, retirements, activations, route
vars == <<owner,generation,native,retirements,activations,route>>
Init == /\ owner="A" /\ generation=1 /\ native=[h \in {"A","B"} |-> IF h="A" THEN 1 ELSE 0]
        /\ retirements={} /\ activations={<<"A",1>>} /\ route= <<"A",1>>
(* Move abstracts the separately checked Drain/Commit/Fence/Activate protocol. *)
Move == /\ generation<3
        /\ owner'=IF owner="A" THEN "B" ELSE "A"
        /\ generation'=generation+1
        /\ native'=[native EXCEPT ![owner']=generation']
        /\ retirements'=retirements \cup {<<owner,generation>>}
        /\ activations'=activations \cup {<<owner',generation'>>}
        /\ route'= <<owner',generation'>>
Retire(r) == /\ r \in retirements
             /\ native'=IF CheckRetireGeneration /\ native[r[1]]#r[2] THEN native ELSE [native EXCEPT ![r[1]]=0]
             /\ UNCHANGED <<owner,generation,retirements,activations,route>>
LateActivation(a) == /\ a \in activations
                     /\ route'=IF CheckActivationPlacement /\ a# <<owner,generation>> THEN route ELSE a
                     /\ UNCHANGED <<owner,generation,native,retirements,activations>>
Next == Move \/ (\E r \in retirements: Retire(r)) \/ (\E a \in activations: LateActivation(a))
ActiveData == native[owner]=generation
CurrentRoute == route= <<owner,generation>>
Spec == Init /\ [][Next]_vars
=============================================================================
