(** A single-instrument limit order book with price-time priority.

    Incoming orders trade against the best opposite level first and, within a
    level, against resting orders in arrival order. Trades print at the resting
    order's price. Prices are integer ticks, quantities positive integers. *)

type order_id = int

type fill = { maker : order_id; taker : order_id; price : int; qty : int; taker_side : Types.side }

type t

val create : unit -> t

val limit : t -> id:order_id -> side:Types.side -> price:int -> qty:int -> fill list
(** Match as far as the limit price allows, then rest the remainder. Fills are
    returned in execution order. Raises [Invalid_argument] on a non-positive
    quantity or an id that is already resting. *)

val market : t -> id:order_id -> side:Types.side -> qty:int -> fill list
(** Match against the book at any price; whatever cannot fill is dropped. *)

val cancel : t -> order_id -> bool
(** Remove a resting order. [false] if it is not resting. *)

val reduce : t -> order_id -> by:int -> bool
(** Cut a resting order's size and keep its place in the queue. Reducing by the
    whole remaining size cancels it. [false] if it is not resting. *)

val best_bid : t -> (int * int) option
(** Best bid price and the total size resting there. *)

val best_ask : t -> (int * int) option

val depth : t -> Types.side -> int -> (int * int) list
(** Up to [n] levels from the touch outwards, as (price, total size). *)

val resting : t -> order_id -> (Types.side * int * int) option
(** Side, price and remaining size of a resting order. *)

val queue : t -> Types.side -> int -> (order_id * int) list
(** Resting orders at one price level in priority order, as (id, size). *)

val order_count : t -> int
