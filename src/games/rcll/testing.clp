; ---------------------------------------------------------------------------
;  testing.clp - LLSF RefBox CLIPS rules for extra testing setup

;  Created: Tue Apr 29 19:20:09 2016
;  Copyright  2016  Frederik Zwilling
;  Licensed under BSD license, cf. LICENSE file
; ---------------------------------------------------------------------------


(defrule init-carologistics-map
  (not (init-carologistics-map))
  (confval (path "/llsfrb/testing/enable") (type BOOL) (value true))
  (confval (path "/llsfrb/testing/use-carologistics-map") (type BOOL) (value true))
  =>
	(printout warn "Using carologistics testing map, setting machine zones accordingly" crlf)
  (assert (init-carologistics-map))
  (do-for-fact ((?m machine)) (eq ?m:name C-BS)  (modify ?m (zone Z9)))
  (do-for-fact ((?m machine)) (eq ?m:name C-DS)  (modify ?m (zone Z4)))
  (do-for-fact ((?m machine)) (eq ?m:name C-CS1)  (modify ?m (zone Z6)))
  (do-for-fact ((?m machine)) (eq ?m:name C-CS2)  (modify ?m (zone Z5)))
  (do-for-fact ((?m machine)) (eq ?m:name C-RS1)  (modify ?m (zone Z11)))
  (do-for-fact ((?m machine)) (eq ?m:name C-RS2)  (modify ?m (zone Z2)))

  (do-for-fact ((?m machine)) (eq ?m:name M-BS)  (modify ?m (zone Z21)))
  (do-for-fact ((?m machine)) (eq ?m:name M-DS)  (modify ?m (zone Z16)))
  (do-for-fact ((?m machine)) (eq ?m:name M-CS1)  (modify ?m (zone Z18)))
  (do-for-fact ((?m machine)) (eq ?m:name M-CS2)  (modify ?m (zone Z17)))
  (do-for-fact ((?m machine)) (eq ?m:name M-RS1)  (modify ?m (zone Z23)))
  (do-for-fact ((?m machine)) (eq ?m:name M-RS2)  (modify ?m (zone Z14)))
)