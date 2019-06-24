(deffacts custom-order-game
  (custom-order)
)

(defrule load-custom-order
  (not (custom-order))
=>
  (assert (custom-order))
)

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
  (printout t "complexity " ?complexity crlf)
  (bind ?base-color (pb-field-value ?order "base_color"))
  (printout t "base-color " ?base-color crlf)
  (bind ?cap-color (pb-field-value ?order "cap_color"))
  (printout t "cap-color " ?cap-color crlf)
  (bind ?ring-colors (create$))
  (progn$ (?color (pb-field-list ?order "ring_colors"))
    (bind ?ring-colors (append$ ?ring-colors  ?color))
    (printout t "ring-color " ?color crlf)
  )

  (assert (order (id ?custom-order-id) 
                 (complexity ?complexity)
                 (competitive FALSE)
                 (base-color ?base-color)
                 (ring-colors ?ring-colors)
                 (cap-color ?cap-color)
                 (quantity-requested 1) 
                 (quantity-delivered 0 0)
                 (start-range ?order-time 1020) 
                 (duration-range 0 1020)
                 (delivery-period 0 1020)
                 (delivery-gate ?gate)
                 (active FALSE) ;-- TODO: determine when this gets activated 
                 (activate-at (+ ?order-time 1.0))
                 (activation-range (max 0.0 (- ?order-time 1.0)) (+ ?order-time 5.0)) ;-- deploy order in the next 5 seconds
                 (allow-overtime TRUE)
          )
  )
)

(defrule enable-custom-order-success
  (custom-order)
=>
  (printout t "INFO: enabling custom-order was successful" crlf)
)

(defrule invalidate-initial-orders
  (custom-order)
  ?of <- (order (id ?id&:(< ?id 10))
                (start-range ?ss&:(< ?ss 9998) ?se&:(< ?se 9999))
         ) 
;  ?of <- (order (id ?id))
=>
  (printout t "INFO: invalidated initial order id " ?id crlf)
  (modify ?of (start-range      9998 9999) 
              (duration-range      0    1) 
              (delivery-period  9998 9999) 
              (activation-range 9998 9999)
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
  (network-peer (group ?grp) (id ?peer-id))

  =>

  ;(printout t "send custom-OrderInfo to " ?grp " " ?peer-id crlf)

  ;-- insert orders from message
  (foreach ?o (pb-field-list ?ptr "orders")
    (insert-custom-order ?o ?now)
  )

  ;-- create order-info from orders in kb
  (bind ?oi (net-create-OrderInfo))
  
  ;-- broadcast order-info to active team
  (pb-broadcast ?peer-id ?oi)

  ;-- release order-info object
  (pb-destroy ?oi)
)

(defrule invalidate-init-orders
  (custom-order)
  ?of <- (order (id ?id&:(>= ?id 10)))
=>
  (printout t "added custom order with id " ?id crlf)
  (ppfact ?of)
)
