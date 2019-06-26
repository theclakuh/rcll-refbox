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

  ;-- build id as string
  (bind ?idstr (str-cat (gensym*)))
  (bind ?idstr-len (str-length ?idstr))

  ;-- turn to interger and add order offset
  (bind ?id (integer (string-to-field (sub-string 4 ?idstr-len ?idstr))))
  (bind ?id (+ ?id (integer 10))) ;-- add init order offset

  (return ?id)
)

(deffunction insert-custom-order
  (?order ?order-time)

  ;-- gather data from message object
  (bind ?custom-order-id (id-from-gensym))
  (bind ?gate (random 1 3))
  (bind ?complexity (pb-field-value ?order "complexity"))
  (bind ?base-color (pb-field-value ?order "base_color"))
  (bind ?cap-color (pb-field-value ?order "cap_color"))
  (bind ?ring-colors (create$))
  (progn$ (?color (pb-field-list ?order "ring_colors"))
    (bind ?ring-colors (append$ ?ring-colors  ?color))
  )

  ;-- compose and insert order fact
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

  ;-- remove original game order
  (retract ?of)  
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
    ;-- remember orders and orderer (client)
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
           (team ?team)
         )
  ?ri <- (response-info 
           (client ?cid) 
           (extern-id ?eid) 
           (order-id ?oid)
         )
  ?of <- (order
           (id ?oid)
         )
=>
  ;-- forget order and orderer
  (retract ?ri)

  (printout t "INFO: responed to delivered " ?oid " (aka. " ?eid ") ordered by " ?cid crlf)

  ;-- compose message to inform controller about delivery
  (bind ?delivery-msg (pb-create "llsf_msgs.SetOrderDelivered"))
  (pb-set-field ?delivery-msg "order_id" ?eid)
                  (pb-set-field ?delivery-msg "team_color" ?team)

  ;-- actually send message
  ;(pb-broadcast ?cid ?delivery-msg)
  (pb-send ?cid ?delivery-msg)
  (pb-destroy ?delivery-msg)
)
