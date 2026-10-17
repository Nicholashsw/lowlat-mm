(* Order state and position, with the C++ OMS's state machine.

   PendingNew -> Working (ack) -> PartiallyFilled -> Filled
              \-> PendingCancel (cancel sent) -> Cancelled (cancel ack)
   A fill can race an ack or a cancel; whichever arrives, the order ends in the
   state the exchange actually left it in. *)

open Types

type status = Pending_new | Working | Partially_filled | Pending_cancel | Filled | Cancelled | Rejected

type order = { id : int; side : side; px : int; qty : int; mutable filled : int; mutable status : status }

let terminal o = match o.status with Filled | Cancelled | Rejected -> true | _ -> false

let leaves o = o.qty - o.filled

type position = { mutable net : int; mutable avg_px : float; mutable realized : float }

type t = {
  orders : (int, order) Hashtbl.t;
  live : (int, order) Hashtbl.t;  (* non-terminal orders only, so risk checks stay O(open) *)
  pos : position;
  mutable next_id : int;
  mutable fills : int;
  mutable submitted : int;
}

let create () =
  {
    orders = Hashtbl.create 4096;
    live = Hashtbl.create 16;
    pos = { net = 0; avg_px = 0.0; realized = 0.0 };
    next_id = 1;
    fills = 0;
    submitted = 0;
  }

let find t id = Hashtbl.find_opt t.orders id

let settle t o = if terminal o then Hashtbl.remove t.live o.id

let submit t ~side ~px ~qty =
  let o = { id = t.next_id; side; px; qty; filled = 0; status = Pending_new } in
  t.next_id <- t.next_id + 1;
  t.submitted <- t.submitted + 1;
  Hashtbl.replace t.orders o.id o;
  Hashtbl.replace t.live o.id o;
  o

(* [true] if a cancel should be sent. *)
let cancel t id =
  match find t id with
  | Some o when not (terminal o || o.status = Pending_cancel) ->
      o.status <- Pending_cancel;
      true
  | _ -> false

let on_ack t id = match find t id with Some o when o.status = Pending_new -> o.status <- Working | _ -> ()

(* Average-price accounting. Adding to a position blends the entry price; a
   reducing fill realises PnL on the closed part and leaves the entry price alone;
   a fill that flips the sign opens the remainder at the fill price. *)
let apply_fill pos side px qty =
  let signed = match side with Buy -> qty | Sell -> -qty in
  let px = float_of_int px in
  if pos.net = 0 || (pos.net > 0) = (signed > 0) then begin
    let net' = pos.net + signed in
    pos.avg_px <- ((pos.avg_px *. float_of_int pos.net) +. (px *. float_of_int signed)) /. float_of_int net';
    pos.net <- net'
  end
  else begin
    let closing = min (abs pos.net) (abs signed) in
    let per_unit = if pos.net > 0 then px -. pos.avg_px else pos.avg_px -. px in
    pos.realized <- pos.realized +. (per_unit *. float_of_int closing);
    let before = pos.net in
    pos.net <- pos.net + signed;
    if pos.net = 0 then pos.avg_px <- 0.0 else if before > 0 <> (pos.net > 0) then pos.avg_px <- px
  end

let on_fill t id ~px ~qty ~final =
  match find t id with
  | None -> None
  | Some o ->
      o.filled <- o.filled + qty;
      apply_fill t.pos o.side px qty;
      t.fills <- t.fills + 1;
      o.status <- (if final || leaves o = 0 then Filled else Partially_filled);
      settle t o;
      Some o

let on_cancel_ack t id ~accepted =
  match find t id with
  | Some o when accepted && not (terminal o) ->
      o.status <- Cancelled;
      settle t o
  | _ -> ()

let unrealized pos mid = if pos.net = 0 then 0.0 else (float_of_int mid -. pos.avg_px) *. float_of_int pos.net

let open_leaves t side =
  Hashtbl.fold (fun _ o acc -> if o.side = side then acc + leaves o else acc) t.live 0

let gross_open t = Hashtbl.fold (fun _ o acc -> acc + leaves o) t.live 0
