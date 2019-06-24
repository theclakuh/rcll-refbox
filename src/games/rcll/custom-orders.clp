;-- force activate custom-orders
(defrule load-custom-order
  (not (custom-order))
=>
  (assert (custom-order))
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
                (start-range ?ss&:(< ?ss 1020) ?se&:(<= ?se 1020))
         ) 
=>
  (printout t "INFO: Invalidated initial order id " ?id crlf)
  
  ;-- shift the standard game orders to the end of the game-time
  (modify ?of (start-range      1020 1020) 
              (duration-range      0    0) 
              (delivery-period  1020 1020) 
              (activation-range 1020 1020)
  )
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
    (insert-custom-order ?o ?now)
  )
)

(defrule broadcast-custom-orders
  (custom-order)
  ?of <- (order (id ?id&:(> ?id 10)))
  ?np <- (network-peer (group ?grp) (id ?peer-id))

=>

  (printout t "INFO: Broadcast custom order " ?id " to peer " ?peer-id " (" ?grp ")" crlf)
  
  ;-- create order-info from orders in kb
  (bind ?oi (net-create-OrderInfo))
  
  ;-- broadcast order-info to active team
  (pb-broadcast ?peer-id ?oi)

  ;-- release order-info object
  (pb-destroy ?oi)
)
