;-- force activate custom-orders
(defrule load-custom-order
  (not (custom-order))
=>
  (assert (custom-order))
)

;--
;-- TEMPLATE DEFS
;--
(deftemplate response-info
  (slot client (type INTEGER)) 
  (slot extern-id (type INTEGER)) 
  (slot order-id (type INTEGER)) 
)

;--
;-- FUNCTIONS
;--

(deffunction id-from-gensym
  ()

  (bind ?idstr (str-cat (gensym*)))
  (bind ?idstr-len (str-length ?idstr))
  (bind ?id (integer (string-to-field (sub-string 4 ?idstr-len ?idstr))))
  (bind ?id (+ ?id (integer 10))) ;-- add init order offset

  (return ?id)
)

(deffunction insert-custom-order
  (?order ?order-time)

  (bind ?custom-order-id (id-from-gensym))
  (bind ?gate (random 1 3))
  (bind ?complexity (pb-field-value ?order "complexity"))
  (bind ?base-color (pb-field-value ?order "base_color"))
  (bind ?cap-color (pb-field-value ?order "cap_color"))
  (bind ?ring-colors (create$))
  (progn$ (?color (pb-field-list ?order "ring_colors"))
    (bind ?ring-colors (append$ ?ring-colors  ?color))
  )

  (assert (order (id ?custom-order-id) 
                 (complexity ?complexity)
                 (competitive FALSE)
                 (base-color ?base-color)
                 (ring-colors ?ring-colors)
                 (cap-color ?cap-color)
                 (quantity-requested 1) 
                 (quantity-delivered 0 0)
                 (start-range 0 0) 
                 (duration-range 0 0)
                 (delivery-period (+ ?order-time 5.0) 1020)
                 (delivery-gate ?gate)
                 (active FALSE) ;-- TODO: determine when this gets activated 
                 (activate-at (+ ?order-time 5.0))
                 (activation-range 0 0)
                 (allow-overtime FALSE)
         )
  )

  (return ?custom-order-id)
)

;--
;-- RULES
;--

(defrule print-custom-order-success
  (custom-order)
=>
  (printout t "INFO: Enabling custom-order was successful" crlf)
)

(defrule invalidate-initial-orders
  (custom-order)
  ?of <- (order (id ?id&:(< ?id 10))
                (start-range ?ss&:(< ?ss 1320) ?se&:(<= ?se 1320))
         ) 
=>
  (printout t "INFO: Invalidated initial order id " ?id crlf)

  (retract ?of)  

  ;-- shift the standard game orders to the end of the game-time
  ;(modify ?of (start-range      1020 1020) 
  ;            (duration-range      0    0) 
  ;            (delivery-period  1020 1020) 
  ;            (activation-range 1020 1020)
  ;)
)

(defrule net-recv-custom-OrderInfo
  (custom-order)
  (gamestate (state RUNNING) (phase PRODUCTION) (game-time ?now))
  ?pf <- (protobuf-msg (type "llsf_msgs.OrderInfo")
            (ptr ?ptr)
            (rcvd-via STREAM)
            (rcvd-from ?from-host ?from-port)
            (client-id ?cid)
         )
=>
  (printout t "INFO: Resceived and insert order information from client " ?cid crlf)
  
  ;-- insert orders from message
  (foreach ?o (pb-field-list ?ptr "orders")
    (bind ?eid (pb-field-value ?o "id"))
    (bind ?oid 
      (insert-custom-order ?o ?now)
    )
    (assert (response-info
              (client ?cid) 
              (extern-id ?eid) 
              (order-id ?oid)
            )
    )
  )
)

(defrule custom-product-delivered
  (custom-order)
  ?gf <- (gamestate (phase PRODUCTION|POST_GAME))
  ?pf <- (product-delivered 
           (order ?oid&~0) 
         )
  ?ri <- (response-info 
           (client ?cid) 
           (extern-id ?eid) 
           (order-id ?oid)
         )
=>
  (retract ?ri)
  (printout t "INFO: responed to delivered " ?oid " (aka. " ?eid ") ordered by " ?cid crlf)
  
)
