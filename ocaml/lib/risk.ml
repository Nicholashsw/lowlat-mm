(* Pre-trade gate and drawdown kill switch, the rules of risk/RiskManager.cpp.
   The position check is worst case: the new order and every open order on the
   same side are assumed to fill. *)

type limits = { max_position : int; max_order_size : int; max_open_qty : int; max_drawdown_ticks : float }

type t = { lim : limits; mutable killed : bool; mutable peak : float option }

let create lim = { lim; killed = false; peak = None }

let check t (oms : Oms.t) side qty =
  if t.killed then Error "kill_switch"
  else if qty <= 0 then Error "non_positive_qty"
  else if qty > t.lim.max_order_size then Error "order_size"
  else
    let same = Oms.open_leaves oms side in
    let signed = match side with Types.Buy -> qty + same | Types.Sell -> -(qty + same) in
    if abs (oms.pos.net + signed) > t.lim.max_position then Error "max_position"
    else if Oms.gross_open oms + qty > t.lim.max_open_qty then Error "max_open_qty"
    else Ok ()

let mark_equity t eq =
  let peak = match t.peak with None -> eq | Some p -> Float.max p eq in
  t.peak <- Some peak;
  if peak -. eq > t.lim.max_drawdown_ticks then t.killed <- true
