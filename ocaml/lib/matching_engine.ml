open Types
module IntMap = Map.Make (Int)

type order_id = int

type fill = { maker : order_id; taker : order_id; price : int; qty : int; taker_side : side }

type order = { id : order_id; side : side; price : int; mutable qty : int }

(* A price level is a FIFO of orders plus the level total. Cancels are lazy: the
   order's size drops to zero and it stays in the queue until matching reaches it,
   so a cancel is O(1) and never walks the queue. *)
type level = { mutable total : int; orders : order Queue.t }

type t = { mutable bids : level IntMap.t; mutable asks : level IntMap.t; index : (order_id, order) Hashtbl.t }

let create () = { bids = IntMap.empty; asks = IntMap.empty; index = Hashtbl.create 1024 }

let book_side t = function Buy -> t.bids | Sell -> t.asks

let set_side t side m = match side with Buy -> t.bids <- m | Sell -> t.asks <- m

(* Best level an incoming order of [taker] side would trade against. *)
let best_opposite t taker =
  match taker with Buy -> IntMap.min_binding_opt t.asks | Sell -> IntMap.max_binding_opt t.bids

let crosses taker ~limit level_px =
  match taker with Buy -> level_px <= limit | Sell -> level_px >= limit

(* Trade [qty] of the taker against one level. Returns the quantity left. *)
let fill_level t taker_id taker_side px lvl qty acc =
  let rec go qty acc =
    if qty = 0 || Queue.is_empty lvl.orders then (qty, acc)
    else
      let o = Queue.peek lvl.orders in
      if o.qty = 0 then begin
        ignore (Queue.pop lvl.orders);
        go qty acc
      end
      else begin
        let q = min qty o.qty in
        o.qty <- o.qty - q;
        lvl.total <- lvl.total - q;
        if o.qty = 0 then begin
          ignore (Queue.pop lvl.orders);
          Hashtbl.remove t.index o.id
        end;
        go (qty - q) ({ maker = o.id; taker = taker_id; price = px; qty = q; taker_side } :: acc)
      end
  in
  go qty acc

let match_incoming t ~id ~side ~limit qty =
  let maker_side = opposite side in
  let rec go qty acc =
    if qty = 0 then (qty, acc)
    else
      match best_opposite t side with
      | Some (px, lvl) when crosses side ~limit px ->
          let qty, acc = fill_level t id side px lvl qty acc in
          if lvl.total = 0 then set_side t maker_side (IntMap.remove px (book_side t maker_side));
          go qty acc
      | _ -> (qty, acc)
  in
  let left, acc = go qty [] in
  (left, List.rev acc)

let check_new t id qty =
  if qty <= 0 then invalid_arg "Matching_engine: quantity must be positive";
  if Hashtbl.mem t.index id then invalid_arg (Printf.sprintf "Matching_engine: order %d already resting" id)

let limit t ~id ~side ~price ~qty =
  check_new t id qty;
  let left, fills = match_incoming t ~id ~side ~limit:price qty in
  if left > 0 then begin
    let o = { id; side; price; qty = left } in
    let m = book_side t side in
    let lvl =
      match IntMap.find_opt price m with
      | Some l -> l
      | None ->
          let l = { total = 0; orders = Queue.create () } in
          set_side t side (IntMap.add price l m);
          l
    in
    Queue.push o lvl.orders;
    lvl.total <- lvl.total + left;
    Hashtbl.replace t.index id o
  end;
  fills

let market t ~id ~side ~qty =
  check_new t id qty;
  let limit = match side with Buy -> max_int | Sell -> min_int in
  snd (match_incoming t ~id ~side ~limit qty)

let remove_qty t o q =
  let m = book_side t o.side in
  match IntMap.find_opt o.price m with
  | None -> assert false
  | Some lvl ->
      o.qty <- o.qty - q;
      lvl.total <- lvl.total - q;
      if o.qty = 0 then Hashtbl.remove t.index o.id;
      if lvl.total = 0 then set_side t o.side (IntMap.remove o.price m)

let cancel t id =
  match Hashtbl.find_opt t.index id with
  | None -> false
  | Some o ->
      remove_qty t o o.qty;
      true

let reduce t id ~by =
  if by <= 0 then invalid_arg "Matching_engine.reduce: amount must be positive";
  match Hashtbl.find_opt t.index id with
  | None -> false
  | Some o ->
      remove_qty t o (min by o.qty);
      true

let top = function Some (px, lvl) -> Some (px, lvl.total) | None -> None

let best_bid t = top (IntMap.max_binding_opt t.bids)

let best_ask t = top (IntMap.min_binding_opt t.asks)

let depth t side n =
  let levels = IntMap.bindings (book_side t side) in
  let levels = match side with Buy -> List.rev levels | Sell -> levels in
  List.filteri (fun i _ -> i < n) levels |> List.map (fun (px, l) -> (px, l.total))

let resting t id = Option.map (fun o -> (o.side, o.price, o.qty)) (Hashtbl.find_opt t.index id)

let queue t side px =
  match IntMap.find_opt px (book_side t side) with
  | None -> []
  | Some lvl -> Queue.fold (fun acc o -> if o.qty > 0 then (o.id, o.qty) :: acc else acc) [] lvl.orders |> List.rev

let order_count t = Hashtbl.length t.index
