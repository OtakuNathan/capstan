-------------------------- MODULE CaptureOverlap --------------------------
EXTENDS Naturals, TLC
(* Composition only: DataReady is abstract. No chunk/window state belongs here. *)
CONSTANTS MaxReaders, ExcludeWriters, HoldCapturePin, FinalHistory
VARIABLES readers, writers, closed, capturing, dataReady, prepared, committed,
          aborting, version, saved, history, savedHistory
vars == <<readers,writers,closed,capturing,dataReady,prepared,committed,aborting,version,saved,history,savedHistory>>
Init == /\ readers=MaxReaders /\ writers \in 0..1
        /\ closed=FALSE /\ capturing=FALSE /\ dataReady=FALSE /\ prepared=FALSE
        /\ committed=FALSE /\ aborting=FALSE /\ version=0 /\ saved=0 /\ history=0 /\ savedHistory=0
Close == /\ ~closed /\ ~aborting /\ closed'=TRUE
         /\ UNCHANGED <<readers,writers,capturing,dataReady,prepared,committed,aborting,version,saved,history,savedHistory>>
ReadDone == /\ readers>0 /\ readers'=readers-1 /\ history'=history+1
            /\ UNCHANGED <<writers,closed,capturing,dataReady,prepared,committed,aborting,version,saved,savedHistory>>
WriteDone == /\ writers>0 /\ writers'=0 /\ version'=version+1 /\ history'=history+1
             /\ UNCHANGED <<readers,closed,capturing,dataReady,prepared,committed,aborting,saved,savedHistory>>
Capture == /\ closed /\ ~aborting /\ ~capturing /\ ~dataReady
           /\ (~ExcludeWriters \/ writers=0) /\ capturing'=TRUE /\ saved'=version
           /\ UNCHANGED <<readers,writers,closed,dataReady,prepared,committed,aborting,version,history,savedHistory>>
CaptureDone == /\ capturing /\ capturing'=FALSE /\ dataReady'=TRUE
               /\ UNCHANGED <<readers,writers,closed,prepared,committed,aborting,version,saved,history,savedHistory>>
Prepare == /\ closed /\ dataReady /\ ~prepared /\ ~aborting
           /\ (~FinalHistory \/ (readers=0 /\ writers=0 /\ ~capturing))
           /\ prepared'=TRUE /\ savedHistory'=history
           /\ UNCHANGED <<readers,writers,closed,capturing,dataReady,committed,aborting,version,saved,history>>
Commit == /\ closed /\ prepared /\ readers=0 /\ writers=0 /\ ~capturing /\ ~aborting /\ ~committed
          /\ committed'=TRUE
          /\ UNCHANGED <<readers,writers,closed,capturing,dataReady,prepared,aborting,version,saved,history,savedHistory>>
Abort == /\ closed /\ ~committed /\ ~aborting /\ aborting'=TRUE
         /\ UNCHANGED <<readers,writers,closed,capturing,dataReady,prepared,committed,version,saved,history,savedHistory>>
Restore == /\ closed /\ aborting /\ readers=0 /\ writers=0 /\ (~HoldCapturePin \/ ~capturing)
           /\ closed'=FALSE
           /\ UNCHANGED <<readers,writers,capturing,dataReady,prepared,committed,aborting,version,saved,history,savedHistory>>
Next == Close \/ ReadDone \/ WriteDone \/ Capture \/ CaptureDone \/ Prepare \/ Commit \/ Abort \/ Restore
TypeOK == /\ readers \in 0..MaxReaders /\ writers \in 0..1
          /\ <<closed,capturing,dataReady,prepared,committed,aborting>> \in BOOLEAN \X BOOLEAN \X BOOLEAN \X BOOLEAN \X BOOLEAN \X BOOLEAN
CaptureExcludesWriters == capturing => writers=0
CaptureProtected == capturing => closed
CommittedImage == committed => (saved=version /\ savedHistory=history /\ dataReady /\ readers=0 /\ writers=0 /\ ~capturing)
NoOverlap == ~(capturing /\ readers>0)
Spec == Init /\ [][Next]_vars
=============================================================================
